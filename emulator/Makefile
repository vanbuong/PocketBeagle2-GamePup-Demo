# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
LIBRETRO_INCLUDE ?= /usr/local/include
KHRONOS_INCLUDE ?= $(CURDIR)/khronos
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic \
	-I$(LIBRETRO_INCLUDE) -I$(KHRONOS_INCLUDE)
LDLIBS += -ldl

.PHONY: all clean gles apps test

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

# Host-side checks (no hardware, no GL): game scaling for 320x240 and 480x272, and the
# changed-rows framebuffer writer.  Needs libretro.h (LIBRETRO_INCLUDE).
test: tests/test_retro_geometry.c gamepup-retro.c
	$(CC) -std=gnu11 -O1 -I$(LIBRETRO_INCLUDE) -I$(KHRONOS_INCLUDE) \
		-Wl,--unresolved-symbols=ignore-all -o tests/test_retro_geometry \
		tests/test_retro_geometry.c $(LDLIBS)
	./tests/test_retro_geometry

clean:
	rm -f gamepup-retro gamepup-oled-status gamepup-gpu-bench tests/test_retro_geometry
