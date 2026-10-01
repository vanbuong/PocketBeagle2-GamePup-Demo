// SPDX-License-Identifier: GPL-2.0-only
// Hardware test: every cape button, LEDs, buzzer tone, backlight and colour patterns.
#include "ui.h"

#include "compat.h"
#ifndef _WIN32
#include <linux/input-event-codes.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEFT_LED "/sys/class/leds/gamepup:left-eye/brightness"
#define RIGHT_LED "/sys/class/leds/gamepup:right-eye/brightness"

typedef struct {
	int code;
	const char *label;
	int x, y, w, h;
	uint32_t color;
} button_def_t;

static const button_def_t BUTTONS[] = {
	{ KEY_UP, LV_SYMBOL_UP, 44, 0, 36, 24, C_BLUE },
	{ KEY_LEFT, LV_SYMBOL_LEFT, 4, 28, 36, 24, C_BLUE },
	{ KEY_RIGHT, LV_SYMBOL_RIGHT, 84, 28, 36, 24, C_BLUE },
	{ KEY_DOWN, LV_SYMBOL_DOWN, 44, 56, 36, 24, C_BLUE },
	{ KEY_ESC, "X", 206, 0, 36, 24, C_ORANGE },
	{ KEY_P, "Y", 166, 28, 36, 24, C_ACCENT },
	{ KEY_TAB, "A", 246, 28, 36, 24, C_RED },
	{ KEY_ENTER, "B", 206, 56, 36, 24, C_YELLOW },
	{ KEY_5, "SEL", 104, 86, 52, 22, C_GRAY },
	{ KEY_1, "START", 164, 86, 62, 22, C_GRAY },
};
#define N_BUTTONS ((int)(sizeof(BUTTONS) / sizeof(BUTTONS[0])))

typedef struct {
	lv_obj_t *chips[N_BUTTONS];
	lv_obj_t *info;
	lv_obj_t *overlay;
	lv_timer_t *timer;
	bool pressed[N_BUTTONS], tested[N_BUTTONS];
	int freq, brightness, original_brightness, pattern;
	uint32_t exit_since;
	bool exiting;
} hw_t;

static hw_t *hw;

static void write_led(const char *path, bool on)
{
	hal_file_write_str(path, on ? "1" : "0");
}

static int index_of(int code)
{
	for (int i = 0; i < N_BUTTONS; i++)
		if (BUTTONS[i].code == code)
			return i;
	return -1;
}

static void chip_style(hw_t *h, int i)
{
	lv_obj_t *chip = h->chips[i];

	if (h->pressed[i]) {
		lv_obj_set_style_bg_color(chip, lv_color_hex(BUTTONS[i].color), 0);
		lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(chip, lv_color_white(), 0);
	} else {
		lv_obj_set_style_bg_color(chip, lv_color_hex(C_CARD), 0);
		lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(chip, lv_color_hex(h->tested[i] ? C_ACCENT : C_CARD_HI), 0);
	}
}

static void update_info(hw_t *h)
{
	int tested = 0;
	char text[96];

	for (int i = 0; i < N_BUTTONS; i++)
		tested += h->tested[i];
	snprintf(text, sizeof(text), "Buttons %d/%d    Tone %d Hz    Backlight %d", tested, N_BUTTONS,
		 h->freq, h->brightness);
	lv_label_set_text(h->info, text);
}

static void set_pattern(hw_t *h, int pattern)
{
	static const uint32_t SOLID[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000 };
	static const uint32_t BARS[] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000,
					 0x0000FF, 0x000000 };

	if (h->overlay) {
		lv_obj_delete(h->overlay);
		h->overlay = NULL;
	}
	h->pattern = pattern;
	if (!pattern)
		return;
	h->overlay = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(h->overlay);
	lv_obj_set_size(h->overlay, SCREEN_W, SCREEN_H);
	lv_obj_remove_flag(h->overlay, LV_OBJ_FLAG_SCROLLABLE);
	if (pattern <= 5) {
		lv_obj_t *label;

		lv_obj_set_style_bg_opa(h->overlay, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(h->overlay, lv_color_hex(SOLID[pattern - 1]), 0);
		label = ui_label(h->overlay, "START: next pattern", FONT_S,
				 SOLID[pattern - 1] == 0xFFFFFF ? 0x000000 : 0xFFFFFF);
		lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
	} else {
		for (int i = 0; i < 8; i++) {
			lv_obj_t *bar = lv_obj_create(h->overlay);

			lv_obj_remove_style_all(bar);
			lv_obj_set_pos(bar, i * (SCREEN_W / 8), 0);
			lv_obj_set_size(bar, SCREEN_W / 8, SCREEN_H);
			lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(bar, lv_color_hex(BARS[i]), 0);
		}
	}
}

static void raw_cb(int code, int value, void *user)
{
	hw_t *h = user;
	int i = index_of(code);

	if (i < 0)
		return;
	if (value) {
		h->pressed[i] = true;
		h->tested[i] = true;
	} else {
		h->pressed[i] = false;
	}
	if (value == 1) {
		int sel = index_of(KEY_5);

		switch (code) {
		case KEY_UP:
			h->freq = h->freq + 100 > 2000 ? 2000 : h->freq + 100;
			break;
		case KEY_DOWN:
			h->freq = h->freq - 100 < 200 ? 200 : h->freq - 100;
			break;
		case KEY_LEFT:
		case KEY_RIGHT:
			h->brightness += code == KEY_LEFT ? -1 : 1;
			h->brightness = h->brightness < 0 ? 0 : h->brightness > 11 ? 11 : h->brightness;
			hal_backlight_set(h->brightness);
			break;
		case KEY_1:
			if (!h->pressed[sel])
				set_pattern(h, (h->pattern + 1) % 7);
			break;
		default:
			break;
		}
	}
	chip_style(h, i);
	update_info(h);
}

static void hw_tick(lv_timer_t *timer)
{
	hw_t *h = lv_timer_get_user_data(timer);
	bool x = h->pressed[index_of(KEY_ESC)], a = h->pressed[index_of(KEY_TAB)];
	bool y = h->pressed[index_of(KEY_P)], b = h->pressed[index_of(KEY_ENTER)];

	write_led(LEFT_LED, x || a);
	write_led(RIGHT_LED, y || a);
	hal_tone(b ? h->freq : 0);
	if (h->pressed[index_of(KEY_1)] && h->pressed[index_of(KEY_5)]) {
		if (!h->exit_since)
			h->exit_since = hal_now_ms();
		else if (hal_now_ms() - h->exit_since >= 1000 && !h->exiting) {
			h->exiting = true;
			hal_set_raw_cb(NULL, NULL);
			hal_set_ui_keys(true);
			lv_timer_pause(timer);
			ui_pop();
		}
	} else {
		h->exit_since = 0;
	}
}

static void hw_hide(ui_page_t *page)
{
	hw_t *h = page->user;

	lv_timer_delete(h->timer);
	if (h->overlay)
		lv_obj_delete(h->overlay);
	hal_set_raw_cb(NULL, NULL);
	hal_set_ui_keys(true);
	write_led(LEFT_LED, false);
	write_led(RIGHT_LED, false);
	hal_buzzer_silence();
	hal_backlight_set(h->original_brightness);
	free(h);
	hw = NULL;
}

void ui_open_hwtest(void)
{
	ui_page_t *page;
	lv_obj_t *pad;
	hw_t *h;

	if (hw)
		return;
	if (!hal_buzzer_ready())
		ui_toast("Buzzer not found");
	h = calloc(1, sizeof(*h));
	hw = h;
	h->freq = 880;
	h->original_brightness = h->brightness = hal_backlight_get();
	page = ui_page_create("Hardware Test", "Hold START+SEL to exit");
	page->user = h;
	page->on_hide = hw_hide;
	pad = lv_obj_create(page->content);
	lv_obj_remove_style_all(pad);
	lv_obj_set_size(pad, 300, 110);
	lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
	for (int i = 0; i < N_BUTTONS; i++) {
		lv_obj_t *label;

		h->chips[i] = lv_obj_create(pad);
		lv_obj_remove_style_all(h->chips[i]);
		lv_obj_set_pos(h->chips[i], BUTTONS[i].x, BUTTONS[i].y);
		lv_obj_set_size(h->chips[i], BUTTONS[i].w, BUTTONS[i].h);
		lv_obj_set_style_radius(h->chips[i], i < 4 ? 8 : (i < 8 ? BUTTONS[i].h / 2 : 11), 0);
		lv_obj_set_style_border_width(h->chips[i], 2, 0);
		lv_obj_remove_flag(h->chips[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		label = ui_label(h->chips[i], BUTTONS[i].label, FONT_S, C_TEXT);
		lv_obj_center(label);
		chip_style(h, i);
	}
	h->info = ui_label(page->content, "", FONT_S, C_TEXT_DIM);
	{
		lv_obj_t *hint = ui_label(page->content,
					  "A/X/Y LEDs   B buzzer   " LV_SYMBOL_UP LV_SYMBOL_DOWN " tone   "
					  LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " backlight   START colours",
					  FONT_XS, C_TEXT_DIM);

		lv_obj_set_width(hint, LV_PCT(100));
		lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
	}
	update_info(h);
	hal_set_ui_keys(false);
	hal_set_raw_cb(raw_cb, h);
	h->timer = lv_timer_create(hw_tick, 40, h);
	ui_push(page);
}
