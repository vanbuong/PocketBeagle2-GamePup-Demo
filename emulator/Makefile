# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
LIBRETRO_INCLUDE ?= /usr/local/include
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -I$(LIBRETRO_INCLUDE)
LDLIBS += -ldl

.PHONY: all clean

all: gamepup-retro gamepup-oled-status gamepup-gpu-bench

gamepup-retro: gamepup-retro.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lEGL -lGLESv2

gamepup-oled-status: gamepup-oled-status.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lgif

gamepup-gpu-bench: gamepup-gpu-bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lEGL -lGLESv2 -lm

clean:
	rm -f gamepup-retro gamepup-oled-status gamepup-gpu-bench
