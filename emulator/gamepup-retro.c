/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Minimal libretro frontend for the PocketBeagle 2 + GamePup A4.
 *
 * Video is scaled with bilinear sampling directly into /dev/fb0.
 * Input comes from the GamePup gpio-keys event device. Emulator audio is
 * reduced to an approximate monophonic pitch for the cape's PWM tone buzzer.
 */

#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "libretro.h"

#define DEFAULT_CORE "/usr/local/lib/libretro/gambatte_libretro.so"
#define DEFAULT_INPUT "/dev/input/by-path/platform-gamepup-buttons-event"
#define DEFAULT_FB "/dev/fb0"
#define DEFAULT_BUZZER "/dev/input/by-path/platform-gamepup-buzzer-event"
#define SYSTEM_DIR "/opt/gamepup/system"
#define SAVE_DIR "/opt/gamepup/saves"
#define MUTE_FILE SAVE_DIR "/audio-muted"
#define BEZEL_DISABLED_FILE SAVE_DIR "/game-bezel-disabled"
#define BEZEL_STYLE_FILE SAVE_DIR "/game-bezel-style"
#define BEZEL_ASSET_DIR "/usr/local/share/gamepup/bezels"
#define FPS_FILE "/run/gamepup/fps"
#define HW_SURFACE_WIDTH 320
#define HW_SURFACE_HEIGHT 240
#define MAX_CORE_VARIABLES 256

struct core_api {
	void (*init)(void);
	void (*deinit)(void);
	unsigned (*api_version)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*set_controller_port_device)(unsigned, unsigned);
	void (*reset)(void);
	void (*run)(void);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
};

static struct core_api core;
static void *core_handle;
static int fb_fd = -1;
static int input_fd = -1;
static int buzzer_fd = -1;
/* Logical landscape canvas used for scaling/bezels (always 32-bit XRGB). */
static uint8_t *fb_frame;
static size_t fb_frame_size;
static unsigned fb_width = 320;
static unsigned fb_height = 240;
static unsigned fb_stride = 320 * 4;
/* Physical /dev/fb0 geometry (may be 16 bpp and/or portrait). */
static uint8_t *fb_present;
static size_t fb_present_size;
static unsigned hw_width;
static unsigned hw_height;
static unsigned hw_stride;
static unsigned hw_bpp;
static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static EGLDisplay hw_display = EGL_NO_DISPLAY;
static EGLSurface hw_surface = EGL_NO_SURFACE;
static EGLContext hw_context = EGL_NO_CONTEXT;
static struct retro_hw_render_callback hw_render_callback;
static uint8_t *hw_pixels;
static unsigned hw_surface_width;
static unsigned hw_surface_height;
static bool hw_render_enabled;
struct saved_core_variable {
	char *key;
	char *value;
};
static struct saved_core_variable core_variables[MAX_CORE_VARIABLES];
static size_t core_variable_count;
static bool key_down[KEY_MAX + 1];
static volatile sig_atomic_t keep_running = 1;
static const char *rom_path;
static uint8_t *rom_data;
static size_t rom_size;
static double audio_sample_rate;
static int32_t previous_audio_sample;
static uint64_t audio_amplitude_sum;
static unsigned audio_window_frames;
static unsigned audio_zero_crossings;
static int buzzer_frequency;
static unsigned buzzer_updates;
static struct timespec fps_started;
static unsigned presented_frames;
static bool fps_started_valid;
enum bezel_style {
	BEZEL_OFF,
	BEZEL_GAMEPUP,
	BEZEL_ARCADE,
	BEZEL_SYSTEM,
};

#define BEZEL_WIDTH 320
#define BEZEL_BAND_HEIGHT 20
#define BEZEL_HEIGHT (BEZEL_BAND_HEIGHT * 2)

static enum bezel_style bezel_style = BEZEL_GAMEPUP;
static uint32_t system_bezel[BEZEL_WIDTH * BEZEL_HEIGHT];
static bool system_bezel_loaded;

struct bezel_glyph {
	char character;
	uint8_t columns[5];
};

static const struct bezel_glyph bezel_font[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
	{'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
	{'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
	{'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
	{'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
	{'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
	{'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
	{'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
	{'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
	{'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
	{'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
	{'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
	{'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
};

static const char *emulator_mode(void)
{
	const char *extension = strrchr(rom_path, '.');

	if (extension && strcasecmp(extension, ".nes") == 0)
		return "NES";
	if (extension && (strcasecmp(extension, ".z64") == 0 ||
			  strcasecmp(extension, ".n64") == 0 ||
			  strcasecmp(extension, ".v64") == 0))
		return "N64";
	if (extension && strcasecmp(extension, ".wad") == 0)
		return "DOOM";
	return "GBC";
}

static void publish_fps(double fps)
{
	char temporary_path[] = FPS_FILE ".XXXXXX";
	char value[64];
	int length;
	int fd = mkstemp(temporary_path);

	if (fd < 0)
		return;
	length = snprintf(value, sizeof(value), "%s %.1f\n", emulator_mode(), fps);
	if (length > 0 && write(fd, value, (size_t)length) == length)
		rename(temporary_path, FPS_FILE);
	else
		unlink(temporary_path);
	close(fd);
}

static void count_presented_frame(void)
{
	struct timespec now;
	double elapsed;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!fps_started_valid) {
		fps_started = now;
		fps_started_valid = true;
	}
	++presented_frames;
	elapsed = (double)(now.tv_sec - fps_started.tv_sec) +
		  (double)(now.tv_nsec - fps_started.tv_nsec) / 1000000000.0;
	if (elapsed < 0.25)
		return;
	publish_fps(presented_frames / elapsed);
	fps_started = now;
	presented_frames = 0;
}

static void stop_handler(int signal_number)
{
	(void)signal_number;
	keep_running = 0;
}

static void core_log(enum retro_log_level level, const char *format, ...)
{
	static const char *names[] = { "debug", "info", "warn", "error" };
	va_list args;
	const char *name = level <= RETRO_LOG_ERROR ? names[level] : "log";

	fprintf(stderr, "[core:%s] ", name);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
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

static uintptr_t hw_get_current_framebuffer(void)
{
	/* The EGL pbuffer's default framebuffer is the core's render target. */
	return 0;
}

static retro_proc_address_t hw_get_proc_address(const char *symbol)
{
	union {
		__eglMustCastToProperFunctionPointerType egl;
		retro_proc_address_t retro;
		void *object;
	} address = {0};

	address.egl = eglGetProcAddress(symbol);
	if (!address.egl)
		address.object = dlsym(RTLD_DEFAULT, symbol);
	return address.retro;
}

static void close_hw_render(void)
{
	free(hw_pixels);
	hw_pixels = NULL;
	if (hw_display != EGL_NO_DISPLAY) {
		eglMakeCurrent(hw_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			       EGL_NO_CONTEXT);
		if (hw_context != EGL_NO_CONTEXT)
			eglDestroyContext(hw_display, hw_context);
		if (hw_surface != EGL_NO_SURFACE)
			eglDestroySurface(hw_display, hw_surface);
		eglTerminate(hw_display);
	}
	hw_display = EGL_NO_DISPLAY;
	hw_surface = EGL_NO_SURFACE;
	hw_context = EGL_NO_CONTEXT;
	hw_surface_width = 0;
	hw_surface_height = 0;
	hw_render_enabled = false;
	memset(&hw_render_callback, 0, sizeof(hw_render_callback));
}

static bool configure_hw_render(struct retro_hw_render_callback *requested)
{
	EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 0,
		EGL_NONE,
	};
	EGLint surface_attributes[] = {
		EGL_WIDTH, HW_SURFACE_WIDTH,
		EGL_HEIGHT, HW_SURFACE_HEIGHT,
		EGL_NONE,
	};
	EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE,
	};
	EGLConfig config;
	EGLint config_count = 0;
	EGLint width = 0;
	EGLint height = 0;
	unsigned requested_major;
	const char *renderer;

	if (!requested)
		return false;
	if (requested->context_type == RETRO_HW_CONTEXT_OPENGLES2)
		requested_major = 2;
	else if (requested->context_type == RETRO_HW_CONTEXT_OPENGLES3)
		requested_major = 3;
	else if (requested->context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION)
		requested_major = requested->version_major;
	else
		return false;
	if (requested_major < 2 || requested_major > 3)
		return false;

	if (hw_render_enabled)
		return true;
	if (requested_major == 2) {
		config_attributes[3] = EGL_OPENGL_ES2_BIT;
		context_attributes[1] = 2;
	}
	if (requested->stencil)
		config_attributes[15] = 8;

	hw_display = open_egl_display();
	if (hw_display == EGL_NO_DISPLAY || !eglInitialize(hw_display, NULL, NULL) ||
	    !eglBindAPI(EGL_OPENGL_ES_API) ||
	    !eglChooseConfig(hw_display, config_attributes, &config, 1,
			     &config_count) || config_count != 1) {
		fprintf(stderr, "Unable to initialize N64 PowerVR EGL (0x%x).\n",
			eglGetError());
		close_hw_render();
		return false;
	}
	hw_surface = eglCreatePbufferSurface(hw_display, config, surface_attributes);
	hw_context = eglCreateContext(hw_display, config, EGL_NO_CONTEXT,
				      context_attributes);
	if (hw_surface == EGL_NO_SURFACE || hw_context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(hw_display, hw_surface, hw_surface, hw_context)) {
		fprintf(stderr, "Unable to create N64 PowerVR context (0x%x).\n",
			eglGetError());
		close_hw_render();
		return false;
	}
	eglQuerySurface(hw_display, hw_surface, EGL_WIDTH, &width);
	eglQuerySurface(hw_display, hw_surface, EGL_HEIGHT, &height);
	if (width <= 0 || height <= 0) {
		close_hw_render();
		return false;
	}
	hw_surface_width = (unsigned)width;
	hw_surface_height = (unsigned)height;
	hw_pixels = malloc((size_t)hw_surface_width * hw_surface_height * 4);
	if (!hw_pixels) {
		perror("allocate hardware frame buffer");
		close_hw_render();
		return false;
	}
	renderer = (const char *)glGetString(GL_RENDERER);
	if (!renderer_is_hardware(renderer)) {
		fprintf(stderr, "N64 refused non-PowerVR renderer: %s\n",
			renderer ? renderer : "unknown");
		close_hw_render();
		return false;
	}

	requested->get_current_framebuffer = hw_get_current_framebuffer;
	requested->get_proc_address = hw_get_proc_address;
	hw_render_callback = *requested;
	hw_render_enabled = true;
	fprintf(stderr, "N64 hardware renderer: %s (GLES %u, %ux%u pbuffer).\n",
		renderer, requested_major, hw_surface_width, hw_surface_height);
	return true;
}

static const char *n64_core_variable(const char *key)
{
	static const struct {
		const char *key;
		const char *value;
	} variables[] = {
		{"mupen64plus-rdp-plugin", "gliden64"},
		{"mupen64plus-rsp-plugin", "hle"},
		{"mupen64plus-cpucore", "dynamic_recompiler"},
		{"mupen64plus-43screensize", "320x240"},
		{"mupen64plus-aspect", "4:3"},
		{"mupen64plus-EnableNativeResFactor", "1"},
		{"mupen64plus-MaxTxCacheSize", "1500"},
		{"mupen64plus-EnableLODEmulation", "False"},
		{"mupen64plus-EnableFBEmulation", "False"},
		{"mupen64plus-EnableCopyColorToRDRAM", "Off"},
		{"mupen64plus-EnableCopyDepthToRDRAM", "Off"},
		{"mupen64plus-EnableTextureCache", "False"},
		{"mupen64plus-EnableOverscan", "Disabled"},
		{"mupen64plus-MultiSampling", "0"},
		{"mupen64plus-FXAA", "0"},
		{"mupen64plus-ThreadedRenderer", "False"},
		{"mupen64plus-FrameDuping", "False"},
		{"mupen64plus-astick-deadzone", "0"},
		{"mupen64plus-astick-sensitivity", "100"},
		{"mupen64plus-pak1", "memory"},
	};

	if (strcmp(emulator_mode(), "N64") != 0)
		return NULL;
	for (size_t index = 0;
	     index < sizeof(variables) / sizeof(variables[0]); ++index)
		if (strcmp(key, variables[index].key) == 0)
			return variables[index].value;
	return NULL;
}

static void clear_core_variables(void)
{
	for (size_t index = 0; index < core_variable_count; ++index) {
		free(core_variables[index].key);
		free(core_variables[index].value);
	}
	memset(core_variables, 0, sizeof(core_variables));
	core_variable_count = 0;
}

static bool save_core_variables(const struct retro_variable *variables)
{
	clear_core_variables();
	if (!variables)
		return true;
	for (; variables->key && core_variable_count < MAX_CORE_VARIABLES;
	     ++variables) {
		const char *choices = variables->value;
		const char *separator;
		const char *end;
		struct saved_core_variable *saved;

		if (!choices)
			continue;
		separator = strchr(choices, ';');
		if (separator) {
			choices = separator + 1;
			while (*choices == ' ')
				++choices;
		}
		end = strchr(choices, '|');
		if (!end)
			end = choices + strlen(choices);
		saved = &core_variables[core_variable_count];
		saved->key = strdup(variables->key);
		saved->value = strndup(choices, (size_t)(end - choices));
		if (!saved->key || !saved->value) {
			free(saved->key);
			free(saved->value);
			saved->key = NULL;
			saved->value = NULL;
			clear_core_variables();
			return false;
		}
		++core_variable_count;
	}
	return true;
}

static const char *saved_core_variable(const char *key)
{
	for (size_t index = 0; index < core_variable_count; ++index)
		if (strcmp(key, core_variables[index].key) == 0)
			return core_variables[index].value;
	return NULL;
}

static bool environment_callback(unsigned command, void *data)
{
	switch (command) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixel_format = *(const enum retro_pixel_format *)data;
		return pixel_format == RETRO_PIXEL_FORMAT_0RGB1555 ||
		       pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ||
		       pixel_format == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = SYSTEM_DIR;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = SAVE_DIR;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *variable = data;

		variable->value = n64_core_variable(variable->key);
		if (!variable->value)
			variable->value = saved_core_variable(variable->key);
		return variable->value != NULL;
	}
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		return save_core_variables(data);
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		*(unsigned *)data = RETRO_HW_CONTEXT_OPENGLES3;
		return true;
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
		return configure_hw_render(data);
	case RETRO_ENVIRONMENT_SET_MESSAGE: {
		const struct retro_message *message = data;
		fprintf(stderr, "[core] %s\n", message->msg);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		return true;
	default:
		return false;
	}
}

static uint32_t pixel_to_xrgb8888(const uint8_t *row, unsigned x)
{
	if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
		return ((const uint32_t *)row)[x] & 0x00ffffffu;

	uint16_t pixel = ((const uint16_t *)row)[x];
	unsigned red;
	unsigned green;
	unsigned blue;

	if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
		red = (pixel >> 11) & 0x1f;
		green = (pixel >> 5) & 0x3f;
		blue = pixel & 0x1f;
		red = (red << 3) | (red >> 2);
		green = (green << 2) | (green >> 4);
		blue = (blue << 3) | (blue >> 2);
	} else {
		red = (pixel >> 10) & 0x1f;
		green = (pixel >> 5) & 0x1f;
		blue = pixel & 0x1f;
		red = (red << 3) | (red >> 2);
		green = (green << 3) | (green >> 2);
		blue = (blue << 3) | (blue >> 2);
	}

	return (red << 16) | (green << 8) | blue;
}

static uint32_t bilinear_pixel(const uint8_t *row0, const uint8_t *row1,
			       unsigned x0, unsigned x1,
			       unsigned x_fraction, unsigned y_fraction)
{
	uint32_t pixels[4] = {
		pixel_to_xrgb8888(row0, x0), pixel_to_xrgb8888(row0, x1),
		pixel_to_xrgb8888(row1, x0), pixel_to_xrgb8888(row1, x1),
	};
	unsigned inverse_x = 256 - x_fraction;
	unsigned inverse_y = 256 - y_fraction;
	unsigned weights[4] = {
		inverse_x * inverse_y, x_fraction * inverse_y,
		inverse_x * y_fraction, x_fraction * y_fraction,
	};
	uint32_t output = 0;

	for (unsigned shift = 0; shift <= 16; shift += 8) {
		unsigned value = 0;

		for (unsigned index = 0; index < 4; ++index)
			value += ((pixels[index] >> shift) & 0xff) * weights[index];
		output |= ((value + 32768) >> 16) << shift;
	}

	return output;
}

static uint64_t source_coordinate(unsigned destination_index,
				  unsigned source_size,
				  unsigned destination_size)
{
	uint64_t coordinate = (((uint64_t)(destination_index * 2 + 1) *
				 source_size) << 15) / destination_size;

	return coordinate > 32768 ? coordinate - 32768 : 0;
}

static uint32_t xrgb_to_rgb565(uint32_t color)
{
	unsigned red = (color >> 16) & 0xff;
	unsigned green = (color >> 8) & 0xff;
	unsigned blue = color & 0xff;

	return (uint32_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

static void write_framebuffer(void)
{
	size_t written = 0;
	const uint32_t *src = (const uint32_t *)fb_frame;

	if (hw_width == fb_width && hw_height == fb_height && hw_bpp == 32) {
		for (unsigned y = 0; y < fb_height; ++y)
			memcpy(fb_present + y * hw_stride,
			       fb_frame + y * fb_stride,
			       (size_t)fb_width * 4);
	} else if (hw_width == fb_width && hw_height == fb_height && hw_bpp == 16) {
		for (unsigned y = 0; y < fb_height; ++y) {
			uint16_t *dst = (uint16_t *)(fb_present + y * hw_stride);

			for (unsigned x = 0; x < fb_width; ++x)
				dst[x] = (uint16_t)xrgb_to_rgb565(src[y * fb_width + x]);
		}
	} else if (hw_width == fb_height && hw_height == fb_width) {
		/* Portrait panel: rotate landscape canvas 90° CCW. */
		for (unsigned y = 0; y < fb_height; ++y) {
			for (unsigned x = 0; x < fb_width; ++x) {
				unsigned dst_x = y;
				unsigned dst_y = fb_width - 1 - x;
				uint32_t color = src[y * fb_width + x];

				if (hw_bpp == 16) {
					uint16_t *row = (uint16_t *)(fb_present +
								     dst_y * hw_stride);
					row[dst_x] = (uint16_t)xrgb_to_rgb565(color);
				} else {
					uint32_t *row = (uint32_t *)(fb_present +
								     dst_y * hw_stride);
					row[dst_x] = color;
				}
			}
		}
	} else {
		fprintf(stderr, "Unsupported framebuffer mapping %ux%u/%ubpp\n",
			hw_width, hw_height, hw_bpp);
		keep_running = 0;
		return;
	}

	while (written < fb_present_size) {
		ssize_t result = pwrite(fb_fd, fb_present + written,
					fb_present_size - written, (off_t)written);
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

static void framebuffer_fill_rect(int x, int y, int width, int height,
				  uint32_t color)
{
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + width > (int)fb_width ? (int)fb_width : x + width;
	int y1 = y + height > (int)fb_height ? (int)fb_height : y + height;

	for (int row = y0; row < y1; ++row) {
		uint32_t *pixels = (uint32_t *)(fb_frame + row * fb_stride);

		for (int column = x0; column < x1; ++column)
			pixels[column] = color;
	}
}

static const uint8_t *bezel_glyph_for(char character)
{
	for (size_t index = 0;
	     index < sizeof(bezel_font) / sizeof(bezel_font[0]); ++index)
		if (bezel_font[index].character == character)
			return bezel_font[index].columns;
	return bezel_font[0].columns;
}

static int bezel_text_width(const char *text)
{
	return text[0] ? (int)strlen(text) * 6 - 1 : 0;
}

static void draw_bezel_text(int x, int y, const char *text, uint32_t color)
{
	for (; *text; ++text, x += 6) {
		const uint8_t *glyph = bezel_glyph_for(*text);

		for (int column = 0; column < 5; ++column)
			for (int row = 0; row < 7; ++row)
				if (glyph[column] & (1u << row))
					framebuffer_fill_rect(x + column, y + row,
							  1, 1, color);
	}
}

static void draw_gamepup_bezel(unsigned top, unsigned game_height)
{
	const uint32_t background = 0x00030c09;
	const uint32_t panel = 0x00081712;
	const uint32_t dark_green = 0x00123d22;
	const uint32_t green = 0x0042d65c;
	const uint32_t yellow = 0x00f4d35e;
	const char *system = emulator_mode();
	int top_text_y = top > 9 ? ((int)top - 7) / 2 : 1;
	int bottom_start = (int)(top + game_height);
	int bottom_height = (int)fb_height - bottom_start;
	int bottom_text_y = bottom_start + (bottom_height - 7) / 2;
	int gamepup_x = ((int)fb_width - bezel_text_width("GAMEPUP")) / 2;
	int system_x = ((int)fb_width - bezel_text_width(system)) / 2;

	framebuffer_fill_rect(0, 0, (int)fb_width, (int)fb_height, background);
	framebuffer_fill_rect(0, 2, (int)fb_width, (int)top - 3, panel);
	framebuffer_fill_rect(0, bottom_start + 1, (int)fb_width,
			      bottom_height - 3, panel);

	/* Segmented accent lines meet the game viewport without covering it. */
	for (int x = 0; x < (int)fb_width; x += 16) {
		framebuffer_fill_rect(x, (int)top - 1, 10, 1, green);
		framebuffer_fill_rect(x + 10, (int)top - 1, 6, 1, dark_green);
		framebuffer_fill_rect(x, bottom_start, 6, 1, dark_green);
		framebuffer_fill_rect(x + 6, bottom_start, 10, 1, green);
	}

	/* Small corner pixels make the letterbox bands feel like a real bezel. */
	framebuffer_fill_rect(5, top_text_y + 2, 3, 3, yellow);
	framebuffer_fill_rect((int)fb_width - 8, top_text_y + 2, 3, 3, yellow);
	framebuffer_fill_rect(5, bottom_text_y + 2, 3, 3, green);
	framebuffer_fill_rect((int)fb_width - 8, bottom_text_y + 2, 3, 3, green);
	draw_bezel_text(gamepup_x, top_text_y, "GAMEPUP", green);
	draw_bezel_text(system_x, bottom_text_y, system, yellow);
}

static void draw_arcade_bezel(unsigned top, unsigned game_height)
{
	const uint32_t background = 0x00080418;
	const uint32_t tile_a = 0x0015082c;
	const uint32_t tile_b = 0x0006172a;
	const uint32_t cyan = 0x003ce8e5;
	const uint32_t magenta = 0x00f044c7;
	const char *system = emulator_mode();
	int bottom_start = (int)(top + game_height);
	int bottom_height = (int)fb_height - bottom_start;
	int top_text_y = top > 9 ? ((int)top - 7) / 2 : 1;
	int bottom_text_y = bottom_start + (bottom_height - 7) / 2;

	framebuffer_fill_rect(0, 0, (int)fb_width, (int)fb_height, background);
	for (int y = 0; y < (int)top; y += 4)
		for (int x = 0; x < (int)fb_width; x += 8)
			framebuffer_fill_rect(x, y, 8, 4,
				((x / 8 + y / 4) & 1) ? tile_a : tile_b);
	for (int y = bottom_start; y < (int)fb_height; y += 4)
		for (int x = 0; x < (int)fb_width; x += 8)
			framebuffer_fill_rect(x, y, 8, 4,
				((x / 8 + y / 4) & 1) ? tile_b : tile_a);
	framebuffer_fill_rect(0, (int)top - 1, (int)fb_width, 1, cyan);
	framebuffer_fill_rect(0, bottom_start, (int)fb_width, 1, magenta);
	{
		int arcade_w = bezel_text_width("ARCADE");
		int system_w = bezel_text_width(system);
		int arcade_x = ((int)fb_width - arcade_w) / 2;
		int system_x = ((int)fb_width - system_w) / 2;

		framebuffer_fill_rect(arcade_x - 2, top_text_y - 1,
				      arcade_w + 4, 9, background);
		framebuffer_fill_rect(system_x - 2, bottom_text_y - 1,
				      system_w + 4, 9, background);
		draw_bezel_text(arcade_x, top_text_y, "ARCADE", magenta);
		draw_bezel_text(system_x, bottom_text_y, system, cyan);
	}
}

static bool load_system_bezel(void)
{
	const char *mode = emulator_mode();
	const char *filename = strcmp(mode, "NES") == 0 ? "nes-system.rgb" :
			       strcmp(mode, "N64") == 0 ? "n64-system.rgb" :
			       strcmp(mode, "DOOM") == 0 ? "doom-system.rgb" :
			       "gbc-system.rgb";
	char path[256];
	uint8_t pixels[BEZEL_WIDTH * BEZEL_HEIGHT * 3];
	size_t offset = 0;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", BEZEL_ASSET_DIR, filename);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	while (offset < sizeof(pixels)) {
		ssize_t count = read(fd, pixels + offset, sizeof(pixels) - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		offset += (size_t)count;
	}
	close(fd);
	if (offset != sizeof(pixels))
		return false;
	for (size_t pixel = 0; pixel < BEZEL_WIDTH * BEZEL_HEIGHT; ++pixel)
		system_bezel[pixel] = ((uint32_t)pixels[pixel * 3] << 16) |
				     ((uint32_t)pixels[pixel * 3 + 1] << 8) |
				     pixels[pixel * 3 + 2];
	return true;
}

static void draw_system_bezel(unsigned top, unsigned game_height)
{
	const char *mode = emulator_mode();
	const uint32_t accent = strcmp(mode, "NES") == 0 ? 0x00e3352f :
				strcmp(mode, "N64") == 0 ? 0x002f74dc :
				0x00ab65d1;
	int bottom_start = (int)(top + game_height);
	int bottom_height = (int)fb_height - bottom_start;

	framebuffer_fill_rect(0, 0, (int)fb_width, (int)fb_height, 0x00000000);
	for (int y = 0; y < (int)top; ++y) {
		unsigned source_y = (unsigned)y * BEZEL_BAND_HEIGHT / top;
		uint32_t *destination = (uint32_t *)(fb_frame + y * fb_stride);

		for (unsigned x = 0; x < fb_width; ++x)
			destination[x] = system_bezel[source_y * BEZEL_WIDTH +
						       x * BEZEL_WIDTH / fb_width];
	}
	for (int y = 0; y < bottom_height; ++y) {
		unsigned source_y = BEZEL_BAND_HEIGHT +
			(unsigned)y * BEZEL_BAND_HEIGHT / (unsigned)bottom_height;
		uint32_t *destination = (uint32_t *)(fb_frame +
						       (bottom_start + y) * fb_stride);

		for (unsigned x = 0; x < fb_width; ++x)
			destination[x] = system_bezel[source_y * BEZEL_WIDTH +
						       x * BEZEL_WIDTH / fb_width];
	}
	framebuffer_fill_rect(0, (int)top - 1, (int)fb_width, 1, accent);
	framebuffer_fill_rect(0, bottom_start, (int)fb_width, 1, accent);
}

static enum bezel_style read_bezel_style(void)
{
	char value[32] = {0};
	FILE *file;

	if (access(BEZEL_DISABLED_FILE, F_OK) == 0)
		return BEZEL_OFF;
	file = fopen(BEZEL_STYLE_FILE, "r");
	if (!file)
		return BEZEL_GAMEPUP;
	if (!fgets(value, sizeof(value), file))
		value[0] = '\0';
	fclose(file);
	value[strcspn(value, "\r\n")] = '\0';
	if (strcasecmp(value, "OFF") == 0)
		return BEZEL_OFF;
	if (strcasecmp(value, "ARCADE") == 0)
		return BEZEL_ARCADE;
	if (strcasecmp(value, "SYSTEM") == 0 ||
	    strcasecmp(value, "PHOTO") == 0)
		return BEZEL_SYSTEM;
	return BEZEL_GAMEPUP;
}

static const char *bezel_style_name(enum bezel_style style)
{
	switch (style) {
	case BEZEL_OFF:
		return "off";
	case BEZEL_ARCADE:
		return "arcade";
	case BEZEL_SYSTEM:
		return "generated system artwork";
	default:
		return "GamePup";
	}
}

static void set_buzzer_frequency(int frequency)
{
	struct input_event event = {
		.type = EV_SND,
		.code = SND_TONE,
		.value = frequency,
	};

	if (buzzer_fd < 0 || frequency == buzzer_frequency)
		return;
	if (write(buzzer_fd, &event, sizeof(event)) != sizeof(event)) {
		perror("write PWM buzzer");
		close(buzzer_fd);
		buzzer_fd = -1;
		return;
	}
	buzzer_frequency = frequency;
	++buzzer_updates;
}

static void process_audio_sample(int16_t left, int16_t right)
{
	int32_t sample;
	unsigned target_frames;
	int target_frequency = 0;

	if (buzzer_fd < 0 || audio_sample_rate <= 0.0)
		return;

	sample = ((int32_t)left + (int32_t)right) / 2;
	audio_amplitude_sum += sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
	if (previous_audio_sample <= 0 && sample > 0)
		++audio_zero_crossings;
	previous_audio_sample = sample;
	++audio_window_frames;

	target_frames = (unsigned)(audio_sample_rate / 30.0);
	if (!target_frames || audio_window_frames < target_frames)
		return;

	if (audio_amplitude_sum / audio_window_frames >= 384 &&
	    audio_zero_crossings > 0) {
		target_frequency = (int)((audio_zero_crossings * audio_sample_rate) /
					 audio_window_frames + 0.5);
		if (target_frequency < 60)
			target_frequency = 60;
		if (target_frequency > 4000)
			target_frequency = 4000;

		/* Avoid reprogramming the PWM for insignificant pitch jitter. */
		if (buzzer_frequency > 0 &&
		    abs(target_frequency - buzzer_frequency) < buzzer_frequency / 32)
			target_frequency = buzzer_frequency;
	}

	set_buzzer_frequency(target_frequency);
	audio_amplitude_sum = 0;
	audio_window_frames = 0;
	audio_zero_crossings = 0;
}

static uint32_t rgba_pixel_to_xrgb8888(const uint8_t *row, unsigned x)
{
	const uint8_t *pixel = row + x * 4;

	return ((uint32_t)pixel[0] << 16) | ((uint32_t)pixel[1] << 8) |
	       pixel[2];
}

static uint32_t bilinear_rgba_pixel(const uint8_t *row0, const uint8_t *row1,
				    unsigned x0, unsigned x1,
				    unsigned x_fraction,
				    unsigned y_fraction)
{
	uint32_t pixels[4] = {
		rgba_pixel_to_xrgb8888(row0, x0),
		rgba_pixel_to_xrgb8888(row0, x1),
		rgba_pixel_to_xrgb8888(row1, x0),
		rgba_pixel_to_xrgb8888(row1, x1),
	};
	unsigned inverse_x = 256 - x_fraction;
	unsigned inverse_y = 256 - y_fraction;
	unsigned weights[4] = {
		inverse_x * inverse_y, x_fraction * inverse_y,
		inverse_x * y_fraction, x_fraction * y_fraction,
	};
	uint32_t output = 0;

	for (unsigned shift = 0; shift <= 16; shift += 8) {
		unsigned value = 0;

		for (unsigned index = 0; index < 4; ++index)
			value += ((pixels[index] >> shift) & 0xff) * weights[index];
		output |= ((value + 32768) >> 16) << shift;
	}
	return output;
}

static void present_hardware_frame(unsigned width, unsigned height)
{
	unsigned dst_width;
	unsigned dst_height;
	unsigned left;
	unsigned top;
	GLenum error;

	if (!hw_render_enabled || !width || !height ||
	    width > hw_surface_width || height > hw_surface_height)
		return;
	if (!eglMakeCurrent(hw_display, hw_surface, hw_surface, hw_context)) {
		fprintf(stderr, "Lost N64 PowerVR context (0x%x).\n", eglGetError());
		keep_running = 0;
		return;
	}
	/* GLideN64 can leave non-fatal capability-probe errors pending. */
	while (glGetError() != GL_NO_ERROR)
		;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height,
		     GL_RGBA, GL_UNSIGNED_BYTE, hw_pixels);
	error = glGetError();
	if (error != GL_NO_ERROR) {
		fprintf(stderr, "Unable to read N64 GPU frame (0x%x).\n", error);
		keep_running = 0;
		return;
	}

	dst_width = fb_width;
	dst_height = (unsigned)(((uint64_t)height * dst_width) / width);
	if (dst_height > fb_height) {
		dst_height = fb_height;
		dst_width = (unsigned)(((uint64_t)width * dst_height) / height);
	}
	left = (fb_width - dst_width) / 2;
	top = (fb_height - dst_height) / 2;
	if (bezel_style == BEZEL_GAMEPUP)
		draw_gamepup_bezel(top, dst_height);
	else if (bezel_style == BEZEL_ARCADE)
		draw_arcade_bezel(top, dst_height);
	else if (bezel_style == BEZEL_SYSTEM && system_bezel_loaded)
		draw_system_bezel(top, dst_height);
	else
		memset(fb_frame, 0, fb_frame_size);

	for (unsigned y = 0; y < dst_height; ++y) {
		uint64_t source_y = source_coordinate(y, height, dst_height);
		unsigned source_y0 = (unsigned)(source_y >> 16);
		unsigned source_y1 = source_y0 + 1 < height ? source_y0 + 1 : source_y0;
		unsigned y_fraction = (unsigned)((source_y >> 8) & 0xff);
		const uint8_t *source_row0 = hw_pixels +
			(size_t)(height - 1 - source_y0) * width * 4;
		const uint8_t *source_row1 = hw_pixels +
			(size_t)(height - 1 - source_y1) * width * 4;
		uint32_t *destination = (uint32_t *)(fb_frame +
			(top + y) * fb_stride) + left;

		for (unsigned x = 0; x < dst_width; ++x) {
			uint64_t source_x = source_coordinate(x, width, dst_width);
			unsigned source_x0 = (unsigned)(source_x >> 16);
			unsigned source_x1 = source_x0 + 1 < width ?
				source_x0 + 1 : source_x0;
			unsigned x_fraction = (unsigned)((source_x >> 8) & 0xff);

			destination[x] = bilinear_rgba_pixel(source_row0, source_row1,
				source_x0, source_x1, x_fraction, y_fraction);
		}
	}
	write_framebuffer();
	eglSwapBuffers(hw_display, hw_surface);
}

static void video_callback(const void *data, unsigned width, unsigned height,
			   size_t pitch)
{
	unsigned dst_width;
	unsigned dst_height;
	unsigned left;
	unsigned top;

	if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		present_hardware_frame(width, height);
		count_presented_frame();
		return;
	}
	if (!data || !width || !height) {
		count_presented_frame();
		return;
	}

	dst_width = fb_width;
	if (strcmp(emulator_mode(), "DOOM") == 0)
		dst_height = dst_width * 3 / 4;
	else
		dst_height = (unsigned)(((uint64_t)height * dst_width) / width);
	if (dst_height > fb_height) {
		dst_height = fb_height;
		dst_width = (unsigned)(((uint64_t)width * dst_height) / height);
	}
	left = (fb_width - dst_width) / 2;
	top = (fb_height - dst_height) / 2;
	if (bezel_style == BEZEL_GAMEPUP)
		draw_gamepup_bezel(top, dst_height);
	else if (bezel_style == BEZEL_ARCADE)
		draw_arcade_bezel(top, dst_height);
	else if (bezel_style == BEZEL_SYSTEM && system_bezel_loaded)
		draw_system_bezel(top, dst_height);
	else
		memset(fb_frame, 0, fb_frame_size);

	for (unsigned y = 0; y < dst_height; ++y) {
		uint64_t source_y = source_coordinate(y, height, dst_height);
		unsigned source_y0 = (unsigned)(source_y >> 16);
		unsigned source_y1 = source_y0 + 1 < height ? source_y0 + 1 : source_y0;
		unsigned y_fraction = (unsigned)((source_y >> 8) & 0xff);
		const uint8_t *source_row0 = (const uint8_t *)data + source_y0 * pitch;
		const uint8_t *source_row1 = (const uint8_t *)data + source_y1 * pitch;
		uint32_t *destination = (uint32_t *)(fb_frame + (top + y) * fb_stride) + left;

		for (unsigned x = 0; x < dst_width; ++x) {
			uint64_t source_x = source_coordinate(x, width, dst_width);
			unsigned source_x0 = (unsigned)(source_x >> 16);
			unsigned source_x1 = source_x0 + 1 < width ? source_x0 + 1 : source_x0;
			unsigned x_fraction = (unsigned)((source_x >> 8) & 0xff);

			destination[x] = bilinear_pixel(source_row0, source_row1,
				source_x0, source_x1, x_fraction, y_fraction);
		}
	}

	write_framebuffer();
	count_presented_frame();
}

static void audio_callback(int16_t left, int16_t right)
{
	process_audio_sample(left, right);
}

static size_t audio_batch_callback(const int16_t *data, size_t frames)
{
	for (size_t frame = 0; frame < frames; ++frame)
		process_audio_sample(data[frame * 2], data[frame * 2 + 1]);
	return frames;
}

static void input_poll_callback(void)
{
	struct input_event events[32];
	ssize_t length;

	while ((length = read(input_fd, events, sizeof(events))) > 0) {
		size_t count = (size_t)length / sizeof(events[0]);
		for (size_t index = 0; index < count; ++index) {
			if (events[index].type == EV_KEY && events[index].code <= KEY_MAX)
				key_down[events[index].code] = events[index].value != 0;
		}
	}

	if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		perror("read input");
		keep_running = 0;
	}
}

static uint16_t joypad_mask(void)
{
	uint16_t mask = 0;
	bool doom = strcmp(emulator_mode(), "DOOM") == 0;
	bool n64 = strcmp(emulator_mode(), "N64") == 0;

	if (n64) {
		/*
		 * Mupen64Plus' standard mapping expects RetroPad B/Y as N64 A/B.
		 * The physical D-pad is exposed separately as the analog stick.
		 */
		if (key_down[KEY_TAB])
			mask |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
		if (key_down[KEY_ENTER])
			mask |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;
		if (key_down[KEY_ESC])
			mask |= 1u << (key_down[KEY_5] ?
				RETRO_DEVICE_ID_JOYPAD_L : RETRO_DEVICE_ID_JOYPAD_L2);
		if (key_down[KEY_P])
			mask |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
		if (key_down[KEY_1])
			mask |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
		return mask;
	}

	if (key_down[KEY_DOWN])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
	if (key_down[KEY_UP])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
	if (key_down[KEY_LEFT])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	if (key_down[KEY_RIGHT])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	if (key_down[KEY_TAB])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
	if (key_down[KEY_ENTER])
		mask |= 1u << (doom ? RETRO_DEVICE_ID_JOYPAD_R2 :
				      RETRO_DEVICE_ID_JOYPAD_B);
	if (key_down[KEY_5])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
	if (key_down[KEY_1])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
	if (key_down[KEY_ESC])
		mask |= 1u << RETRO_DEVICE_ID_JOYPAD_X;
	if (key_down[KEY_P])
		mask |= 1u << (doom ? RETRO_DEVICE_ID_JOYPAD_L2 :
				      RETRO_DEVICE_ID_JOYPAD_Y);

	return mask;
}

static int16_t input_state_callback(unsigned port, unsigned device,
				    unsigned index, unsigned id)
{
	uint16_t mask;
	bool n64 = strcmp(emulator_mode(), "N64") == 0;

	if (port != 0)
		return 0;
	if (n64 && device == RETRO_DEVICE_ANALOG && index <= 1 &&
	    (id == RETRO_DEVICE_ID_ANALOG_X ||
	     id == RETRO_DEVICE_ID_ANALOG_Y)) {
		bool c_buttons = key_down[KEY_5];
		int16_t value = 0;

		if ((index == RETRO_DEVICE_INDEX_ANALOG_LEFT && !c_buttons) ||
		    (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT && c_buttons)) {
			if (id == RETRO_DEVICE_ID_ANALOG_X) {
				if (key_down[KEY_RIGHT])
					value = 32767;
				else if (key_down[KEY_LEFT])
					value = -32767;
				/* Mupen's right-stick C mapping has an inverted X axis. */
				if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
					value = -value;
			} else {
				if (key_down[KEY_DOWN])
					value = 32767;
				else if (key_down[KEY_UP])
					value = -32767;
			}
		}
		return value;
	}
	if (index != 0 || device != RETRO_DEVICE_JOYPAD)
		return 0;

	mask = joypad_mask();
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return (int16_t)mask;
	if (id > RETRO_DEVICE_ID_JOYPAD_R3)
		return 0;
	return (mask & (1u << id)) != 0;
}

static void *load_symbol(const char *name)
{
	void *symbol = dlsym(core_handle, name);
	if (!symbol) {
		fprintf(stderr, "Missing core symbol %s: %s\n", name, dlerror());
		exit(EXIT_FAILURE);
	}
	return symbol;
}

#define LOAD_CORE_SYMBOL(member, symbol_name) \
	do { *(void **)(&core.member) = load_symbol(symbol_name); } while (0)

static void load_core(const char *path)
{
	core_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!core_handle) {
		fprintf(stderr, "Unable to load %s: %s\n", path, dlerror());
		exit(EXIT_FAILURE);
	}

	LOAD_CORE_SYMBOL(init, "retro_init");
	LOAD_CORE_SYMBOL(deinit, "retro_deinit");
	LOAD_CORE_SYMBOL(api_version, "retro_api_version");
	LOAD_CORE_SYMBOL(get_system_info, "retro_get_system_info");
	LOAD_CORE_SYMBOL(get_system_av_info, "retro_get_system_av_info");
	LOAD_CORE_SYMBOL(set_environment, "retro_set_environment");
	LOAD_CORE_SYMBOL(set_video_refresh, "retro_set_video_refresh");
	LOAD_CORE_SYMBOL(set_audio_sample, "retro_set_audio_sample");
	LOAD_CORE_SYMBOL(set_audio_sample_batch, "retro_set_audio_sample_batch");
	LOAD_CORE_SYMBOL(set_input_poll, "retro_set_input_poll");
	LOAD_CORE_SYMBOL(set_input_state, "retro_set_input_state");
	LOAD_CORE_SYMBOL(set_controller_port_device, "retro_set_controller_port_device");
	LOAD_CORE_SYMBOL(reset, "retro_reset");
	LOAD_CORE_SYMBOL(run, "retro_run");
	LOAD_CORE_SYMBOL(load_game, "retro_load_game");
	LOAD_CORE_SYMBOL(unload_game, "retro_unload_game");
	LOAD_CORE_SYMBOL(get_memory_data, "retro_get_memory_data");
	LOAD_CORE_SYMBOL(get_memory_size, "retro_get_memory_size");
}

static void load_rom(const char *path)
{
	struct stat details;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 || fstat(fd, &details) < 0) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	rom_size = (size_t)details.st_size;
	rom_data = malloc(rom_size);
	if (!rom_data) {
		perror("malloc ROM");
		exit(EXIT_FAILURE);
	}

	size_t offset = 0;
	while (offset < rom_size) {
		ssize_t count = read(fd, rom_data + offset, rom_size - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			fprintf(stderr, "Short read from %s\n", path);
			exit(EXIT_FAILURE);
		}
		offset += (size_t)count;
	}
	close(fd);
}

static char *save_path_for_rom(void)
{
	const char *name = strrchr(rom_path, '/');
	size_t length;
	char *path;

	name = name ? name + 1 : rom_path;
	length = strlen(SAVE_DIR) + strlen(name) + 6;
	path = malloc(length);
	if (!path)
		return NULL;
	snprintf(path, length, "%s/%s.sav", SAVE_DIR, name);
	return path;
}

static void load_save_ram(void)
{
	void *memory = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	char *path = save_path_for_rom();
	int fd;

	if (!memory || !size || !path)
		goto done;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t count = read(fd, memory, size);
		if (count > 0)
			fprintf(stderr, "Loaded save RAM from %s\n", path);
		close(fd);
	}
done:
	free(path);
}

static void save_save_ram(void)
{
	void *memory = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	char *path = save_path_for_rom();
	int fd;

	if (!memory || !size || !path)
		goto done;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		if (write(fd, memory, size) == (ssize_t)size)
			fprintf(stderr, "Saved save RAM to %s\n", path);
		else
			perror("write save RAM");
		close(fd);
	} else {
		perror(path);
	}
done:
	free(path);
}

static void open_devices(const char *framebuffer_path, const char *input_path)
{
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	bool audio_muted;

	fb_fd = open(framebuffer_path, O_RDWR | O_CLOEXEC);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &variable) < 0 ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fixed) < 0) {
		perror(framebuffer_path);
		exit(EXIT_FAILURE);
	}
	hw_width = variable.xres;
	hw_height = variable.yres;
	hw_bpp = variable.bits_per_pixel;
	hw_stride = fixed.line_length;
	if (hw_bpp != 16 && hw_bpp != 32) {
		fprintf(stderr, "Expected a 16- or 32-bit framebuffer, got %u bpp\n",
			hw_bpp);
		exit(EXIT_FAILURE);
	}
	if (!((hw_width == 320 && hw_height == 240) ||
	      (hw_width == 240 && hw_height == 320))) {
		fprintf(stderr,
			"Unsupported framebuffer %ux%u (need 320x240 or 240x320)\n",
			hw_width, hw_height);
		exit(EXIT_FAILURE);
	}
	fb_width = 320;
	fb_height = 240;
	fb_stride = fb_width * 4;
	fb_frame_size = (size_t)fb_stride * fb_height;
	fb_frame = calloc(1, fb_frame_size);
	fb_present_size = (size_t)hw_stride * hw_height;
	fb_present = calloc(1, fb_present_size);
	if (!fb_frame || !fb_present) {
		perror("calloc framebuffer");
		exit(EXIT_FAILURE);
	}
	fprintf(stderr, "GamePup framebuffer view 320x240 -> hw %ux%u stride=%u bpp=%u\n",
		hw_width, hw_height, hw_stride, hw_bpp);

	input_fd = open(input_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (input_fd < 0) {
		perror(input_path);
		exit(EXIT_FAILURE);
	}
	if (ioctl(input_fd, EVIOCGRAB, 1) < 0)
		perror("EVIOCGRAB buttons");

	audio_muted = access(MUTE_FILE, F_OK) == 0;
	bezel_style = read_bezel_style();
	if (bezel_style == BEZEL_SYSTEM)
		system_bezel_loaded = load_system_bezel();
	if (bezel_style == BEZEL_SYSTEM && !system_bezel_loaded) {
		fprintf(stderr, "Generated system bezel asset unavailable; using GamePup bezel.\n");
		bezel_style = BEZEL_GAMEPUP;
	}
	fprintf(stderr, "Game bezel: %s.\n", bezel_style_name(bezel_style));
	if (audio_muted) {
		fprintf(stderr, "PWM buzzer muted from the GamePup menu.\n");
	} else {
		buzzer_fd = open(DEFAULT_BUZZER, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (buzzer_fd < 0)
			fprintf(stderr, "PWM buzzer unavailable: %s\n", strerror(errno));
	}
}

static void add_nanoseconds(struct timespec *time, uint64_t nanoseconds)
{
	time->tv_nsec += (long)nanoseconds;
	time->tv_sec += time->tv_nsec / 1000000000L;
	time->tv_nsec %= 1000000000L;
}

int main(int argc, char **argv)
{
	const char *core_path = DEFAULT_CORE;
	const char *input_path = DEFAULT_INPUT;
	const char *framebuffer_path = DEFAULT_FB;
	struct retro_system_info system_info = {0};
	struct retro_system_av_info av_info = {0};
	struct retro_game_info game_info = {0};
	struct timespec next_frame;
	uint64_t frame_period;
	unsigned exit_frames = 0;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s ROM [core.so] [input-event] [framebuffer]\n", argv[0]);
		return EXIT_FAILURE;
	}
	rom_path = argv[1];
	if (argc > 2)
		core_path = argv[2];
	if (argc > 3)
		input_path = argv[3];
	if (argc > 4)
		framebuffer_path = argv[4];

	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	signal(SIGHUP, stop_handler);

	open_devices(framebuffer_path, input_path);
	load_rom(rom_path);
	load_core(core_path);
	if (core.api_version() != RETRO_API_VERSION) {
		fprintf(stderr, "Unsupported libretro API version\n");
		return EXIT_FAILURE;
	}

	core.set_environment(environment_callback);
	core.set_video_refresh(video_callback);
	core.set_audio_sample(audio_callback);
	core.set_audio_sample_batch(audio_batch_callback);
	core.set_input_poll(input_poll_callback);
	core.set_input_state(input_state_callback);
	core.init();
	core.get_system_info(&system_info);
	fprintf(stderr, "Core: %s %s\n", system_info.library_name,
		system_info.library_version);

	game_info.path = rom_path;
	game_info.data = rom_data;
	game_info.size = rom_size;
	if (!core.load_game(&game_info)) {
		fprintf(stderr, "The core rejected %s\n", rom_path);
		return EXIT_FAILURE;
	}
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	if (hw_render_enabled) {
		if (!eglMakeCurrent(hw_display, hw_surface, hw_surface, hw_context)) {
			fprintf(stderr, "Unable to activate N64 PowerVR context (0x%x).\n",
				eglGetError());
			return EXIT_FAILURE;
		}
		if (!hw_render_callback.context_reset) {
			fprintf(stderr, "N64 core did not provide a context reset callback.\n");
			return EXIT_FAILURE;
		}
		hw_render_callback.context_reset();
	}
	core.get_system_av_info(&av_info);
	audio_sample_rate = av_info.timing.sample_rate;
	load_save_ram();

	frame_period = (uint64_t)(1000000000.0 / av_info.timing.fps);
	if (!frame_period)
		frame_period = 16666667;
	clock_gettime(CLOCK_MONOTONIC, &next_frame);
	publish_fps(0.0);
	fprintf(stderr, "Running %.3f fps on %ux%u framebuffer. "
		"Hold Start+Select to quit.\n", av_info.timing.fps,
		fb_width, fb_height);
	if (buzzer_fd >= 0)
		fprintf(stderr, "PWM buzzer audio enabled at %.0f Hz source rate.\n",
			audio_sample_rate);

	while (keep_running) {
		core.run();
		if (key_down[KEY_1] && key_down[KEY_5]) {
			if (++exit_frames >= 60)
				keep_running = 0;
		} else {
			exit_frames = 0;
		}

		add_nanoseconds(&next_frame, frame_period);
		if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL) != 0) {
			clock_gettime(CLOCK_MONOTONIC, &next_frame);
		}
	}

	set_buzzer_frequency(0);
	unlink(FPS_FILE);
	if (buzzer_fd >= 0)
		fprintf(stderr, "PWM buzzer produced %u tone updates.\n",
			buzzer_updates);
	save_save_ram();
	core.unload_game();
	core.deinit();
	if (hw_render_enabled && hw_render_callback.context_destroy)
		hw_render_callback.context_destroy();
	close_hw_render();
	dlclose(core_handle);
	close(input_fd);
	if (buzzer_fd >= 0)
		close(buzzer_fd);
	close(fb_fd);
	free(fb_frame);
	free(fb_present);
	free(rom_data);
	clear_core_variables();
	return EXIT_SUCCESS;
}
