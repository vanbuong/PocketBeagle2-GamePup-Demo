// SPDX-License-Identifier: GPL-2.0-only
// Music player (mpv over a UNIX socket) and voice memos (arecord/aplay).
#define _GNU_SOURCE
#include "ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MUSIC_ROOT "/opt/gamepup/music"
#define MEMO_DIR "/opt/gamepup/voice-memos"
#define VOLUME_FILE "/opt/gamepup/saves/music-volume"
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
	bool playing, paused;
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

static void mp_send(const char *json)
{
	if (mp.sock >= 0 && send(mp.sock, json, strlen(json), MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
	    errno != EAGAIN) {
		close(mp.sock);
		mp.sock = -1;
	}
}

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

static void mp_parse_line(char *line)
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
	snprintf(mp.socket_path, sizeof(mp.socket_path), "/tmp/gamepup-mpv-%d.sock", (int)getuid());
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
	if (mp.state == MP_READY) {
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
/* Now Playing                                                             */
/* ====================================================================== */

typedef struct {
	lv_obj_t *title, *folder, *bar, *t_pos, *t_dur, *play, *vol_bar, *vol_txt, *hint2;
	lv_timer_t *timer;
} now_t;

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

static void now_update(lv_timer_t *timer)
{
	now_t *n = lv_timer_get_user_data(timer);
	char text[96], t[16];

	if (mp.index >= 0 && mp.index < mp.ntracks) {
		snprintf(text, sizeof(text), "%s", base_name(mp.tracks[mp.index]));
		char *dot = strrchr(text, '.');

		if (dot)
			*dot = '\0';
		lv_label_set_text(n->title, text);
		snprintf(text, sizeof(text), "Track %d of %d", mp.index + 1, mp.ntracks);
		lv_label_set_text(n->folder, text);
	} else {
		lv_label_set_text(n->title, "Nothing playing");
		lv_label_set_text(n->folder, "");
	}
	fmt_time(t, sizeof(t), mp.playing ? mp.pos : -1);
	lv_label_set_text(n->t_pos, t);
	fmt_time(t, sizeof(t), mp.playing && mp.dur > 0 ? mp.dur : -1);
	lv_label_set_text(n->t_dur, t);
	lv_bar_set_value(n->bar, mp.dur > 0 ? (int)(mp.pos * 1000 / mp.dur) : 0, LV_ANIM_OFF);
	lv_label_set_text(n->play, mp.playing && !mp.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	lv_bar_set_value(n->vol_bar, mp.volume, LV_ANIM_OFF);
	snprintf(text, sizeof(text), "%d%%", mp.volume);
	lv_label_set_text(n->vol_txt, text);
}

static bool now_key(ui_page_t *page, uint32_t key)
{
	now_t *n = page->user;

	switch (key) {
	case LV_KEY_ENTER:
	case GP_KEY_Y:
		mp_toggle_pause();
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

static void open_now_playing(void)
{
	ui_page_t *page = ui_page_create("Now Playing", "A Pause");
	now_t *n = calloc(1, sizeof(*n));
	lv_obj_t *card, *art, *info, *row, *ctrl;

	page->user = n;
	page->on_key = now_key;
	page->on_hide = now_hide;
	ui_page_add_sink(page);

	card = ui_card(page->content);
	row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 12, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	art = ui_icon(row, LV_SYMBOL_AUDIO, C_PINK, 64);
	(void)art;
	info = lv_obj_create(row);
	lv_obj_remove_style_all(info);
	lv_obj_set_height(info, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(info, 1);
	lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(info, 3, 0);
	lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
	n->title = ui_label(info, "", FONT_L, C_TEXT);
	lv_obj_set_width(n->title, LV_PCT(100));
	lv_label_set_long_mode(n->title, LV_LABEL_LONG_SCROLL_CIRCULAR);
	n->folder = ui_label(info, "", FONT_S, C_TEXT_DIM);
	ctrl = lv_obj_create(info);
	lv_obj_remove_style_all(ctrl);
	lv_obj_set_size(ctrl, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);
	ui_label(ctrl, LV_SYMBOL_PREV, FONT_XL, C_TEXT_DIM);
	n->play = ui_label(ctrl, LV_SYMBOL_PLAY, FONT_XXL, C_TEXT);
	ui_label(ctrl, LV_SYMBOL_NEXT, FONT_XL, C_TEXT_DIM);

	n->bar = lv_bar_create(card);
	lv_obj_set_size(n->bar, LV_PCT(100), 6);
	lv_bar_set_range(n->bar, 0, 1000);
	lv_obj_set_style_bg_color(n->bar, lv_color_hex(0x48506A), LV_PART_MAIN);
	lv_obj_set_style_bg_color(n->bar, lv_color_hex(C_PINK), LV_PART_INDICATOR);
	lv_obj_set_style_radius(n->bar, 3, LV_PART_MAIN);
	lv_obj_set_style_radius(n->bar, 3, LV_PART_INDICATOR);
	row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	n->t_pos = ui_label(row, "0:00", FONT_XS, C_TEXT_DIM);
	n->t_dur = ui_label(row, "--:--", FONT_XS, C_TEXT_DIM);

	card = ui_card(page->content);
	row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	ui_label(row, LV_SYMBOL_VOLUME_MID, FONT_M, C_TEXT_DIM);
	n->vol_bar = lv_bar_create(row);
	lv_obj_set_height(n->vol_bar, 6);
	lv_obj_set_flex_grow(n->vol_bar, 1);
	lv_bar_set_range(n->vol_bar, 0, 100);
	lv_obj_set_style_bg_color(n->vol_bar, lv_color_hex(0x48506A), LV_PART_MAIN);
	lv_obj_set_style_bg_color(n->vol_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
	n->vol_txt = ui_label(row, "", FONT_S, C_TEXT_DIM);
	n->hint2 = ui_label(card, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " seek   " LV_SYMBOL_UP LV_SYMBOL_DOWN
			    " volume   START next   SEL prev   X stop", FONT_XS, C_TEXT_DIM);

	n->timer = lv_timer_create(now_update, 250, n);
	now_update(n->timer);
	ui_push(page);
}

/* ====================================================================== */
/* Music browser                                                           */
/* ====================================================================== */

typedef struct {
	char cwd[512];
	char root[512];
	ui_page_t *page;
	lv_timer_t *timer;
	lv_obj_t *bar_title;
} browser_t;

typedef struct {
	char path[768];
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
	char path[768];
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
	char path[768];
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
	char target[768];

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
			char full[768];

			if (ent->d_name[0] == '.' || !is_audio_name(ent->d_name))
				continue;
			snprintf(full, sizeof(full), "%s/%s", b->cwd, ent->d_name);
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

static void browser_load(browser_t *b, const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *ent;
	entry_t *list = NULL;
	int n = 0, cap = 0;
	char label[600];

	if (!d) {
		ui_toast("Cannot open folder");
		return;
	}
	snprintf(b->cwd, sizeof(b->cwd), "%s", dir);
	lv_group_remove_all_objs(b->page->group);
	lv_obj_clean(b->page->content);
	while ((ent = readdir(d))) {
		char full[768];
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
		lv_obj_t *row = ui_row(b->page, b->page->content, LV_SYMBOL_PLAY, C_PINK, "Now playing",
				       mp.index >= 0 && mp.index < mp.ntracks ?
				       base_name(mp.tracks[mp.index]) : NULL);
		ui_row_chevron(row);
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

	if (key == LV_KEY_ESC && strcmp(b->cwd, b->root)) {
		char target[768], *slash;

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
