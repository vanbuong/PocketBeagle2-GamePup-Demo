# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
LIBRETRO_INCLUDE ?= /usr/local/include
KHRONOS_INCLUDE ?= $(CURDIR)/khronos
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic \
	-I$(LIBRETRO_INCLUDE) -I$(KHRONOS_INCLUDE)
LDLIBS += -ldl -lasound

.PHONY: all clean gles apps

# GLES targets need libEGL/libGLESv2 at link time (TI PVR or Mesa).
all: gamepup-oled-status gamepup-retro gamepup-gpu-bench

gles: gamepup-retro gamepup-gpu-bench

apps: gamepup-oled-status

gamepup-retro: gamepup-retro.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lEGL -lGLESv2

gamepup-oled-status: gamepup-oled-status.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lgif

gamepup-gpu-bench: gamepup-gpu-bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lEGL -lGLESv2 -lm

clean:
	rm -f gamepup-retro gamepup-oled-status gamepup-gpu-bench
