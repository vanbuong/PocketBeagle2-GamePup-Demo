// SPDX-License-Identifier: GPL-2.0-only
// USB ROM import: instructions, one-tap import, and a result summary.
#include "ui.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMPORT_HELPER "/usr/local/libexec/gamepup-rom-import"
#define IMPORT_TIMEOUT_MS 120000

typedef struct {
	ui_page_t *page;
	lv_obj_t *button;
	lv_obj_t *result; /* card replaced on every run */
	lv_timer_t *timer;
	int pid, fd;
	uint32_t started;
	char buf[2048];
	size_t len;
} import_t;

static void import_stop(import_t *im)
{
	if (im->timer) {
		lv_timer_delete(im->timer);
		im->timer = NULL;
	}
	if (im->pid > 0) {
		hal_kill(im->pid, 15);
		hal_child_done(im->pid, NULL);
		im->pid = 0;
	}
	if (im->fd > 0) {
		close(im->fd);
		im->fd = 0;
	}
}

static void import_hide(ui_page_t *page)
{
	import_t *im = page->user;

	import_stop(im);
	free(im);
}

static const char *field(const char *buf, const char *key, char *out, size_t size)
{
	char needle[32];
	const char *p;

	snprintf(needle, sizeof(needle), "%s ", key);
	p = buf;
	while ((p = strstr(p, needle))) {
		if (p == buf || p[-1] == '\n') {
			size_t n = 0;

			p += strlen(needle);
			while (p[n] && p[n] != '\n' && n < size - 1)
				n++;
			memcpy(out, p, n);
			out[n] = '\0';
			return out;
		}
		p++;
	}
	out[0] = '\0';
	return NULL;
}

static lv_obj_t *new_result(import_t *im)
{
	if (im->result)
		lv_obj_delete(im->result);
	im->result = ui_card(im->page->content);
	return im->result;
}

static void stat_chip(lv_obj_t *parent, const char *name, const char *value, uint32_t color)
{
	lv_obj_t *chip = lv_obj_create(parent);

	lv_obj_remove_style_all(chip);
	lv_obj_set_size(chip, 66, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
	ui_label(chip, value, FONT_XL, color);
	ui_label(chip, name, FONT_XS, C_TEXT_DIM);
}

static void show_result(import_t *im)
{
	char value[96], msg[96];
	lv_obj_t *card = new_result(im);

	if (!field(im->buf, "RESULT", value, sizeof(value)) || strcmp(value, "OK")) {
		ui_label(card, LV_SYMBOL_WARNING "  Import failed", FONT_M, C_RED);
		if (!field(im->buf, "MESSAGE", msg, sizeof(msg)))
			snprintf(msg, sizeof(msg), "Check the USB drive and try again");
		lv_obj_t *l = ui_label(card, msg, FONT_S, C_TEXT_DIM);

		lv_obj_set_width(l, LV_PCT(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		return;
	}
	ui_label(card, LV_SYMBOL_OK "  Import complete", FONT_M, C_ACCENT);
	lv_obj_t *chips = lv_obj_create(card);

	lv_obj_remove_style_all(chips);
	lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
	static const struct { const char *key, *name; uint32_t color; } STATS[] = {
		{ "IMPORTED", "Added", C_ACCENT }, { "REPLACED", "Updated", C_BLUE },
		{ "UNCHANGED", "Same", C_TEXT_DIM }, { "REJECTED", "Rejected", C_ORANGE },
	};
	for (int i = 0; i < 4; i++) {
		if (!field(im->buf, STATS[i].key, value, sizeof(value)))
			snprintf(value, sizeof(value), "0");
		stat_chip(chips, STATS[i].name, value, STATS[i].color);
	}
	ui_games_rescan();
	ui_home_refresh();
}

static void import_tick(lv_timer_t *timer)
{
	import_t *im = lv_timer_get_user_data(timer);
	ssize_t n;
	int status;

	while (im->fd > 0 && (n = read(im->fd, im->buf + im->len, sizeof(im->buf) - 1 - im->len)) > 0)
		im->len += (size_t)n;
	im->buf[im->len] = '\0';
	if (hal_child_done(im->pid, &status)) {
		im->pid = 0;
		import_stop(im);
		lv_obj_remove_state(im->button, LV_STATE_DISABLED);
		show_result(im);
		hal_beep("exit");
	} else if (hal_now_ms() - im->started > IMPORT_TIMEOUT_MS) {
		import_stop(im);
		lv_obj_remove_state(im->button, LV_STATE_DISABLED);
		snprintf(im->buf, sizeof(im->buf), "RESULT FAIL\nMESSAGE Import timed out\n");
		show_result(im);
	}
}

static void import_click(lv_event_t *e)
{
	import_t *im = lv_event_get_user_data(e);
	const char *argv[] = { "/usr/bin/sudo", "-n", IMPORT_HELPER, NULL };
	lv_obj_t *card;
	lv_obj_t *spin;

	if (im->pid > 0)
		return;
	im->len = 0;
	im->buf[0] = '\0';
	card = new_result(im);
	im->pid = hal_spawn(argv, &im->fd, true);
	if (im->pid < 0) {
		im->pid = 0;
		snprintf(im->buf, sizeof(im->buf), "RESULT FAIL\nMESSAGE Cannot start import helper\n");
		show_result(im);
		return;
	}
	lv_obj_add_state(im->button, LV_STATE_DISABLED);
	spin = lv_spinner_create(card);
	lv_obj_set_size(spin, 28, 28);
	lv_obj_set_style_arc_width(spin, 4, LV_PART_MAIN);
	lv_obj_set_style_arc_width(spin, 4, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(spin, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(spin, lv_color_hex(C_CARD_HI), LV_PART_MAIN);
	lv_obj_set_style_align(spin, LV_ALIGN_CENTER, 0);
	ui_label(card, "Importing... keep the drive attached", FONT_S, C_TEXT_DIM);
	im->started = hal_now_ms();
	im->timer = lv_timer_create(import_tick, 150, im);
}

static lv_obj_t *step(lv_obj_t *parent, int number, const char *text)
{
	lv_obj_t *line = lv_obj_create(parent);
	char digit[4];
	lv_obj_t *dot;

	lv_obj_remove_style_all(line);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 8, 0);
	lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
	snprintf(digit, sizeof(digit), "%d", number);
	dot = ui_icon(line, digit, C_TEAL, 20);
	lv_obj_set_style_radius(dot, 10, 0);
	lv_obj_t *label = ui_label(line, text, FONT_S, C_TEXT);

	lv_obj_set_flex_grow(label, 1);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	return label;
}

void ui_open_import(void)
{
	ui_page_t *page = ui_page_create("Import ROMs", "A Import");
	import_t *im = calloc(1, sizeof(*im));
	lv_obj_t *card = ui_card(page->content);

	im->page = page;
	page->user = im;
	page->on_hide = import_hide;
	lv_obj_set_style_pad_row(card, 6, 0);
	step(card, 1, "Drag ROMs or ZIPs onto the GamePup USB drive");
	step(card, 2, "Eject the drive on your computer");
	step(card, 3, "Imports run automatically, or tap below");
	im->button = ui_button(page, page->content, LV_SYMBOL_USB "  Import now", C_TEAL);
	lv_obj_add_event_cb(im->button, import_click, LV_EVENT_CLICKED, im);
	ui_push(page);
}
