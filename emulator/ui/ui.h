/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared widgets and navigation for the GamePup phone-style UI. */
#ifndef GAMEPUP_UI_H
#define GAMEPUP_UI_H

#include "hal.h"

#define UI_VERSION "2.0.0"

/* ---- palette (dark, iOS-like) ---- */
#define C_BG_TOP 0x1B2540
#define C_BG_BOT 0x0A0E1A
#define C_CARD 0x1D2436
#define C_CARD_HI 0x2A3450
#define C_TEXT 0xF2F4F8
#define C_TEXT_DIM 0x8C95AB
#define C_ACCENT 0x34C759
#define C_BLUE 0x0A84FF
#define C_RED 0xFF453A
#define C_ORANGE 0xFF9F0A
#define C_YELLOW 0xFFD60A
#define C_PINK 0xFF375F
#define C_PURPLE 0x5E5CE6
#define C_TEAL 0x30B0C7
#define C_GRAY 0x636366

#define FONT_XS (&lv_font_montserrat_10)
#define FONT_S (&lv_font_montserrat_12)
#define FONT_M (&lv_font_montserrat_14)
#define FONT_L (&lv_font_montserrat_16)
#define FONT_XL (&lv_font_montserrat_20)
#define FONT_XXL (&lv_font_montserrat_28)

typedef struct ui_page ui_page_t;

struct ui_page {
	lv_obj_t *scr;
	lv_obj_t *content;  /* vertical flex container below the title bar */
	lv_obj_t *clock;
	lv_obj_t *sound_icon;
	lv_obj_t *temp;
	lv_obj_t *hint;
	lv_group_t *group;
	/* Return true when the page consumed the key. */
	bool (*on_key)(ui_page_t *page, uint32_t key);
	void (*on_show)(ui_page_t *page);
	void (*on_hide)(ui_page_t *page);
	void *user;
};

/* Page with status bar, back-arrow title bar (title may be NULL for none) and nav bar. */
ui_page_t *ui_page_create(const char *title, const char *hint);
void ui_page_set_hint(ui_page_t *page, const char *hint);
/* Invisible focus target so pages without rows still receive key events. */
lv_obj_t *ui_page_add_sink(ui_page_t *page);
void ui_push(ui_page_t *page);
void ui_pop(void);
void ui_home(void);
ui_page_t *ui_top(void);
ui_page_t *ui_root(void);
void ui_set_root(ui_page_t *page);
void ui_status_refresh(void);
void ui_toast(const char *text);
void ui_focus_step(ui_page_t *page, int direction);

/* ---- widgets ---- */
lv_obj_t *ui_section(lv_obj_t *parent, const char *text);
lv_obj_t *ui_card(lv_obj_t *parent);
/* Icon tile (rounded gradient square) holding a symbol or short text. */
lv_obj_t *ui_icon(lv_obj_t *parent, const char *glyph, uint32_t color, int size);
/* Focusable list row. Activation fires LV_EVENT_CLICKED on the returned row. */
lv_obj_t *ui_row(ui_page_t *page, lv_obj_t *parent, const char *glyph, uint32_t color,
		 const char *title, const char *subtitle);
lv_obj_t *ui_row_title(lv_obj_t *row);
lv_obj_t *ui_row_subtitle(lv_obj_t *row);
lv_obj_t *ui_row_chevron(lv_obj_t *row);
lv_obj_t *ui_row_switch(lv_obj_t *row, bool on);
lv_obj_t *ui_row_value(lv_obj_t *row, const char *text);
lv_obj_t *ui_row_bar(lv_obj_t *row, int min, int max, int value);
lv_obj_t *ui_button(ui_page_t *page, lv_obj_t *parent, const char *text, uint32_t color);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);

/* ---- app screens ---- */
ui_page_t *ui_build_home(void);
void ui_home_refresh(void);
void ui_open_games(const char *platform);
void ui_launch_doom(void);
void ui_open_benchmarks(void);
void ui_open_settings(void);
void ui_open_second_screen(void);
void ui_open_about(void);
void ui_open_import(void);
void ui_open_music(void);
void ui_open_voice(void);
void ui_open_hwtest(void);
void ui_games_rescan(void);
int ui_games_count(const char *platform);
int ui_games_total(void);
bool ui_exit_requested(void);
void ui_request_exit(void);
void ui_media_shutdown(void);

#endif
