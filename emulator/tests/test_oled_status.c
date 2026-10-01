/* Host test for the colour (framebuffer) backend of gamepup-oled-status. */
#define main oled_main
#include "../gamepup-oled-status.c"
#undef main

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); ++fails; } } while (0)

static uint32_t hw_pixel(unsigned x, unsigned y)
{
	uint32_t value = 0;

	if (fb_hw_bpp == 32) {
		memcpy(&value, fb_present + (size_t)y * fb_hw_stride + x * 4, 4);
	} else {
		uint16_t v;
		memcpy(&v, fb_present + (size_t)y * fb_hw_stride + x * 2, 2);
		value = ((uint32_t)(v >> 11) << 19) | ((uint32_t)((v >> 5) & 0x3f) << 10) |
			((uint32_t)(v & 0x1f) << 3);
	}
	return value;
}

static void open_fake(const char *path, unsigned bpp)
{
	fb_hw_width = COLOR_WIDTH; fb_hw_height = COLOR_HEIGHT; fb_hw_bpp = bpp;
	fb_hw_stride = COLOR_WIDTH * bpp / 8;
	fb_present_size = (size_t)fb_hw_stride * fb_hw_height;
	free(fb_present); free(fb_prev);
	fb_present = calloc(1, fb_present_size); fb_prev = calloc(1, fb_present_size);
	fb_prev_valid = false; fb_dim = 256; backend = BACKEND_FB;
	fb_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fb_fd < 0 || ftruncate(fb_fd, (off_t)fb_present_size) != 0) { perror(path); exit(2); }
}

static unsigned count_color(uint32_t color)
{
	unsigned n = 0;
	for (unsigned i = 0; i < COLOR_WIDTH * COLOR_HEIGHT; ++i) n += color_canvas[i] == color;
	return n;
}

int main(int argc, char **argv)
{
	const char *path = "/tmp/gamepup-oled-test.bin";
	const char *gif = argc > 1 ? argv[1] : NULL;
	unsigned cores[4] = {10, 55, 90, 3};

	for (int bpp = 16; bpp <= 32; bpp += 16) {
		open_fake(path, (unsigned)bpp);

		render_status_color(42, 70, 700, 1000, 30, true, "NES", 59.9, 1.4,
				    true, false, true, cores, 4);
		CHECK(count_color(COL_GREEN) > 100, "%dbpp header/bars should use green", bpp);
		CHECK(count_color(COL_YELLOW) > 20, "%dbpp RAM 70%% bar / FPS should be yellow", bpp);
		color_present();
		/* the RAM bar (y 72..79) is 70% filled: yellow at x=8, empty (bar bg) at the right end */
		CHECK(hw_pixel(10, 75) != 0, "%dbpp RAM bar start not drawn", bpp);

		/* an identical frame writes nothing */
		{
			uint8_t sentinel[64];
			memset(sentinel, 0xEE, sizeof(sentinel));
			CHECK(pwrite(fb_fd, sentinel, sizeof(sentinel), 0) == (ssize_t)sizeof(sentinel), "sentinel");
			color_present();
			uint8_t back[64];
			CHECK(pread(fb_fd, back, sizeof(back), 0) == (ssize_t)sizeof(back), "pread");
			CHECK(memcmp(back, sentinel, sizeof(back)) == 0, "%dbpp identical frame was rewritten", bpp);
		}

		/* a change in the footer rows only rewrites rows from there down */
		render_status_color(42, 70, 700, 1000, 30, true, "N64", 59.9, 1.4,
				    true, false, true, cores, 4);
		{
			uint8_t sentinel[256];
			memset(sentinel, 0xEE, sizeof(sentinel));
			CHECK(pwrite(fb_fd, sentinel, sizeof(sentinel), 0) == (ssize_t)sizeof(sentinel), "sentinel2");
			color_present();
			uint8_t back[256];
			CHECK(pread(fb_fd, back, sizeof(back), 0) == (ssize_t)sizeof(back), "pread2");
			CHECK(memcmp(back, sentinel, sizeof(back)) == 0, "%dbpp unchanged top rows were rewritten", bpp);
		}

		/* software dimming: half brightness halves white */
		fb_dim = 128;
		cfill_rect(0, 0, COLOR_WIDTH, COLOR_HEIGHT, 0x00ffffff);
		fb_prev_valid = false;
		color_present();
		{
			uint32_t px = hw_pixel(5, 5);
			unsigned red = (px >> 16) & 0xff;
			CHECK(red > 100 && red < 150, "%dbpp dim 50%% white red=%u", bpp, red);
		}
		close(fb_fd);
	}

	/* every layout variant fits the canvas (no crash) */
	render_status_color(100, 100, 1000, 1000, 100, false, "DOOM", 999.9, 1.4, false, true, false, cores, 4);
	render_gif_error_color();

	if (gif) {
		struct gif_animation animation = {0};
		CHECK(load_gif_animation(gif, &animation, true), "colour GIF load");
		CHECK(animation.frame_count >= 2 && animation.frame_bytes == COLOR_WIDTH * COLOR_HEIGHT * 4,
		      "colour GIF frames: %zu x %zu", animation.frame_count, animation.frame_bytes);
		memcpy(color_canvas, animation.frames, sizeof(color_canvas));
		unsigned colored = 0;
		for (unsigned i = 0; i < COLOR_WIDTH * COLOR_HEIGHT; ++i) {
			uint32_t c = color_canvas[i];
			colored += ((c >> 16) & 0xff) != (c & 0xff);       /* r != b: really colour */
		}
		CHECK(colored > 500, "GIF frame kept its colours (%u coloured px)", colored);
		/* centred: 96x96 GIF scaled to 135x135, so the side margins are empty */
		CHECK(color_canvas[60 * COLOR_WIDTH + 2] == 0, "GIF should be centred with empty margins");
		free_gif_animation(&animation);
		CHECK(load_gif_animation(gif, &animation, false) && animation.frame_bytes == OLED_WIDTH * OLED_HEIGHT,
		      "mono GIF load still works");
		free_gif_animation(&animation);
		if (getenv("OLED_TEST_DUMP")) {            /* dump frames for eyeballing */
			open_fake(path, 32);
			render_status_color(42, 70, 700, 1000, 30, true, "NES", 59.9, 1.4, true, false, true, cores, 4);
			color_present();
			FILE *f = fopen(getenv("OLED_TEST_DUMP"), "wb"); fwrite(fb_present, 1, fb_present_size, f); fclose(f);
		}
	}
	unlink(path);
	printf(fails ? "oled-status tests: %d FAILED\n" : "oled-status tests: all passed\n", fails);
	return fails != 0;
}
