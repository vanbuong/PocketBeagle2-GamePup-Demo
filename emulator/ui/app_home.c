// SPDX-License-Identifier: GPL-2.0-only
// Phone-style home screen: widget, paged app grid, About.
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COLS 4
#define PER_PAGE 8
#define CELL_W 80
#define CELL_H 70
#define ICON 48

typedef struct {
	const char *glyph;
	const char *label;
	uint32_t color;
	void (*open)(void);
	const char *platform; /* shows a game-count badge when set */
} app_t;

static void open_nes(void) { ui_open_games("nes"); }
static void open_gbc(void) { ui_open_games("gbc"); }
static void open_n64(void) { ui_open_games("n64"); }

static const app_t APPS[] = {
	{ "NES", "NES", 0xE04F5F, open_nes, "nes" },
	{ "GB", "Game Boy", 0x7BC043, open_gbc, "gbc" },
	{ "64", "N64", 0x3E7BFA, open_n64, "n64" },
	{ "DOOM", "DOOM", 0xB3261E, ui_launch_doom, NULL },
	{ LV_SYMBOL_AUDIO, "Music", C_PINK, ui_open_music, NULL },
	{ LV_SYMBOL_BELL, "Voice Memo", C_PURPLE, ui_open_voice, NULL },
	{ LV_SYMBOL_CHARGE, "Benchmarks", C_ORANGE, ui_open_benchmarks, NULL },
	{ LV_SYMBOL_SETTINGS, "Settings", C_GRAY, ui_open_settings, NULL },
	{ LV_SYMBOL_USB, "Import", C_TEAL, ui_open_import, NULL },
	{ LV_SYMBOL_KEYBOARD, "Hardware", 0x8E8E93, ui_open_hwtest, NULL },
	{ LV_SYMBOL_IMAGE, "2nd Screen", 0x64D2FF, ui_open_second_screen, NULL },
	{ LV_SYMBOL_LIST, "About", C_BLUE, ui_open_about, NULL },
};
#define N_APPS ((int)(sizeof(APPS) / sizeof(APPS[0])))
#define N_PAGES ((N_APPS + PER_PAGE - 1) / PER_PAGE)

static struct {
	ui_page_t *page;
	lv_obj_t *pager;
	lv_obj_t *tiles[N_APPS];
	lv_obj_t *icons[N_APPS];
	lv_obj_t *labels[N_APPS];
	lv_obj_t *badges[N_APPS];
	lv_obj_t *badge_text[N_APPS];
	lv_obj_t *dots[N_PAGES];
	lv_obj_t *clock, *date, *info;
	int current_page;
	lv_timer_t *timer;
} home;

static void widget_update(void)
{
	time_t now = time(NULL);
	struct tm tm;
	char text[64];
	bool muted = hal_file_exists(hal_p("/opt/gamepup/saves/audio-muted"));

	localtime_r(&now, &tm);
	strftime(text, sizeof(text), "%H:%M", &tm);
	lv_label_set_text(home.clock, text);
	strftime(text, sizeof(text), "%a, %d %b", &tm);
	lv_label_set_text(home.date, text);
	snprintf(text, sizeof(text), "%d games  •  sound %s", ui_games_total(),
		 muted ? "muted" : "on");
	lv_label_set_text(home.info, text);
}

void ui_home_refresh(void)
{
	if (!home.page)
		return;
	for (int i = 0; i < N_APPS; i++) {
		if (!APPS[i].platform)
			continue;
		int count = ui_games_count(APPS[i].platform);
		char text[8];

		snprintf(text, sizeof(text), "%d", count);
		lv_label_set_text(home.badge_text[i], text);
		lv_obj_center(home.badge_text[i]);
		if (count)
			lv_obj_remove_flag(home.badges[i], LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(home.badges[i], LV_OBJ_FLAG_HIDDEN);
	}
	widget_update();
}

static void timer_cb(lv_timer_t *timer)
{
	(void)timer;
	widget_update();
}

static void pager_anim_cb(void *obj, int32_t value)
{
	lv_obj_set_x(obj, value);
}

static void show_page(int page)
{
	lv_anim_t a;

	if (page == home.current_page)
		return;
	home.current_page = page;
	lv_anim_init(&a);
	lv_anim_set_var(&a, home.pager);
	lv_anim_set_exec_cb(&a, pager_anim_cb);
	lv_anim_set_values(&a, lv_obj_get_x(home.pager), -page * SCREEN_W);
	lv_anim_set_duration(&a, 240);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
	for (int i = 0; i < N_PAGES; i++) {
		lv_obj_set_style_bg_opa(home.dots[i], i == page ? LV_OPA_COVER : LV_OPA_30, 0);
		lv_obj_set_width(home.dots[i], i == page ? 16 : 6);
	}
}

static int tile_index(lv_obj_t *tile)
{
	for (int i = 0; i < N_APPS; i++)
		if (home.tiles[i] == tile)
			return i;
	return -1;
}

static void tile_focus_cb(lv_event_t *e)
{
	int i = tile_index(lv_event_get_target_obj(e));
	bool focused = lv_event_get_code(e) == LV_EVENT_FOCUSED;

	if (i < 0)
		return;
	lv_obj_set_style_outline_opa(home.icons[i], focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_text_color(home.labels[i],
				    lv_color_hex(focused ? 0xFFFFFF : 0xB4BBCC), 0);
	if (focused)
		show_page(i / PER_PAGE);
}

static void tile_click_cb(lv_event_t *e)
{
	int i = tile_index(lv_event_get_target_obj(e));

	if (i >= 0)
		APPS[i].open();
}

static void focus_index(int i)
{
	lv_obj_t *before = lv_group_get_focused(home.page->group);

	lv_group_focus_obj(home.tiles[i]);
	if (before != home.tiles[i])
		hal_beep(i > tile_index(before) ? "down" : "up");
}

static bool home_key(ui_page_t *page, uint32_t key)
{
	int i = tile_index(lv_group_get_focused(page->group));
	int local = i % PER_PAGE;
	int row = local / COLS;
	int target = i;

	(void)page;
	if (i < 0)
		return false;
	switch (key) {
	case LV_KEY_LEFT:
		target = (i + N_APPS - 1) % N_APPS;
		break;
	case LV_KEY_RIGHT:
		target = (i + 1) % N_APPS;
		break;
	case LV_KEY_UP:
	case LV_KEY_DOWN:
		/* Two rows per page: the other row, same column, when it exists. */
		target = row == 1 ? i - COLS : i + COLS;
		if (target >= N_APPS)
			target = i;
		break;
	case LV_KEY_ESC:
	case GP_KEY_X:
		return true;
	default:
		return false;
	}
	if (target != i)
		focus_index(target);
	return true;
}

ui_page_t *ui_build_home(void)
{
	ui_page_t *page = ui_page_create(NULL, "A Open");
	lv_obj_t *viewport, *dotbar;

	memset(&home, 0, sizeof(home));
	home.page = page;
	page->on_key = home_key;
	lv_obj_delete(page->content);
	page->content = NULL;
	lv_obj_add_flag(page->clock, LV_OBJ_FLAG_HIDDEN);
	{
		lv_obj_t *brand = ui_label(page->scr, "GAMEPUP", FONT_S, C_TEXT_DIM);

		lv_obj_set_style_text_letter_space(brand, 2, 0);
		lv_obj_set_pos(brand, 12, 4);
	}

	/* Widget: big clock + date */
	home.clock = ui_label(page->scr, "--:--", FONT_XXL, C_TEXT);
	lv_obj_set_pos(home.clock, 14, 24);
	home.date = ui_label(page->scr, "", FONT_M, C_TEXT);
	lv_obj_set_pos(home.date, 112, 27);
	home.info = ui_label(page->scr, "", FONT_S, C_TEXT_DIM);
	lv_obj_set_pos(home.info, 112, 45);

	viewport = lv_obj_create(page->scr);
	lv_obj_remove_style_all(viewport);
	lv_obj_set_pos(viewport, 0, 68);
	lv_obj_set_size(viewport, SCREEN_W, CELL_H * 2);
	lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(viewport, LV_OBJ_FLAG_EVENT_BUBBLE);

	home.pager = lv_obj_create(viewport);
	lv_obj_remove_style_all(home.pager);
	lv_obj_set_pos(home.pager, 0, 0);
	lv_obj_set_size(home.pager, SCREEN_W * N_PAGES, CELL_H * 2);
	lv_obj_remove_flag(home.pager, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(home.pager, LV_OBJ_FLAG_EVENT_BUBBLE);

	for (int i = 0; i < N_APPS; i++) {
		int pg = i / PER_PAGE, local = i % PER_PAGE;
		lv_obj_t *tile = lv_obj_create(home.pager);

		lv_obj_remove_style_all(tile);
		lv_obj_set_size(tile, CELL_W, CELL_H);
		lv_obj_set_pos(tile, pg * SCREEN_W + (local % COLS) * CELL_W,
			       (local / COLS) * CELL_H);
		lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
		home.tiles[i] = tile;

		home.icons[i] = ui_icon(tile, APPS[i].glyph, APPS[i].color, ICON);
		lv_obj_align(home.icons[i], LV_ALIGN_TOP_MID, 0, 5);
		lv_obj_set_style_outline_width(home.icons[i], 2, 0);
		lv_obj_set_style_outline_pad(home.icons[i], 2, 0);
		lv_obj_set_style_outline_color(home.icons[i], lv_color_white(), 0);
		lv_obj_set_style_outline_opa(home.icons[i], LV_OPA_TRANSP, 0);

		home.labels[i] = ui_label(tile, APPS[i].label, FONT_S, 0xB4BBCC);
		lv_obj_align(home.labels[i], LV_ALIGN_BOTTOM_MID, 0, -1);

		if (APPS[i].platform) {
			home.badges[i] = lv_obj_create(tile);
			lv_obj_remove_style_all(home.badges[i]);
			lv_obj_set_size(home.badges[i], LV_SIZE_CONTENT, 16);
			lv_obj_set_style_min_width(home.badges[i], 16, 0);
			lv_obj_set_style_pad_hor(home.badges[i], 4, 0);
			lv_obj_set_style_radius(home.badges[i], 8, 0);
			lv_obj_set_style_bg_opa(home.badges[i], LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(home.badges[i], lv_color_hex(C_RED), 0);
			lv_obj_remove_flag(home.badges[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
			lv_obj_align(home.badges[i], LV_ALIGN_TOP_MID, 24, 0);
			home.badge_text[i] = ui_label(home.badges[i], "0", FONT_XS, 0xFFFFFF);
			lv_obj_center(home.badge_text[i]);
		}

		lv_obj_add_event_cb(tile, tile_focus_cb, LV_EVENT_FOCUSED, NULL);
		lv_obj_add_event_cb(tile, tile_focus_cb, LV_EVENT_DEFOCUSED, NULL);
		lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, NULL);
		lv_group_add_obj(page->group, tile);
	}

	/* Page dots */
	dotbar = lv_obj_create(page->scr);
	lv_obj_remove_style_all(dotbar);
	lv_obj_set_size(dotbar, SCREEN_W, 8);
	lv_obj_set_pos(dotbar, 0, 210);
	lv_obj_set_flex_flow(dotbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(dotbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(dotbar, 6, 0);
	for (int i = 0; i < N_PAGES; i++) {
		home.dots[i] = lv_obj_create(dotbar);
		lv_obj_remove_style_all(home.dots[i]);
		lv_obj_set_size(home.dots[i], i == 0 ? 16 : 6, 6);
		lv_obj_set_style_radius(home.dots[i], 3, 0);
		lv_obj_set_style_bg_color(home.dots[i], lv_color_white(), 0);
		lv_obj_set_style_bg_opa(home.dots[i], i == 0 ? LV_OPA_COVER : LV_OPA_30, 0);
	}

	home.timer = lv_timer_create(timer_cb, 1000, NULL);
	lv_group_focus_obj(home.tiles[0]);
	ui_games_rescan();
	ui_home_refresh();
	return page;
}

/* ------------------------------------------------------------------ About */

void ui_open_about(void)
{
	ui_page_t *page = ui_page_create("About", "B Back");
	lv_obj_t *card = ui_card(page->content), *row;
	lv_obj_t *head, *logo, *names;

	ui_page_add_sink(page);
	head = lv_obj_create(card);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(head, 12, 0);
	lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
	logo = ui_icon(head, LV_SYMBOL_PLAY, C_ACCENT, 48);
	names = lv_obj_create(head);
	lv_obj_remove_style_all(names);
	lv_obj_set_size(names, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
	ui_label(names, "GamePup A4", FONT_XL, C_TEXT);
	ui_label(names, "PocketBeagle 2 handheld", FONT_S, C_TEXT_DIM);
	(void)logo;

	ui_section(page->content, "DETAILS");
	card = ui_card(page->content);
	lv_obj_set_style_pad_row(card, 8, 0);
	{
		static const char *const KEYS[] = { "Version", "System", "Display", "GitHub" };
		const char *vals[] = { UI_VERSION " (LVGL " "9)", "Armbian Linux", "ILI9341 320x240",
				       "@Grippy98" };

		for (int i = 0; i < 4; i++) {
			row = lv_obj_create(card);
			lv_obj_remove_style_all(row);
			lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
			lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
					      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
			ui_label(row, KEYS[i], FONT_M, C_TEXT_DIM);
			ui_label(row, vals[i], FONT_M, i == 3 ? C_ACCENT : C_TEXT);
		}
	}
	ui_push(page);
}
