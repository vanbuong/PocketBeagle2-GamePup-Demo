/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Hardware-only PowerVR benchmark for PocketBeagle 2 + GamePup A4.
 *
 * The AM625 GPU has no display connector of its own on this setup, so the
 * benchmark renders an off-screen OpenGL ES surface with the TI PowerVR stack,
 * reads it back, and presents it through the GamePup SPI framebuffer.
 */

#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
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

#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define RENDER_SCALE 1
#define RENDER_WIDTH (LCD_WIDTH * RENDER_SCALE)
#define RENDER_HEIGHT (LCD_HEIGHT * RENDER_SCALE)
#define FRAMEBUFFER "/dev/fb0"
#define INPUT_DEVICE "/dev/input/by-path/platform-gamepup-buttons-event"
#define INPUT_FALLBACK "/dev/input/event0"
#define FPS_FILE "/run/gamepup/fps"
#define EXIT_HOLD_SECONDS 0.8
#define FILL_LAYERS 48
#define TRIANGLE_COUNT 2048
#define GEAR_SEGMENTS 80

static volatile sig_atomic_t keep_running = 1;
static unsigned fb_width;
static unsigned fb_height;
static unsigned fb_bpp;
static unsigned fb_stride;
static size_t fb_frame_size;
static uint8_t *fb_frame;

enum benchmark_kind {
	BENCHMARK_PLASMA,
	BENCHMARK_FILL,
	BENCHMARK_TRIANGLES,
	BENCHMARK_GEARS,
};

struct benchmark_mode {
	const char *argument;
	const char *label;
	const char *telemetry_label;
	enum benchmark_kind kind;
	const char *vertex_shader;
	const char *fragment_shader;
};

struct triangle_vertex {
	GLfloat x;
	GLfloat y;
	GLfloat red;
	GLfloat green;
	GLfloat blue;
};

struct gear_vertex {
	GLfloat x;
	GLfloat y;
	GLfloat z;
	GLfloat normal_x;
	GLfloat normal_y;
	GLfloat normal_z;
};

static const uint8_t font[42][7] = {
	/* space, A-Z, 0-9, -, ., :, %, + */
	{0, 0, 0, 0, 0, 0, 0},
	{14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
	{15, 16, 16, 16, 16, 16, 15}, {30, 17, 17, 17, 17, 17, 30},
	{31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
	{15, 16, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
	{31, 4, 4, 4, 4, 4, 31}, {7, 2, 2, 2, 18, 18, 12},
	{17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
	{17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
	{14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
	{14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
	{15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
	{17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
	{17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
	{17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31},
	{14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
	{14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
	{2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30},
	{14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
	{14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
	{0, 0, 0, 31, 0, 0, 0}, {0, 0, 0, 0, 0, 12, 12},
	{0, 12, 12, 0, 12, 12, 0}, {25, 26, 4, 8, 22, 6, 0},
	{0, 4, 4, 31, 4, 4, 0},
};

static const char quad_vertex_shader[] =
	"attribute vec2 position;\n"
	"void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

static const char plasma_fragment_shader[] =
	"precision mediump float;\n"
	"uniform float time;\n"
	"uniform vec2 resolution;\n"
	"void main() {\n"
	"  vec2 p = (2.0 * gl_FragCoord.xy - resolution) / resolution.y;\n"
	"  float a = 0.0;\n"
	"  vec2 q = p;\n"
	"  for (int i = 0; i < 8; ++i) {\n"
	"    float f = float(i);\n"
	"    a += sin(q.x * (5.0 + f * 0.17) + time * (1.1 + f * 0.013)\n"
	"             + sin(q.y * (4.0 + f * 0.11) - time)) * 0.045;\n"
	"    q = mat2(0.984, -0.178, 0.178, 0.984) * q * 1.018;\n"
	"  }\n"
	"  float rings = sin(18.0 * length(p) - 4.0 * time + 5.0 * a);\n"
	"  vec3 color = 0.52 + 0.48 * cos(vec3(0.0, 2.1, 4.2)\n"
	"               + a * 5.0 + rings + time * vec3(0.7, 0.9, 1.1));\n"
	"  float grid = smoothstep(0.93, 1.0, abs(sin(p.x * 22.0 + time)))\n"
	"             + smoothstep(0.94, 1.0, abs(sin(p.y * 22.0 - time)));\n"
	"  gl_FragColor = vec4(mix(color, vec3(0.9), min(grid, 1.0) * 0.25), 1.0);\n"
	"}\n";

static const char fill_fragment_shader[] =
	"precision mediump float;\n"
	"uniform float time;\n"
	"uniform float layer;\n"
	"uniform vec2 resolution;\n"
	"void main() {\n"
	"  vec2 uv = gl_FragCoord.xy / resolution;\n"
	"  float phase = layer * 0.37 + time;\n"
	"  float wave = 0.5 + 0.5 * sin(uv.x * 19.0 + uv.y * 13.0 + phase);\n"
	"  vec3 color = 0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2)\n"
	"               + phase + wave * 2.4);\n"
	"  gl_FragColor = vec4(color / 48.0, 1.0);\n"
	"}\n";

static const char triangle_vertex_shader[] =
	"attribute vec2 position;\n"
	"attribute vec3 color;\n"
	"uniform float time;\n"
	"varying vec3 vertex_color;\n"
	"void main() {\n"
	"  float angle = time * 0.31;\n"
	"  mat2 spin = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));\n"
	"  vec2 wobble = vec2(sin(position.y * 17.0 + time * 1.7),\n"
	"                     cos(position.x * 19.0 - time * 1.3)) * 0.012;\n"
	"  gl_Position = vec4(spin * position + wobble, 0.0, 1.0);\n"
	"  vertex_color = color;\n"
	"}\n";

static const char triangle_fragment_shader[] =
	"precision mediump float;\n"
	"varying vec3 vertex_color;\n"
	"void main() { gl_FragColor = vec4(vertex_color, 1.0); }\n";

static const char gears_vertex_shader[] =
	"attribute vec3 position;\n"
	"attribute vec3 normal;\n"
	"uniform vec2 offset;\n"
	"uniform float angle;\n"
	"uniform float scale;\n"
	"uniform vec3 gear_color;\n"
	"varying vec3 shaded_color;\n"
	"void main() {\n"
	"  mat2 spin = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));\n"
	"  vec3 p = vec3(spin * position.xy, position.z);\n"
	"  vec3 n = vec3(spin * normal.xy, normal.z);\n"
	"  p = vec3(p.x, p.y * 0.82 - p.z * 0.57, p.y * 0.57 + p.z * 0.82);\n"
	"  n = normalize(vec3(n.x, n.y * 0.82 - n.z * 0.57,\n"
	"                     n.y * 0.57 + n.z * 0.82));\n"
	"  p.xy = p.xy * scale + offset;\n"
	"  float light = 0.24 + 0.76 * max(dot(n, normalize(vec3(-0.4, 0.6, 0.8))), 0.0);\n"
	"  shaded_color = gear_color * light;\n"
	"  gl_Position = vec4(p.x * 1.25, p.y, p.z * 0.7, 1.0);\n"
	"}\n";

static const char gears_fragment_shader[] =
	"precision mediump float;\n"
	"varying vec3 shaded_color;\n"
	"void main() { gl_FragColor = vec4(shaded_color, 1.0); }\n";

static const struct benchmark_mode benchmark_modes[] = {
	{"plasma", "PLASMA", "PLASMA", BENCHMARK_PLASMA,
	 quad_vertex_shader, plasma_fragment_shader},
	{"fill", "FILL RATE", "FILL", BENCHMARK_FILL,
	 quad_vertex_shader, fill_fragment_shader},
	{"triangles", "TRIANGLES", "TRIS", BENCHMARK_TRIANGLES,
	 triangle_vertex_shader, triangle_fragment_shader},
	{"gears", "GL GEARS", "GEARS", BENCHMARK_GEARS,
	 gears_vertex_shader, gears_fragment_shader},
};

static void stop_handler(int signal_number)
{
	(void)signal_number;
	keep_running = 0;
}

static double monotonic_seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static const uint8_t *glyph_for(char character)
{
	if (character >= 'a' && character <= 'z')
		character = (char)(character - 'a' + 'A');
	if (character == ' ')
		return font[0];
	if (character >= 'A' && character <= 'Z')
		return font[1 + character - 'A'];
	if (character >= '0' && character <= '9')
		return font[27 + character - '0'];
	if (character == '-')
		return font[37];
	if (character == '.')
		return font[38];
	if (character == ':')
		return font[39];
	if (character == '%')
		return font[40];
	if (character == '+')
		return font[41];
	return font[0];
}

static void fill_rect(uint32_t *pixels, int x, int y, int width, int height,
		      uint32_t color)
{
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + width > LCD_WIDTH ? LCD_WIDTH : x + width;
	int y1 = y + height > LCD_HEIGHT ? LCD_HEIGHT : y + height;

	for (int row = y0; row < y1; ++row)
		for (int column = x0; column < x1; ++column)
			pixels[row * LCD_WIDTH + column] = color;
}

static int text_width(const char *text, int scale)
{
	return *text ? (int)strlen(text) * 6 * scale - scale : 0;
}

static void draw_text(uint32_t *pixels, int x, int y, const char *text,
		      int scale, uint32_t color)
{
	for (; *text; ++text, x += 6 * scale) {
		const uint8_t *glyph = glyph_for(*text);

		for (int row = 0; row < 7; ++row)
			for (int column = 0; column < 5; ++column)
				if (glyph[row] & (1u << (4 - column)))
					fill_rect(pixels, x + column * scale,
						  y + row * scale, scale, scale, color);
	}
}

static bool open_framebuffer(int *framebuffer_fd)
{
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	int fd = open(FRAMEBUFFER, O_RDWR | O_CLOEXEC);

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &variable) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fixed) < 0) {
		perror(FRAMEBUFFER);
		if (fd >= 0)
			close(fd);
		return false;
	}
	fb_width = variable.xres;
	fb_height = variable.yres;
	fb_bpp = variable.bits_per_pixel;
	fb_stride = fixed.line_length;
	if (fb_bpp != 16 && fb_bpp != 32) {
		fprintf(stderr, "Expected a 16- or 32-bit framebuffer, got %u bpp\n",
			fb_bpp);
		close(fd);
		return false;
	}
	if (!((fb_width == LCD_WIDTH && fb_height == LCD_HEIGHT) ||
	      (fb_width == LCD_HEIGHT && fb_height == LCD_WIDTH))) {
		fprintf(stderr,
			"Unsupported framebuffer %ux%u (need 320x240 or 240x320)\n",
			fb_width, fb_height);
		close(fd);
		return false;
	}
	fb_frame_size = (size_t)fb_stride * fb_height;
	fb_frame = calloc(1, fb_frame_size);
	if (!fb_frame) {
		perror("calloc framebuffer");
		close(fd);
		return false;
	}
	fprintf(stderr, "GamePup framebuffer %ux%u stride=%u bpp=%u\n",
		fb_width, fb_height, fb_stride, fb_bpp);
	*framebuffer_fd = fd;
	return true;
}

/* Present a logical 320x240 XRGB canvas into the real /dev/fb0 geometry. */
static void present_canvas(int framebuffer_fd, const uint32_t *canvas)
{
	size_t written = 0;

	if (fb_width == LCD_WIDTH && fb_height == LCD_HEIGHT && fb_bpp == 32) {
		for (unsigned y = 0; y < fb_height; ++y) {
			uint32_t *row = (uint32_t *)(fb_frame + y * fb_stride);

			memcpy(row, canvas + y * LCD_WIDTH,
			       (size_t)LCD_WIDTH * sizeof(*canvas));
		}
	} else if (fb_width == LCD_WIDTH && fb_height == LCD_HEIGHT && fb_bpp == 16) {
		for (unsigned y = 0; y < fb_height; ++y) {
			uint16_t *row = (uint16_t *)(fb_frame + y * fb_stride);

			for (int x = 0; x < LCD_WIDTH; ++x) {
				uint32_t color = canvas[y * LCD_WIDTH + x];
				unsigned red = (color >> 16) & 0xff;
				unsigned green = (color >> 8) & 0xff;
				unsigned blue = color & 0xff;

				row[x] = (uint16_t)(((red >> 3) << 11) |
						    ((green >> 2) << 5) |
						    (blue >> 3));
			}
		}
	} else {
		/* Portrait FB 240x320: rotate landscape canvas 90° CCW. */
		for (int y = 0; y < LCD_HEIGHT; ++y) {
			for (int x = 0; x < LCD_WIDTH; ++x) {
				int dst_x = y;
				int dst_y = LCD_WIDTH - 1 - x;
				uint32_t color = canvas[y * LCD_WIDTH + x];

				if (fb_bpp == 16) {
					uint16_t *row = (uint16_t *)(fb_frame +
								     (size_t)dst_y * fb_stride);
					unsigned red = (color >> 16) & 0xff;
					unsigned green = (color >> 8) & 0xff;
					unsigned blue = color & 0xff;

					row[dst_x] = (uint16_t)(((red >> 3) << 11) |
								((green >> 2) << 5) |
								(blue >> 3));
				} else {
					uint32_t *row = (uint32_t *)(fb_frame +
								     (size_t)dst_y * fb_stride);

					row[dst_x] = color;
				}
			}
		}
	}

	while (written < fb_frame_size) {
		ssize_t result = pwrite(framebuffer_fd, fb_frame + written,
					fb_frame_size - written, (off_t)written);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			perror("pwrite framebuffer");
			keep_running = 0;
			return;
		}
		written += (size_t)result;
	}
}

static GLuint compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint compiled = GL_FALSE;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		char log[2048];
		GLsizei length = 0;

		glGetShaderInfoLog(shader, sizeof(log), &length, log);
		fprintf(stderr, "GPU benchmark shader error: %.*s\n", length, log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint create_program(const char *vertex_source, const char *fragment_source)
{
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	GLuint program;
	GLint linked = GL_FALSE;

	if (!vertex || !fragment)
		return 0;
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "color");
	glBindAttribLocation(program, 1, "normal");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (!linked) {
		char log[2048];
		GLsizei length = 0;

		glGetProgramInfoLog(program, sizeof(log), &length, log);
		fprintf(stderr, "GPU benchmark link error: %.*s\n", length, log);
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

static EGLDisplay open_egl_display(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	EGLDisplay display = EGL_NO_DISPLAY;

	get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (get_platform_display)
		display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
					       EGL_DEFAULT_DISPLAY, NULL);
	if (display == EGL_NO_DISPLAY)
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	return display;
}

static bool renderer_is_hardware(const char *renderer)
{
	if (!renderer || strcasestr(renderer, "llvmpipe") ||
	    strcasestr(renderer, "softpipe"))
		return false;
	return strcasestr(renderer, "powervr") || strcasestr(renderer, "pvr") ||
	       strcasestr(renderer, "axe") || strcasestr(renderer, "imagination");
}

static const struct benchmark_mode *select_benchmark(int argc, char **argv)
{
	const char *requested = argc > 1 ? argv[1] : "plasma";

	if (argc > 2)
		return NULL;
	for (size_t index = 0;
	     index < sizeof(benchmark_modes) / sizeof(benchmark_modes[0]); ++index)
		if (strcmp(requested, benchmark_modes[index].argument) == 0)
			return &benchmark_modes[index];
	return NULL;
}

static float random_unit(uint32_t *state)
{
	*state = *state * 1664525u + 1013904223u;
	return (float)(*state >> 8) / 16777215.0f;
}

static struct triangle_vertex *create_triangle_vertices(void)
{
	struct triangle_vertex *vertices =
		malloc((size_t)TRIANGLE_COUNT * 3 * sizeof(*vertices));
	uint32_t random_state = 0x625a4u;

	if (!vertices)
		return NULL;
	for (int triangle = 0; triangle < TRIANGLE_COUNT; ++triangle) {
		float center_x = random_unit(&random_state) * 1.84f - 0.92f;
		float center_y = random_unit(&random_state) * 1.84f - 0.92f;
		float radius = 0.006f + random_unit(&random_state) * 0.025f;
		float red = 0.2f + random_unit(&random_state) * 0.8f;
		float green = 0.2f + random_unit(&random_state) * 0.8f;
		float blue = 0.2f + random_unit(&random_state) * 0.8f;
		struct triangle_vertex *vertex = &vertices[triangle * 3];

		vertex[0] = (struct triangle_vertex){center_x, center_y + radius,
						     red, green, blue};
		vertex[1] = (struct triangle_vertex){center_x - radius, center_y - radius,
						     red, green, blue};
		vertex[2] = (struct triangle_vertex){center_x + radius, center_y - radius,
						     red, green, blue};
	}
	return vertices;
}

static void append_gear_vertex(struct gear_vertex *vertices, size_t *count,
			       float x, float y, float z,
			       float normal_x, float normal_y, float normal_z)
{
	vertices[(*count)++] = (struct gear_vertex){
		x, y, z, normal_x, normal_y, normal_z,
	};
}

static void append_gear_quad(struct gear_vertex *vertices, size_t *count,
			     const float a[3], const float b[3],
			     const float c[3], const float d[3],
			     float normal_x, float normal_y, float normal_z)
{
	append_gear_vertex(vertices, count, a[0], a[1], a[2],
			   normal_x, normal_y, normal_z);
	append_gear_vertex(vertices, count, b[0], b[1], b[2],
			   normal_x, normal_y, normal_z);
	append_gear_vertex(vertices, count, c[0], c[1], c[2],
			   normal_x, normal_y, normal_z);
	append_gear_vertex(vertices, count, a[0], a[1], a[2],
			   normal_x, normal_y, normal_z);
	append_gear_vertex(vertices, count, c[0], c[1], c[2],
			   normal_x, normal_y, normal_z);
	append_gear_vertex(vertices, count, d[0], d[1], d[2],
			   normal_x, normal_y, normal_z);
}

static struct gear_vertex *create_gear_vertices(size_t *vertex_count)
{
	const float tau = 6.28318530718f;
	const float inner_radius = 0.32f;
	const float half_depth = 0.13f;
	struct gear_vertex *vertices =
		malloc((size_t)GEAR_SEGMENTS * 4 * 6 * sizeof(*vertices));
	size_t count = 0;

	if (!vertices)
		return NULL;
	for (int segment = 0; segment < GEAR_SEGMENTS; ++segment) {
		int next_segment = (segment + 1) % GEAR_SEGMENTS;
		float angle0 = tau * segment / GEAR_SEGMENTS;
		float angle1 = tau * next_segment / GEAR_SEGMENTS;
		int phase0 = segment % 4;
		int phase1 = next_segment % 4;
		float radius0 = phase0 == 1 || phase0 == 2 ? 1.0f : 0.78f;
		float radius1 = phase1 == 1 || phase1 == 2 ? 1.0f : 0.78f;
		float outer0_x = cosf(angle0) * radius0;
		float outer0_y = sinf(angle0) * radius0;
		float outer1_x = cosf(angle1) * radius1;
		float outer1_y = sinf(angle1) * radius1;
		float inner0_x = cosf(angle0) * inner_radius;
		float inner0_y = sinf(angle0) * inner_radius;
		float inner1_x = cosf(angle1) * inner_radius;
		float inner1_y = sinf(angle1) * inner_radius;
		float middle = (angle0 + angle1) * 0.5f;
		float normal_x = cosf(middle);
		float normal_y = sinf(middle);

		const float inner0_front[] = {inner0_x, inner0_y, half_depth};
		const float inner1_front[] = {inner1_x, inner1_y, half_depth};
		const float outer0_front[] = {outer0_x, outer0_y, half_depth};
		const float outer1_front[] = {outer1_x, outer1_y, half_depth};
		const float inner0_back[] = {inner0_x, inner0_y, -half_depth};
		const float inner1_back[] = {inner1_x, inner1_y, -half_depth};
		const float outer0_back[] = {outer0_x, outer0_y, -half_depth};
		const float outer1_back[] = {outer1_x, outer1_y, -half_depth};

		append_gear_quad(vertices, &count, inner0_front, outer0_front,
				 outer1_front, inner1_front, 0.0f, 0.0f, 1.0f);
		append_gear_quad(vertices, &count, inner1_back, outer1_back,
				 outer0_back, inner0_back, 0.0f, 0.0f, -1.0f);
		append_gear_quad(vertices, &count, outer0_front, outer0_back,
				 outer1_back, outer1_front, normal_x, normal_y, 0.0f);
		append_gear_quad(vertices, &count, inner1_front, inner1_back,
				 inner0_back, inner0_front, -normal_x, -normal_y, 0.0f);
	}
	*vertex_count = count;
	return vertices;
}

static void write_fps(const char *mode, double fps)
{
	char temporary[] = FPS_FILE ".XXXXXX";
	int descriptor = mkstemp(temporary);
	char text[64];
	int length;

	if (descriptor < 0)
		return;
	length = snprintf(text, sizeof(text), "%s %.1f\n", mode, fps);
	if (write(descriptor, text, (size_t)length) != length) {
		close(descriptor);
		unlink(temporary);
		return;
	}
	close(descriptor);
	rename(temporary, FPS_FILE);
}

static bool exit_buttons_held(int input_fd)
{
	unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
			       (8 * sizeof(unsigned long))] = {0};

	if (ioctl(input_fd, EVIOCGKEY(sizeof(key_bits)), key_bits) < 0)
		return false;
	return (key_bits[KEY_1 / (8 * sizeof(unsigned long))] &
		(1UL << (KEY_1 % (8 * sizeof(unsigned long))))) &&
	       (key_bits[KEY_5 / (8 * sizeof(unsigned long))] &
		(1UL << (KEY_5 % (8 * sizeof(unsigned long)))));
}

static void show_error(int framebuffer, const char *message)
{
	uint32_t pixels[LCD_WIDTH * LCD_HEIGHT] = {0};
	const uint32_t red = 0x00e85d5d;
	const uint32_t white = 0x00f4f4e8;

	draw_text(pixels, 7, 40, "GPU ERROR", 2, red);
	draw_text(pixels, 4, 72, message, 1, white);
	draw_text(pixels, 7, 142, "RETURNING TO MENU", 1, white);
	if (fb_frame)
		present_canvas(framebuffer, pixels);
	sleep(3);
}

int main(int argc, char **argv)
{
	static const GLfloat quad_vertices[] = {-1.0f, -1.0f, 1.0f, -1.0f,
						 -1.0f, 1.0f, 1.0f, 1.0f};
	EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE,
	};
	EGLint surface_attributes[] = {
		EGL_WIDTH, RENDER_WIDTH, EGL_HEIGHT, RENDER_HEIGHT, EGL_NONE,
	};
	EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig config;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLContext context = EGL_NO_CONTEXT;
	EGLint config_count = 0;
	GLuint program = 0;
	GLint time_location;
	GLint resolution_location;
	GLint layer_location;
	GLint offset_location;
	GLint angle_location;
	GLint scale_location;
	GLint color_location;
	uint8_t *rendered_pixels = NULL;
	uint32_t *lcd_pixels = NULL;
	struct triangle_vertex *triangle_vertices = NULL;
	struct gear_vertex *gear_vertices = NULL;
	size_t gear_vertex_count = 0;
	int framebuffer = -1;
	int input_fd = -1;
	double started;
	double sample_started;
	double displayed_fps = 0.0;
	double exit_started = 0.0;
	unsigned frames = 0;
	int result = EXIT_FAILURE;
	const char *renderer;
	const struct benchmark_mode *mode = select_benchmark(argc, argv);

	if (!mode) {
		fprintf(stderr, "Usage: %s [plasma|fill|triangles|gears]\n", argv[0]);
		return EXIT_FAILURE;
	}

	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	if (!open_framebuffer(&framebuffer))
		goto cleanup;
	input_fd = open(INPUT_DEVICE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (input_fd < 0)
		input_fd = open(INPUT_FALLBACK, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (input_fd < 0) {
		perror("open GamePup buttons");
		goto cleanup;
	}
	if (ioctl(input_fd, EVIOCGRAB, 1) < 0)
		perror("EVIOCGRAB buttons");
	display = open_egl_display();
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL) ||
	    !eglBindAPI(EGL_OPENGL_ES_API) ||
	    !eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
	    config_count != 1) {
		fprintf(stderr, "Unable to initialize TI PowerVR EGL (0x%x).\n", eglGetError());
		show_error(framebuffer, "TI EGL NOT READY");
		goto cleanup;
	}
	surface = eglCreatePbufferSurface(display, config, surface_attributes);
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(display, surface, surface, context)) {
		fprintf(stderr, "Unable to create PowerVR surface (0x%x).\n", eglGetError());
		show_error(framebuffer, "GPU CONTEXT FAILED");
		goto cleanup;
	}
	renderer = (const char *)glGetString(GL_RENDERER);
	fprintf(stderr, "GPU benchmark renderer: %s\n", renderer ? renderer : "unknown");
	if (!renderer_is_hardware(renderer)) {
		show_error(framebuffer, "SOFTWARE REJECTED");
		goto cleanup;
	}
	program = create_program(mode->vertex_shader, mode->fragment_shader);
	if (!program) {
		show_error(framebuffer, "SHADER FAILED");
		goto cleanup;
	}
	rendered_pixels = malloc((size_t)RENDER_WIDTH * RENDER_HEIGHT * 4);
	lcd_pixels = malloc((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(*lcd_pixels));
	if (mode->kind == BENCHMARK_TRIANGLES)
		triangle_vertices = create_triangle_vertices();
	if (mode->kind == BENCHMARK_GEARS)
		gear_vertices = create_gear_vertices(&gear_vertex_count);
	if (!rendered_pixels || !lcd_pixels ||
	    (mode->kind == BENCHMARK_TRIANGLES && !triangle_vertices) ||
	    (mode->kind == BENCHMARK_GEARS && !gear_vertices)) {
		show_error(framebuffer, "OUT OF MEMORY");
		goto cleanup;
	}

	glUseProgram(program);
	time_location = glGetUniformLocation(program, "time");
	resolution_location = glGetUniformLocation(program, "resolution");
	layer_location = glGetUniformLocation(program, "layer");
	offset_location = glGetUniformLocation(program, "offset");
	angle_location = glGetUniformLocation(program, "angle");
	scale_location = glGetUniformLocation(program, "scale");
	color_location = glGetUniformLocation(program, "gear_color");
	glUniform2f(resolution_location, RENDER_WIDTH, RENDER_HEIGHT);
	if (mode->kind == BENCHMARK_TRIANGLES) {
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
				      sizeof(*triangle_vertices), &triangle_vertices[0].x);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
				      sizeof(*triangle_vertices), &triangle_vertices[0].red);
		glEnableVertexAttribArray(1);
	} else if (mode->kind == BENCHMARK_GEARS) {
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
				      sizeof(*gear_vertices), &gear_vertices[0].x);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
				      sizeof(*gear_vertices), &gear_vertices[0].normal_x);
		glEnableVertexAttribArray(1);
	} else {
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad_vertices);
	}
	glEnableVertexAttribArray(0);
	glViewport(0, 0, RENDER_WIDTH, RENDER_HEIGHT);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glClearColor(0.01f, 0.02f, 0.04f, 1.0f);
	started = sample_started = monotonic_seconds();

	while (keep_running) {
		double now = monotonic_seconds();
		char stats[32];

		if (exit_buttons_held(input_fd)) {
			if (exit_started == 0.0)
				exit_started = now;
			else if (now - exit_started >= EXIT_HOLD_SECONDS)
				break;
		} else {
			exit_started = 0.0;
		}

		GLfloat elapsed = (GLfloat)(now - started);

		glUniform1f(time_location, elapsed);
		glClear(GL_COLOR_BUFFER_BIT |
			(mode->kind == BENCHMARK_GEARS ? GL_DEPTH_BUFFER_BIT : 0));
		if (mode->kind == BENCHMARK_GEARS) {
			static const GLfloat offsets[][2] = {
				{-0.29f, 0.18f}, {0.38f, 0.12f}, {0.12f, -0.47f},
			};
			static const GLfloat scales[] = {0.36f, 0.27f, 0.24f};
			static const GLfloat colors[][3] = {
				{0.92f, 0.24f, 0.18f},
				{0.18f, 0.72f, 0.94f},
				{0.94f, 0.70f, 0.16f},
			};
			static const GLfloat speeds[] = {0.75f, -1.0f, 1.15f};

			glEnable(GL_DEPTH_TEST);
			for (size_t gear = 0; gear < 3; ++gear) {
				glUniform2f(offset_location, offsets[gear][0], offsets[gear][1]);
				glUniform1f(angle_location, elapsed * speeds[gear] + gear * 0.7f);
				glUniform1f(scale_location, scales[gear]);
				glUniform3f(color_location, colors[gear][0],
					    colors[gear][1], colors[gear][2]);
				glDrawArrays(GL_TRIANGLES, 0, (GLsizei)gear_vertex_count);
			}
			glDisable(GL_DEPTH_TEST);
		} else if (mode->kind == BENCHMARK_FILL) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE);
			for (int layer = 0; layer < FILL_LAYERS; ++layer) {
				glUniform1f(layer_location, (GLfloat)layer);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
			glDisable(GL_BLEND);
		} else if (mode->kind == BENCHMARK_TRIANGLES) {
			glDrawArrays(GL_TRIANGLES, 0, TRIANGLE_COUNT * 3);
		} else {
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glFinish();
		glReadPixels(0, 0, RENDER_WIDTH, RENDER_HEIGHT,
			     GL_RGBA, GL_UNSIGNED_BYTE, rendered_pixels);

		for (int y = 0; y < LCD_HEIGHT; ++y) {
			int source_y = RENDER_HEIGHT - 1 - y * RENDER_SCALE;
			for (int x = 0; x < LCD_WIDTH; ++x) {
				size_t offset = ((size_t)source_y * RENDER_WIDTH +
						 (size_t)x * RENDER_SCALE) * 4;
				lcd_pixels[y * LCD_WIDTH + x] =
					((uint32_t)rendered_pixels[offset] << 16) |
					((uint32_t)rendered_pixels[offset + 1] << 8) |
					rendered_pixels[offset + 2];
			}
		}
		fill_rect(lcd_pixels, 0, 0, LCD_WIDTH, 28, 0x00030a10);
		fill_rect(lcd_pixels, 0, 210, LCD_WIDTH, 30, 0x00030a10);
		draw_text(lcd_pixels,
			  (LCD_WIDTH - text_width("AXE-1-16M GPU", 1)) / 2,
			  4, "AXE-1-16M GPU", 1, 0x0041cedc);
		draw_text(lcd_pixels,
			  (LCD_WIDTH - text_width(mode->label, 1)) / 2,
			  16, mode->label, 1, 0x00f4f4e8);
		snprintf(stats, sizeof(stats), "%.1f FPS", displayed_fps);
		draw_text(lcd_pixels, (LCD_WIDTH - text_width(stats, 1)) / 2,
			  216, stats, 1, 0x00f4d35e);
		draw_text(lcd_pixels,
			  (LCD_WIDTH - text_width("HOLD START+SELECT", 1)) / 2,
			  228, "HOLD START+SELECT", 1, 0x00f4f4e8);
		present_canvas(framebuffer, lcd_pixels);
		if (!keep_running)
			break;

		++frames;
		now = monotonic_seconds();
		if (now - sample_started >= 0.5) {
			displayed_fps = frames / (now - sample_started);
			write_fps(mode->telemetry_label, displayed_fps);
			frames = 0;
			sample_started = now;
		}
	}
	result = EXIT_SUCCESS;

cleanup:
	unlink(FPS_FILE);
	free(gear_vertices);
	free(triangle_vertices);
	free(lcd_pixels);
	free(rendered_pixels);
	free(fb_frame);
	fb_frame = NULL;
	if (program)
		glDeleteProgram(program);
	if (display != EGL_NO_DISPLAY) {
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (context != EGL_NO_CONTEXT)
			eglDestroyContext(display, context);
		if (surface != EGL_NO_SURFACE)
			eglDestroySurface(display, surface);
		eglTerminate(display);
	}
	if (input_fd >= 0)
		close(input_fd);
	if (framebuffer >= 0)
		close(framebuffer);
	return result;
}
