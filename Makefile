# SPDX-License-Identifier: GPL-2.0-only
KERNEL_VERSION ?= $(shell uname -r)
KERNEL_BUILD ?= /lib/modules/$(KERNEL_VERSION)/build

obj-m += drm_mipi_dbi.o
obj-m += ili9341.o
# Generic MIPI-DBI panel driver for the Tang Nano 9K FPGA display (second,
# non-fatal build pass: make PANEL_MIPI_DBI=1 once panel-mipi-dbi.c is present).
ifeq ($(PANEL_MIPI_DBI),1)
obj-m += panel-mipi-dbi.o
endif

.PHONY: modules clean

modules:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) clean
