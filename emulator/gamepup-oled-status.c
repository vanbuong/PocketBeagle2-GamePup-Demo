/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * System status display for a 1.3" 128x64 SH1106 I2C OLED on PocketBeagle 2
 * + GamePup A4, with optional EC11 rotary-encoder controls.
 *
 * Display: /dev/i2c-2 @ 0x3C (SH1106, 128x64 monochrome)
 * Encoder: input device "gamepup-encoder" (REL_DIAL + KEY_ENTER)
 *   Rotate  = brightness 1–8
 *   Click   = toggle status ↔ GIF mode
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <gif_lib.h>
#include <glob.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_FB_SIZE (OLED_WIDTH * OLED_PAGES)
/* SH1106 GDDRAM is 132 columns; visible 128 starts at column 2. */
#define SH1106_COLUMN_OFFSET 2
#define I2C_DEVICE_DEFAULT "/dev/i2c-2"
#define I2C_ADDR_DEFAULT 0x3c
#define FPS_FILE "/run/gamepup/fps"
#define OLED_DISABLED_FILE "/opt/gamepup/saves/oled-disabled"
#define OLED_BRIGHTNESS_FILE "/opt/gamepup/saves/oled-brightness"
#define OLED_REFRESH_FILE "/opt/gamepup/saves/oled-refresh-hz"
#define OLED_CLOCK_HIDDEN_FILE "/opt/gamepup/saves/oled-clock-hidden"
#define OLED_PER_CORE_FILE "/opt/gamepup/saves/oled-per-core"
#define OLED_GPU_HIDDEN_FILE "/opt/gamepup/saves/oled-gpu-hidden"
#define OLED_GIF_MODE_FILE "/opt/gamepup/saves/oled-gif-mode"
#define OLED_GIF_SELECTION_FILE "/opt/gamepup/saves/oled-gif-selection"
#define OLED_GIF_DIRECTORY "/opt/gamepup/gifs"
#define OLED_DEFAULT_GIF "bongo-cat.gif"
#define ENCODER_DIAL_NAME "gamepup-encoder"
#define ENCODER_BUTTON_NAME "gamepup-encoder-button"
#define MAX_ENCODER_FDS 2
#define MAX_CPU_CORES 4
#define MAX_GIF_FRAMES 256

#ifndef I2C_SLAVE
#define I2C_SLAVE 0x0703
#endif

enum pixel {
	PIXEL_OFF = 0,
	PIXEL_ON = 1,
};

static int i2c_fd = -1;
static int encoder_fds[MAX_ENCODER_FDS];
static size_t encoder_fd_count;
static uint8_t i2c_addr = I2C_ADDR_DEFAULT;
static uint8_t canvas[OLED_WIDTH * OLED_HEIGHT];
static uint8_t presented_pages[OLED_FB_SIZE];
static bool presented_valid;
static volatile sig_atomic_t keep_running = 1;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	keep_running = 0;
}

static void fill_rect(int x, int y, int width, int height, uint8_t color)
{
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + width > OLED_WIDTH ? OLED_WIDTH : x + width;
	int y1 = y + height > OLED_HEIGHT ? OLED_HEIGHT : y + height;

	for (int row = y0; row < y1; ++row)
		for (int column = x0; column < x1; ++column)
			canvas[row * OLED_WIDTH + column] = color ? PIXEL_ON : PIXEL_OFF;
}

struct glyph {
	char character;
	uint8_t columns[5];
};

/* Five-column, seven-pixel-high glyphs; bit zero is the top pixel. */
static const struct glyph font[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
	{'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
	{'@', {0x3e, 0x41, 0x5d, 0x55, 0x1e}},
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
	{'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
	{'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
	{'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
	{'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
	{'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
	{'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
	{'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
	{'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
	{'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
	{'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
	{'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
	{'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
	{'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
	{'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
	{'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
	{'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
	{'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
	{'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
	{'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
	{'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
	{'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
	{'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
	{'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
	{'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
	{'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
	{'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
	{'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
	{'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}},
	{'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
	{'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
	{'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
	{'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
	{'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
	{'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
	{'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static const uint8_t *find_glyph(char character)
{
	for (size_t index = 0; index < sizeof(font) / sizeof(font[0]); ++index)
		if (font[index].character == character)
			return font[index].columns;
	return font[0].columns;
}

static int text_width(const char *text, int scale)
{
	return text[0] ? (int)strlen(text) * 6 * scale - scale : 0;
}

static void draw_text(int x, int y, const char *text, int scale, uint8_t color)
{
	for (; *text; ++text, x += 6 * scale) {
		const uint8_t *glyph = find_glyph(*text >= 'a' && *text <= 'z' ?
						  (char)(*text - 'a' + 'A') : *text);

		for (int column = 0; column < 5; ++column)
			for (int row = 0; row < 7; ++row)
				if (glyph[column] & (1u << row))
					fill_rect(x + column * scale, y + row * scale,
						  scale, scale, color);
	}
}

struct gif_animation {
	uint8_t *frames;
	unsigned *delays_ms;
	size_t frame_count;
	char path[PATH_MAX];
};

static void free_gif_animation(struct gif_animation *animation)
{
	free(animation->frames);
	free(animation->delays_ms);
	memset(animation, 0, sizeof(*animation));
}

static void scale_gif_frame_mono(const uint8_t *source, int source_width,
				 int source_height, uint8_t *destination)
{
	int scaled_width = OLED_WIDTH;
	int scaled_height = source_height * OLED_WIDTH / source_width;
	int offset_x;
	int offset_y;

	if (scaled_height > OLED_HEIGHT) {
		scaled_height = OLED_HEIGHT;
		scaled_width = source_width * OLED_HEIGHT / source_height;
	}
	if (scaled_width < 1)
		scaled_width = 1;
	if (scaled_height < 1)
		scaled_height = 1;
	offset_x = (OLED_WIDTH - scaled_width) / 2;
	offset_y = (OLED_HEIGHT - scaled_height) / 2;
	memset(destination, PIXEL_OFF, OLED_WIDTH * OLED_HEIGHT);
	for (int y = 0; y < scaled_height; ++y) {
		int source_y = y * source_height / scaled_height;

		for (int x = 0; x < scaled_width; ++x) {
			int source_x = x * source_width / scaled_width;
			uint8_t gray = source[source_y * source_width + source_x];

			destination[(offset_y + y) * OLED_WIDTH + offset_x + x] =
				gray >= 128 ? PIXEL_ON : PIXEL_OFF;
		}
	}
}

static bool load_gif_animation(const char *path, struct gif_animation *animation)
{
	int error = 0;
	GifFileType *gif = DGifOpenFileName(path, &error);
	GraphicsControlBlock control;
	size_t capacity = 8;
	uint8_t *gray = NULL;
	int canvas_width;
	int canvas_height;

	free_gif_animation(animation);
	if (!gif)
		return false;
	if (DGifSlurp(gif) != GIF_OK) {
		DGifCloseFile(gif, &error);
		return false;
	}
	canvas_width = gif->SWidth;
	canvas_height = gif->SHeight;
	if (canvas_width < 1 || canvas_height < 1 ||
	    gif->ImageCount < 1) {
		DGifCloseFile(gif, &error);
		return false;
	}
	gray = calloc((size_t)canvas_width * (size_t)canvas_height, 1);
	animation->frames = calloc(capacity, OLED_WIDTH * OLED_HEIGHT);
	animation->delays_ms = calloc(capacity, sizeof(*animation->delays_ms));
	if (!gray || !animation->frames || !animation->delays_ms) {
		free(gray);
		free_gif_animation(animation);
		DGifCloseFile(gif, &error);
		return false;
	}
	snprintf(animation->path, sizeof(animation->path), "%s", path);
	for (int frame = 0; frame < gif->ImageCount &&
	     animation->frame_count < MAX_GIF_FRAMES; ++frame) {
		const SavedImage *image = &gif->SavedImages[frame];
		const ColorMapObject *map = image->ImageDesc.ColorMap ?
					    image->ImageDesc.ColorMap :
					    gif->SColorMap;
		int transparent = -1;
		unsigned delay_ms = 100;
		int disposal = DISPOSAL_UNSPECIFIED;

		if (animation->frame_count == capacity) {
			size_t next = capacity * 2;
			uint8_t *frames = realloc(animation->frames,
						  next * OLED_WIDTH * OLED_HEIGHT);
			unsigned *delays = realloc(animation->delays_ms,
						   next * sizeof(*delays));

			if (!frames || !delays) {
				free(frames);
				free(delays);
				break;
			}
			animation->frames = frames;
			animation->delays_ms = delays;
			capacity = next;
		}
		if (DGifSavedExtensionToGCB(gif, frame, &control) == GIF_OK) {
			if (control.DelayTime > 0)
				delay_ms = (unsigned)control.DelayTime * 10u;
			if (control.TransparentColor != NO_TRANSPARENT_COLOR)
				transparent = control.TransparentColor;
			disposal = control.DisposalMode;
		}
		for (int y = 0; y < image->ImageDesc.Height; ++y) {
			int dest_y = image->ImageDesc.Top + y;

			if (dest_y < 0 || dest_y >= canvas_height)
				continue;
			for (int x = 0; x < image->ImageDesc.Width; ++x) {
				int dest_x = image->ImageDesc.Left + x;
				int index = image->RasterBits[y * image->ImageDesc.Width + x];
				GifColorType color;

				if (dest_x < 0 || dest_x >= canvas_width)
					continue;
				if (index == transparent || !map || index >= map->ColorCount)
					continue;
				color = map->Colors[index];
				gray[dest_y * canvas_width + dest_x] =
					(uint8_t)((color.Red * 30 + color.Green * 59 +
						   color.Blue * 11) / 100);
			}
		}
		scale_gif_frame_mono(gray, canvas_width, canvas_height,
				     animation->frames +
				     animation->frame_count * OLED_WIDTH * OLED_HEIGHT);
		animation->delays_ms[animation->frame_count++] = delay_ms < 20 ? 20 : delay_ms;
		if (disposal == DISPOSE_BACKGROUND)
			memset(gray, 0, (size_t)canvas_width * (size_t)canvas_height);
	}
	free(gray);
	DGifCloseFile(gif, &error);
	return animation->frame_count > 0;
}

static bool selected_gif_path(char *path, size_t path_size)
{
	FILE *file = fopen(OLED_GIF_SELECTION_FILE, "r");
	char name[NAME_MAX + 1] = {0};
	glob_t matches = {0};

	if (file) {
		if (fgets(name, sizeof(name), file)) {
			char *newline = strchr(name, '\n');

			if (newline)
				*newline = '\0';
		}
		fclose(file);
	}
	if (!name[0])
		snprintf(name, sizeof(name), "%s", OLED_DEFAULT_GIF);
	snprintf(path, path_size, "%s/%s", OLED_GIF_DIRECTORY, name);
	if (access(path, R_OK) == 0)
		return true;
	if (glob(OLED_GIF_DIRECTORY "/*.gif", 0, NULL, &matches) == 0 &&
	    matches.gl_pathc > 0) {
		strncpy(path, matches.gl_pathv[0], path_size - 1);
		path[path_size - 1] = '\0';
		globfree(&matches);
		return true;
	}
	globfree(&matches);
	return false;
}

static void add_milliseconds(struct timespec *time, unsigned milliseconds)
{
	time->tv_nsec += (long)milliseconds * 1000000L;
	time->tv_sec += time->tv_nsec / 1000000000L;
	time->tv_nsec %= 1000000000L;
}

static bool timespec_reached(const struct timespec *now,
			     const struct timespec *deadline)
{
	return now->tv_sec > deadline->tv_sec ||
	       (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

static void render_gif_error(void)
{
	fill_rect(0, 0, OLED_WIDTH, OLED_HEIGHT, PIXEL_OFF);
	draw_text(37, 20, "GIF ERROR", 1, PIXEL_ON);
	draw_text(28, 34, "ADD GIF FILE", 1, PIXEL_ON);
	draw_text(31, 46, "IN SETTINGS", 1, PIXEL_ON);
}

static void i2c_write_all(const uint8_t *data, size_t length)
{
	while (length > 0) {
		ssize_t result = write(i2c_fd, data, length);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			perror("write OLED I2C");
			exit(EXIT_FAILURE);
		}
		data += (size_t)result;
		length -= (size_t)result;
	}
}

static void oled_command(uint8_t command)
{
	uint8_t packet[] = {0x00, command};

	i2c_write_all(packet, sizeof(packet));
}

static void oled_command_one(uint8_t command, uint8_t argument)
{
	oled_command(command);
	oled_command(argument);
}

static void open_i2c(void)
{
	const char *device = getenv("GAMEPUP_OLED_I2C");
	const char *addr_text = getenv("GAMEPUP_OLED_ADDR");
	unsigned long addr;

	if (!device || !device[0])
		device = I2C_DEVICE_DEFAULT;
	if (addr_text && addr_text[0]) {
		addr = strtoul(addr_text, NULL, 0);
		if (addr > 0x7f) {
			fprintf(stderr, "invalid GAMEPUP_OLED_ADDR\n");
			exit(EXIT_FAILURE);
		}
		i2c_addr = (uint8_t)addr;
	}
	i2c_fd = open(device, O_RDWR | O_CLOEXEC);
	if (i2c_fd < 0 || ioctl(i2c_fd, I2C_SLAVE, i2c_addr) < 0) {
		perror(device);
		exit(EXIT_FAILURE);
	}
}

static int open_named_input(const char *wanted)
{
	glob_t matches = {0};
	int found = -1;

	if (glob("/dev/input/event*", 0, NULL, &matches) != 0)
		return -1;
	for (size_t index = 0; index < matches.gl_pathc; ++index) {
		char name[256] = {0};
		int fd = open(matches.gl_pathv[index], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
		    strcmp(name, wanted) == 0) {
			found = fd;
			break;
		}
		close(fd);
	}
	globfree(&matches);
	return found;
}

static void open_encoder(void)
{
	static const char *names[] = {
		ENCODER_DIAL_NAME,
		ENCODER_BUTTON_NAME,
	};

	encoder_fd_count = 0;
	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
		int fd = open_named_input(names[index]);

		if (fd < 0) {
			fprintf(stderr, "EC11 input '%s' not found.\n", names[index]);
			continue;
		}
		encoder_fds[encoder_fd_count++] = fd;
		fprintf(stderr, "Using EC11 input '%s'.\n", names[index]);
	}
	if (!encoder_fd_count)
		fprintf(stderr, "Continuing without EC11 encoder controls.\n");
}

static void write_config_unsigned(const char *path, unsigned value)
{
	char temporary[PATH_MAX];
	FILE *file;
	int length;

	length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (length < 0 || (size_t)length >= sizeof(temporary))
		return;
	file = fopen(temporary, "w");
	if (!file)
		return;
	fprintf(file, "%u\n", value);
	fclose(file);
	if (rename(temporary, path) != 0)
		unlink(temporary);
}

static void set_gif_mode(bool enabled)
{
	if (enabled) {
		FILE *file = fopen(OLED_GIF_MODE_FILE, "w");

		if (file)
			fclose(file);
	} else {
		unlink(OLED_GIF_MODE_FILE);
	}
}

static void oled_initialize(uint8_t contrast)
{
	struct timespec delay = {.tv_nsec = 10000000};

	oled_command(0xae); /* Display off. */
	oled_command_one(0xd5, 0x80); /* Clock divide. */
	oled_command_one(0xa8, 0x3f); /* Multiplex 1/64. */
	oled_command_one(0xd3, 0x00); /* Display offset. */
	oled_command(0x40); /* Start line 0. */
	oled_command_one(0xad, 0x8b); /* SH1106 DC-DC on. */
	oled_command(0x33); /* Pump voltage 9V (SH1106). */
	oled_command(0xa1); /* Segment remap. */
	oled_command(0xc8); /* COM scan remapped. */
	oled_command_one(0xda, 0x12); /* COM pins. */
	oled_command_one(0x81, contrast);
	oled_command_one(0xd9, 0x1f); /* Pre-charge. */
	oled_command_one(0xdb, 0x40); /* VCOM detect. */
	oled_command(0xa4); /* Resume from RAM. */
	oled_command(0xa6); /* Normal (not inverted). */
	oled_command(0xaf); /* Display on. */
	nanosleep(&delay, NULL);
	presented_valid = false;
}

static void oled_set_contrast(uint8_t contrast)
{
	oled_command_one(0x81, contrast);
}

static void oled_display_off(void)
{
	oled_command(0xae);
}

static void pack_page(int page, uint8_t *out)
{
	for (int column = 0; column < OLED_WIDTH; ++column) {
		uint8_t byte = 0;

		for (int bit = 0; bit < 8; ++bit) {
			int y = page * 8 + bit;

			if (canvas[y * OLED_WIDTH + column])
				byte |= (uint8_t)(1u << bit);
		}
		out[column] = byte;
	}
}

static void oled_present_page(int page, const uint8_t *page_bytes)
{
	uint8_t column = (uint8_t)(SH1106_COLUMN_OFFSET);
	uint8_t packet[OLED_WIDTH + 1];

	oled_command((uint8_t)(0xb0 | page));
	oled_command((uint8_t)(0x00 | (column & 0x0f)));
	oled_command((uint8_t)(0x10 | (column >> 4)));
	packet[0] = 0x40; /* Data mode. */
	memcpy(packet + 1, page_bytes, OLED_WIDTH);
	i2c_write_all(packet, sizeof(packet));
	memcpy(presented_pages + page * OLED_WIDTH, page_bytes, OLED_WIDTH);
}

static void oled_present(void)
{
	uint8_t page_bytes[OLED_WIDTH];

	for (int page = 0; page < OLED_PAGES; ++page) {
		pack_page(page, page_bytes);
		if (presented_valid &&
		    memcmp(presented_pages + page * OLED_WIDTH, page_bytes,
			   OLED_WIDTH) == 0)
			continue;
		oled_present_page(page, page_bytes);
	}
	presented_valid = true;
}

struct cpu_sample {
	uint64_t total;
	uint64_t idle;
};

struct cpu_snapshot {
	struct cpu_sample total;
	struct cpu_sample cores[MAX_CPU_CORES];
	unsigned core_count;
};

static bool parse_cpu_line(const char *line, char *name, size_t name_size,
			   struct cpu_sample *sample)
{
	unsigned long long user = 0;
	unsigned long long nice = 0;
	unsigned long long system = 0;
	unsigned long long idle = 0;
	unsigned long long iowait = 0;
	unsigned long long irq = 0;
	unsigned long long softirq = 0;
	unsigned long long steal = 0;
	char parsed_name[16];
	int count;

	count = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
		       parsed_name, &user, &nice, &system, &idle, &iowait, &irq,
		       &softirq, &steal);
	if (count < 5)
		return false;
	strncpy(name, parsed_name, name_size);
	name[name_size - 1] = '\0';
	sample->idle = idle + iowait;
	sample->total = user + nice + system + idle + iowait + irq + softirq + steal;
	return true;
}

static bool read_cpu_snapshot(struct cpu_snapshot *snapshot)
{
	FILE *file = fopen("/proc/stat", "r");
	char line[512];
	bool found_total = false;

	if (!file)
		return false;
	memset(snapshot, 0, sizeof(*snapshot));
	while (fgets(line, sizeof(line), file)) {
		struct cpu_sample sample;
		char name[16];
		char *end;
		unsigned long core;

		if (strncmp(line, "cpu", 3) != 0)
			break;
		if (!parse_cpu_line(line, name, sizeof(name), &sample))
			continue;
		if (strcmp(name, "cpu") == 0) {
			snapshot->total = sample;
			found_total = true;
			continue;
		}
		core = strtoul(name + 3, &end, 10);
		if (*end != '\0' || core >= MAX_CPU_CORES)
			continue;
		snapshot->cores[core] = sample;
		if (snapshot->core_count < core + 1)
			snapshot->core_count = (unsigned)core + 1;
	}
	fclose(file);
	return found_total;
}

static unsigned cpu_usage(const struct cpu_sample *previous,
			  const struct cpu_sample *current)
{
	uint64_t total = current->total - previous->total;
	uint64_t idle = current->idle - previous->idle;

	if (!total || idle > total)
		return 0;
	return (unsigned)(((total - idle) * 100 + total / 2) / total);
}

static bool read_memory(unsigned *used_megabytes,
			unsigned *total_megabytes, unsigned *percent)
{
	FILE *file = fopen("/proc/meminfo", "r");
	char key[64];
	char unit[16];
	unsigned long value;
	unsigned long total = 0;
	unsigned long available = 0;

	if (!file)
		return false;
	while (fscanf(file, "%63s %lu %15s", key, &value, unit) == 3) {
		if (strcmp(key, "MemTotal:") == 0)
			total = value;
		else if (strcmp(key, "MemAvailable:") == 0)
			available = value;
		if (total && available)
			break;
	}
	fclose(file);
	if (!total || available > total)
		return false;
	*used_megabytes = (unsigned)((total - available + 512) / 1024);
	*total_megabytes = (unsigned)((total + 512) / 1024);
	*percent = (unsigned)(((total - available) * 100 + total / 2) / total);
	return true;
}

static bool read_gpu_usage(unsigned *percent)
{
	static bool firmware_ready;
	static struct timespec client_seen_at;
	static char utilisation_path[PATH_MAX];
	FILE *file;
	char line[256];
	unsigned usage;

	if (!firmware_ready) {
		struct timespec now;
		unsigned references = 0;

		file = fopen("/sys/module/pvrsrvkm/refcnt", "r");
		if (!file || fscanf(file, "%u", &references) != 1) {
			if (file)
				fclose(file);
			return false;
		}
		fclose(file);
		if (!references) {
			client_seen_at.tv_sec = 0;
			client_seen_at.tv_nsec = 0;
			return false;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return false;
		if (!client_seen_at.tv_sec && !client_seen_at.tv_nsec) {
			client_seen_at = now;
			return false;
		}
		if (now.tv_sec - client_seen_at.tv_sec < 2)
			return false;
	}

	if (!utilisation_path[0]) {
		static const char *direct_paths[] = {
			"/sys/kernel/debug/pvr/utilisation_stats",
			"/sys/kernel/debug/pvr/0/utilisation_stats",
		};
		glob_t matches = {0};

		for (size_t index = 0;
		     index < sizeof(direct_paths) / sizeof(direct_paths[0]); ++index) {
			if (access(direct_paths[index], R_OK) == 0) {
				strncpy(utilisation_path, direct_paths[index],
					sizeof(utilisation_path) - 1);
				break;
			}
		}
		if (!utilisation_path[0] &&
		    glob("/sys/kernel/debug/pvr/*/utilisation_stats", 0, NULL, &matches) == 0 &&
		    matches.gl_pathc > 0)
			strncpy(utilisation_path, matches.gl_pathv[0],
				sizeof(utilisation_path) - 1);
		globfree(&matches);
	}
	if (!utilisation_path[0])
		return false;
	file = fopen(utilisation_path, "r");
	if (!file) {
		utilisation_path[0] = '\0';
		return false;
	}
	while (fgets(line, sizeof(line), file)) {
		char *label = strstr(line, "GPU Utilisation:");

		if (label && sscanf(label, "GPU Utilisation: %u%%", &usage) == 1) {
			fclose(file);
			firmware_ready = true;
			*percent = usage > 100 ? 100 : usage;
			return true;
		}
	}
	fclose(file);
	if (!firmware_ready)
		clock_gettime(CLOCK_MONOTONIC, &client_seen_at);
	return false;
}

static void read_fps(char *mode, size_t mode_size, double *fps)
{
	struct stat details;
	struct timespec now;
	FILE *file;

	strncpy(mode, "MENU", mode_size);
	mode[mode_size - 1] = '\0';
	*fps = 0.0;
	if (stat(FPS_FILE, &details) < 0 ||
	    clock_gettime(CLOCK_REALTIME, &now) < 0 ||
	    now.tv_sec - details.st_mtim.tv_sec > 2)
		return;
	file = fopen(FPS_FILE, "r");
	if (!file)
		return;
	if (fscanf(file, "%7s %lf", mode, fps) != 2) {
		strncpy(mode, "MENU", mode_size);
		mode[mode_size - 1] = '\0';
		*fps = 0.0;
	}
	fclose(file);
}

static double read_cpu_frequency_ghz(void)
{
	static const char *paths[] = {
		"/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
		"/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq",
	};

	for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index) {
		FILE *file = fopen(paths[index], "r");
		unsigned long kilohertz;

		if (!file)
			continue;
		if (fscanf(file, "%lu", &kilohertz) == 1) {
			fclose(file);
			return (double)kilohertz / 1000000.0;
		}
		fclose(file);
	}
	return 0.0;
}

struct display_config {
	bool enabled;
	bool gif_mode;
	bool show_clock;
	bool per_core;
	bool show_gpu;
	unsigned brightness;
	unsigned refresh_hz;
};

static unsigned read_config_value(const char *path, unsigned fallback,
				  unsigned minimum, unsigned maximum)
{
	FILE *file = fopen(path, "r");
	unsigned value;

	if (!file)
		return fallback;
	if (fscanf(file, "%u", &value) != 1)
		value = fallback;
	fclose(file);
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

static struct display_config read_display_config(void)
{
	struct display_config config = {
		.enabled = access(OLED_DISABLED_FILE, F_OK) != 0,
		.gif_mode = access(OLED_GIF_MODE_FILE, F_OK) == 0,
		.show_clock = access(OLED_CLOCK_HIDDEN_FILE, F_OK) != 0,
		.per_core = access(OLED_PER_CORE_FILE, F_OK) == 0,
		.show_gpu = access(OLED_GPU_HIDDEN_FILE, F_OK) != 0,
		.brightness = read_config_value(OLED_BRIGHTNESS_FILE, 7, 1, 8),
		.refresh_hz = read_config_value(OLED_REFRESH_FILE, 20, 5, 30),
	};

	return config;
}

static uint8_t contrast_for_brightness(unsigned brightness)
{
	static const uint8_t levels[] = {
		0x10, 0x28, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xff,
	};

	return levels[brightness - 1];
}

static bool config_equal(const struct display_config *left,
			 const struct display_config *right)
{
	return left->enabled == right->enabled &&
	       left->gif_mode == right->gif_mode &&
	       left->show_clock == right->show_clock &&
	       left->per_core == right->per_core &&
	       left->show_gpu == right->show_gpu &&
	       left->brightness == right->brightness &&
	       left->refresh_hz == right->refresh_hz;
}

static void log_config(const struct display_config *config)
{
	fprintf(stderr, "Second screen %s, mode %s, brightness %u/8, refresh %u Hz, "
		"live clock %s, CPU %s, GPU %s.\n", config->enabled ? "on" : "off",
		config->gif_mode ? "GIF" : "status",
		config->brightness, config->refresh_hz,
		config->show_clock ? "on" : "off",
		config->per_core ? "per-core" : "total",
		config->show_gpu ? "on" : "off");
}

static void draw_bar(int y, unsigned percent)
{
	int width = percent > 100 ? 118 : (int)(percent * 118 / 100);

	fill_rect(5, y, 118, 4, PIXEL_OFF);
	for (int x = 5; x < 123; x += 2)
		canvas[y * OLED_WIDTH + x] = PIXEL_ON;
	fill_rect(5, y, width, 4, PIXEL_ON);
}

static void render_status(unsigned cpu, unsigned ram,
			  unsigned ram_used, unsigned ram_total,
			  unsigned gpu, bool gpu_available, const char *mode,
			  double fps, double frequency_ghz,
			  bool show_clock, bool per_core, bool show_gpu,
			  const unsigned *core_usage, unsigned core_count)
{
	char text[40];
	int x;

	fill_rect(0, 0, OLED_WIDTH, OLED_HEIGHT, PIXEL_OFF);
	if (show_clock)
		snprintf(text, sizeof(text), "AM625 @%.1fGHZ", frequency_ghz);
	else
		snprintf(text, sizeof(text), "POCKETBEAGLE 2");
	x = (OLED_WIDTH - text_width(text, 1)) / 2;
	draw_text(x, 0, text, 1, PIXEL_ON);
	fill_rect(4, 9, 120, 1, PIXEL_ON);

	if (per_core) {
		for (unsigned core = 0; core < core_count && core < MAX_CPU_CORES; ++core) {
			int column = (int)(core % 2);
			int row = (int)(core / 2);

			snprintf(text, sizeof(text), "C%u %u%%", core, core_usage[core]);
			draw_text(4 + column * 64, 12 + row * 9, text, 1, PIXEL_ON);
		}
	} else {
		draw_text(5, 12, "CPU", 1, PIXEL_ON);
		snprintf(text, sizeof(text), "%u%%", cpu);
		draw_text(123 - text_width(text, 1), 12, text, 1, PIXEL_ON);
		draw_bar(21, cpu);
	}

	draw_text(5, 28, "RAM", 1, PIXEL_ON);
	snprintf(text, sizeof(text), "%u%%", ram);
	draw_text(123 - text_width(text, 1), 28, text, 1, PIXEL_ON);
	draw_bar(37, ram);

	if (show_gpu) {
		draw_text(5, 44, "GPU", 1, PIXEL_ON);
		if (gpu_available)
			snprintf(text, sizeof(text), "%u%%", gpu);
		else
			snprintf(text, sizeof(text), "--");
		draw_text(40, 44, text, 1, PIXEL_ON);
		snprintf(text, sizeof(text), fps >= 999.5 ? "999" : "%.0f", fps);
		draw_text(70, 44, "FPS", 1, PIXEL_ON);
		draw_text(92, 44, text, 1, PIXEL_ON);
	} else {
		snprintf(text, sizeof(text), "%u/%uM", ram_used, ram_total);
		draw_text(5, 44, text, 1, PIXEL_ON);
		snprintf(text, sizeof(text), fps >= 99.95 ? "100" : "%.1f", fps);
		draw_text(70, 44, "FPS", 1, PIXEL_ON);
		draw_text(92, 44, text, 1, PIXEL_ON);
	}

	fill_rect(4, 54, 120, 1, PIXEL_ON);
	x = (OLED_WIDTH - text_width(mode, 1)) / 2;
	draw_text(x, 56, mode, 1, PIXEL_ON);
}

static void sleep_until_or_input(const struct timespec *deadline)
{
	for (;;) {
		struct timespec now;
		struct timespec remaining;
		struct pollfd poll_fds[MAX_ENCODER_FDS];
		int timeout_ms;
		int result;

		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return;
		if (timespec_reached(&now, deadline))
			return;
		remaining.tv_sec = deadline->tv_sec - now.tv_sec;
		remaining.tv_nsec = deadline->tv_nsec - now.tv_nsec;
		if (remaining.tv_nsec < 0) {
			remaining.tv_sec -= 1;
			remaining.tv_nsec += 1000000000L;
		}
		timeout_ms = (int)(remaining.tv_sec * 1000 + remaining.tv_nsec / 1000000L);
		if (timeout_ms < 1)
			timeout_ms = 1;
		if (!encoder_fd_count) {
			result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
						 deadline, NULL);
			if (result == EINTR && keep_running)
				continue;
			return;
		}
		for (size_t index = 0; index < encoder_fd_count; ++index) {
			poll_fds[index].fd = encoder_fds[index];
			poll_fds[index].events = POLLIN;
			poll_fds[index].revents = 0;
		}
		result = poll(poll_fds, (nfds_t)encoder_fd_count, timeout_ms);
		if (result < 0 && errno == EINTR && keep_running)
			continue;
		if (result > 0)
			return;
	}
}

static bool handle_encoder_events(struct display_config *config)
{
	struct input_event event;
	bool changed = false;
	bool click = false;
	int dial = 0;

	for (size_t index = 0; index < encoder_fd_count; ++index) {
		int fd = encoder_fds[index];

		for (;;) {
			ssize_t result = read(fd, &event, sizeof(event));

			if (result < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				if (errno == EINTR)
					continue;
				perror("read encoder");
				close(fd);
				encoder_fds[index] = encoder_fds[encoder_fd_count - 1];
				encoder_fd_count -= 1;
				--index;
				break;
			}
			if (result != (ssize_t)sizeof(event))
				break;
			if (event.type == EV_REL &&
			    (event.code == REL_DIAL || event.code == REL_WHEEL ||
			     event.code == REL_X))
				dial += event.value;
			else if (event.type == EV_KEY &&
				 (event.code == KEY_ENTER || event.code == KEY_OK ||
				  event.code == BTN_0) &&
				 event.value == 1)
				click = true;
		}
	}
	if (dial) {
		int next = (int)config->brightness + (dial > 0 ? 1 : -1);

		if (next < 1)
			next = 1;
		if (next > 8)
			next = 8;
		if ((unsigned)next != config->brightness) {
			config->brightness = (unsigned)next;
			write_config_unsigned(OLED_BRIGHTNESS_FILE, config->brightness);
			changed = true;
		}
	}
	if (click) {
		config->gif_mode = !config->gif_mode;
		set_gif_mode(config->gif_mode);
		changed = true;
	}
	return changed;
}

int main(void)
{
	struct cpu_snapshot previous = {0};
	struct cpu_snapshot current = {0};
	struct timespec next_update;
	struct timespec gif_deadline = {0};
	struct display_config config;
	struct gif_animation animation = {0};
	size_t gif_frame = 0;
	bool oled_active = false;
	unsigned applied_brightness = 0;

	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	signal(SIGHUP, stop_handler);

	open_i2c();
	open_encoder();
	config = read_display_config();
	if (config.enabled) {
		oled_initialize(contrast_for_brightness(config.brightness));
		oled_active = true;
		applied_brightness = config.brightness;
		read_cpu_snapshot(&previous);
	}
	log_config(&config);
	clock_gettime(CLOCK_MONOTONIC, &next_update);

	while (keep_running) {
		unsigned cpu = 0;
		unsigned ram = 0;
		unsigned ram_used = 0;
		unsigned ram_total = 0;
		unsigned core_usage[MAX_CPU_CORES] = {0};
		unsigned gpu = 0;
		unsigned core_count = 0;
		bool gpu_available;
		char mode[8];
		double fps;
		double frequency_ghz;
		struct display_config new_config;
		long update_nanoseconds = 1000000000L / (long)config.refresh_hz;
		bool encoder_changed;

		next_update.tv_nsec += update_nanoseconds;
		next_update.tv_sec += next_update.tv_nsec / 1000000000L;
		next_update.tv_nsec %= 1000000000L;
		sleep_until_or_input(&next_update);
		if (!keep_running)
			break;

		encoder_changed = handle_encoder_events(&config);
		new_config = read_display_config();
		if (encoder_changed) {
			new_config.brightness = config.brightness;
			new_config.gif_mode = config.gif_mode;
		}
		if (!config_equal(&config, &new_config))
			log_config(&new_config);
		if (config.gif_mode != new_config.gif_mode) {
			gif_deadline.tv_sec = 0;
			gif_deadline.tv_nsec = 0;
			gif_frame = 0;
		}
		config = new_config;
		if (!config.enabled) {
			if (oled_active) {
				oled_display_off();
				oled_active = false;
			}
			continue;
		}
		if (!oled_active) {
			oled_initialize(contrast_for_brightness(config.brightness));
			oled_active = true;
			applied_brightness = config.brightness;
			read_cpu_snapshot(&previous);
		} else if (applied_brightness != config.brightness) {
			oled_set_contrast(contrast_for_brightness(config.brightness));
			applied_brightness = config.brightness;
		}
		if (config.gif_mode) {
			char gif_path[PATH_MAX] = {0};
			struct timespec now;

			if (!selected_gif_path(gif_path, sizeof(gif_path))) {
				free_gif_animation(&animation);
				render_gif_error();
				oled_present();
				continue;
			}
			if (strcmp(gif_path, animation.path) != 0) {
				load_gif_animation(gif_path, &animation);
				gif_frame = 0;
				gif_deadline.tv_sec = 0;
				gif_deadline.tv_nsec = 0;
			}
			if (!animation.frame_count ||
			    clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
				render_gif_error();
				oled_present();
				continue;
			}
			if (!gif_deadline.tv_sec && !gif_deadline.tv_nsec) {
				gif_deadline = now;
				add_milliseconds(&gif_deadline,
						 animation.delays_ms[gif_frame]);
			}
			while (timespec_reached(&now, &gif_deadline)) {
				gif_frame = (gif_frame + 1) % animation.frame_count;
				add_milliseconds(&gif_deadline,
						 animation.delays_ms[gif_frame]);
			}
			memcpy(canvas,
			       animation.frames + gif_frame * OLED_WIDTH * OLED_HEIGHT,
			       sizeof(canvas));
			oled_present();
			continue;
		}
		if (read_cpu_snapshot(&current)) {
			cpu = cpu_usage(&previous.total, &current.total);
			core_count = current.core_count < previous.core_count ?
				     current.core_count : previous.core_count;
			for (unsigned core = 0; core < core_count; ++core)
				core_usage[core] = cpu_usage(&previous.cores[core],
							     &current.cores[core]);
			previous = current;
		}
		read_memory(&ram_used, &ram_total, &ram);
		gpu_available = read_gpu_usage(&gpu);
		read_fps(mode, sizeof(mode), &fps);
		frequency_ghz = read_cpu_frequency_ghz();
		render_status(cpu, ram, ram_used, ram_total, gpu, gpu_available,
			      mode, fps, frequency_ghz, config.show_clock,
			      config.per_core, config.show_gpu,
			      core_usage, core_count);
		oled_present();
	}

	if (oled_active)
		oled_display_off();
	free_gif_animation(&animation);
	for (size_t index = 0; index < encoder_fd_count; ++index)
		close(encoder_fds[index]);
	close(i2c_fd);
	return EXIT_SUCCESS;
}
