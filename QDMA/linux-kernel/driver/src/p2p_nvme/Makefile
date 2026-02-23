#/*
# * This file is part of the Xilinx DMA IP Core driver for Linux
# *
# * Copyright (c) 2017-2022, Xilinx, Inc. All rights reserved.
# * Copyright (c) 2022-2024, Advanced Micro Devices, Inc. All rights reserved.
# *
# * This source code is free software; you can redistribute it and/or modify it
# * under the terms and conditions of the GNU General Public License,
# * version 2, as published by the Free Software Foundation.
# */

# P2P NVMe to HBM DMA kernel module
# Enables true zero-copy transfers from NVMe to V80 HBM
#
# This module is standalone - it finds V80 devices by PCI ID and uses
# the P2P memory that the QDMA driver registered via pci_p2pdma_add_resource()

SHELL = /bin/bash

MOD_NAME := qdma_p2p_nvme

# Kernel build directory
ifeq ($(KDIR),)
  KDIR := /lib/modules/$(shell uname -r)/build
endif

ifeq ($(KSRC),)
  KSRC := $(KDIR)
endif

# Compiler flags
EXTRA_CFLAGS += -DLINUX -D__KERNEL__ -DMODULE -O2 -pipe -Wall

# Module objects
obj-m += $(MOD_NAME).o
$(MOD_NAME)-objs := qdma_p2p_nvme.o

.PHONY: all modules clean install help

all: modules

modules:
	@echo "========================================"
	@echo "Building QDMA P2P NVMe Module"
	@echo "========================================"
	@echo "Kernel: $(KSRC)"
	$(MAKE) -C $(KSRC) M=$(shell pwd) modules
	@echo ""
	@echo "Build complete: $(MOD_NAME).ko"

clean:
	@-/bin/rm -rf *.ko* ?odule* .tmp_versions *.mod.* *.o *.o.* .*.o.* .*.cmd
	@-/bin/rm -rf .cache.mk .*.d

install: modules
	@echo "Installing $(MOD_NAME).ko..."
	@install -D -m 644 $(MOD_NAME).ko \
		/lib/modules/$(shell uname -r)/extra/$(MOD_NAME).ko
	@depmod -a
	@echo "Installation complete"

help:
	@echo "QDMA P2P NVMe to HBM Module"
	@echo ""
	@echo "This module enables true zero-copy peer-to-peer DMA transfers"
	@echo "from NVMe storage directly to V80 HBM memory."
	@echo ""
	@echo "Build targets:"
	@echo "  make              - Build the kernel module"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make install      - Install module to /lib/modules"
	@echo ""
	@echo "Prerequisites:"
	@echo "  1. Load QDMA driver with hbm_bar parameter:"
	@echo "     insmod qdma.ko hbm_bar=<bus>:<pf>:<bar_num>"
	@echo ""
	@echo "  2. Load this module:"
	@echo "     insmod qdma_p2p_nvme.ko"
	@echo ""
	@echo "  3. Use /dev/qdma_p2p_nvme via ioctl for P2P transfers"
	@echo ""
	@echo "Module parameters:"
	@echo "  v80_vendor=0x10EE  - V80 PCI vendor ID (default: Xilinx)"
	@echo "  v80_device=-1      - V80 PCI device ID (-1 for any)"
	@echo ""
	@echo "Requirements:"
	@echo "  - Linux kernel 5.1+ for P2P DMA support"
	@echo "  - NVMe and V80 on same PCIe root complex or switch"
	@echo "  - ACS may need to be disabled for P2P"
