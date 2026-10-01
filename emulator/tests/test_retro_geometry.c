#define main retro_main
#include "../gamepup-retro.c"
#undef main
#include <assert.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); ++fails; } } while (0)

static void expect(unsigned fw, unsigned fh, unsigned w, unsigned h, bool doom,
		   unsigned ew, unsigned eh, const char *what)
{
	unsigned dw, dh;
	fb_width = fw; fb_height = fh;
	fit_game_size(w, h, doom, &dw, &dh);
	CHECK(dw == ew && dh == eh, "%ux%u %s: got %ux%u expected %ux%u", fw, fh, what, dw, dh, ew, eh);
}

static void setup(unsigned w, unsigned h, unsigned bpp, const char *path)
{
	fb_width = w; fb_height = h; fb_stride = w * 4;
	hw_width = w; hw_height = h; hw_bpp = bpp; hw_stride = w * bpp / 8;
	free(fb_frame); free(fb_present); free(fb_prev);
	fb_frame_size = (size_t)fb_stride * fb_height; fb_frame = calloc(1, fb_frame_size);
	fb_present_size = (size_t)hw_stride * hw_height; fb_present = calloc(1, fb_present_size);
	fb_prev = calloc(1, fb_present_size); fb_prev_valid = false;
	fb_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	assert(fb_fd >= 0 && ftruncate(fb_fd, (off_t)fb_present_size) == 0);
}

int main(void)
{
	/* ---- game size: 320x240 must match the old behaviour ---- */
	expect(320, 240, 256, 240, false, 256, 240, "NES");
	expect(320, 240, 160, 144, false, 266, 240, "GBC");
	expect(320, 240, 320, 200, true, 320, 240, "Doom");
	expect(320, 240, 320, 240, false, 320, 240, "N64");
	/* ---- 480x272 ---- */
	expect(480, 272, 256, 240, false, 290, 272, "NES");
	expect(480, 272, 160, 144, false, 302, 272, "GBC");
	expect(480, 272, 320, 200, true, 362, 272, "Doom 4:3");
	expect(480, 272, 320, 240, false, 362, 272, "N64 4:3");
	expect(480, 272, 480, 272, false, 480, 272, "native");
	expect(480, 272, 640, 240, false, 480, 180, "wide");

	/* ---- dirty-row writes (RGB565 480x272) ---- */
	const char *path = "/tmp/gamepup-fb-test.bin";
	for (int bpp = 16; bpp <= 32; bpp += 16) {
		setup(480, 272, (unsigned)bpp, path);
		uint8_t *check = malloc(fb_present_size);
		uint32_t *px = (uint32_t *)fb_frame;
		for (unsigned i = 0; i < 480 * 272; ++i) px[i] = 0x00112233 + i;

		write_framebuffer();                       /* first frame: everything */
		assert(pread(fb_fd, check, fb_present_size, 0) == (ssize_t)fb_present_size);
		CHECK(memcmp(check, fb_present, fb_present_size) == 0, "%dbpp first frame not fully written", bpp);

		memset(check, 0xEE, fb_present_size);       /* sentinel */
		assert(pwrite(fb_fd, check, fb_present_size, 0) == (ssize_t)fb_present_size);
		write_framebuffer();                       /* identical: must not touch the file */
		assert(pread(fb_fd, check, fb_present_size, 0) == (ssize_t)fb_present_size);
		int untouched = 1;
		for (size_t i = 0; i < fb_present_size; ++i) if (check[i] != 0xEE) { untouched = 0; break; }
		CHECK(untouched, "%dbpp identical frame was written", bpp);

		px[100 * 480 + 5] = 0x00FFFFFF; px[150 * 480 + 7] = 0x00ABCDEF;   /* rows 100 and 150 change */
		write_framebuffer();
		assert(pread(fb_fd, check, fb_present_size, 0) == (ssize_t)fb_present_size);
		for (unsigned y = 0; y < 272; ++y) {
			const uint8_t *row = check + (size_t)y * hw_stride;
			int is_sentinel = 1, matches = memcmp(row, fb_present + (size_t)y * hw_stride, hw_stride) == 0;
			for (unsigned i = 0; i < hw_stride; ++i) if (row[i] != 0xEE) { is_sentinel = 0; break; }
			if (y >= 100 && y <= 150) CHECK(matches, "%dbpp row %u in dirty range not written", bpp, y);
			else CHECK(is_sentinel, "%dbpp row %u outside dirty range was written", bpp, y);
		}
		free(check); close(fb_fd);
	}
	unlink(path);
	printf(fails ? "retro tests: %d FAILED\n" : "retro tests: all passed\n", fails);
	return fails != 0;
}
