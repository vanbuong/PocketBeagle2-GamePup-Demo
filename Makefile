# SPDX-License-Identifier: GPL-2.0-only
KERNEL_VERSION ?= $(shell uname -r)
KERNEL_BUILD ?= /lib/modules/$(KERNEL_VERSION)/build

obj-m += drm_mipi_dbi.o
obj-m += ili9341.o

.PHONY: modules clean

modules:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) clean
