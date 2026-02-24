# CLAUDE.md

This file provides guidance for Claude Code (claude.ai/code) when working with this repository.

## Project Overview

This repository contains reference drivers for Xilinx DMA IP subsystems:

- **QDMA**: PCIe Multi-Queue DMA for UltraScale+ devices (2048+ queues, SR-IOV support)
- **XDMA**: PCIe DMA for UltraScale+, UltraScale, Virtex-7, and 7 Series Gen2 devices
- **XVSEC**: PCIe Vendor Specific Extended Capability driver for MCAP features

## Repository Structure

```
QDMA/
├── linux-kernel/          # Linux kernel driver
│   ├── driver/src/        # Kernel module source
│   ├── driver/libqdma/    # Shared library with hardware access
│   └── apps/              # User-space utilities (dma-ctl, dma-perf, etc.)
├── DPDK/                  # DPDK Poll Mode Driver
│   ├── drivers/net/qdma/  # PMD implementation
│   └── examples/          # DPDK test applications
└── windows/               # Windows kernel driver

XDMA/
└── linux-kernel/
    ├── xdma/              # Driver source
    ├── tools/             # Test utilities
    └── tests/             # Test scripts and data

XVSEC/
└── linux-kernel/
    ├── drv/               # Driver source with MCAP support
    ├── libxvsec/          # User-space library
    └── tools/             # Utility programs
```

## Build Commands

### QDMA Linux Kernel Driver
```bash
cd QDMA/linux-kernel
make                       # Build driver and apps
make install               # Install kernel module
make clean                 # Clean build artifacts
```

### XDMA Linux Kernel Driver
```bash
cd XDMA/linux-kernel/xdma
make                       # Build driver
make install               # Install kernel module
```

### XVSEC Linux Kernel Driver
```bash
cd XVSEC/linux-kernel
make                       # Build driver and library
make install               # Install kernel module
```

### QDMA Windows Driver
Open `QDMA/windows/QDMA.sln` in Visual Studio 2017+ with WDK installed.

## Key Development Patterns

### Hardware Abstraction
QDMA uses a hardware abstraction layer in `driver/libqdma/qdma_access/` with platform-specific implementations:
- `qdma_soft_access/` - Soft IP
- `eqdma_soft_access/` - EQDMA Soft IP
- `qdma_cpm4_access/` - CPM4 devices
- `eqdma_cpm5_access/` - CPM5 devices

### Character Device Interface
All drivers expose functionality via character devices in `/dev/`:
- QDMA: `/dev/qdma*` (control, H2C, C2H queues)
- XDMA: `/dev/xdma*` (control, events, H2C, C2H)
- XVSEC: `/dev/xvsec*`

### Kernel Version Compatibility
Build system handles kernel differences via:
- `make_rules/kernel_check.mk` - Kernel version detection
- `make_rules/distro_check.mk` - Distribution detection
- Conditional compilation for APIs changed in kernels 5.x, 6.x

## Testing

### XDMA Tests
```bash
cd XDMA/linux-kernel
./load_driver.sh           # Load kernel module
./run_test.sh              # Run test suite
```

### QDMA Tools
```bash
dma-ctl qdma<bdf> q list   # List queues
dma-perf -c <cfg>          # Performance benchmark
dma-to-device -d <dev>     # H2C transfer test
dma-from-device -d <dev>   # C2H transfer test
```

## Important Files

- `QDMA/RELEASE`, `XDMA/linux-kernel/RELEASE` - Version history and supported platforms
- `QDMA/linux-kernel/driver/src/qdma_mod.c` - QDMA module entry point
- `XDMA/linux-kernel/xdma/libxdma.c` - XDMA core implementation
- `README.md` - Main documentation with links to full docs

## Code Conventions

- GPL-2.0 license for kernel code
- C99 style with Linux kernel coding standards
- Hardware registers accessed via abstraction functions
- Extensive use of debug macros (`pr_debug`, `pr_info`, `pr_err`)
- Netlink interface for QDMA user-space communication

## Platform Support

- **Linux**: Kernels 3.10 - 6.16+ (RHEL, Ubuntu, Fedora, etc.)
- **Windows**: Windows 10+ with WDK 10.0.17134.0+
- **DPDK**: Compatible with DPDK framework versions

## External Documentation

Full documentation available at: https://xilinx.github.io/dma_ip_drivers/
