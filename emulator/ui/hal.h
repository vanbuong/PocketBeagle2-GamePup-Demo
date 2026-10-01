/* SPDX-License-Identifier: GPL-2.0-only */
/* Platform layer for gamepup-ui: framebuffer, buttons, buzzer, files, children. */
#ifndef GAMEPUP_HAL_H
#define GAMEPUP_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#define SCREEN_W 320
#define SCREEN_H 240

/* Key values delivered to LVGL. Cape buttons: A=ENTER, B=ESC, plus the rest. */
#define GP_KEY_START 0x101
#define GP_KEY_SELECT 0x102
#define GP_KEY_X 0x103
#define GP_KEY_Y 0x104

/* Directory prefix for tests (env GAMEPUP_ROOT); empty on the device. */
const char *hal_path(const char *absolute, char *buf, size_t size);
const char *hal_p(const char *absolute); /* returns a rotating static buffer */

bool hal_file_exists(const char *path);
bool hal_file_toggle(const char *path);
int hal_file_read_int(const char *path, int fallback);
bool hal_file_write_int(const char *path, int value);
bool hal_file_read_str(const char *path, char *out, size_t size);
bool hal_file_write_str(const char *path, const char *text);
int64_t hal_file_mtime_ns(const char *path);

/* Display + input. Returns false if the framebuffer/buttons cannot be opened. */
bool hal_open(void);
void hal_close(void);
bool hal_is_open(void);
void hal_headless_start(void);
/* Dump the last rendered frame as a PPM (headless mode only). */
bool hal_headless_dump(const char *path);
void hal_inject_key(uint32_t key, bool pressed);

/* Raw cape key events (Linux input codes) for the hardware test. */
typedef void (*hal_raw_cb)(int code, int value, void *user);
void hal_set_raw_cb(hal_raw_cb cb, void *user);
/* While suspended LVGL ignores keys (the hardware test uses raw events). */
void hal_set_ui_keys(bool enabled);
lv_indev_t *hal_keypad(void);

/* Sleep up to ms, waking early when a button event arrives. */
void hal_wait(uint32_t ms);

/* Buzzer */
void hal_beep(const char *name);
void hal_tone(int hz);
void hal_buzzer_silence(void);
bool hal_buzzer_ready(void);

/* Backlight and root helpers */
bool hal_backlight_available(void);
int hal_backlight_get(void);
bool hal_backlight_set(int level);
bool hal_run_root_helper(const char *const *argv);
void hal_claim_display(void);

/* Children. hal_run releases the display/buttons, runs, then takes them back. */
int hal_run(const char *const *argv);
/* Spawn without blocking; returns pid or -1. Output goes to *out_fd if given. */
int hal_spawn(const char *const *argv, int *out_fd, bool silence_stderr);
void hal_spawn_detached(const char *const *argv);
/* waitpid(WNOHANG). Returns true and fills status when the child exited. */
bool hal_child_done(int pid, int *status);
void hal_kill(int pid, int sig);

/* System */
int hal_cpu_temp_c(void);
bool hal_which(const char *program);
char *hal_alsa_card_id(char *buf, size_t size);
uint32_t hal_now_ms(void);

#endif
