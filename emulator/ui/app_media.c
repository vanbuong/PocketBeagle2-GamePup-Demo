// SPDX-License-Identifier: GPL-2.0-only
// Music player (mpv over a UNIX socket) and voice memos (arecord/aplay).
#define _GNU_SOURCE
#include "ui.h"
#include "compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>

#define MUSIC_ROOT "/opt/gamepup/music"
#define MEMO_DIR "/opt/gamepup/voice-memos"
#define VOLUME_FILE "/opt/gamepup/saves/music-volume"
#ifdef _WIN32
#define MPV_UID 0
#else
#define MPV_UID ((int)getuid())
#endif
#define SEEK_STEP 5
#define VOLUME_STEP 5
#define MEMO_RATE 48000
#define MEMO_BYTES_PER_FRAME 8
#define MEMO_MAX_SECONDS 120

static void fmt_time(char *out, size_t size, double seconds)
{
	int total;

	if (seconds < 0) {
		snprintf(out, size, "--:--");
		return;
	}
	total = (int)(seconds + 0.5);
	snprintf(out, size, "%d:%02d", total / 60, total % 60);
}

/* ====================================================================== */
/* mpv client                                                              */
/* ====================================================================== */

typedef enum { MP_OFF, MP_STARTING, MP_READY } mp_state_t;

static struct {
	mp_state_t state;
	int pid, sock;
	int candidate;
	uint32_t started_at, ipc_after;
	char socket_path[96];
	char card[64];
	char rx[4096];
	size_t rx_len;
	char **tracks;
	int ntracks, index;
	bool want_play;
	bool playing, paused, loop;
	double pos, dur;
	int volume;
	bool volume_pending;
	lv_timer_t *timer;
} mp = { .sock = -1, .volume = 70 };

static const char *mp_candidate(int n, char *buf, size_t size)
{
	const char *env = getenv("GAMEPUP_ALSA_DEVICE");

	switch (n) {
	case 0:
		return "alsa/gamepup";
	case 1:
		if (env && *env)
			snprintf(buf, size, "%s%s", strncmp(env, "alsa/", 5) ? "alsa/" : "", env);
		else if (mp.card[0])
			snprintf(buf, size, "alsa/plughw:%s,0", mp.card);
		else
			return "alsa/default";
		return buf;
	case 2:
		return "alsa/default";
	default:
		return NULL;
	}
}

#ifndef _WIN32
static void mp_send(const char *json)
{
	if (mp.sock >= 0 && send(mp.sock, json, strlen(json), MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
	    errno != EAGAIN) {
		close(mp.sock);
		mp.sock = -1;
	}
}
#else
static void mp_send(const char *json)
{
	(void)json;
}
#endif

static void mp_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void mp_cmd(const char *fmt, ...)
{
	char json[1024];
	int n;
	va_list ap;

	n = snprintf(json, sizeof(json), "{\"command\":[");
	va_start(ap, fmt);
	n += vsnprintf(json + n, sizeof(json) - (size_t)n - 4, fmt, ap);
	va_end(ap);
	snprintf(json + n, sizeof(json) - (size_t)n, "]}\n");
	mp_send(json);
}

static void json_escape(const char *in, char *out, size_t size)
{
	size_t n = 0;

	for (; *in && n + 3 < size; in++) {
		if (*in == '"' || *in == '\\')
			out[n++] = '\\';
		out[n++] = *in;
	}
	out[n] = '\0';
}

static void mp_apply_volume(void)
{
	char level[16];
	const char *argv[] = { "amixer", "-q", "-c", mp.card, "sset", "PCM", level, NULL };

	if (!mp.card[0] || !hal_which("amixer")) {
		mp.volume_pending = false;
		return;
	}
	snprintf(level, sizeof(level), "%d%%", mp.volume);
	hal_spawn_detached(argv);
	mp.volume_pending = false;
}

static void mp_set_volume(int level)
{
	if (level < 0)
		level = 0;
	if (level > 100)
		level = 100;
	mp.volume = level;
	hal_file_write_int(hal_p(VOLUME_FILE), level);
	/* The ALSA open can stall while the DAC starts; defer the mixer write. */
	if (hal_now_ms() >= mp.ipc_after)
		mp_apply_volume();
	else
		mp.volume_pending = true;
}

static void __attribute__((unused)) mp_parse_line(char *line)
{
	char *name, *data, *end;
	char key[32];

	if (!strstr(line, "property-change"))
		return;
	name = strstr(line, "\"name\":\"");
	data = strstr(line, "\"data\":");
	if (!name)
		return;
	name += 8;
	end = strchr(name, '"');
	if (!end || (size_t)(end - name) >= sizeof(key))
		return;
	memcpy(key, name, (size_t)(end - name));
	key[end - name] = '\0';
	if (!data)
		return;
	data += 7;
	if (!strcmp(key, "time-pos"))
		mp.pos = strtod(data, NULL);
	else if (!strcmp(key, "duration"))
		mp.dur = strtod(data, NULL);
	else if (!strcmp(key, "pause"))
		mp.paused = !strncmp(data, "true", 4);
	else if (!strcmp(key, "idle-active")) {
		if (!strncmp(data, "true", 4) && hal_now_ms() > mp.ipc_after - 4500) {
			mp.playing = false;
			mp.paused = false;
		} else if (!strncmp(data, "false", 5)) {
			mp.playing = true;
		}
	} else if (!strcmp(key, "playlist-pos")) {
		int idx = atoi(data);

		if (idx >= 0 && idx < mp.ntracks)
			mp.index = idx;
	}
}

#ifndef _WIN32
static void mp_pump(void)
{
	ssize_t n;
	char *nl;

	if (mp.sock < 0)
		return;
	while ((n = recv(mp.sock, mp.rx + mp.rx_len, sizeof(mp.rx) - 1 - mp.rx_len, MSG_DONTWAIT)) > 0) {
		mp.rx_len += (size_t)n;
		mp.rx[mp.rx_len] = '\0';
		while ((nl = strchr(mp.rx, '\n'))) {
			*nl = '\0';
			mp_parse_line(mp.rx);
			mp.rx_len -= (size_t)(nl + 1 - mp.rx);
			memmove(mp.rx, nl + 1, mp.rx_len + 1);
		}
		if (mp.rx_len >= sizeof(mp.rx) - 1)
			mp.rx_len = 0;
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		close(mp.sock);
		mp.sock = -1;
	}
}
#else
static void mp_pump(void)
{
}
#endif

static void mp_free_tracks(void)
{
	for (int i = 0; i < mp.ntracks; i++)
		free(mp.tracks[i]);
	free(mp.tracks);
	mp.tracks = NULL;
	mp.ntracks = 0;
	mp.index = -1;
}

static void mp_kill(void)
{
	if (mp.sock >= 0)
		close(mp.sock);
	mp.sock = -1;
	if (mp.pid > 0) {
		hal_kill(mp.pid, SIGTERM);
		for (int i = 0; i < 40 && !hal_child_done(mp.pid, NULL); i++)
			usleep(25000);
		hal_kill(mp.pid, SIGKILL);
		hal_child_done(mp.pid, NULL);
	}
	mp.pid = 0;
	mp.state = MP_OFF;
	unlink(mp.socket_path);
}

static void mp_try_start(void)
{
	char dev[160], audio[200];
	const char *cand = mp_candidate(mp.candidate, dev, sizeof(dev));
	const char *argv[] = {
		"mpv", "--no-config", "--idle=yes", "--vo=null", "--no-video", "--force-window=no",
		"--really-quiet", "--no-terminal", "--no-input-default-bindings", "--keep-open=no",
		"--ao=alsa", audio, NULL, NULL,
	};
	char ipc[160];

	if (!cand) {
		mp.state = MP_OFF;
		mp.want_play = false;
		ui_toast("Audio device unavailable");
		return;
	}
	snprintf(audio, sizeof(audio), "--audio-device=%s", cand);
	snprintf(mp.socket_path, sizeof(mp.socket_path), "/tmp/gamepup-mpv-%d.sock", MPV_UID);
	unlink(mp.socket_path);
	snprintf(ipc, sizeof(ipc), "--input-ipc-server=%s", mp.socket_path);
	argv[12] = ipc;
	mp.pid = hal_spawn(argv, NULL, true);
	if (mp.pid < 0) {
		mp.pid = 0;
		mp.state = MP_OFF;
		ui_toast("mpv is not installed");
		return;
	}
	mp.state = MP_STARTING;
	mp.started_at = hal_now_ms();
}

#ifndef _WIN32
static void mp_connect(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	static const char *const OBSERVE[] = { "time-pos", "duration", "pause", "idle-active",
					       "playlist-pos" };

	if (fd < 0)
		return;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", mp.socket_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		close(fd);
		return;
	}
	mp.sock = fd;
	mp.state = MP_READY;
	for (int i = 0; i < 5; i++)
		mp_cmd("\"observe_property\",%d,\"%s\"", i + 1, OBSERVE[i]);
}
#else
static void mp_connect(void)
{
}
#endif

static void mp_load_playlist(void)
{
	char esc[1024];

	for (int i = 0; i < mp.ntracks; i++) {
		json_escape(mp.tracks[i], esc, sizeof(esc));
		mp_cmd("\"loadfile\",\"%s\",\"%s\"", esc, i == 0 ? "replace" : "append");
	}
	mp_cmd("\"playlist-play-index\",%d", mp.index);
	mp_cmd("\"set_property\",\"pause\",false");
	mp.playing = true;
	mp.paused = false;
	mp.pos = 0;
	mp.dur = 0;
	mp.ipc_after = hal_now_ms() + 5000;
	mp.volume_pending = true;
	mp.want_play = false;
}

static void mp_tick(lv_timer_t *timer)
{
	(void)timer;
	if (mp.state == MP_STARTING) {
		int st;

		if (hal_child_done(mp.pid, &st)) {
			mp.pid = 0;
			mp.candidate++;
			mp_try_start();
		} else if (access(mp.socket_path, F_OK) == 0) {
			mp_connect();
			if (mp.state == MP_READY && mp.want_play)
				mp_load_playlist();
		} else if (hal_now_ms() - mp.started_at > 5000) {
			mp_kill();
			mp.candidate++;
			mp_try_start();
		}
	} else if (mp.state == MP_READY) {
		int st;

		if (mp.sock < 0 && mp.pid == 0 && getenv("GAMEPUP_FAKE_PLAYER") && mp.playing && !mp.paused) {
			mp.pos += 0.1;
			if (mp.pos > mp.dur)
				mp.pos = 0;
		}
		if (mp.pid > 0 && hal_child_done(mp.pid, &st)) {
			mp.pid = 0;
			mp.state = MP_OFF;
			mp.playing = false;
		}
		mp_pump();
		if (mp.volume_pending && hal_now_ms() >= mp.ipc_after && mp.playing)
			mp_apply_volume();
	}
}

static bool is_audio_name(const char *name)
{
	static const char *const EXT[] = { ".mp3", ".ogg", ".flac", ".wav", ".m4a", ".aac",
					   ".opus", ".wma" };
	const char *dot = strrchr(name, '.');

	if (!dot)
		return false;
	for (size_t i = 0; i < sizeof(EXT) / sizeof(EXT[0]); i++)
		if (!strcasecmp(dot, EXT[i]))
			return true;
	return false;
}

static void mp_play(char **paths, int count, int index)
{
	mp_free_tracks();
	mp.tracks = paths;
	mp.ntracks = count;
	mp.index = index;
	mp.want_play = true;
	if (!mp.timer)
		mp.timer = lv_timer_create(mp_tick, 100, NULL);
	if (getenv("GAMEPUP_FAKE_PLAYER")) {
		/* Simulator/demo: pretend playback so the UI can be previewed without mpv. */
		mp.state = MP_READY;
		mp_load_playlist();
		mp.dur = 212;
		mp.pos = 64;
	} else if (mp.state == MP_READY) {
		mp_load_playlist();
	} else if (mp.state == MP_OFF) {
		mp.candidate = 0;
		hal_alsa_card_id(mp.card, sizeof(mp.card));
		mp.volume = hal_file_read_int(hal_p(VOLUME_FILE), 100);
		mp_try_start();
	}
}

static void mp_toggle_pause(void)
{
	if (mp.state != MP_READY || !mp.playing)
		return;
	mp_cmd("\"cycle\",\"pause\"");
	mp.paused = !mp.paused;
}

static void mp_seek(int seconds)
{
	mp_cmd("\"seek\",%d,\"relative\"", seconds);
}

static void mp_next(void)
{
	if (mp.index + 1 < mp.ntracks)
		mp_cmd("\"playlist-next\",\"weak\"");
	else
		mp_cmd("\"playlist-play-index\",%d", mp.index);
}

static void mp_prev(void)
{
	if (mp.pos > 3 || mp.index <= 0)
		mp_cmd("\"seek\",0,\"absolute\"");
	else
		mp_cmd("\"playlist-prev\",\"weak\"");
}

static void mp_toggle_loop(void)
{
	mp.loop = !mp.loop;
	mp_cmd("\"set_property\",\"loop-playlist\",\"%s\"", mp.loop ? "inf" : "no");
}

static void mp_stop(void)
{
	mp_cmd("\"stop\"");
	mp.playing = false;
	mp.paused = false;
	mp_free_tracks();
}

void ui_media_shutdown(void)
{
	mp_kill();
	mp_free_tracks();
}

/* ====================================================================== */
/* Now Playing (styled after the LVGL music demo)                          */
/* ====================================================================== */

#define DEMO_DARK 0x504d6d
#define DEMO_DIM 0x8a86b8
#define DEMO_BLUE 0x569af8
#define DEMO_PURPLE 0xa666f1
#define BARS 32

typedef struct {
	lv_obj_t *title, *artist, *genre, *slider, *t_pos, *t_dur, *play_btn, *play_icon;
	lv_obj_t *loop, *spectrum, *vol_bar, *vol_txt;
	lv_timer_t *timer;
	int level[BARS];
	uint32_t phase;
} now_t;

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

static void spectrum_draw_cb(lv_event_t *e)
{
	now_t *n = lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t area;
	lv_draw_line_dsc_t dsc;
	int cx, cy;

	lv_obj_get_coords(obj, &area);
	cx = (area.x1 + area.x2) / 2;
	cy = (area.y1 + area.y2) / 2;
	lv_draw_line_dsc_init(&dsc);
	dsc.width = 3;
	dsc.round_start = 1;
	dsc.round_end = 1;
	for (int i = 0; i < BARS; i++) {
		int deg = i * 360 / BARS - 90;
		int r0 = 30, r1 = 33 + n->level[i] * 13 / 100;
		int32_t c = lv_trigo_cos(deg), sn = lv_trigo_sin(deg);

		dsc.color = lv_color_mix(lv_color_hex(DEMO_PURPLE), lv_color_hex(DEMO_BLUE),
					 (uint8_t)(i * 255 / BARS));
		dsc.p1.x = cx + ((r0 * c) >> 15);
		dsc.p1.y = cy + ((r0 * sn) >> 15);
		dsc.p2.x = cx + ((r1 * c) >> 15);
		dsc.p2.y = cy + ((r1 * sn) >> 15);
		lv_draw_line(layer, &dsc);
	}
}

static void now_update(lv_timer_t *timer)
{
	now_t *n = lv_timer_get_user_data(timer);
	char text[96], t[16];
	bool live = mp.playing && !mp.paused;

	/* The demo animates a spectrum from audio data; we animate it from the playback state. */
	n->phase += 11;
	for (int i = 0; i < BARS; i++) {
		int target = 8;

		if (live) {
			int32_t a = lv_trigo_sin((int)((n->phase * 3 + (uint32_t)i * 37) % 360));
			int32_t b = lv_trigo_sin((int)((n->phase * 5 + (uint32_t)i * 71) % 360));

			target = 45 + (int)(((a + b) * 50) >> 16);
		}
		n->level[i] = (n->level[i] * 2 + target) / 3;
	}
	lv_obj_invalidate(n->spectrum);

	if (mp.index >= 0 && mp.index < mp.ntracks) {
		char folder[96];
		const char *file = base_name(mp.tracks[mp.index]);
		const char *slash = strrchr(mp.tracks[mp.index], '/');

		snprintf(text, sizeof(text), "%s", file);
		char *dot = strrchr(text, '.');

		if (dot)
			*dot = '\0';
		lv_label_set_text(n->title, text);
		folder[0] = '\0';
		if (slash && slash != mp.tracks[mp.index]) {
			const char *p = slash - 1;

			while (p > mp.tracks[mp.index] && *p != '/')
				p--;
			if (*p == '/')
				p++;
			snprintf(folder, sizeof(folder), "%.*s", (int)(slash - p), p);
		}
		lv_label_set_text(n->artist, folder[0] ? folder : "GamePup Music");
		snprintf(text, sizeof(text), "Track %d of %d", mp.index + 1, mp.ntracks);
		lv_label_set_text(n->genre, text);
	} else {
		lv_label_set_text(n->title, "Nothing playing");
		lv_label_set_text(n->artist, "");
		lv_label_set_text(n->genre, "");
	}
	fmt_time(t, sizeof(t), mp.playing ? mp.pos : 0);
	lv_label_set_text(n->t_pos, t);
	fmt_time(t, sizeof(t), mp.playing && mp.dur > 0 ? mp.dur : -1);
	lv_label_set_text(n->t_dur, t);
	lv_slider_set_value(n->slider, mp.dur > 0 ? (int)(mp.pos * 1000 / mp.dur) : 0, LV_ANIM_OFF);
	lv_label_set_text(n->play_icon, live ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	lv_obj_set_style_text_color(n->loop, lv_color_hex(mp.loop ? DEMO_PURPLE : DEMO_DIM), 0);
	lv_bar_set_value(n->vol_bar, mp.volume, LV_ANIM_OFF);
	snprintf(text, sizeof(text), "%d%%", mp.volume);
	lv_label_set_text(n->vol_txt, text);
}

static bool now_key(ui_page_t *page, uint32_t key)
{
	now_t *n = page->user;

	switch (key) {
	case LV_KEY_ENTER:
		mp_toggle_pause();
		break;
	case GP_KEY_Y:
		mp_toggle_loop();
		break;
	case LV_KEY_LEFT:
		mp_seek(-SEEK_STEP);
		break;
	case LV_KEY_RIGHT:
		mp_seek(SEEK_STEP);
		break;
	case LV_KEY_UP:
		mp_set_volume(mp.volume + VOLUME_STEP);
		break;
	case LV_KEY_DOWN:
		mp_set_volume(mp.volume - VOLUME_STEP);
		break;
	case GP_KEY_START:
		mp_next();
		break;
	case GP_KEY_SELECT:
		mp_prev();
		break;
	case GP_KEY_X:
		mp_stop();
		ui_pop();
		return true;
	default:
		return false;
	}
	now_update(n->timer);
	return true;
}

static void now_hide(ui_page_t *page)
{
	now_t *n = page->user;

	lv_timer_delete(n->timer);
	free(n);
}

static lv_obj_t *demo_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
			    uint32_t color, int x, int y)
{
	lv_obj_t *label = ui_label(parent, text, font, color);

	lv_obj_set_pos(label, x, y);
	return label;
}

static lv_obj_t *round_btn(lv_obj_t *parent, int size, int x, int y, const char *glyph,
			   const lv_font_t *font, uint32_t color, bool filled, lv_obj_t **icon)
{
	lv_obj_t *btn = lv_obj_create(parent);
	lv_obj_t *label;

	lv_obj_remove_style_all(btn);
	lv_obj_set_size(btn, size, size);
	lv_obj_set_pos(btn, x, y);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	if (filled) {
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(btn, lv_color_hex(DEMO_BLUE), 0);
		lv_obj_set_style_bg_grad_color(btn, lv_color_hex(DEMO_PURPLE), 0);
		lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
	}
	label = ui_label(btn, glyph, font, color);
	lv_obj_center(label);
	if (icon)
		*icon = label;
	return filled ? btn : label;
}

static void open_now_playing(void)
{
	ui_page_t *page = ui_page_create("Now Playing", "A Pause  Y Loop");
	now_t *n = calloc(1, sizeof(*n));
	lv_obj_t *card, *disc, *vol_icon;

	page->user = n;
	page->on_key = now_key;
	page->on_hide = now_hide;
	ui_page_add_sink(page);

	card = lv_obj_create(page->content);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, 300, 164);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_white(), 0);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

	/* Album disc with radial spectrum bars. */
	n->spectrum = lv_obj_create(card);
	lv_obj_remove_style_all(n->spectrum);
	lv_obj_set_size(n->spectrum, 96, 96);
	lv_obj_set_pos(n->spectrum, 6, 4);
	lv_obj_remove_flag(n->spectrum, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(n->spectrum, spectrum_draw_cb, LV_EVENT_DRAW_POST, n);
	disc = lv_obj_create(n->spectrum);
	lv_obj_remove_style_all(disc);
	lv_obj_set_size(disc, 52, 52);
	lv_obj_center(disc);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(disc, lv_color_hex(DEMO_BLUE), 0);
	lv_obj_set_style_bg_grad_color(disc, lv_color_hex(DEMO_PURPLE), 0);
	lv_obj_set_style_bg_grad_dir(disc, LV_GRAD_DIR_VER, 0);
	lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_center(ui_label(disc, LV_SYMBOL_AUDIO, FONT_XL, 0xFFFFFF));

	/* Title block */
	n->title = demo_label(card, "", FONT_L, DEMO_DARK, 112, 10);
	lv_obj_set_width(n->title, 176);
	lv_label_set_long_mode(n->title, LV_LABEL_LONG_SCROLL_CIRCULAR);
	n->artist = demo_label(card, "", FONT_S, DEMO_DARK, 112, 34);
	lv_obj_set_width(n->artist, 176);
	lv_label_set_long_mode(n->artist, LV_LABEL_LONG_DOT);
	n->genre = demo_label(card, "", FONT_XS, DEMO_DIM, 112, 52);

	vol_icon = demo_label(card, LV_SYMBOL_VOLUME_MID, FONT_S, DEMO_DIM, 112, 72);
	(void)vol_icon;
	n->vol_bar = lv_bar_create(card);
	lv_obj_set_size(n->vol_bar, 100, 4);
	lv_obj_set_pos(n->vol_bar, 136, 79);
	lv_bar_set_range(n->vol_bar, 0, 100);
	lv_obj_set_style_bg_color(n->vol_bar, lv_color_hex(0xE4E0F5), LV_PART_MAIN);
	lv_obj_set_style_bg_color(n->vol_bar, lv_color_hex(DEMO_BLUE), LV_PART_INDICATOR);
	lv_obj_set_style_bg_grad_color(n->vol_bar, lv_color_hex(DEMO_PURPLE), LV_PART_INDICATOR);
	lv_obj_set_style_bg_grad_dir(n->vol_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
	n->vol_txt = demo_label(card, "", FONT_XS, DEMO_DIM, 244, 72);

	/* Progress slider and times */
	n->slider = lv_slider_create(card);
	lv_obj_set_size(n->slider, 272, 5);
	lv_obj_set_pos(n->slider, 14, 108);
	lv_slider_set_range(n->slider, 0, 1000);
	lv_obj_remove_flag(n->slider, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(n->slider, lv_color_hex(0xE4E0F5), LV_PART_MAIN);
	lv_obj_set_style_bg_color(n->slider, lv_color_hex(DEMO_BLUE), LV_PART_INDICATOR);
	lv_obj_set_style_bg_grad_color(n->slider, lv_color_hex(DEMO_PURPLE), LV_PART_INDICATOR);
	lv_obj_set_style_bg_grad_dir(n->slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(n->slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_border_width(n->slider, 0, LV_PART_MAIN);
	lv_obj_set_style_border_width(n->slider, 2, LV_PART_KNOB);
	lv_obj_set_style_border_color(n->slider, lv_color_hex(DEMO_PURPLE), LV_PART_KNOB);
	lv_obj_set_style_pad_all(n->slider, 2, LV_PART_KNOB);
	n->t_pos = demo_label(card, "0:00", FONT_XS, DEMO_DIM, 14, 118);
	n->t_dur = demo_label(card, "--:--", FONT_XS, DEMO_DIM, 258, 118);

	/* Transport row: loop, prev, play, next, stop */
	n->loop = round_btn(card, 32, 24, 126, LV_SYMBOL_LOOP, FONT_L, DEMO_DIM, false, NULL);
	round_btn(card, 32, 76, 126, LV_SYMBOL_PREV, FONT_L, DEMO_DARK, false, NULL);
	n->play_btn = round_btn(card, 40, 130, 122, LV_SYMBOL_PLAY, FONT_L, 0xFFFFFF, true, &n->play_icon);
	round_btn(card, 32, 192, 126, LV_SYMBOL_NEXT, FONT_L, DEMO_DARK, false, NULL);
	round_btn(card, 32, 244, 126, LV_SYMBOL_STOP, FONT_L, DEMO_DIM, false, NULL);

	n->timer = lv_timer_create(now_update, 60, n);
	now_update(n->timer);
	ui_push(page);
}

/* ====================================================================== */
/* Music browser                                                           */
/* ====================================================================== */

typedef struct {
	char cwd[1100];
	char root[1100];
	ui_page_t *page;
	lv_timer_t *timer;
	lv_obj_t *bar_title;
} browser_t;

typedef struct {
	char path[1100];
	bool dir;
} entry_t;

static int entry_cmp(const void *a, const void *b)
{
	const entry_t *x = a, *y = b;

	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return strcasecmp(base_name(x->path), base_name(y->path));
}

static void browser_load(browser_t *b, const char *dir);

/* Rebuilding the list deletes the row whose event is being dispatched, so defer it. */
typedef struct {
	browser_t *b;
	char path[1100];
} load_req_t;

static void load_async(void *arg)
{
	load_req_t *req = arg;

	browser_load(req->b, req->path);
	free(req);
}

static void browser_load_later(browser_t *b, const char *dir)
{
	load_req_t *req = calloc(1, sizeof(*req));

	req->b = b;
	snprintf(req->path, sizeof(req->path), "%s", dir);
	lv_async_call(load_async, req);
}

static void now_row_cb(lv_event_t *e)
{
	(void)e;
	open_now_playing();
}

typedef struct {
	browser_t *b;
	char path[1100];
	bool dir, up;
} click_t;

static void click_free(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void browser_entry_cb(lv_event_t *e)
{
	click_t *c = lv_event_get_user_data(e);
	browser_t *b = c->b;
	char target[1100];

	if (c->up) {
		char *slash;

		snprintf(target, sizeof(target), "%s", b->cwd);
		slash = strrchr(target, '/');
		if (slash && slash != target)
			*slash = '\0';
		browser_load_later(b, target);
		return;
	}
	if (c->dir) {
		snprintf(target, sizeof(target), "%s", c->path);
		browser_load_later(b, target);
		return;
	}
	/* Audio file: play every track in this folder starting here. */
	{
		DIR *d = opendir(b->cwd);
		struct dirent *ent;
		entry_t *list = NULL;
		int n = 0, cap = 0, start = 0;
		char **paths;

		if (!d)
			return;
		while ((ent = readdir(d))) {
			char full[1100];

			if (ent->d_name[0] == '.' || !is_audio_name(ent->d_name))
				continue;
			if (snprintf(full, sizeof(full), "%s/%s", b->cwd, ent->d_name) >= (int)sizeof(full))
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 32;
				list = realloc(list, (size_t)cap * sizeof(*list));
			}
			snprintf(list[n].path, sizeof(list[n].path), "%s", full);
			list[n++].dir = false;
		}
		closedir(d);
		qsort(list, (size_t)n, sizeof(*list), entry_cmp);
		paths = calloc((size_t)n, sizeof(char *));
		for (int i = 0; i < n; i++) {
			paths[i] = strdup(list[i].path);
			if (!strcmp(list[i].path, c->path))
				start = i;
		}
		free(list);
		mp_play(paths, n, start);
		open_now_playing();
	}
}

static void browser_load(browser_t *b, const char *dir_in)
{
	char dir[1100];
	DIR *d;

	/* dir_in may alias b->cwd; copy before overwriting it. */
	snprintf(dir, sizeof(dir), "%s", dir_in);
	d = opendir(dir);
	struct dirent *ent;
	entry_t *list = NULL;
	int n = 0, cap = 0;
	char label[600];

	if (!d) {
		if (!b->cwd[0]) {
			/* First load failed: show why instead of an empty, key-less page. */
			lv_obj_t *card;
			char msg[1200];

			lv_group_remove_all_objs(b->page->group);
			lv_obj_clean(b->page->content);
			card = ui_card(b->page->content);
			ui_label(card, LV_SYMBOL_WARNING "  Music folder unavailable", FONT_M, C_RED);
			snprintf(msg, sizeof(msg), "Cannot open %s\nCreate it and copy music in, e.g.\nsudo mkdir -p %s\nsudo chown -R $USER %s",
				 dir, dir, dir);
			lv_obj_t *l = ui_label(card, msg, FONT_S, C_TEXT_DIM);

			lv_obj_set_width(l, LV_PCT(100));
			lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
			ui_page_add_sink(b->page);
			return;
		}
		ui_toast("Cannot open folder");
		return;
	}
	snprintf(b->cwd, sizeof(b->cwd), "%s", dir);
	lv_group_remove_all_objs(b->page->group);
	lv_obj_clean(b->page->content);
	while ((ent = readdir(d))) {
		char full[1100];
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
		if (stat(full, &st))
			continue;
		if (!S_ISDIR(st.st_mode) && !is_audio_name(ent->d_name))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 32;
			list = realloc(list, (size_t)cap * sizeof(*list));
		}
		snprintf(list[n].path, sizeof(list[n].path), "%s", full);
		list[n++].dir = S_ISDIR(st.st_mode);
	}
	closedir(d);
	qsort(list, (size_t)n, sizeof(*list), entry_cmp);

	snprintf(label, sizeof(label), "%s", strlen(dir) > strlen(b->root) ? dir + strlen(b->root) : "/");
	ui_section(b->page->content, label);

	if (mp.playing || mp.ntracks) {
		/* Mini player in the music demo's light-card style. */
		lv_obj_t *row = ui_row(b->page, b->page->content, LV_SYMBOL_AUDIO, DEMO_PURPLE, "Now playing",
				       mp.index >= 0 && mp.index < mp.ntracks ?
				       base_name(mp.tracks[mp.index]) : NULL);

		lv_obj_set_style_bg_color(row, lv_color_white(), 0);
		lv_obj_set_style_bg_color(row, lv_color_hex(0xEFEAFE), LV_STATE_FOCUSED);
		lv_obj_set_style_text_color(ui_row_title(row), lv_color_hex(DEMO_DARK), 0);
		if (ui_row_subtitle(row))
			lv_obj_set_style_text_color(ui_row_subtitle(row), lv_color_hex(DEMO_DIM), 0);
		lv_obj_set_style_text_color(ui_row_chevron(row), lv_color_hex(DEMO_PURPLE), 0);
		lv_label_set_text(lv_obj_get_child(row, -1), mp.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
		lv_obj_add_event_cb(row, now_row_cb, LV_EVENT_CLICKED, NULL);
	}
	if (strcmp(dir, b->root)) {
		click_t *c = calloc(1, sizeof(*c));
		lv_obj_t *row = ui_row(b->page, b->page->content, LV_SYMBOL_LEFT, C_GRAY, "..", "Up one level");

		c->b = b;
		c->up = true;
		lv_obj_add_event_cb(row, browser_entry_cb, LV_EVENT_CLICKED, c);
		lv_obj_add_event_cb(row, click_free, LV_EVENT_DELETE, c);
	}
	if (!n) {
		lv_obj_t *card = ui_card(b->page->content);

		ui_label(card, "No music here yet", FONT_M, C_TEXT);
		ui_label(card, "Copy mp3, ogg, flac, wav, m4a, aac or\nopus files to /opt/gamepup/music",
			 FONT_S, C_TEXT_DIM);
		ui_page_add_sink(b->page);
	}
	for (int i = 0; i < n; i++) {
		click_t *c = calloc(1, sizeof(*c));
		char name[160];
		lv_obj_t *row;

		snprintf(name, sizeof(name), "%s", base_name(list[i].path));
		if (!list[i].dir) {
			char *dot = strrchr(name, '.');

			if (dot)
				*dot = '\0';
		}
		row = ui_row(b->page, b->page->content,
			     list[i].dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO,
			     list[i].dir ? C_ORANGE : C_PINK, name, NULL);
		if (list[i].dir)
			ui_row_chevron(row);
		else if (mp.index >= 0 && mp.index < mp.ntracks && !strcmp(mp.tracks[mp.index], list[i].path)) {
			lv_obj_set_style_text_color(ui_row_title(row), lv_color_hex(DEMO_PURPLE), 0);
			ui_row_value(row, LV_SYMBOL_PLAY);
			lv_obj_set_style_text_color(lv_obj_get_child(row, -1), lv_color_hex(DEMO_PURPLE), 0);
		}
		c->b = b;
		c->dir = list[i].dir;
		snprintf(c->path, sizeof(c->path), "%s", list[i].path);
		lv_obj_add_event_cb(row, browser_entry_cb, LV_EVENT_CLICKED, c);
		lv_obj_add_event_cb(row, click_free, LV_EVENT_DELETE, c);
	}
	free(list);
	if (lv_group_get_obj_count(b->page->group))
		lv_group_focus_obj(lv_group_get_obj_by_index(b->page->group, 0));
	lv_obj_scroll_to_y(b->page->content, 0, LV_ANIM_OFF);
}

static bool browser_key(ui_page_t *page, uint32_t key)
{
	browser_t *b = page->user;

	if (key == LV_KEY_ESC && b->cwd[0] && strcmp(b->cwd, b->root)) {
		char target[1100], *slash;

		snprintf(target, sizeof(target), "%s", b->cwd);
		slash = strrchr(target, '/');
		if (slash && slash != target)
			*slash = '\0';
		hal_beep("exit");
		browser_load_later(b, target);
		return true;
	}
	if (key == GP_KEY_Y) {
		mp_toggle_pause();
		return true;
	}
	return false;
}

static void browser_show(ui_page_t *page)
{
	browser_t *b = page->user;

	/* Coming back from Now Playing: refresh the "now playing" row. */
	if (b->cwd[0])
		browser_load(b, b->cwd);
}

static void browser_hide(ui_page_t *page)
{
	free(page->user);
}

void ui_open_music(void)
{
	ui_page_t *page = ui_page_create("Music", "A Open  Y Pause");
	browser_t *b = calloc(1, sizeof(*b));
	struct stat st;
	const char *root = hal_p(MUSIC_ROOT);

	if (!hal_which("mpv"))
		ui_toast("Install mpv for music playback");
	if (stat(root, &st))
		mkdir(root, 0755);
	snprintf(b->root, sizeof(b->root), "%s", root);
	b->page = page;
	page->user = b;
	page->on_key = browser_key;
	page->on_hide = browser_hide;
	browser_load(b, root);
	ui_push(page);
	page->on_show = browser_show;
}

/* ====================================================================== */
/* Voice memos                                                             */
/* ====================================================================== */

static char *memo_device(char *buf, size_t size)
{
	const char *env = getenv("GAMEPUP_ALSA_DEVICE");
	char card[64];

	if (env && *env) {
		snprintf(buf, size, "%s", env);
		return buf;
	}
	hal_alsa_card_id(card, sizeof(card));
	if (card[0])
		snprintf(buf, size, "plughw:%s,0", card);
	else
		snprintf(buf, size, "default");
	return buf;
}

static double memo_seconds(const char *path)
{
	struct stat st;

	if (stat(path, &st) || st.st_size <= 44)
		return 0;
	return (double)(st.st_size - 44) / (MEMO_RATE * MEMO_BYTES_PER_FRAME);
}

static int memo_cmp_desc(const void *a, const void *b)
{
	return strcmp(b, a);
}

static int list_memos(char names[][64], int max)
{
	DIR *d = opendir(hal_p(MEMO_DIR));
	struct dirent *ent;
	int n = 0;

	if (!d)
		return 0;
	while ((ent = readdir(d)) && n < max) {
		size_t len = strlen(ent->d_name);

		if (!strncmp(ent->d_name, "memo-", 5) && len > 9 && len < 64 &&
		    !strcmp(ent->d_name + len - 4, ".wav") && !strstr(ent->d_name, ".partial"))
			snprintf(names[n++], 64, "%s", ent->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, 64, memo_cmp_desc);
	return n;
}

static void pretty_stamp(const char *file, char *out, size_t size)
{
	/* memo-20260101-120000.wav -> 2026-01-01  12:00:00 */
	int y, mo, d, h, mi, s;

	if (sscanf(file, "memo-%4d%2d%2d-%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6)
		snprintf(out, size, "%04d-%02d-%02d  %02d:%02d:%02d", y, mo, d, h, mi, s);
	else
		snprintf(out, size, "%s", file);
}

/* ---- recording ---- */

typedef struct {
	int pid;
	char temp[512], final[512];
	uint32_t started;
	lv_obj_t *dot, *time, *bar, *left;
	lv_timer_t *timer;
	bool finishing;
} rec_t;

static void rec_anim_cb(void *obj, int32_t v)
{
	lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

static void rec_finish(rec_t *r, bool keep)
{
	struct stat st;
	int status = 0;

	if (r->finishing)
		return;
	r->finishing = true;
	if (r->pid > 0) {
		hal_kill(r->pid, SIGINT);
		for (int i = 0; i < 80; i++) {
			if (hal_child_done(r->pid, &status))
				break;
			usleep(25000);
			if (i == 79) {
				hal_kill(r->pid, SIGKILL);
				hal_child_done(r->pid, NULL);
			}
		}
		r->pid = 0;
	}
	if (r->timer) {
		lv_timer_delete(r->timer);
		r->timer = NULL;
	}
	if (keep && !stat(r->temp, &st) && st.st_size >= 1024) {
		rename(r->temp, r->final);
		ui_toast("Memo saved");
	} else {
		unlink(r->temp);
		ui_toast(keep ? "No audio captured. Check the mic" : "Recording discarded");
	}
}

static void rec_tick(lv_timer_t *timer)
{
	rec_t *r = lv_timer_get_user_data(timer);
	uint32_t elapsed = (hal_now_ms() - r->started) / 1000;
	char text[24];

	snprintf(text, sizeof(text), "%u:%02u", elapsed / 60, elapsed % 60);
	lv_label_set_text(r->time, text);
	lv_bar_set_value(r->bar, (int)(hal_now_ms() - r->started), LV_ANIM_OFF);
	snprintf(text, sizeof(text), "%us left", elapsed >= MEMO_MAX_SECONDS ? 0 : MEMO_MAX_SECONDS - elapsed);
	lv_label_set_text(r->left, text);
	if (r->pid > 0 && hal_child_done(r->pid, NULL)) {
		r->pid = 0;
		rec_finish(r, true);
		ui_pop();
	}
}

static bool rec_key(ui_page_t *page, uint32_t key)
{
	rec_t *r = page->user;

	if (key == LV_KEY_ENTER || key == GP_KEY_START) {
		rec_finish(r, true);
		ui_pop();
		return true;
	}
	if (key == LV_KEY_ESC || key == GP_KEY_X) {
		rec_finish(r, false);
		ui_pop();
		return true;
	}
	return key == LV_KEY_UP || key == LV_KEY_DOWN;
}

static void rec_hide(ui_page_t *page)
{
	rec_t *r = page->user;

	if (!r->finishing)
		rec_finish(r, false);
	free(r);
}

static void open_record(void)
{
	ui_page_t *page;
	rec_t *r;
	char dev[96], stamp[32], dur[16], dir[400];
	time_t now = time(NULL);
	struct tm tm;
	lv_obj_t *card, *ring, *row;
	lv_anim_t a;

	snprintf(dir, sizeof(dir), "%s", hal_p(MEMO_DIR));
	mkdir(dir, 0755);
	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
	r = calloc(1, sizeof(*r));
	snprintf(r->final, sizeof(r->final), "%s/memo-%s.wav", dir, stamp);
	snprintf(r->temp, sizeof(r->temp), "%s/memo-%s.partial.wav", dir, stamp);
	snprintf(dur, sizeof(dur), "%d", MEMO_MAX_SECONDS);
	memo_device(dev, sizeof(dev));
	{
		const char *argv[] = { "arecord", "-D", dev, "-c", "2", "-r", "48000", "-f", "S32_LE",
				       "-t", "wav", "-d", dur, r->temp, NULL };

		r->pid = hal_spawn(argv, NULL, true);
	}
	if (r->pid < 0) {
		free(r);
		ui_toast("arecord is not available");
		return;
	}
	r->started = hal_now_ms();

	page = ui_page_create("Recording", "A Save");
	page->user = r;
	page->on_key = rec_key;
	page->on_hide = rec_hide;
	ui_page_add_sink(page);
	card = ui_card(page->content);
	lv_obj_set_style_pad_all(card, 14, 0);
	row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 16, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	ring = lv_obj_create(row);
	lv_obj_remove_style_all(ring);
	lv_obj_set_size(ring, 56, 56);
	lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(ring, 3, 0);
	lv_obj_set_style_border_color(ring, lv_color_hex(C_RED), 0);
	lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
	r->dot = lv_obj_create(ring);
	lv_obj_remove_style_all(r->dot);
	lv_obj_set_size(r->dot, 34, 34);
	lv_obj_set_style_radius(r->dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(r->dot, lv_color_hex(C_RED), 0);
	lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);
	lv_obj_center(r->dot);
	lv_anim_init(&a);
	lv_anim_set_var(&a, r->dot);
	lv_anim_set_exec_cb(&a, rec_anim_cb);
	lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
	lv_anim_set_duration(&a, 700);
	lv_anim_set_playback_duration(&a, 700);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
	{
		lv_obj_t *col = lv_obj_create(row);

		lv_obj_remove_style_all(col);
		lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		r->time = ui_label(col, "0:00", FONT_XXL, C_TEXT);
		r->left = ui_label(col, "", FONT_S, C_TEXT_DIM);
	}
	r->bar = lv_bar_create(card);
	lv_obj_set_size(r->bar, LV_PCT(100), 6);
	lv_bar_set_range(r->bar, 0, MEMO_MAX_SECONDS * 1000);
	lv_obj_set_style_bg_color(r->bar, lv_color_hex(0x48506A), LV_PART_MAIN);
	lv_obj_set_style_bg_color(r->bar, lv_color_hex(C_RED), LV_PART_INDICATOR);
	pretty_stamp(strrchr(r->final, '/') + 1, dir, sizeof(dir));
	ui_label(card, dir, FONT_XS, C_TEXT_DIM);
	ui_label(card, "A / START save    B discard", FONT_XS, C_TEXT_DIM);
	r->timer = lv_timer_create(rec_tick, 100, r);
	rec_tick(r->timer);
	ui_push(page);
}

/* ---- playback ---- */

typedef struct {
	int pid;
	bool paused;
	double duration;
	uint32_t started, paused_at, paused_total;
	lv_obj_t *bar, *pos, *icon;
	lv_timer_t *timer;
} play_t;

static void play_stop(play_t *p)
{
	if (p->pid > 0) {
		if (p->paused)
			hal_kill(p->pid, SIGCONT);
		hal_kill(p->pid, SIGINT);
		for (int i = 0; i < 40 && !hal_child_done(p->pid, NULL); i++)
			usleep(25000);
		hal_kill(p->pid, SIGKILL);
		hal_child_done(p->pid, NULL);
		p->pid = 0;
	}
}

static void play_tick(lv_timer_t *timer)
{
	play_t *p = lv_timer_get_user_data(timer);
	uint32_t now = p->paused ? p->paused_at : hal_now_ms();
	double elapsed = (now - p->started - p->paused_total) / 1000.0;
	char a[16], b[16], text[40];

	if (elapsed > p->duration)
		elapsed = p->duration;
	fmt_time(a, sizeof(a), elapsed);
	fmt_time(b, sizeof(b), p->duration);
	snprintf(text, sizeof(text), "%s / %s", a, b);
	lv_label_set_text(p->pos, text);
	lv_bar_set_value(p->bar, (int)(elapsed * 1000 / (p->duration > 0.1 ? p->duration : 0.1)), LV_ANIM_OFF);
	lv_label_set_text(p->icon, p->paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
	if (p->pid > 0 && hal_child_done(p->pid, NULL)) {
		p->pid = 0;
		lv_timer_pause(timer);
		ui_pop();
	}
}

static bool play_key(ui_page_t *page, uint32_t key)
{
	play_t *p = page->user;

	if (key == LV_KEY_ENTER || key == GP_KEY_START) {
		if (p->paused) {
			p->paused_total += hal_now_ms() - p->paused_at;
			hal_kill(p->pid, SIGCONT);
		} else {
			p->paused_at = hal_now_ms();
			hal_kill(p->pid, SIGSTOP);
		}
		p->paused = !p->paused;
		play_tick(p->timer);
		return true;
	}
	if (key == LV_KEY_ESC || key == GP_KEY_X) {
		play_stop(p);
		ui_pop();
		return true;
	}
	return key == LV_KEY_UP || key == LV_KEY_DOWN;
}

static void play_hide(ui_page_t *page)
{
	play_t *p = page->user;

	play_stop(p);
	lv_timer_delete(p->timer);
	free(p);
}

static void open_playback(const char *file)
{
	char path[512], dev[96], label[80];
	play_t *p = calloc(1, sizeof(*p));
	ui_page_t *page;
	lv_obj_t *card;

	snprintf(path, sizeof(path), "%s/%s", hal_p(MEMO_DIR), file);
	memo_device(dev, sizeof(dev));
	{
		const char *argv[] = { "aplay", "-D", dev, "-q", path, NULL };

		p->pid = hal_spawn(argv, NULL, true);
	}
	if (p->pid < 0) {
		free(p);
		ui_toast("aplay is not available");
		return;
	}
	p->duration = memo_seconds(path);
	if (p->duration < 0.1)
		p->duration = 0.1;
	p->started = hal_now_ms();
	page = ui_page_create("Playing", "A Pause");
	page->user = p;
	page->on_key = play_key;
	page->on_hide = play_hide;
	ui_page_add_sink(page);
	card = ui_card(page->content);
	lv_obj_set_style_pad_all(card, 14, 0);
	pretty_stamp(file, label, sizeof(label));
	ui_label(card, label, FONT_L, C_TEXT);
	p->icon = ui_label(card, LV_SYMBOL_PAUSE, FONT_XXL, C_PURPLE);
	p->bar = lv_bar_create(card);
	lv_obj_set_size(p->bar, LV_PCT(100), 6);
	lv_bar_set_range(p->bar, 0, 1000);
	lv_obj_set_style_bg_color(p->bar, lv_color_hex(0x48506A), LV_PART_MAIN);
	lv_obj_set_style_bg_color(p->bar, lv_color_hex(C_PURPLE), LV_PART_INDICATOR);
	p->pos = ui_label(card, "", FONT_S, C_TEXT_DIM);
	ui_label(card, "A pause    B stop", FONT_XS, C_TEXT_DIM);
	p->timer = lv_timer_create(play_tick, 100, p);
	play_tick(p->timer);
	ui_push(page);
}

/* ---- delete confirmation ---- */

static void confirm_yes(lv_event_t *e)
{
	char *file = lv_event_get_user_data(e);
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", hal_p(MEMO_DIR), file);
	unlink(path);
	ui_pop();
	ui_toast("Memo deleted");
}

static void confirm_no(lv_event_t *e)
{
	(void)e;
	ui_pop();
}

static void confirm_free(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void open_confirm(const char *file)
{
	ui_page_t *page = ui_page_create("Delete memo?", "A Choose");
	lv_obj_t *card = ui_card(page->content), *btn;
	char label[80];
	char *copy = strdup(file);

	pretty_stamp(file, label, sizeof(label));
	ui_label(card, label, FONT_M, C_TEXT);
	ui_label(card, "This cannot be undone.", FONT_S, C_TEXT_DIM);
	btn = ui_button(page, page->content, LV_SYMBOL_TRASH "  Delete", C_RED);
	lv_obj_add_event_cb(btn, confirm_yes, LV_EVENT_CLICKED, copy);
	lv_obj_add_event_cb(btn, confirm_free, LV_EVENT_DELETE, copy);
	btn = ui_button(page, page->content, "Cancel", C_GRAY);
	lv_obj_add_event_cb(btn, confirm_no, LV_EVENT_CLICKED, NULL);
	lv_group_focus_obj(btn);
	ui_push(page);
}

/* ---- memo list ---- */

typedef struct {
	char names[24][64];
	int count;
} memos_t;

static void memo_list_build(ui_page_t *page);

static void record_cb(lv_event_t *e)
{
	(void)e;
	open_record();
}

static void memo_cb(lv_event_t *e)
{
	open_playback(lv_event_get_user_data(e));
}

static bool memo_list_key(ui_page_t *page, uint32_t key)
{
	memos_t *m = page->user;
	lv_obj_t *focus = lv_group_get_focused(page->group);

	if (key == GP_KEY_SELECT && focus) {
		/* Row 0 is the record button; memo rows follow in order. */
		for (int i = 0; i < m->count; i++) {
			if (lv_group_get_obj_by_index(page->group, i + 1) == focus) {
				open_confirm(m->names[i]);
				break;
			}
		}
		return true;
	}
	return false;
}

static void memo_list_hide(ui_page_t *page)
{
	free(page->user);
}

static void memo_list_build(ui_page_t *page)
{
	memos_t *m = page->user;
	lv_obj_t *row;
	char label[80], sub[32], t[16];

	lv_group_remove_all_objs(page->group);
	lv_obj_clean(page->content);
	m->count = list_memos(m->names, 24);
	row = ui_row(page, page->content, LV_SYMBOL_PLUS, C_RED, "New recording",
		     "Press A to start, A again to save");
	ui_row_chevron(row);
	lv_obj_add_event_cb(row, record_cb, LV_EVENT_CLICKED, NULL);
	if (m->count)
		ui_section(page->content, "RECORDINGS");
	for (int i = 0; i < m->count; i++) {
		char path[512];

		pretty_stamp(m->names[i], label, sizeof(label));
		snprintf(path, sizeof(path), "%s/%s", hal_p(MEMO_DIR), m->names[i]);
		fmt_time(t, sizeof(t), memo_seconds(path));
		snprintf(sub, sizeof(sub), "%s", t);
		row = ui_row(page, page->content, LV_SYMBOL_AUDIO, C_PURPLE, label, sub);
		ui_row_chevron(row);
		lv_obj_add_event_cb(row, memo_cb, LV_EVENT_CLICKED, m->names[i]);
	}
	if (lv_group_get_obj_count(page->group))
		lv_group_focus_obj(lv_group_get_obj_by_index(page->group, 0));
	lv_obj_scroll_to_y(page->content, 0, LV_ANIM_OFF);
}

static void memo_list_show(ui_page_t *page)
{
	memo_list_build(page);
}

void ui_open_voice(void)
{
	ui_page_t *page = ui_page_create("Voice Memo", "A Select  SEL Delete");

	page->user = calloc(1, sizeof(memos_t));
	page->on_key = memo_list_key;
	page->on_hide = memo_list_hide;
	page->on_show = memo_list_show;
	memo_list_build(page);
	ui_push(page);
}
