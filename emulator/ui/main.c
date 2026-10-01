// SPDX-License-Identifier: GPL-2.0-only
// gamepup-ui: LVGL phone-style launcher for the PocketBeagle 2 + GamePup A4.
#define _GNU_SOURCE
#include "ui.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMPORT_STATUS "/opt/gamepup/saves/rom-import-status"

static volatile sig_atomic_t running = 1;
static bool exit_requested;
static uint32_t fake_ms;

static void stop_handler(int sig)
{
	(void)sig;
	running = 0;
}

void ui_request_exit(void)
{
	exit_requested = true;
}

bool ui_exit_requested(void)
{
	return exit_requested;
}

static uint32_t fake_tick(void)
{
	return fake_ms;
}

static void apply_saved_brightness(void)
{
	int level = hal_file_read_int(hal_p("/opt/gamepup/saves/brightness"), -1);

	if (level >= 0 && hal_backlight_available())
		hal_backlight_set(level);
}

static void import_watch(lv_timer_t *timer)
{
	static int64_t last;
	int64_t now = hal_file_mtime_ns(hal_p(IMPORT_STATUS));

	(void)timer;
	if (!last) {
		last = now;
		return;
	}
	if (now != last) {
		last = now;
		ui_games_rescan();
		ui_home_refresh();
		ui_toast("ROM library updated");
	}
}

/* ---- headless tour: render scripted key presses to PPM files ---- */

static void frames(int count)
{
	for (int i = 0; i < count; i++) {
		fake_ms += 20;
		lv_timer_handler();
	}
}

static uint32_t key_by_name(const char *name)
{
	static const struct { const char *name; uint32_t key; } KEYS[] = {
		{ "up", LV_KEY_UP }, { "down", LV_KEY_DOWN }, { "left", LV_KEY_LEFT },
		{ "right", LV_KEY_RIGHT }, { "enter", LV_KEY_ENTER }, { "esc", LV_KEY_ESC },
		{ "start", GP_KEY_START }, { "select", GP_KEY_SELECT }, { "x", GP_KEY_X },
		{ "y", GP_KEY_Y },
	};

	for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
		if (!strcmp(KEYS[i].name, name))
			return KEYS[i].key;
	return 0;
}

static int run_script(const char *outdir, const char *script)
{
	char *copy = strdup(script), *save = NULL;

	frames(30);
	for (char *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
		if (!strncmp(tok, "shot:", 5)) {
			char path[512];

			frames(30);
			snprintf(path, sizeof(path), "%s/%s.ppm", outdir, tok + 5);
			if (!hal_headless_dump(path))
				fprintf(stderr, "cannot write %s\n", path);
		} else if (!strncmp(tok, "wait:", 5)) {
			frames(atoi(tok + 5) / 20 + 1);
		} else {
			char *star = strchr(tok, '*');
			int times = 1;
			uint32_t key;

			if (star) {
				*star = '\0';
				times = atoi(star + 1);
			}
			key = key_by_name(tok);
			if (!key) {
				fprintf(stderr, "unknown script token '%s'\n", tok);
				continue;
			}
			while (times-- > 0) {
				hal_inject_key(key, true);
				frames(3);
				hal_inject_key(key, false);
				frames(12);
			}
		}
	}
	free(copy);
	return 0;
}

int main(int argc, char **argv)
{
	const char *headless_dir = NULL, *script = "shot:home";
	ui_page_t *home;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--headless") && i + 1 < argc)
			headless_dir = argv[++i];
		else if (!strcmp(argv[i], "--script") && i + 1 < argc)
			script = argv[++i];
		else if (!strcmp(argv[i], "--version")) {
			printf("gamepup-ui %s\n", UI_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "--list")) {
			ui_games_rescan();
			printf("%d games\n", ui_games_total());
			return 0;
		} else {
			fprintf(stderr, "usage: %s [--headless DIR [--script STEPS]]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	signal(SIGPIPE, SIG_IGN);

	lv_init();
	if (headless_dir) {
		lv_tick_set_cb(fake_tick);
		hal_headless_start();
	} else {
		lv_tick_set_cb(hal_now_ms);
		hal_claim_display();
		apply_saved_brightness();
		if (!hal_open())
			return 1;
		lv_indev_set_long_press_time(hal_keypad(), 800);
	}
	home = ui_build_home();
	ui_set_root(home);
	lv_timer_create(import_watch, 1000, NULL);

	if (headless_dir) {
		int rc = run_script(headless_dir, script);

		ui_media_shutdown();
		return rc;
	}

	while (running && !exit_requested) {
		uint32_t wait = lv_timer_handler();

		hal_wait(wait > 20 ? 20 : (wait < 2 ? 2 : wait));
	}
	ui_media_shutdown();
	hal_buzzer_silence();
	hal_close();
	return 0;
}
