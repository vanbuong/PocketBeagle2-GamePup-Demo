// SPDX-License-Identifier: GPL-2.0-only
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STACK 8
#define STATUS_H 22
#define TITLE_H 30
#define NAV_H 18

static ui_page_t *stack[MAX_STACK];
static int depth;
static bool styles_ready;
static lv_style_t st_row, st_row_focus, st_row_press, st_btn, st_btn_focus;
static lv_style_transition_dsc_t focus_trans;
static const lv_style_prop_t focus_props[] = { LV_STYLE_BG_COLOR, LV_STYLE_BORDER_OPA,
					       LV_STYLE_OUTLINE_OPA, 0 };
static lv_timer_t *status_timer;

/* ------------------------------------------------------------------ styles */

static void init_styles(void)
{
	if (styles_ready)
		return;
	styles_ready = true;
	lv_style_transition_dsc_init(&focus_trans, focus_props, lv_anim_path_ease_out, 140, 0, NULL);

	lv_style_init(&st_row);
	lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
	lv_style_set_bg_color(&st_row, lv_color_hex(C_CARD));
	lv_style_set_radius(&st_row, 12);
	lv_style_set_border_width(&st_row, 2);
	lv_style_set_border_color(&st_row, lv_color_hex(C_ACCENT));
	lv_style_set_border_opa(&st_row, LV_OPA_TRANSP);
	lv_style_set_pad_all(&st_row, 6);
	lv_style_set_pad_column(&st_row, 10);
	lv_style_set_text_color(&st_row, lv_color_hex(C_TEXT));
	lv_style_set_transition(&st_row, &focus_trans);

	lv_style_init(&st_row_focus);
	lv_style_set_bg_color(&st_row_focus, lv_color_hex(C_CARD_HI));
	lv_style_set_border_opa(&st_row_focus, LV_OPA_COVER);

	lv_style_init(&st_row_press);
	lv_style_set_bg_color(&st_row_press, lv_color_hex(0x3A4668));

	lv_style_init(&st_btn);
	lv_style_set_radius(&st_btn, 14);
	lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
	lv_style_set_border_width(&st_btn, 2);
	lv_style_set_border_color(&st_btn, lv_color_white());
	lv_style_set_border_opa(&st_btn, LV_OPA_TRANSP);
	lv_style_set_pad_hor(&st_btn, 18);
	lv_style_set_pad_ver(&st_btn, 9);
	lv_style_set_text_color(&st_btn, lv_color_white());
	lv_style_set_transition(&st_btn, &focus_trans);

	lv_style_init(&st_btn_focus);
	lv_style_set_border_opa(&st_btn_focus, LV_OPA_COVER);
}

/* ------------------------------------------------------------------ status bar */

static void page_status_update(ui_page_t *p)
{
	time_t now = time(NULL);
	struct tm tm;
	char text[24];
	int temp = hal_cpu_temp_c();
	bool muted = hal_file_exists(hal_p("/opt/gamepup/saves/audio-muted"));

	localtime_r(&now, &tm);
	strftime(text, sizeof(text), "%H:%M", &tm);
	lv_label_set_text(p->clock, text);
	lv_label_set_text(p->sound_icon, muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
	lv_obj_set_style_text_color(p->sound_icon, lv_color_hex(muted ? C_RED : C_TEXT), 0);
	if (temp >= 0) {
		snprintf(text, sizeof(text), "%d°C", temp);
		lv_label_set_text(p->temp, text);
	} else {
		lv_label_set_text(p->temp, "");
	}
}

void ui_status_refresh(void)
{
	for (int i = 0; i < depth; i++)
		page_status_update(stack[i]);
}

static void status_tick(lv_timer_t *timer)
{
	(void)timer;
	ui_status_refresh();
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
	lv_obj_t *obj = lv_obj_create(parent);

	lv_obj_remove_style_all(obj);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
	return obj;
}

/* ------------------------------------------------------------------ page */

static void page_key_cb(lv_event_t *e);
static void page_delete_cb(lv_event_t *e);
static void page_click_cb(lv_event_t *e);

void ui_focus_step(ui_page_t *page, int direction)
{
	lv_obj_t *before = lv_group_get_focused(page->group);

	if (direction > 0)
		lv_group_focus_next(page->group);
	else
		lv_group_focus_prev(page->group);
	if (lv_group_get_focused(page->group) != before)
		hal_beep(direction > 0 ? "down" : "up");
}

ui_page_t *ui_page_create(const char *title, const char *hint)
{
	ui_page_t *p = calloc(1, sizeof(*p));
	int top = STATUS_H;
	lv_obj_t *bar, *pill, *left;

	init_styles();
	p->scr = lv_obj_create(NULL);
	lv_obj_remove_style_all(p->scr);
	lv_obj_set_size(p->scr, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_opa(p->scr, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p->scr, lv_color_hex(C_BG_TOP), 0);
	lv_obj_set_style_bg_grad_color(p->scr, lv_color_hex(C_BG_BOT), 0);
	lv_obj_set_style_bg_grad_dir(p->scr, LV_GRAD_DIR_VER, 0);
	lv_obj_remove_flag(p->scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_user_data(p->scr, p);
	p->group = lv_group_create();
	lv_group_set_wrap(p->group, true);

	/* Status bar */
	bar = plain(p->scr);
	lv_obj_set_size(bar, SCREEN_W, STATUS_H);
	lv_obj_set_pos(bar, 0, 0);
	p->clock = ui_label(bar, "--:--", FONT_M, C_TEXT);
	lv_obj_align(p->clock, LV_ALIGN_LEFT_MID, 12, 0);
	p->sound_icon = ui_label(bar, LV_SYMBOL_VOLUME_MAX, FONT_S, C_TEXT);
	lv_obj_align(p->sound_icon, LV_ALIGN_RIGHT_MID, -12, 0);
	p->temp = ui_label(bar, "", FONT_S, C_TEXT_DIM);
	lv_obj_align_to(p->temp, p->sound_icon, LV_ALIGN_OUT_LEFT_MID, -8, 0);

	if (title) {
		lv_obj_t *back = ui_label(p->scr, LV_SYMBOL_LEFT, FONT_L, C_ACCENT);

		lv_obj_set_pos(back, 12, top + 6);
		lv_obj_t *t = ui_label(p->scr, title, FONT_XL, C_TEXT);

		lv_obj_set_pos(t, 34, top + 3);
		top += TITLE_H;
	}

	p->content = lv_obj_create(p->scr);
	lv_obj_remove_style_all(p->content);
	lv_obj_set_pos(p->content, 0, top);
	lv_obj_set_size(p->content, SCREEN_W, SCREEN_H - NAV_H - top);
	lv_obj_set_flex_flow(p->content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(p->content, 10, 0);
	lv_obj_set_style_pad_top(p->content, 4, 0);
	lv_obj_set_style_pad_bottom(p->content, 6, 0);
	lv_obj_set_style_pad_row(p->content, 6, 0);
	lv_obj_set_scrollbar_mode(p->content, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_flag(p->content, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_remove_flag(p->content, LV_OBJ_FLAG_SCROLL_ELASTIC);

	/* Navigation bar with a home-indicator pill */
	bar = plain(p->scr);
	lv_obj_set_size(bar, SCREEN_W, NAV_H);
	lv_obj_set_pos(bar, 0, SCREEN_H - NAV_H);
	left = ui_label(bar, title ? "B  Back" : "X  Home", FONT_XS, C_TEXT_DIM);
	lv_obj_align(left, LV_ALIGN_LEFT_MID, 12, -1);
	if (!title)
		lv_label_set_text(left, "");
	pill = plain(bar);
	lv_obj_set_size(pill, 56, 4);
	lv_obj_set_style_radius(pill, 2, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_50, 0);
	lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
	lv_obj_align(pill, LV_ALIGN_CENTER, 0, 1);
	p->hint = ui_label(bar, hint ? hint : "", FONT_XS, C_TEXT_DIM);
	lv_obj_align(p->hint, LV_ALIGN_RIGHT_MID, -12, -1);

	lv_obj_add_event_cb(p->scr, page_key_cb, LV_EVENT_KEY, p);
	lv_obj_add_event_cb(p->scr, page_click_cb, LV_EVENT_CLICKED, p);
	lv_obj_add_event_cb(p->scr, page_delete_cb, LV_EVENT_DELETE, p);
	page_status_update(p);
	return p;
}

void ui_page_set_hint(ui_page_t *page, const char *hint)
{
	lv_label_set_text(page->hint, hint);
	lv_obj_align(page->hint, LV_ALIGN_RIGHT_MID, -12, -1);
}

lv_obj_t *ui_page_add_sink(ui_page_t *page)
{
	lv_obj_t *sink = plain(page->scr);

	lv_obj_set_size(sink, 1, 1);
	lv_obj_add_flag(sink, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_group_add_obj(page->group, sink);
	return sink;
}

static void page_delete_cb(lv_event_t *e)
{
	ui_page_t *p = lv_event_get_user_data(e);

	if (p->on_hide)
		p->on_hide(p);
	if (p->group)
		lv_group_delete(p->group);
	free(p);
}

static void page_click_cb(lv_event_t *e)
{
	(void)e;
	hal_beep("select");
}

static void page_key_cb(lv_event_t *e)
{
	ui_page_t *p = lv_event_get_user_data(e);
	uint32_t key = lv_event_get_key(e);

	if (p->on_key && p->on_key(p, key))
		return;
	switch (key) {
	case LV_KEY_UP:
		ui_focus_step(p, -1);
		break;
	case LV_KEY_DOWN:
		ui_focus_step(p, +1);
		break;
	case GP_KEY_START: {
		lv_obj_t *focused = lv_group_get_focused(p->group);

		if (focused)
			lv_obj_send_event(focused, LV_EVENT_CLICKED, NULL);
		break;
	}
	case LV_KEY_ESC:
		if (depth > 1) {
			hal_beep("exit");
			ui_pop();
		}
		break;
	case GP_KEY_X:
		if (depth > 1) {
			hal_beep("exit");
			ui_home();
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ navigation */

static void activate(ui_page_t *p)
{
	lv_indev_set_group(hal_keypad(), p->group);
	if (p->on_show)
		p->on_show(p);
	ui_status_refresh();
}

void ui_set_root(ui_page_t *page)
{
	stack[0] = page;
	depth = 1;
	lv_screen_load(page->scr);
	activate(page);
	if (!status_timer)
		status_timer = lv_timer_create(status_tick, 1000, NULL);
}

ui_page_t *ui_root(void)
{
	return depth ? stack[0] : NULL;
}

ui_page_t *ui_top(void)
{
	return depth ? stack[depth - 1] : NULL;
}

void ui_push(ui_page_t *page)
{
	if (depth >= MAX_STACK) {
		lv_obj_delete(page->scr);
		return;
	}
	stack[depth++] = page;
	lv_screen_load_anim(page->scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
	activate(page);
}

void ui_pop(void)
{
	ui_page_t *gone, *prev;

	if (depth <= 1)
		return;
	gone = stack[--depth];
	prev = stack[depth - 1];
	lv_indev_set_group(hal_keypad(), prev->group);
	lv_screen_load_anim(prev->scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
	lv_obj_delete_delayed(gone->scr, 300);
	if (prev->on_show)
		prev->on_show(prev);
}

void ui_home(void)
{
	ui_page_t *root;

	if (depth <= 1)
		return;
	root = stack[0];
	for (int i = 1; i < depth; i++)
		lv_obj_delete_delayed(stack[i]->scr, 300);
	depth = 1;
	lv_indev_set_group(hal_keypad(), root->group);
	lv_screen_load_anim(root->scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
	if (root->on_show)
		root->on_show(root);
}

/* ------------------------------------------------------------------ toast */

void ui_toast(const char *text)
{
	lv_obj_t *pill = lv_obj_create(lv_layer_top());
	lv_obj_t *label;

	lv_obj_remove_style_all(pill);
	lv_obj_set_style_bg_opa(pill, LV_OPA_90, 0);
	lv_obj_set_style_bg_color(pill, lv_color_hex(0x000000), 0);
	lv_obj_set_style_border_width(pill, 1, 0);
	lv_obj_set_style_border_color(pill, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_radius(pill, 16, 0);
	lv_obj_set_style_pad_hor(pill, 14, 0);
	lv_obj_set_style_pad_ver(pill, 7, 0);
	lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	label = ui_label(pill, text, FONT_S, C_TEXT);
	(void)label;
	lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -26);
	lv_obj_fade_out(pill, 300, 1700);
	lv_obj_delete_delayed(pill, 2100);
}

/* ------------------------------------------------------------------ widgets */

lv_obj_t *ui_section(lv_obj_t *parent, const char *text)
{
	lv_obj_t *label = ui_label(parent, text, FONT_XS, C_TEXT_DIM);

	lv_obj_set_style_pad_left(label, 6, 0);
	lv_obj_set_style_pad_top(label, 4, 0);
	lv_obj_set_style_text_letter_space(label, 1, 0);
	lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
	return label;
}

lv_obj_t *ui_card(lv_obj_t *parent)
{
	lv_obj_t *card = plain(parent);

	lv_obj_set_width(card, LV_PCT(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_radius(card, 14, 0);
	lv_obj_set_style_pad_all(card, 10, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(card, 4, 0);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
	return card;
}

lv_obj_t *ui_icon(lv_obj_t *parent, const char *glyph, uint32_t color, int size)
{
	lv_obj_t *tile = plain(parent);
	lv_obj_t *label;
	lv_color_t base = lv_color_hex(color);
	bool symbol = (unsigned char)glyph[0] >= 0xEF;

	lv_obj_set_size(tile, size, size);
	lv_obj_set_style_radius(tile, size * 28 / 100 + 2, 0);
	lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tile, lv_color_lighten(base, 40), 0);
	lv_obj_set_style_bg_grad_color(tile, lv_color_darken(base, 40), 0);
	lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
	lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
	label = ui_label(tile, glyph, size >= 44 ? (symbol ? FONT_XXL : (strlen(glyph) > 3 ? FONT_M : FONT_L))
						   : (symbol ? FONT_M : FONT_S), 0xFFFFFF);
	lv_obj_center(label);
	return tile;
}

typedef struct {
	lv_obj_t *title, *sub, *accessory;
} row_data_t;

static void row_delete_cb(lv_event_t *e)
{
	free(lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

lv_obj_t *ui_row(ui_page_t *page, lv_obj_t *parent, const char *glyph, uint32_t color,
		 const char *title, const char *subtitle)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_t *text;
	row_data_t *d = calloc(1, sizeof(*d));

	lv_obj_remove_style_all(row);
	lv_obj_add_style(row, &st_row, 0);
	lv_obj_add_style(row, &st_row_focus, LV_STATE_FOCUSED);
	lv_obj_add_style(row, &st_row_press, LV_STATE_PRESSED);
	lv_obj_set_width(row, LV_PCT(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(row, subtitle ? 44 : 38, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_user_data(row, d);
	lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, NULL);

	if (glyph)
		ui_icon(row, glyph, color, 28);
	text = plain(row);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_flag(text, LV_OBJ_FLAG_EVENT_BUBBLE);
	d->title = ui_label(text, title, FONT_M, C_TEXT);
	lv_obj_set_width(d->title, LV_PCT(100));
	lv_label_set_long_mode(d->title, LV_LABEL_LONG_DOT);
	if (subtitle) {
		d->sub = ui_label(text, subtitle, FONT_XS, C_TEXT_DIM);
		lv_obj_set_width(d->sub, LV_PCT(100));
		lv_label_set_long_mode(d->sub, LV_LABEL_LONG_DOT);
	}
	lv_group_add_obj(page->group, row);
	return row;
}

lv_obj_t *ui_row_title(lv_obj_t *row)
{
	return ((row_data_t *)lv_obj_get_user_data(row))->title;
}

lv_obj_t *ui_row_subtitle(lv_obj_t *row)
{
	return ((row_data_t *)lv_obj_get_user_data(row))->sub;
}

lv_obj_t *ui_row_chevron(lv_obj_t *row)
{
	lv_obj_t *chev = ui_label(row, LV_SYMBOL_RIGHT, FONT_S, C_TEXT_DIM);

	((row_data_t *)lv_obj_get_user_data(row))->accessory = chev;
	return chev;
}

lv_obj_t *ui_row_switch(lv_obj_t *row, bool on)
{
	lv_obj_t *sw = lv_switch_create(row);

	lv_obj_set_size(sw, 38, 22);
	lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(sw, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_style_bg_color(sw, lv_color_hex(0x48506A), LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
	if (on)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	((row_data_t *)lv_obj_get_user_data(row))->accessory = sw;
	return sw;
}

lv_obj_t *ui_row_value(lv_obj_t *row, const char *text)
{
	lv_obj_t *label = ui_label(row, text, FONT_M, C_TEXT_DIM);

	((row_data_t *)lv_obj_get_user_data(row))->accessory = label;
	return label;
}

lv_obj_t *ui_row_bar(lv_obj_t *row, int min, int max, int value)
{
	lv_obj_t *bar = lv_bar_create(row);

	lv_obj_set_size(bar, 96, 8);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_bar_set_range(bar, min, max);
	lv_bar_set_value(bar, value, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x48506A), LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
	((row_data_t *)lv_obj_get_user_data(row))->accessory = bar;
	return bar;
}

lv_obj_t *ui_button(ui_page_t *page, lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *btn = lv_obj_create(parent);
	lv_obj_t *label;

	lv_obj_remove_style_all(btn);
	lv_obj_add_style(btn, &st_btn, 0);
	lv_obj_add_style(btn, &st_btn_focus, LV_STATE_FOCUSED);
	lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
	label = ui_label(btn, text, FONT_M, 0xFFFFFF);
	lv_obj_center(label);
	lv_group_add_obj(page->group, btn);
	return btn;
}
