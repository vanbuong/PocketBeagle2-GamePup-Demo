// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "hal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FRAMEBUFFER "/dev/fb0"
#define INPUT_PRIMARY "/dev/input/by-path/platform-gamepup-buttons-event"
#define INPUT_FALLBACK "/dev/input/event0"
#define FBCON_HELPER "/usr/local/libexec/gamepup-fbcon"
#define BACKLIGHT_HELPER "/usr/local/libexec/gamepup-backlight-permissions"
#define KEY_QUEUE 64

static const char *const BACKLIGHT_PATHS[] = {
	"/sys/class/backlight/backlight-gamepup/brightness",
	"/sys/class/backlight/lcd-backlight/brightness",
};

/* ---------------------------------------------------------------- paths */

const char *hal_path(const char *absolute, char *buf, size_t size)
{
	const char *root = getenv("GAMEPUP_ROOT");

	if (root && *root)
		snprintf(buf, size, "%s%s", root, absolute);
	else
		snprintf(buf, size, "%s", absolute);
	return buf;
}

const char *hal_p(const char *absolute)
{
	static char ring[8][512];
	static unsigned int next;
	char *slot = ring[next++ & 7];

	return hal_path(absolute, slot, sizeof(ring[0]));
}

uint32_t hal_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ---------------------------------------------------------------- files */

bool hal_file_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

bool hal_file_read_str(const char *path, char *out, size_t size)
{
	FILE *fp = fopen(path, "r");
	size_t n;

	if (!fp)
		return false;
	n = fread(out, 1, size - 1, fp);
	fclose(fp);
	out[n] = '\0';
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
		out[--n] = '\0';
	return true;
}

bool hal_file_write_str(const char *path, const char *text)
{
	FILE *fp = fopen(path, "w");
	bool ok;

	if (!fp)
		return false;
	ok = fprintf(fp, "%s\n", text) >= 0;
	return fclose(fp) == 0 && ok;
}

int hal_file_read_int(const char *path, int fallback)
{
	char text[32];
	char *end;
	long value;

	if (!hal_file_read_str(path, text, sizeof(text)))
		return fallback;
	value = strtol(text, &end, 10);
	return end == text ? fallback : (int)value;
}

bool hal_file_write_int(const char *path, int value)
{
	char text[16];

	snprintf(text, sizeof(text), "%d", value);
	return hal_file_write_str(path, text);
}

bool hal_file_toggle(const char *path)
{
	if (hal_file_exists(path)) {
		unlink(path);
		return false;
	}
	hal_file_write_str(path, "1");
	return true;
}

int64_t hal_file_mtime_ns(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return 0;
	return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

/* ---------------------------------------------------------------- display */

static lv_display_t *display;
static lv_indev_t *keypad;
static int fb_fd = -1;
static int input_fd = -1;
static int buzzer_fd = -1;
static bool headless;
static bool opened;
static int fb_w = SCREEN_W, fb_h = SCREEN_H, fb_stride, fb_bpp = 16;
static bool fb_rotated;
static uint8_t *shadow;
static size_t shadow_size;
static uint8_t render_a[SCREEN_W * 40 * 4] __attribute__((aligned(4)));
static uint8_t render_b[SCREEN_W * 40 * 4] __attribute__((aligned(4)));

static void write_span(size_t offset, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len) {
		ssize_t n = pwrite(fb_fd, p, len, (off_t)offset);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		p += n;
		offset += (size_t)n;
		len -= (size_t)n;
	}
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	const int bytes = fb_bpp / 8;
	const int w = lv_area_get_width(area);
	const int h = lv_area_get_height(area);

	if (shadow && (headless || fb_fd >= 0)) {
		if (!fb_rotated) {
			for (int row = 0; row < h; row++) {
				size_t off = (size_t)(area->y1 + row) * fb_stride +
					     (size_t)area->x1 * bytes;

				memcpy(shadow + off, px_map + (size_t)row * w * bytes,
				       (size_t)w * bytes);
				if (!headless)
					write_span(off, shadow + off, (size_t)w * bytes);
			}
		} else {
			/* Portrait panel scanned 240x320: (x,y) -> (y, W-1-x). */
			for (int row = 0; row < h; row++) {
				for (int col = 0; col < w; col++) {
					int sx = area->x1 + col;
					int sy = area->y1 + row;
					int dx = sy;
					int dy = SCREEN_W - 1 - sx;

					memcpy(shadow + (size_t)dy * fb_stride + (size_t)dx * bytes,
					       px_map + ((size_t)row * w + col) * bytes, bytes);
				}
			}
			if (!headless) {
				int y0 = SCREEN_W - 1 - area->x2;
				int y1 = SCREEN_W - 1 - area->x1;

				write_span((size_t)y0 * fb_stride, shadow + (size_t)y0 * fb_stride,
					   (size_t)(y1 - y0 + 1) * fb_stride);
			}
		}
	}
	lv_display_flush_ready(disp);
}

static bool fb_prepare(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	size_t size;

	fb_fd = open(FRAMEBUFFER, O_RDWR | O_CLOEXEC);
	if (fb_fd < 0) {
		fprintf(stderr, "gamepup-ui: cannot open %s: %s\n", FRAMEBUFFER, strerror(errno));
		return false;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
		fprintf(stderr, "gamepup-ui: framebuffer ioctl failed\n");
		close(fb_fd);
		fb_fd = -1;
		return false;
	}
	if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
		fprintf(stderr, "gamepup-ui: expected 16/32 bpp, got %u\n", var.bits_per_pixel);
		close(fb_fd);
		fb_fd = -1;
		return false;
	}
	if (!((var.xres == SCREEN_W && var.yres == SCREEN_H) ||
	      (var.xres == SCREEN_H && var.yres == SCREEN_W))) {
		fprintf(stderr, "gamepup-ui: unsupported framebuffer %ux%u\n", var.xres, var.yres);
		close(fb_fd);
		fb_fd = -1;
		return false;
	}
	fb_w = (int)var.xres;
	fb_h = (int)var.yres;
	fb_bpp = (int)var.bits_per_pixel;
	fb_stride = (int)fix.line_length;
	fb_rotated = fb_w == SCREEN_H;
	size = (size_t)fb_stride * fb_h;
	if (size != shadow_size) {
		free(shadow);
		shadow = calloc(1, size);
		shadow_size = size;
	}
	return shadow != NULL;
}

/* ---------------------------------------------------------------- input */

struct key_event {
	uint32_t key;
	bool pressed;
};

static struct key_event queue[KEY_QUEUE];
static int q_head, q_tail;
static hal_raw_cb raw_cb;
static void *raw_user;
static bool ui_keys = true;

static void queue_key(uint32_t key, bool pressed)
{
	int next = (q_tail + 1) % KEY_QUEUE;

	if (next == q_head)
		return;
	queue[q_tail].key = key;
	queue[q_tail].pressed = pressed;
	q_tail = next;
}

static uint32_t map_code(int code)
{
	switch (code) {
	case KEY_UP: return LV_KEY_UP;
	case KEY_DOWN: return LV_KEY_DOWN;
	case KEY_LEFT: return LV_KEY_LEFT;
	case KEY_RIGHT: return LV_KEY_RIGHT;
	case KEY_TAB: return LV_KEY_ENTER;
	case KEY_ENTER: return LV_KEY_ESC;
	case KEY_1: return GP_KEY_START;
	case KEY_5: return GP_KEY_SELECT;
	case KEY_ESC: return GP_KEY_X;
	case KEY_P: return GP_KEY_Y;
	default: return 0;
	}
}

static void input_pump(void)
{
	struct input_event ev[32];

	if (input_fd < 0)
		return;
	for (;;) {
		ssize_t n = read(input_fd, ev, sizeof(ev));
		size_t count;

		if (n <= 0)
			break;
		count = (size_t)n / sizeof(ev[0]);
		for (size_t i = 0; i < count; i++) {
			uint32_t key;

			if (ev[i].type != EV_KEY || ev[i].value > 1)
				continue;
			if (raw_cb)
				raw_cb(ev[i].code, ev[i].value, raw_user);
			key = map_code(ev[i].code);
			if (key && ui_keys)
				queue_key(key, ev[i].value == 1);
		}
	}
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	input_pump();
	if (q_head == q_tail) {
		data->state = LV_INDEV_STATE_RELEASED;
		return;
	}
	data->key = queue[q_head].key;
	data->state = queue[q_head].pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	q_head = (q_head + 1) % KEY_QUEUE;
	data->continue_reading = q_head != q_tail;
}

static int open_buttons(void)
{
	const char *paths[] = { INPUT_PRIMARY, INPUT_FALLBACK };

	for (size_t i = 0; i < 2; i++) {
		int fd = open(paths[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGRAB, 1))
			fprintf(stderr, "gamepup-ui: cannot grab %s\n", paths[i]);
		return fd;
	}
	return -1;
}

void hal_inject_key(uint32_t key, bool pressed)
{
	static const struct { uint32_t key; int code; } REVERSE[] = {
		{ LV_KEY_UP, KEY_UP }, { LV_KEY_DOWN, KEY_DOWN }, { LV_KEY_LEFT, KEY_LEFT },
		{ LV_KEY_RIGHT, KEY_RIGHT }, { LV_KEY_ENTER, KEY_TAB }, { LV_KEY_ESC, KEY_ENTER },
		{ GP_KEY_START, KEY_1 }, { GP_KEY_SELECT, KEY_5 }, { GP_KEY_X, KEY_ESC },
		{ GP_KEY_Y, KEY_P },
	};

	if (raw_cb) {
		for (size_t i = 0; i < sizeof(REVERSE) / sizeof(REVERSE[0]); i++)
			if (REVERSE[i].key == key)
				raw_cb(REVERSE[i].code, pressed ? 1 : 0, raw_user);
	}
	if (ui_keys)
		queue_key(key, pressed);
}

void hal_set_raw_cb(hal_raw_cb cb, void *user)
{
	raw_cb = cb;
	raw_user = user;
}

void hal_set_ui_keys(bool enabled)
{
	ui_keys = enabled;
	if (!enabled)
		q_head = q_tail = 0;
}

lv_indev_t *hal_keypad(void)
{
	return keypad;
}

void hal_wait(uint32_t ms)
{
	struct pollfd pfd = { .fd = input_fd, .events = POLLIN };

	if (headless || input_fd < 0) {
		usleep(ms * 1000u);
		return;
	}
	poll(&pfd, 1, (int)ms);
}

/* ---------------------------------------------------------------- buzzer */

static const char *buzzer_path(void)
{
	static char found[256];
	const char *candidates[] = {
		"/dev/input/by-path/platform-gamepup-buzzer-event",
		"/dev/input/by-path/platform-pwm-beeper-event",
	};

	for (size_t i = 0; i < 2; i++)
		if (access(candidates[i], W_OK) == 0)
			return candidates[i];
	for (int n = 0; n < 16; n++) {
		char name_path[96], name[64];

		snprintf(name_path, sizeof(name_path), "/sys/class/input/event%d/device/name", n);
		if (!hal_file_read_str(name_path, name, sizeof(name)))
			continue;
		if (strstr(name, "buzzer") || strstr(name, "beeper")) {
			snprintf(found, sizeof(found), "/dev/input/event%d", n);
			return found;
		}
	}
	return NULL;
}

bool hal_buzzer_ready(void)
{
	return buzzer_path() != NULL;
}

void hal_tone(int hz)
{
	struct input_event ev;

	if (buzzer_fd < 0) {
		const char *path = buzzer_path();

		if (!path)
			return;
		buzzer_fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (buzzer_fd < 0)
			return;
	}
	memset(&ev, 0, sizeof(ev));
	ev.type = EV_SND;
	ev.code = SND_TONE;
	ev.value = hz;
	if (write(buzzer_fd, &ev, sizeof(ev)) < 0) {
		close(buzzer_fd);
		buzzer_fd = -1;
	}
}

void hal_buzzer_silence(void)
{
	hal_tone(0);
	if (buzzer_fd >= 0) {
		close(buzzer_fd);
		buzzer_fd = -1;
	}
}

struct note { int hz, ms; };
static const struct note SND_UP[] = { { 660, 22 }, { 880, 32 }, { 0, 0 } };
static const struct note SND_DOWN[] = { { 880, 22 }, { 660, 32 }, { 0, 0 } };
static const struct note SND_SELECT[] = { { 988, 35 }, { 1319, 55 }, { 0, 0 } };
static const struct note SND_EXIT[] = { { 988, 35 }, { 659, 65 }, { 0, 0 } };
static const struct note SND_UNMUTE[] = { { 659, 25 }, { 988, 45 }, { 0, 0 } };

static lv_timer_t *beep_timer;
static const struct note *beep_notes;
static int beep_index;

static void beep_step(lv_timer_t *timer)
{
	const struct note *note = &beep_notes[beep_index];

	if (note->ms == 0) {
		hal_tone(0);
		lv_timer_pause(timer);
		return;
	}
	hal_tone(note->hz);
	lv_timer_set_period(timer, (uint32_t)note->ms);
	beep_index++;
}

void hal_beep(const char *name)
{
	const struct note *notes = NULL;

	if (headless || hal_file_exists(hal_p("/opt/gamepup/saves/audio-muted")) ||
	    hal_file_exists(hal_p("/opt/gamepup/saves/menu-beeps-muted")))
		return;
	if (!strcmp(name, "up")) notes = SND_UP;
	else if (!strcmp(name, "down")) notes = SND_DOWN;
	else if (!strcmp(name, "select")) notes = SND_SELECT;
	else if (!strcmp(name, "exit")) notes = SND_EXIT;
	else if (!strcmp(name, "unmute")) notes = SND_UNMUTE;
	if (!notes)
		return;
	if (!beep_timer) {
		beep_timer = lv_timer_create(beep_step, 1, NULL);
		lv_timer_pause(beep_timer);
	}
	beep_notes = notes;
	beep_index = 0;
	lv_timer_set_period(beep_timer, 1);
	lv_timer_resume(beep_timer);
}

/* ---------------------------------------------------------------- backlight */

static const char *backlight_path(void)
{
	for (size_t i = 0; i < sizeof(BACKLIGHT_PATHS) / sizeof(BACKLIGHT_PATHS[0]); i++)
		if (access(BACKLIGHT_PATHS[i], F_OK) == 0)
			return BACKLIGHT_PATHS[i];
	return NULL;
}

bool hal_backlight_available(void)
{
	return backlight_path() != NULL;
}

int hal_backlight_get(void)
{
	const char *path = backlight_path();

	return path ? hal_file_read_int(path, 11) : 11;
}

bool hal_backlight_set(int level)
{
	const char *path = backlight_path();

	return path && hal_file_write_int(path, level);
}

/* ---------------------------------------------------------------- children */

int hal_spawn(const char *const *argv, int *out_fd, bool silence_stderr)
{
	int pipefd[2] = { -1, -1 };
	pid_t pid;

	if (out_fd && pipe2(pipefd, O_CLOEXEC))
		return -1;
	pid = fork();
	if (pid < 0) {
		if (out_fd) {
			close(pipefd[0]);
			close(pipefd[1]);
		}
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		if (out_fd)
			dup2(pipefd[1], STDOUT_FILENO);
		else if (null >= 0)
			dup2(null, STDOUT_FILENO);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			if (silence_stderr)
				dup2(null, STDERR_FILENO);
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (out_fd) {
		close(pipefd[1]);
		fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
		*out_fd = pipefd[0];
	}
	return pid;
}

void hal_spawn_detached(const char *const *argv)
{
	pid_t pid = fork();

	if (pid < 0)
		return;
	if (pid == 0) {
		pid_t inner = fork();

		if (inner == 0) {
			int null = open("/dev/null", O_RDWR);

			if (null >= 0) {
				dup2(null, 0);
				dup2(null, 1);
				dup2(null, 2);
			}
			execvp(argv[0], (char *const *)argv);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}

bool hal_child_done(int pid, int *status)
{
	int st = 0;
	pid_t r = waitpid(pid, &st, WNOHANG);

	if (r == 0)
		return false;
	if (status)
		*status = st;
	return true;
}

void hal_kill(int pid, int sig)
{
	if (pid > 0)
		kill(pid, sig);
}

bool hal_run_root_helper(const char *const *argv)
{
	const char *sudo_argv[16] = { "sudo", "-n" };
	size_t n = 2;

	for (size_t i = 0; argv[i] && n < 14; i++)
		sudo_argv[n++] = argv[i];
	sudo_argv[n] = NULL;
	for (int attempt = 0; attempt < 2; attempt++) {
		const char *const *cmd = attempt == 0 ? sudo_argv : argv;
		int pid = hal_spawn(cmd, NULL, true);
		int st = 0;

		if (pid < 0)
			continue;
		waitpid(pid, &st, 0);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
			return true;
	}
	return false;
}

void hal_claim_display(void)
{
	const char *grant[] = { BACKLIGHT_HELPER, "grant", NULL };
	const char *detach[] = { FBCON_HELPER, "detach", NULL };

	if (headless)
		return;
	hal_run_root_helper(grant);
	hal_run_root_helper(detach);
}

static bool key_held(int fd, int code)
{
	uint8_t bits[96] = { 0 };

	if (ioctl(fd, EVIOCGKEY(sizeof(bits)), bits) < 0)
		return false;
	return bits[code / 8] & (1 << (code % 8));
}

int hal_run(const char *const *argv)
{
	int status = -1;
	int pid;

	hal_close();
	pid = hal_spawn(argv, NULL, false);
	if (pid > 0) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	hal_buzzer_silence();
	hal_claim_display();
	if (hal_open()) {
		uint32_t deadline = hal_now_ms() + 3000;

		while (hal_now_ms() < deadline && input_fd >= 0 &&
		       (key_held(input_fd, KEY_1) || key_held(input_fd, KEY_5) ||
			key_held(input_fd, KEY_TAB)))
			usleep(50000);
		hal_set_ui_keys(true);
		input_pump();
		q_head = q_tail = 0;
	}
	return pid > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ---------------------------------------------------------------- open/close */

static void make_display(void)
{
	display = lv_display_create(SCREEN_W, SCREEN_H);
	lv_display_set_color_format(display, fb_bpp == 32 ? LV_COLOR_FORMAT_XRGB8888
							  : LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(display, render_a, render_b,
			       fb_bpp == 32 ? sizeof(render_a) : sizeof(render_a) / 2,
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush_cb);
	keypad = lv_indev_create();
	lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(keypad, keypad_read);
}

void hal_headless_start(void)
{
	headless = true;
	fb_bpp = 16;
	fb_stride = SCREEN_W * 2;
	fb_w = SCREEN_W;
	fb_h = SCREEN_H;
	shadow_size = (size_t)fb_stride * fb_h;
	shadow = calloc(1, shadow_size);
	make_display();
	opened = true;
}

bool hal_headless_dump(const char *path)
{
	FILE *fp = fopen(path, "wb");

	if (!fp)
		return false;
	fprintf(fp, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
	for (int y = 0; y < SCREEN_H; y++) {
		for (int x = 0; x < SCREEN_W; x++) {
			uint16_t v;
			uint8_t rgb[3];

			memcpy(&v, shadow + (size_t)y * fb_stride + (size_t)x * 2, 2);
			rgb[0] = (uint8_t)(((v >> 11) & 0x1f) * 255 / 31);
			rgb[1] = (uint8_t)(((v >> 5) & 0x3f) * 255 / 63);
			rgb[2] = (uint8_t)((v & 0x1f) * 255 / 31);
			fwrite(rgb, 1, 3, fp);
		}
	}
	return fclose(fp) == 0;
}

bool hal_is_open(void)
{
	return opened;
}

bool hal_open(void)
{
	if (headless)
		return true;
	if (!fb_prepare())
		return false;
	input_fd = open_buttons();
	if (input_fd < 0) {
		fprintf(stderr, "gamepup-ui: cannot open GamePup buttons (%s)\n", INPUT_PRIMARY);
		close(fb_fd);
		fb_fd = -1;
		return false;
	}
	if (!display)
		make_display();
	opened = true;
	/* Repaint everything: a child owned the panel in the meantime. */
	if (lv_screen_active())
		lv_obj_invalidate(lv_screen_active());
	return true;
}

void hal_close(void)
{
	if (headless)
		return;
	if (input_fd >= 0)
		close(input_fd);
	if (fb_fd >= 0)
		close(fb_fd);
	input_fd = fb_fd = -1;
}

/* ---------------------------------------------------------------- system */

int hal_cpu_temp_c(void)
{
	int raw = hal_file_read_int("/sys/class/thermal/thermal_zone0/temp", -1);

	return raw < 0 ? -1 : raw / 1000;
}

bool hal_which(const char *program)
{
	const char *path = getenv("PATH");
	char *copy, *save = NULL, *dir;
	bool found = false;

	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	copy = strdup(path);
	for (dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
		char full[512];

		snprintf(full, sizeof(full), "%s/%s", dir, program);
		found = access(full, X_OK) == 0;
	}
	free(copy);
	return found;
}

char *hal_alsa_card_id(char *buf, size_t size)
{
	const char *env = getenv("GAMEPUP_ALSA_CARD");
	const char *argv[] = { "aplay", "-l", NULL };
	char line[256];
	int fd = -1, pid;
	FILE *fp;

	buf[0] = '\0';
	if (env && *env) {
		snprintf(buf, size, "%s", env);
		return buf;
	}
	pid = hal_spawn(argv, &fd, true);
	if (pid < 0)
		return buf;
	fcntl(fd, F_SETFL, 0);
	fp = fdopen(fd, "r");
	while (fp && fgets(line, sizeof(line), fp)) {
		if (strcasestr(line, "GamePup") || strcasestr(line, "MAX98357") ||
		    strcasestr(line, "I2S")) {
			char id[64];

			if (sscanf(line, "card %*d: %63s", id) == 1) {
				snprintf(buf, size, "%s", id);
				break;
			}
		}
	}
	if (fp)
		fclose(fp);
	waitpid(pid, NULL, 0);
	return buf;
}
