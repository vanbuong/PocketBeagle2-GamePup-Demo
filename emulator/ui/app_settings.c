// SPDX-License-Identifier: GPL-2.0-only
// Settings and Second Screen pages (iOS-style grouped switches and sliders).
#define _GNU_SOURCE
#include "ui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define SAVES "/opt/gamepup/saves/"
#define MUTE_FILE SAVES "audio-muted"
#define MENU_MUTE_FILE SAVES "menu-beeps-muted"
#define BRIGHTNESS_FILE SAVES "brightness"
#define BEZEL_DISABLED_FILE SAVES "game-bezel-disabled"
#define BEZEL_STYLE_FILE SAVES "game-bezel-style"
#define OLED_DISABLED_FILE SAVES "oled-disabled"
#define OLED_BRIGHTNESS_FILE SAVES "oled-brightness"
#define OLED_REFRESH_FILE SAVES "oled-refresh-hz"
#define OLED_CLOCK_HIDDEN_FILE SAVES "oled-clock-hidden"
#define OLED_PER_CORE_FILE SAVES "oled-per-core"
#define OLED_GPU_HIDDEN_FILE SAVES "oled-gpu-hidden"
#define OLED_GIF_MODE_FILE SAVES "oled-gif-mode"
#define OLED_GIF_SELECTION_FILE SAVES "oled-gif-selection"
#define OLED_GIF_ROOT "/opt/gamepup/gifs"

static const int BRIGHTNESS_LEVELS[] = { 4, 5, 6, 7, 8, 9, 10, 11 };
static const int OLED_BRIGHTNESS_LEVELS[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const int OLED_REFRESH_LEVELS[] = { 5, 10, 15, 20, 25, 30 };
static const char *const BEZEL_STYLES[] = { "OFF", "GAMEPUP", "ARCADE", "SYSTEM" };

typedef enum { S_SWITCH, S_SLIDER, S_CYCLE, S_NAV, S_ACTION } kind_t;

typedef struct setting {
	kind_t kind;
	const char *title, *glyph;
	uint32_t color;
	bool (*is_on)(void);
	void (*toggle)(void);
	const int *levels;
	int nlevels;
	int (*get_level)(void);
	bool (*set_level)(int level);
	const char *unit;
	void (*text)(char *out, size_t size);
	bool (*step)(int direction);
	void (*activate)(void);
	/* runtime */
	lv_obj_t *row, *acc, *val;
} setting_t;

typedef struct {
	setting_t *items;
	int count;
} sheet_t;

/* ------------------------------------------------------------------ helpers */

static int nearest(const int *levels, int n, int value)
{
	int best = 0;

	for (int i = 1; i < n; i++)
		if (abs(levels[i] - value) < abs(levels[best] - value))
			best = i;
	return best;
}

static int level_index(const setting_t *s)
{
	return nearest(s->levels, s->nlevels, s->get_level());
}

static bool step_level(const setting_t *s, int direction)
{
	int pos = level_index(s), next = pos + direction;

	if (next < 0 || next >= s->nlevels)
		return false;
	return s->set_level(s->levels[next]);
}

static void sheet_refresh(sheet_t *sh)
{
	char text[64];

	for (int i = 0; i < sh->count; i++) {
		setting_t *s = &sh->items[i];

		if (!s->title)
			continue;
		switch (s->kind) {
		case S_SWITCH:
			if (s->is_on())
				lv_obj_add_state(s->acc, LV_STATE_CHECKED);
			else
				lv_obj_remove_state(s->acc, LV_STATE_CHECKED);
			break;
		case S_SLIDER:
			lv_bar_set_value(s->acc, level_index(s), LV_ANIM_OFF);
			if (s->val) {
				snprintf(text, sizeof(text), "%d%s", s->get_level(), s->unit ? s->unit : "");
				lv_label_set_text(s->val, text);
			}
			break;
		case S_CYCLE:
			s->text(text, sizeof(text));
			lv_label_set_text(s->acc, text);
			break;
		default:
			break;
		}
	}
}

static setting_t *focused_setting(ui_page_t *page)
{
	sheet_t *sh = page->user;
	lv_obj_t *focus = lv_group_get_focused(page->group);

	for (int i = 0; i < sh->count; i++)
		if (sh->items[i].row == focus)
			return &sh->items[i];
	return NULL;
}

static void activate_setting(setting_t *s)
{
	switch (s->kind) {
	case S_SWITCH:
		s->toggle();
		break;
	case S_CYCLE:
		s->step(1);
		break;
	case S_NAV:
	case S_ACTION:
		s->activate();
		break;
	default:
		break;
	}
}

static void row_click(lv_event_t *e)
{
	ui_page_t *page = lv_event_get_user_data(e);
	setting_t *s = focused_setting(page);

	if (!s)
		return;
	activate_setting(s);
	sheet_refresh(page->user);
}

static bool sheet_key(ui_page_t *page, uint32_t key)
{
	setting_t *s = focused_setting(page);
	bool changed = false;
	int dir = key == LV_KEY_LEFT ? -1 : key == LV_KEY_RIGHT ? 1 : 0;

	if (!s || !dir)
		return false;
	if (s->kind == S_SLIDER)
		changed = step_level(s, dir);
	else if (s->kind == S_CYCLE)
		changed = s->step(dir);
	else
		return false;
	if (changed)
		hal_beep(dir < 0 ? "down" : "up");
	sheet_refresh(page->user);
	return true;
}

static void sheet_free(ui_page_t *page)
{
	sheet_t *sh = page->user;

	free(sh->items);
	free(sh);
}

/* Build a page from a table; NULL title entries are group headers (glyph = text). */
static ui_page_t *build_sheet(const char *title, setting_t *table, int count)
{
	ui_page_t *page = ui_page_create(title, "A Change  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " Adjust");
	sheet_t *sh = calloc(1, sizeof(*sh));

	sh->items = table;
	sh->count = count;
	page->user = sh;
	page->on_key = sheet_key;
	page->on_hide = sheet_free;
	for (int i = 0; i < count; i++) {
		setting_t *s = &table[i];
		lv_obj_t *row;

		if (!s->title) {
			s->row = ui_section(page->content, s->glyph);
			continue;
		}
		row = ui_row(page, page->content, s->glyph, s->color, s->title, NULL);
		s->row = row;
		switch (s->kind) {
		case S_SWITCH:
			s->acc = ui_row_switch(row, s->is_on());
			break;
		case S_SLIDER:
			if (s->unit)
				s->val = ui_label(row, "", FONT_S, C_TEXT_DIM);
			s->acc = ui_row_bar(row, 0, s->nlevels - 1, 0);
			break;
		case S_CYCLE:
			s->acc = ui_row_value(row, "");
			break;
		case S_NAV:
			ui_row_chevron(row);
			break;
		case S_ACTION:
			lv_obj_set_style_text_color(ui_row_title(row), lv_color_hex(C_RED), 0);
			break;
		}
		lv_obj_add_event_cb(row, row_click, LV_EVENT_CLICKED, page);
	}
	sheet_refresh(sh);
	/* Focus the first real row. */
	for (int i = 0; i < count; i++) {
		if (table[i].title) {
			lv_group_focus_obj(table[i].row);
			break;
		}
	}
	return page;
}

/* ------------------------------------------------------------------ Settings */

static bool sound_on(void) { return !hal_file_exists(hal_p(MUTE_FILE)); }

static void toggle_sound(void)
{
	if (hal_file_exists(hal_p(MUTE_FILE))) {
		unlink(hal_p(MUTE_FILE));
		hal_beep("unmute");
	} else {
		hal_file_write_str(hal_p(MUTE_FILE), "muted");
		hal_buzzer_silence();
	}
	ui_status_refresh();
}

static bool beeps_on(void) { return !hal_file_exists(hal_p(MENU_MUTE_FILE)); }

static void toggle_beeps(void)
{
	if (hal_file_exists(hal_p(MENU_MUTE_FILE))) {
		unlink(hal_p(MENU_MUTE_FILE));
		hal_beep("unmute");
	} else {
		hal_beep("down");
		hal_file_write_str(hal_p(MENU_MUTE_FILE), "muted");
	}
}

static int get_brightness(void)
{
	int saved = hal_file_read_int(hal_p(BRIGHTNESS_FILE), -1);

	return saved >= 0 ? saved : hal_backlight_get();
}

static bool set_brightness(int level)
{
	if (!hal_backlight_set(level)) {
		ui_toast("No backlight control found");
		return false;
	}
	hal_file_write_int(hal_p(BRIGHTNESS_FILE), level);
	return true;
}

static int bezel_index(void)
{
	char style[32];

	if (hal_file_exists(hal_p(BEZEL_DISABLED_FILE)))
		return 0;
	if (!hal_file_read_str(hal_p(BEZEL_STYLE_FILE), style, sizeof(style)))
		return 1;
	if (!strcasecmp(style, "PHOTO"))
		return 3;
	for (int i = 0; i < 4; i++)
		if (!strcasecmp(style, BEZEL_STYLES[i]))
			return i;
	return 1;
}

static void bezel_text(char *out, size_t size)
{
	snprintf(out, size, "%s", BEZEL_STYLES[bezel_index()]);
}

static bool bezel_step(int dir)
{
	int next = (bezel_index() + dir + 4) % 4;

	hal_file_write_str(hal_p(BEZEL_STYLE_FILE), BEZEL_STYLES[next]);
	unlink(hal_p(BEZEL_DISABLED_FILE));
	return true;
}

static void exit_to_tty(void)
{
	hal_beep("exit");
	ui_request_exit();
}

void ui_open_settings(void)
{
	setting_t *t = calloc(10, sizeof(*t));
	int n = 0;

	t[n++] = (setting_t){ .glyph = "SOUND" };
	t[n++] = (setting_t){ .kind = S_SWITCH, .title = "All sound", .glyph = LV_SYMBOL_VOLUME_MAX,
			      .color = C_PINK, .is_on = sound_on, .toggle = toggle_sound };
	t[n++] = (setting_t){ .kind = S_SWITCH, .title = "Menu beeps", .glyph = LV_SYMBOL_BELL,
			      .color = C_ORANGE, .is_on = beeps_on, .toggle = toggle_beeps };
	t[n++] = (setting_t){ .glyph = "DISPLAY" };
	t[n++] = (setting_t){ .kind = S_SLIDER, .title = "Brightness", .glyph = LV_SYMBOL_EYE_OPEN,
			      .color = C_YELLOW, .levels = BRIGHTNESS_LEVELS, .nlevels = 8,
			      .get_level = get_brightness, .set_level = set_brightness };
	t[n++] = (setting_t){ .kind = S_CYCLE, .title = "Game bezel", .glyph = LV_SYMBOL_IMAGE,
			      .color = C_PURPLE, .text = bezel_text, .step = bezel_step };
	t[n++] = (setting_t){ .glyph = "GENERAL" };
	t[n++] = (setting_t){ .kind = S_NAV, .title = "Second screen", .glyph = LV_SYMBOL_IMAGE,
			      .color = 0x64D2FF, .activate = ui_open_second_screen };
	t[n++] = (setting_t){ .kind = S_NAV, .title = "About", .glyph = LV_SYMBOL_LIST,
			      .color = C_BLUE, .activate = ui_open_about };
	t[n++] = (setting_t){ .kind = S_ACTION, .title = "Exit to terminal", .glyph = LV_SYMBOL_POWER,
			      .color = C_RED, .activate = exit_to_tty };
	ui_push(build_sheet("Settings", t, n));
}

/* ------------------------------------------------------------------ Second screen */

static bool oled_on(void) { return !hal_file_exists(hal_p(OLED_DISABLED_FILE)); }
static void oled_toggle(void) { hal_file_toggle(hal_p(OLED_DISABLED_FILE)); }
static bool clock_on(void) { return !hal_file_exists(hal_p(OLED_CLOCK_HIDDEN_FILE)); }
static void clock_toggle(void) { hal_file_toggle(hal_p(OLED_CLOCK_HIDDEN_FILE)); }
static bool gpu_on(void) { return !hal_file_exists(hal_p(OLED_GPU_HIDDEN_FILE)); }
static void gpu_toggle(void) { hal_file_toggle(hal_p(OLED_GPU_HIDDEN_FILE)); }

static int oled_brightness(void)
{
	return OLED_BRIGHTNESS_LEVELS[nearest(OLED_BRIGHTNESS_LEVELS, 8,
					      hal_file_read_int(hal_p(OLED_BRIGHTNESS_FILE), 7))];
}
static bool oled_set_brightness(int v) { return hal_file_write_int(hal_p(OLED_BRIGHTNESS_FILE), v); }
static int oled_refresh(void)
{
	return OLED_REFRESH_LEVELS[nearest(OLED_REFRESH_LEVELS, 6,
					   hal_file_read_int(hal_p(OLED_REFRESH_FILE), 20))];
}
static bool oled_set_refresh(int v) { return hal_file_write_int(hal_p(OLED_REFRESH_FILE), v); }

static void mode_text(char *out, size_t size)
{
	snprintf(out, size, "%s", hal_file_exists(hal_p(OLED_GIF_MODE_FILE)) ? "GIF" : "Status");
}
static bool mode_step(int dir)
{
	(void)dir;
	hal_file_toggle(hal_p(OLED_GIF_MODE_FILE));
	return true;
}

static void core_text(char *out, size_t size)
{
	snprintf(out, size, "%s", hal_file_exists(hal_p(OLED_PER_CORE_FILE)) ? "Per core" : "Total");
}
static bool core_step(int dir)
{
	(void)dir;
	hal_file_toggle(hal_p(OLED_PER_CORE_FILE));
	return true;
}

/* GIF choices: sorted *.gif in the GIF folder. */
static int gif_cmp(const void *a, const void *b)
{
	return strcasecmp(a, b);
}

static int list_gifs(char names[][64], int max)
{
	DIR *d = opendir(hal_p(OLED_GIF_ROOT));
	struct dirent *ent;
	int n = 0;

	if (!d)
		return 0;
	while ((ent = readdir(d)) && n < max) {
		size_t len = strlen(ent->d_name);

		if (len > 4 && len < 64 && !strcasecmp(ent->d_name + len - 4, ".gif"))
			snprintf(names[n++], 64, "%s", ent->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, 64, gif_cmp);
	return n;
}

static int gif_current(char names[][64], int n)
{
	char sel[64] = "";

	hal_file_read_str(hal_p(OLED_GIF_SELECTION_FILE), sel, sizeof(sel));
	for (int i = 0; i < n; i++)
		if (!strcmp(names[i], sel))
			return i;
	return 0;
}

static void gif_text(char *out, size_t size)
{
	char names[32][64];
	int n = list_gifs(names, 32);
	char label[64];
	size_t k = 0;

	if (!n) {
		snprintf(out, size, "None");
		return;
	}
	snprintf(label, sizeof(label), "%s", names[gif_current(names, n)]);
	for (char *p = label; *p; p++) {
		if (*p == '-' || *p == '_')
			*p = ' ';
	}
	if (strlen(label) > 4)
		label[strlen(label) - 4] = '\0';
	for (; label[k] && k < 14; k++)
		;
	label[k] = '\0';
	snprintf(out, size, "%s", label);
}

static bool gif_step(int dir)
{
	char names[32][64];
	int n = list_gifs(names, 32);

	if (!n)
		return false;
	return hal_file_write_str(hal_p(OLED_GIF_SELECTION_FILE),
				  names[(gif_current(names, n) + dir + n) % n]);
}

void ui_open_second_screen(void)
{
	setting_t *t = calloc(12, sizeof(*t));
	int n = 0;

	t[n++] = (setting_t){ .glyph = "OLED 128x64" };
	t[n++] = (setting_t){ .kind = S_SWITCH, .title = "Display", .glyph = LV_SYMBOL_POWER,
			      .color = C_ACCENT, .is_on = oled_on, .toggle = oled_toggle };
	t[n++] = (setting_t){ .kind = S_SLIDER, .title = "Brightness", .glyph = LV_SYMBOL_EYE_OPEN,
			      .color = C_YELLOW, .levels = OLED_BRIGHTNESS_LEVELS, .nlevels = 8,
			      .get_level = oled_brightness, .set_level = oled_set_brightness };
	t[n++] = (setting_t){ .kind = S_SLIDER, .title = "Refresh", .glyph = LV_SYMBOL_REFRESH,
			      .color = C_TEAL, .levels = OLED_REFRESH_LEVELS, .nlevels = 6,
			      .get_level = oled_refresh, .set_level = oled_set_refresh, .unit = " Hz" };
	t[n++] = (setting_t){ .glyph = "CONTENT" };
	t[n++] = (setting_t){ .kind = S_CYCLE, .title = "Mode", .glyph = LV_SYMBOL_LIST,
			      .color = C_BLUE, .text = mode_text, .step = mode_step };
	t[n++] = (setting_t){ .kind = S_CYCLE, .title = "GIF", .glyph = LV_SYMBOL_IMAGE,
			      .color = C_PINK, .text = gif_text, .step = gif_step };
	t[n++] = (setting_t){ .kind = S_SWITCH, .title = "Live clock", .glyph = LV_SYMBOL_BELL,
			      .color = C_ORANGE, .is_on = clock_on, .toggle = clock_toggle };
	t[n++] = (setting_t){ .kind = S_CYCLE, .title = "CPU usage", .glyph = LV_SYMBOL_CHARGE,
			      .color = C_PURPLE, .text = core_text, .step = core_step };
	t[n++] = (setting_t){ .kind = S_SWITCH, .title = "GPU status", .glyph = LV_SYMBOL_VIDEO,
			      .color = 0x64D2FF, .is_on = gpu_on, .toggle = gpu_toggle };
	ui_push(build_sheet("Second Screen", t, n));
}
