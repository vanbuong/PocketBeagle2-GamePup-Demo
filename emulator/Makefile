# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
LIBRETRO_INCLUDE ?= /usr/local/include
KHRONOS_INCLUDE ?= $(CURDIR)/khronos
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic \
	-I$(LIBRETRO_INCLUDE) -I$(KHRONOS_INCLUDE)
LDLIBS += -ldl -lasound

.PHONY: all clean gles apps ui gamepup-ui gamepup-ui.bin

# GLES targets need libEGL/libGLESv2 at link time (TI PVR or Mesa).
all: gamepup-oled-status gamepup-retro gamepup-gpu-bench

gles: gamepup-retro gamepup-gpu-bench

apps: gamepup-oled-status

ui: gamepup-ui

gamepup-retro: gamepup-retro.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lEGL -lGLESv2

gamepup-oled-status: gamepup-oled-status.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lgif

gamepup-gpu-bench: gamepup-gpu-bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lEGL -lGLESv2 -lm

# LVGL phone-style launcher. LVGL is fetched at a pinned tag into ui/lvgl.
UI_DIR := $(CURDIR)/ui
LVGL_DIR := $(UI_DIR)/lvgl
UI_BUILD := $(UI_DIR)/build
UI_CFLAGS := -O2 -pipe -std=gnu11 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE \
	-I$(UI_DIR) -I$(LVGL_DIR)
UI_SRCS := $(wildcard $(UI_DIR)/*.c)
LVGL_SRCS = $(shell find $(LVGL_DIR)/src -name '*.c' \
	-not -path '*/drivers/*')
UI_OBJS = $(patsubst $(UI_DIR)/%.c,$(UI_BUILD)/app/%.o,$(UI_SRCS))
LVGL_OBJS = $(patsubst $(LVGL_DIR)/%.c,$(UI_BUILD)/lvgl/%.o,$(LVGL_SRCS))

$(LVGL_DIR)/lvgl.h:
	$(UI_DIR)/fetch-lvgl.sh $(LVGL_DIR)

$(UI_BUILD)/lvgl/%.o: $(LVGL_DIR)/%.c $(UI_DIR)/lv_conf.h | $(LVGL_DIR)/lvgl.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CFLAGS) -w -c -o $@ $<

$(UI_BUILD)/app/%.o: $(UI_DIR)/%.c $(wildcard $(UI_DIR)/*.h) | $(LVGL_DIR)/lvgl.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CFLAGS) -c -o $@ $<

gamepup-ui: $(LVGL_DIR)/lvgl.h
	$(MAKE) --no-print-directory gamepup-ui.bin

gamepup-ui.bin: $(UI_OBJS) $(LVGL_OBJS)
	$(CC) -o gamepup-ui $^ $(LDFLAGS) -lm

clean:
	rm -f gamepup-retro gamepup-oled-status gamepup-gpu-bench gamepup-ui
	rm -rf $(UI_BUILD)
