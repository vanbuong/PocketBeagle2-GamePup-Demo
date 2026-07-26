/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * System status display for OLED C Click on PocketBeagle 2 + GamePup A4.
 *
 * The current OLED C Click uses a 96x96 SSD1351 controller.  It is driven
 * directly through spidev so it remains independent from the GamePup LCD.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <gif_lib.h>
#include <glob.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <limits.h>
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

#define OLED_WIDTH 96
#define OLED_HEIGHT 96
#define OLED_SPI_HZ 18000000
#define SPI_DEVICE "/dev/spidev0.0"
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
#define MAX_CPU_CORES 4
#define MAX_GIF_FRAMES 256

enum gpio_index {
	GPIO_RW,
	GPIO_EN,
	GPIO_RESET,
	GPIO_DC,
	GPIO_COUNT,
};

static int spi_fd = -1;
static int gpio_fd = -1;
static struct gpiohandle_data gpio_values;
static uint16_t canvas[OLED_WIDTH * OLED_HEIGHT];
static uint16_t presented_canvas[OLED_WIDTH * OLED_HEIGHT];
static bool presented_canvas_valid;
static volatile sig_atomic_t keep_running = 1;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	keep_running = 0;
}

static uint16_t rgb565(unsigned red, unsigned green, unsigned blue)
{
	return (uint16_t)(((red & 0xf8) << 8) |
			  ((green & 0xfc) << 3) | (blue >> 3));
}

static void fill_rect(int x, int y, int width, int height, uint16_t color)
{
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + width > OLED_WIDTH ? OLED_WIDTH : x + width;
	int y1 = y + height > OLED_HEIGHT ? OLED_HEIGHT : y + height;

	for (int row = y0; row < y1; ++row)
		for (int column = x0; column < x1; ++column)
			canvas[row * OLED_WIDTH + column] = color;
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

static void draw_text(int x, int y, const char *text, int scale, uint16_t color)
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
	uint16_t *frames;
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

static void scale_gif_frame(const uint16_t *source, int source_width,
			    int source_height, uint16_t *destination)
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
	memset(destination, 0, OLED_WIDTH * OLED_HEIGHT * sizeof(*destination));
	for (int y = 0; y < scaled_height; ++y) {
		int source_y = y * source_height / scaled_height;

		for (int x = 0; x < scaled_width; ++x) {
			int source_x = x * source_width / scaled_width;

			destination[(y + offset_y) * OLED_WIDTH + x + offset_x] =
				source[source_y * source_width + source_x];
		}
	}
}

static bool load_gif_animation(const char *path, struct gif_animation *animation)
{
	GifFileType *gif = NULL;
	uint16_t *working = NULL;
	uint16_t *restore = NULL;
	uint16_t background = 0;
	int error = 0;
	bool success = false;

	free_gif_animation(animation);
	snprintf(animation->path, sizeof(animation->path), "%s", path);
	gif = DGifOpenFileName(path, &error);
	if (!gif || DGifSlurp(gif) == GIF_ERROR)
		goto cleanup;
	if (gif->SWidth < 1 || gif->SHeight < 1 || gif->SWidth > 1024 ||
	    gif->SHeight > 1024 || gif->ImageCount < 1 ||
	    gif->ImageCount > MAX_GIF_FRAMES ||
	    (size_t)gif->SWidth * gif->SHeight > 1024 * 1024)
		goto cleanup;
	animation->frame_count = (size_t)gif->ImageCount;
	animation->frames = calloc(animation->frame_count * OLED_WIDTH * OLED_HEIGHT,
				   sizeof(*animation->frames));
	animation->delays_ms = calloc(animation->frame_count,
				      sizeof(*animation->delays_ms));
	working = calloc((size_t)gif->SWidth * gif->SHeight, sizeof(*working));
	restore = malloc((size_t)gif->SWidth * gif->SHeight * sizeof(*restore));
	if (!animation->frames || !animation->delays_ms || !working || !restore)
		goto cleanup;
	if (gif->SColorMap && gif->SBackGroundColor >= 0 &&
	    gif->SBackGroundColor < gif->SColorMap->ColorCount) {
		GifColorType color = gif->SColorMap->Colors[gif->SBackGroundColor];

		background = rgb565(color.Red, color.Green, color.Blue);
		for (size_t pixel = 0; pixel < (size_t)gif->SWidth * gif->SHeight; ++pixel)
			working[pixel] = background;
	}

	for (int frame_index = 0; frame_index < gif->ImageCount; ++frame_index) {
		SavedImage *saved = &gif->SavedImages[frame_index];
		GifImageDesc *description = &saved->ImageDesc;
		ColorMapObject *color_map = description->ColorMap ?
			description->ColorMap : gif->SColorMap;
		GraphicsControlBlock control;
		size_t working_size = (size_t)gif->SWidth * gif->SHeight * sizeof(*working);

		memset(&control, 0, sizeof(control));
		control.TransparentColor = NO_TRANSPARENT_COLOR;
		control.DelayTime = 10;
		DGifSavedExtensionToGCB(gif, frame_index, &control);
		if (!color_map || description->Left < 0 || description->Top < 0 ||
		    description->Width < 1 || description->Height < 1 ||
		    description->Left + description->Width > gif->SWidth ||
		    description->Top + description->Height > gif->SHeight)
			goto cleanup;
		if (control.DisposalMode == DISPOSE_PREVIOUS)
			memcpy(restore, working, working_size);
		for (int y = 0; y < description->Height; ++y) {
			for (int x = 0; x < description->Width; ++x) {
				int color_index = saved->RasterBits[y * description->Width + x];
				GifColorType color;

				if (color_index == control.TransparentColor)
					continue;
				if (color_index >= color_map->ColorCount)
					goto cleanup;
				color = color_map->Colors[color_index];
				working[(y + description->Top) * gif->SWidth +
					x + description->Left] =
					rgb565(color.Red, color.Green, color.Blue);
			}
		}
		scale_gif_frame(working, gif->SWidth, gif->SHeight,
				animation->frames + (size_t)frame_index * OLED_WIDTH * OLED_HEIGHT);
		animation->delays_ms[frame_index] =
			control.DelayTime < 2 ? 50u : (unsigned)control.DelayTime * 10u;
		if (control.DisposalMode == DISPOSE_BACKGROUND) {
			for (int y = 0; y < description->Height; ++y)
				for (int x = 0; x < description->Width; ++x)
					working[(y + description->Top) * gif->SWidth +
						x + description->Left] = background;
		} else if (control.DisposalMode == DISPOSE_PREVIOUS) {
			memcpy(working, restore, working_size);
		}
	}
	success = true;

cleanup:
	free(restore);
	free(working);
	if (gif)
		DGifCloseFile(gif, &error);
	if (!success) {
		fprintf(stderr, "Unable to load OLED GIF %s.\n", path);
		free(animation->frames);
		free(animation->delays_ms);
		animation->frames = NULL;
		animation->delays_ms = NULL;
		animation->frame_count = 0;
	}
	return success;
}

static bool selected_gif_path(char *path, size_t path_size)
{
	char name[NAME_MAX + 1] = OLED_DEFAULT_GIF;
	FILE *file = fopen(OLED_GIF_SELECTION_FILE, "r");
	int length;

	if (file) {
		if (fgets(name, sizeof(name), file))
			name[strcspn(name, "\r\n")] = '\0';
		fclose(file);
	}
	if (!name[0] || strchr(name, '/') || strstr(name, ".."))
		strncpy(name, OLED_DEFAULT_GIF, sizeof(name) - 1);
	length = snprintf(path, path_size, "%s/%s", OLED_GIF_DIRECTORY, name);
	if (length > 0 && (size_t)length < path_size && access(path, R_OK) == 0)
		return true;
	{
		glob_t matches = {0};
		bool found = false;

		if (glob(OLED_GIF_DIRECTORY "/*.[gG][iI][fF]", 0, NULL, &matches) == 0 &&
		    matches.gl_pathc > 0) {
			strncpy(path, matches.gl_pathv[0], path_size - 1);
			path[path_size - 1] = '\0';
			found = true;
		}
		globfree(&matches);
		return found;
	}
}

static void add_milliseconds(struct timespec *time_value, unsigned milliseconds)
{
	time_value->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
	time_value->tv_sec += milliseconds / 1000 + time_value->tv_nsec / 1000000000L;
	time_value->tv_nsec %= 1000000000L;
}

static bool timespec_reached(const struct timespec *now,
			     const struct timespec *deadline)
{
	return now->tv_sec > deadline->tv_sec ||
	       (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

static void render_gif_error(void)
{
	const uint16_t background = rgb565(3, 10, 16);
	const uint16_t red = rgb565(232, 79, 79);
	const uint16_t gray = rgb565(112, 128, 120);

	fill_rect(0, 0, OLED_WIDTH, OLED_HEIGHT, background);
	draw_text(19, 32, "GIF ERROR", 1, red);
	draw_text(10, 49, "ADD GIF FILE", 1, gray);
	draw_text(13, 61, "IN SETTINGS", 1, gray);
}

static void gpio_write(enum gpio_index index, int value)
{
	gpio_values.values[index] = value != 0;
	if (ioctl(gpio_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &gpio_values) < 0) {
		perror("set OLED GPIO");
		exit(EXIT_FAILURE);
	}
}

static int open_main_gpio1(void)
{
	for (unsigned index = 0; index < 16; ++index) {
		char path[32];
		struct gpiochip_info information;
		int fd;

		snprintf(path, sizeof(path), "/dev/gpiochip%u", index);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(&information, 0, sizeof(information));
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &information) == 0 &&
		    strcmp(information.label, "601000.gpio") == 0)
			return fd;
		close(fd);
	}
	errno = ENODEV;
	return -1;
}

static void open_gpio(void)
{
	struct gpiohandle_request request;

	gpio_fd = open_main_gpio1();
	if (gpio_fd < 0) {
		perror("open main GPIO1");
		exit(EXIT_FAILURE);
	}

	memset(&request, 0, sizeof(request));
	request.lineoffsets[GPIO_RW] = 1;      /* P1.19 / mikroBUS AN */
	request.lineoffsets[GPIO_EN] = 2;      /* P1.34 / mikroBUS INT */
	request.lineoffsets[GPIO_RESET] = 10;  /* P1.02 / mikroBUS RST */
	request.lineoffsets[GPIO_DC] = 28;     /* P1.36 / mikroBUS PWM */
	request.default_values[GPIO_RW] = 0;
	request.default_values[GPIO_EN] = 0;
	request.default_values[GPIO_RESET] = 1;
	request.default_values[GPIO_DC] = 1;
	request.lines = GPIO_COUNT;
	request.flags = GPIOHANDLE_REQUEST_OUTPUT;
	strncpy(request.consumer_label, "gamepup-oled-status",
		sizeof(request.consumer_label) - 1);
	if (ioctl(gpio_fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
		perror("request OLED GPIO lines");
		exit(EXIT_FAILURE);
	}
	close(gpio_fd);
	gpio_fd = request.fd;
	memcpy(gpio_values.values, request.default_values,
	       sizeof(gpio_values.values));
}

static void spi_write_all(const uint8_t *data, size_t length)
{
	while (length > 0) {
		size_t chunk = length > 4096 ? 4096 : length;
		ssize_t result = write(spi_fd, data, chunk);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			perror("write OLED SPI");
			exit(EXIT_FAILURE);
		}
		data += result;
		length -= (size_t)result;
	}
}

static void oled_command(uint8_t command, const uint8_t *arguments,
			 size_t argument_count)
{
	gpio_write(GPIO_DC, 0);
	spi_write_all(&command, 1);
	if (argument_count > 0) {
		gpio_write(GPIO_DC, 1);
		spi_write_all(arguments, argument_count);
	}
}

static void command_one(uint8_t command, uint8_t argument)
{
	oled_command(command, &argument, 1);
}

static void open_spi(void)
{
	uint8_t mode = SPI_MODE_0;
	uint8_t bits = 8;
	uint32_t speed = OLED_SPI_HZ;

	spi_fd = open(SPI_DEVICE, O_WRONLY | O_CLOEXEC);
	if (spi_fd < 0 || ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		perror(SPI_DEVICE);
		exit(EXIT_FAILURE);
	}
}

static void oled_initialize(uint8_t master_contrast)
{
	static const uint8_t contrast[] = {0x8a, 0x51, 0x8a};
	static const uint8_t vsl[] = {0xa0, 0xb5, 0x55};
	struct timespec millisecond = {.tv_nsec = 1000000};
	struct timespec hundred_milliseconds = {.tv_nsec = 100000000};

	gpio_write(GPIO_RW, 0);
	gpio_write(GPIO_EN, 1);
	gpio_write(GPIO_RESET, 1);
	nanosleep(&millisecond, NULL);
	gpio_write(GPIO_RESET, 0);
	nanosleep(&millisecond, NULL);
	gpio_write(GPIO_RESET, 1);
	nanosleep(&hundred_milliseconds, NULL);

	command_one(0xfd, 0x12); /* Unlock OLED commands. */
	command_one(0xfd, 0xb1); /* Unlock command interface. */
	oled_command(0xae, NULL, 0);
	command_one(0xa0, 0x32); /* RGB565, horizontal increment, reversed scan. */
	command_one(0xca, 95);
	command_one(0xa1, 0x80);
	command_one(0xa2, 0x20);
	command_one(0xbe, 0x05);
	command_one(0xb3, 0xf1);
	command_one(0xb1, 0x32);
	command_one(0xb6, 0x01);
	command_one(0xc7, master_contrast);
	oled_command(0xc1, contrast, sizeof(contrast));
	oled_command(0xb4, vsl, sizeof(vsl));
	oled_command(0xa6, NULL, 0);
	oled_command(0xaf, NULL, 0);
	presented_canvas_valid = false;
}

static void oled_present_rect(int x0, int y0, int x1, int y1)
{
	uint8_t columns[] = {(uint8_t)(0x10 + x0), (uint8_t)(0x10 + x1 - 1)};
	uint8_t rows[] = {(uint8_t)y0, (uint8_t)(y1 - 1)};
	uint8_t pixels[OLED_WIDTH * OLED_HEIGHT * 2];
	size_t offset = 0;

	oled_command(0x15, columns, sizeof(columns));
	oled_command(0x75, rows, sizeof(rows));
	oled_command(0x5c, NULL, 0);
	gpio_write(GPIO_DC, 1);

	for (int row = y0; row < y1; ++row) {
		for (int column = x0; column < x1; ++column) {
			uint16_t color = canvas[row * OLED_WIDTH + column];

			pixels[offset++] = (uint8_t)(color >> 8);
			pixels[offset++] = (uint8_t)color;
			presented_canvas[row * OLED_WIDTH + column] = color;
		}
	}
	spi_write_all(pixels, offset);
}

static bool oled_row_changed(int row)
{
	return memcmp(canvas + row * OLED_WIDTH,
		      presented_canvas + row * OLED_WIDTH,
		      OLED_WIDTH * sizeof(canvas[0])) != 0;
}

static void oled_present(void)
{
	if (!presented_canvas_valid) {
		oled_present_rect(0, 0, OLED_WIDTH, OLED_HEIGHT);
		presented_canvas_valid = true;
		return;
	}

	for (int y = 0; y < OLED_HEIGHT;) {
		int x0 = OLED_WIDTH;
		int x1 = 0;
		int y0;

		while (y < OLED_HEIGHT && !oled_row_changed(y))
			++y;
		if (y == OLED_HEIGHT)
			break;
		y0 = y;
		while (y < OLED_HEIGHT && oled_row_changed(y))
			++y;
		for (int row = y0; row < y; ++row) {
			for (int column = 0; column < OLED_WIDTH; ++column) {
				if (canvas[row * OLED_WIDTH + column] ==
				    presented_canvas[row * OLED_WIDTH + column])
					continue;
				if (x0 > column)
					x0 = column;
				if (x1 <= column)
					x1 = column + 1;
			}
		}
		oled_present_rect(x0, y0, x1, y);
	}
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

	/* TI exposes the file before the first EGL/Vulkan client starts the
	 * firmware. Reading it during that window returns EINVAL and floods the
	 * kernel log. Its status endpoint also reports OK prematurely, so use the
	 * module reference count and allow the first client two seconds to start. */
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
		0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xcf, 0xff,
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

static uint16_t usage_color(unsigned percent)
{
	if (percent >= 85)
		return rgb565(232, 79, 79);
	if (percent >= 65)
		return rgb565(244, 191, 71);
	return rgb565(66, 214, 92);
}

static void draw_bar(int y, unsigned percent, uint16_t color)
{
	int width = percent > 100 ? 86 : (int)(percent * 86 / 100);

	fill_rect(5, y, 86, 6, rgb565(19, 37, 48));
	fill_rect(5, y, width, 6, color);
	fill_rect(5, y, 86, 1, rgb565(61, 83, 91));
}

static void render_status(unsigned cpu, unsigned ram,
			  unsigned ram_used, unsigned ram_total,
			  unsigned gpu, bool gpu_available, const char *mode,
			  double fps, double frequency_ghz,
			  bool show_clock, bool per_core, bool show_gpu,
			  const unsigned *core_usage, unsigned core_count)
{
	const uint16_t background = rgb565(3, 10, 16);
	const uint16_t white = rgb565(238, 244, 238);
	const uint16_t gray = rgb565(112, 128, 120);
	const uint16_t cyan = rgb565(65, 206, 220);
	char text[32];
	int x;

	fill_rect(0, 0, OLED_WIDTH, OLED_HEIGHT, background);
	if (show_clock)
		snprintf(text, sizeof(text), "TI AM625 @%.1fGHZ", frequency_ghz);
	else
		snprintf(text, sizeof(text), "POCKETBEAGLE 2");
	x = (OLED_WIDTH - text_width(text, 1)) / 2;
	draw_text(x, 2, text, 1, cyan);
	fill_rect(4, 11, 88, 1, rgb565(18, 61, 70));

	if (per_core) {
		for (unsigned core = 0; core < core_count && core < MAX_CPU_CORES; ++core) {
			int column = (int)(core % 2);
			int row = (int)(core / 2);

			snprintf(text, sizeof(text), "C%u %u%%", core, core_usage[core]);
			draw_text(4 + column * 48, 14 + row * 10, text, 1,
				  usage_color(core_usage[core]));
		}
	} else {
		draw_text(5, 15, "CPU", 1, gray);
		snprintf(text, sizeof(text), "%u%%", cpu);
		draw_text(91 - text_width(text, 1), 15, text, 1, white);
		draw_bar(24, cpu, usage_color(cpu));
	}

	draw_text(5, 34, "RAM", 1, gray);
	snprintf(text, sizeof(text), "%u%%", ram);
	draw_text(91 - text_width(text, 1), 34, text, 1, white);
	draw_bar(43, ram, usage_color(ram));
	if (show_gpu) {
		draw_text(5, 52, "GPU", 1, gray);
		if (gpu_available)
			snprintf(text, sizeof(text), "%u%%", gpu);
		else
			snprintf(text, sizeof(text), "--");
		draw_text(91 - text_width(text, 1), 52, text, 1,
			  gpu_available ? white : gray);
		draw_bar(61, gpu_available ? gpu : 0,
			 gpu_available ? usage_color(gpu) : gray);
		draw_text(5, 72, "FPS", 1, gray);
		snprintf(text, sizeof(text), fps >= 999.5 ? "999" : "%.1f", fps);
		draw_text(91 - text_width(text, 1), 72, text, 1,
			  fps > 0.0 ? cyan : gray);
	} else {
		snprintf(text, sizeof(text), "%u/%uM", ram_used, ram_total);
		x = (OLED_WIDTH - text_width(text, 1)) / 2;
		draw_text(x, 52, text, 1, gray);
		draw_text(5, 67, "FPS", 1, gray);
		snprintf(text, sizeof(text), fps >= 99.95 ? "100" : "%.1f", fps);
		x = 91 - text_width(text, 2);
		draw_text(x, 63, text, 2, fps > 0.0 ? cyan : gray);
	}

	fill_rect(4, 82, 88, 1, rgb565(18, 61, 70));
	x = (OLED_WIDTH - text_width(mode, 1)) / 2;
	draw_text(x, 87, mode, 1, usage_color(cpu));
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

	open_gpio();
	open_spi();
	config = read_display_config();
	if (config.enabled) {
		oled_initialize(contrast_for_brightness(config.brightness));
		oled_active = true;
		applied_brightness = config.brightness;
		read_cpu_snapshot(&previous);
	} else {
		gpio_write(GPIO_RW, 0);
		gpio_write(GPIO_EN, 0);
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
		int sleep_result;
		struct display_config new_config;
		long update_nanoseconds = 1000000000L / (long)config.refresh_hz;

		next_update.tv_nsec += update_nanoseconds;
		next_update.tv_sec += next_update.tv_nsec / 1000000000L;
		next_update.tv_nsec %= 1000000000L;
		do {
			sleep_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
						       &next_update, NULL);
		} while (sleep_result == EINTR && keep_running);
		if (!keep_running)
			break;
		new_config = read_display_config();
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
				oled_command(0xae, NULL, 0);
				gpio_write(GPIO_EN, 0);
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
			command_one(0xc7, contrast_for_brightness(config.brightness));
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
		oled_command(0xae, NULL, 0);
	free_gif_animation(&animation);
	gpio_write(GPIO_EN, 0);
	close(spi_fd);
	close(gpio_fd);
	return EXIT_SUCCESS;
}
