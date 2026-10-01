// SPDX-License-Identifier: GPL-2.0-only
// ROM library, platform game lists, DOOM launcher and GPU benchmarks.
#define _GNU_SOURCE
#include "ui.h"
#include "compat.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define FRONTEND "/usr/local/bin/gamepup-retro"
#define GPU_BENCH "/usr/local/bin/gamepup-gpu-bench"
#define GAME_ROOT "/opt/gamepup/games"
#define SELECTION "/opt/gamepup/selected-rom"
#define CORE_DIR "/usr/local/lib/libretro/"

typedef struct {
	char path[400];
	char name[96];
	const char *ext;
	const char *core;
	const char *platform;
	long size;
} game_t;

static const struct {
	const char *ext, *core, *platform;
} CORES[] = {
	{ ".nes", CORE_DIR "nestopia_libretro.so", "nes" },
	{ ".gb", CORE_DIR "gambatte_libretro.so", "gbc" },
	{ ".gbc", CORE_DIR "gambatte_libretro.so", "gbc" },
	{ ".z64", CORE_DIR "mupen64plus_next_libretro.so", "n64" },
	{ ".n64", CORE_DIR "mupen64plus_next_libretro.so", "n64" },
	{ ".v64", CORE_DIR "mupen64plus_next_libretro.so", "n64" },
	{ ".wad", CORE_DIR "prboom_libretro.so", "doom" },
};

static game_t *games;
static int ngames, cap;

static void clean_name(const char *file, char *out, size_t size)
{
	char tmp[160];
	size_t n = 0;
	int depth = 0;
	bool space = false;
	const char *dot = strrchr(file, '.');
	size_t len = dot ? (size_t)(dot - file) : strlen(file);

	for (size_t i = 0; i < len && n < sizeof(tmp) - 1; i++) {
		char c = file[i];

		if (c == '(' || c == '[') {
			depth++;
			continue;
		}
		if ((c == ')' || c == ']') && depth) {
			depth--;
			continue;
		}
		if (depth)
			continue;
		if (c == '_')
			c = ' ';
		if (c == ' ') {
			if (space || n == 0)
				continue;
			space = true;
		} else {
			space = false;
		}
		tmp[n++] = c;
	}
	while (n && tmp[n - 1] == ' ')
		n--;
	tmp[n] = '\0';
	snprintf(out, size, "%s", n ? tmp : file);
}

static void scan_dir(const char *dir, int level)
{
	DIR *d = opendir(dir);
	struct dirent *ent;

	if (!d)
		return;
	while ((ent = readdir(d))) {
		char full[400];
		struct stat st;
		const char *dot;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
		if (stat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (level < 4)
				scan_dir(full, level + 1);
			continue;
		}
		dot = strrchr(ent->d_name, '.');
		if (!dot)
			continue;
		for (size_t i = 0; i < sizeof(CORES) / sizeof(CORES[0]); i++) {
			if (strcasecmp(dot, CORES[i].ext))
				continue;
			if (ngames == cap) {
				cap = cap ? cap * 2 : 32;
				games = realloc(games, (size_t)cap * sizeof(*games));
			}
			game_t *g = &games[ngames++];

			snprintf(g->path, sizeof(g->path), "%s", full);
			clean_name(ent->d_name, g->name, sizeof(g->name));
			g->ext = CORES[i].ext;
			g->core = CORES[i].core;
			g->platform = CORES[i].platform;
			g->size = (long)st.st_size;
			break;
		}
	}
	closedir(d);
}

static int game_cmp(const void *a, const void *b)
{
	return strcasecmp(((const game_t *)a)->name, ((const game_t *)b)->name);
}

void ui_games_rescan(void)
{
	ngames = 0;
	scan_dir(hal_p(GAME_ROOT), 0);
	qsort(games, (size_t)ngames, sizeof(*games), game_cmp);
}

int ui_games_count(const char *platform)
{
	int n = 0;

	for (int i = 0; i < ngames; i++)
		n += !strcmp(games[i].platform, platform);
	return n;
}

int ui_games_total(void)
{
	int n = 0;

	for (int i = 0; i < ngames; i++)
		n += strcmp(games[i].platform, "doom") != 0;
	return n;
}

/* ------------------------------------------------------------------ launch */

static void launch_rom(const char *path, const char *core)
{
	const char *argv[] = { FRONTEND, path, core, NULL };

	hal_file_write_str(hal_p(SELECTION), path);
	hal_run(argv);
	ui_games_rescan();
	ui_home_refresh();
	hal_beep("exit");
}

void ui_launch_doom(void)
{
	const game_t *pick = NULL;

	for (int i = 0; i < ngames; i++) {
		if (strcmp(games[i].platform, "doom"))
			continue;
		if (!pick)
			pick = &games[i];
		if (!strcasecmp(strrchr(games[i].path, '/') ? strrchr(games[i].path, '/') + 1
							  : games[i].path, "doom shareware.wad")) {
			pick = &games[i];
			break;
		}
	}
	if (!pick) {
		ui_toast("No DOOM WAD found");
		return;
	}
	char path[400], core[200];

	snprintf(path, sizeof(path), "%s", pick->path);
	snprintf(core, sizeof(core), "%s", pick->core);
	launch_rom(path, core);
}

/* ------------------------------------------------------------------ platform lists */

typedef struct {
	char **paths;
	char **cores;
	int count;
} list_data_t;

static void list_hide(ui_page_t *page)
{
	list_data_t *d = page->user;

	for (int i = 0; i < d->count; i++) {
		free(d->paths[i]);
		free(d->cores[i]);
	}
	free(d->paths);
	free(d->cores);
	free(d);
}

typedef struct {
	list_data_t *list;
	int index;
} game_click_t;

static void game_click_cb(lv_event_t *e)
{
	game_click_t *c = lv_event_get_user_data(e);
	char path[400], core[200];

	snprintf(path, sizeof(path), "%s", c->list->paths[c->index]);
	snprintf(core, sizeof(core), "%s", c->list->cores[c->index]);
	launch_rom(path, core);
}

static void game_click_free(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

void ui_open_games(const char *platform)
{
	static const struct {
		const char *key, *title, *glyph, *label;
		uint32_t color;
	} INFO[] = {
		{ "nes", "NES Games", "NES", "NES", 0xE04F5F },
		{ "gbc", "Game Boy", "GB", "GB", 0x7BC043 },
		{ "n64", "N64 Games", "64", "N64", 0x3E7BFA },
	};
	size_t which = 0;
	ui_page_t *page;
	list_data_t *d;
	char selected[400] = "";
	lv_obj_t *focus = NULL;
	char sub[64];

	for (size_t i = 0; i < 3; i++)
		if (!strcmp(INFO[i].key, platform))
			which = i;
	if (!ui_games_count(platform)) {
		ui_toast("No games here yet. Try Import ROMs");
		hal_beep("exit");
		return;
	}
	page = ui_page_create(INFO[which].title, "A Play");
	d = calloc(1, sizeof(*d));
	page->user = d;
	page->on_hide = list_hide;
	hal_file_read_str(hal_p(SELECTION), selected, sizeof(selected));
	d->paths = calloc((size_t)ngames, sizeof(char *));
	d->cores = calloc((size_t)ngames, sizeof(char *));
	for (int i = 0; i < ngames; i++) {
		lv_obj_t *row;
		game_click_t *click;

		if (strcmp(games[i].platform, platform))
			continue;
		d->paths[d->count] = strdup(games[i].path);
		d->cores[d->count] = strdup(games[i].core);
		if (games[i].size >= 1024 * 1024)
			snprintf(sub, sizeof(sub), "%s  •  %.1f MB", INFO[which].label,
				 games[i].size / 1048576.0);
		else
			snprintf(sub, sizeof(sub), "%s  •  %ld KB", INFO[which].label,
				 games[i].size / 1024);
		row = ui_row(page, page->content, INFO[which].glyph, INFO[which].color,
			     games[i].name, sub);
		ui_row_chevron(row);
		click = malloc(sizeof(*click));
		click->list = d;
		click->index = d->count;
		lv_obj_add_event_cb(row, game_click_cb, LV_EVENT_CLICKED, click);
		lv_obj_add_event_cb(row, game_click_free, LV_EVENT_DELETE, click);
		if (!strcmp(games[i].path, selected))
			focus = row;
		d->count++;
	}
	if (focus)
		lv_group_focus_obj(focus);
	ui_push(page);
}

/* ------------------------------------------------------------------ benchmarks */

static const struct {
	const char *title, *sub, *arg, *glyph;
	uint32_t color;
} BENCH[] = {
	{ "GPU Plasma", "Fragment shader stress", "plasma", LV_SYMBOL_IMAGE, C_PURPLE },
	{ "GPU Fill Rate", "Full-screen blending throughput", "fill", LV_SYMBOL_CHARGE, C_ORANGE },
	{ "GPU Triangles", "Geometry and vertex load", "triangles", LV_SYMBOL_SHUFFLE, C_TEAL },
	{ "GL Gears", "Classic OpenGL ES gears", "gears", LV_SYMBOL_REFRESH, C_PINK },
};

static void bench_cb(lv_event_t *e)
{
	const char *argv[] = { GPU_BENCH, lv_event_get_user_data(e), NULL };

	hal_run(argv);
	hal_beep("exit");
}

void ui_open_benchmarks(void)
{
	ui_page_t *page = ui_page_create("Benchmarks", "A Run");

	ui_section(page->content, "POWERVR GPU");
	for (size_t i = 0; i < sizeof(BENCH) / sizeof(BENCH[0]); i++) {
		lv_obj_t *row = ui_row(page, page->content, BENCH[i].glyph, BENCH[i].color,
				       BENCH[i].title, BENCH[i].sub);

		ui_row_chevron(row);
		lv_obj_add_event_cb(row, bench_cb, LV_EVENT_CLICKED, (void *)BENCH[i].arg);
	}
	ui_push(page);
}
