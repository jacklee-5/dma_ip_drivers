# P2P DMA Implementation Plan for QDMA V80 (HBM Provider Model)

## Overview

This plan describes implementing P2P (Peer-to-Peer) DMA functionality for the QDMA driver on Versal V80, where **V80 acts as a P2P memory provider** exposing its HBM (High Bandwidth Memory) to other PCIe devices.

### Key Constraints
- **V80 does not support outbound P2P** (QDMA cannot initiate transfers to peer devices)
- **V80 exposes HBM via a PCIe BAR** that other devices (GPUs, NICs, NVMe) can directly access
- Uses Linux kernel's P2P DMA framework (`pci_p2pdma_add_resource()`)

### Configuration
- **HBM BAR**: Configurable via module parameter (unknown at design time)
- **HBM Size**: 42GB
- **HBM Offset**: 0 (full BAR used for HBM)

---

## Architecture

```
+------------------+                     +------------------+
|   Peer Device    |   P2P DMA Transfer  |   QDMA V80       |
|   (GPU/NIC/SSD)  | ==================> |   HBM Memory     |
|   (Initiator)    |   Direct PCIe Path  |   (Provider)     |
+------------------+                     +------------------+
        |                                        |
        | DMA Engine                             | BAR mapped
        | initiates                              | to 42GB HBM
        |                                        |
+-------v----------------------------------------v-------+
|                 Linux P2P DMA Framework                |
|   pci_p2pdma_add_resource() registers HBM as P2P mem   |
+--------------------------------------------------------+
```

### P2P Provider Model Flow

1. QDMA driver loads and identifies V80 device (CPM5)
2. Driver reads HBM BAR number from module parameter
3. Driver registers HBM BAR region with `pci_p2pdma_add_resource()`
4. Peer devices (GPU, NVMe, etc.) query P2P capability
5. Peer device DMA engine transfers directly to/from V80 HBM BAR
6. No QDMA descriptor involvement - hardware BAR access only

---

## Implementation Details

### Phase 1: Module Parameter and Device Structure

#### 1.1 New Module Parameter

**File:** `QDMA/linux-kernel/driver/src/qdma_mod.c`

Add new module parameter for HBM BAR configuration:

```c
static char hbm_bar[500] = {0};
module_param_string(hbm_bar, hbm_bar, sizeof(hbm_bar), 0);
MODULE_PARM_DESC(hbm_bar, "HBM BAR number for P2P memory, format is \"<bus_num>:<pf_num>:<bar_num>\" and multiple comma separated entries can be specified");
```

Add extraction function (similar to existing `extract_mod_param()`):

```c
static int extract_hbm_bar_param(struct pci_dev *pdev)
{
    /* Parse hbm_bar parameter string */
    /* Return BAR number or -1 if not configured */
}
```

#### 1.2 Device Structure Extension

**File:** `QDMA/linux-kernel/driver/libqdma/xdev.h`

Add P2P fields to `struct xlnx_dma_dev`:

```c
struct xlnx_dma_dev {
    /* ... existing fields ... */

    /* P2P HBM Provider Support */
    bool p2p_enabled;              /* P2P provider mode active */
    int hbm_bar_num;               /* BAR number mapped to HBM (-1 if none) */
    resource_size_t hbm_bar_start; /* HBM BAR physical address */
    resource_size_t hbm_bar_len;   /* HBM BAR length */
    void __iomem *hbm_bar_base;    /* HBM BAR kernel mapping (optional) */
};
```

**File:** `QDMA/linux-kernel/driver/libqdma/libqdma_export.h`

Add P2P fields to `struct qdma_dev_conf`:

```c
struct qdma_dev_conf {
    /* ... existing fields ... */

    int bar_num_hbm;               /* HBM BAR number for P2P (-1 if none) */
    bool p2p_provider_enabled;     /* Enable P2P provider mode */
};
```

---

### Phase 2: P2P Core Implementation

#### 2.1 New P2P Provider Module

**New File:** `QDMA/linux-kernel/driver/libqdma/qdma_p2p.h`

```c
#ifndef __QDMA_P2P_H__
#define __QDMA_P2P_H__

#include <linux/pci.h>
#include <linux/version.h>

/* P2P requires kernel 4.20+ */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
#define QDMA_P2P_SUPPORT
#include <linux/pci-p2pdma.h>
#endif

/* V80 HBM Configuration */
#define QDMA_V80_HBM_SIZE        (42ULL * 1024 * 1024 * 1024)  /* 42GB */
#define QDMA_V80_HBM_OFFSET      0                              /* Start at 0 */

/* P2P provider status */
enum qdma_p2p_status {
    QDMA_P2P_DISABLED = 0,
    QDMA_P2P_ENABLED,
    QDMA_P2P_ERROR,
    QDMA_P2P_UNSUPPORTED,     /* Kernel too old */
};

/* Forward declaration */
struct xlnx_dma_dev;

/**
 * qdma_p2p_init() - Initialize P2P provider for device
 * @xdev: QDMA device handle
 * @hbm_bar: BAR number mapped to HBM
 *
 * Registers HBM BAR as P2P memory provider using
 * pci_p2pdma_add_resource().
 *
 * Return: 0 on success, negative error code on failure
 */
int qdma_p2p_init(struct xlnx_dma_dev *xdev, int hbm_bar);

/**
 * qdma_p2p_cleanup() - Cleanup P2P provider resources
 * @xdev: QDMA device handle
 */
void qdma_p2p_cleanup(struct xlnx_dma_dev *xdev);

/**
 * qdma_p2p_get_status() - Get P2P provider status
 * @xdev: QDMA device handle
 *
 * Return: P2P status enum
 */
enum qdma_p2p_status qdma_p2p_get_status(struct xlnx_dma_dev *xdev);

/**
 * qdma_p2p_get_hbm_info() - Get HBM memory info
 * @xdev: QDMA device handle
 * @bar_num: Output BAR number
 * @base_addr: Output physical base address
 * @size: Output size in bytes
 *
 * Return: 0 on success, -ENODEV if P2P not enabled
 */
int qdma_p2p_get_hbm_info(struct xlnx_dma_dev *xdev,
                          int *bar_num,
                          resource_size_t *base_addr,
                          resource_size_t *size);

#endif /* __QDMA_P2P_H__ */
```

**New File:** `QDMA/linux-kernel/driver/libqdma/qdma_p2p.c`

```c
#define pr_fmt(fmt) KBUILD_MODNAME ":%s: " fmt, __func__

#include "qdma_p2p.h"
#include "xdev.h"

#include <linux/pci.h>

#ifdef QDMA_P2P_SUPPORT

int qdma_p2p_init(struct xlnx_dma_dev *xdev, int hbm_bar)
{
    struct pci_dev *pdev;
    resource_size_t bar_start, bar_len;
    int rv;

    if (!xdev || !xdev->conf.pdev) {
        pr_err("Invalid device handle\n");
        return -EINVAL;
    }

    pdev = xdev->conf.pdev;

    /* Validate BAR number */
    if (hbm_bar < 0 || hbm_bar >= QDMA_BAR_NUM) {
        pr_err("Invalid HBM BAR number: %d\n", hbm_bar);
        return -EINVAL;
    }

    /* Check if BAR exists and get its size */
    bar_start = pci_resource_start(pdev, hbm_bar);
    bar_len = pci_resource_len(pdev, hbm_bar);

    if (!bar_start || !bar_len) {
        pr_err("HBM BAR %d not available\n", hbm_bar);
        return -ENODEV;
    }

    pr_info("HBM BAR %d: start=0x%llx, len=0x%llx (%llu GB)\n",
            hbm_bar, (u64)bar_start, (u64)bar_len,
            (u64)bar_len / (1024 * 1024 * 1024));

    /* Verify BAR size matches expected HBM size */
    if (bar_len < QDMA_V80_HBM_SIZE) {
        pr_warn("HBM BAR size (%llu GB) less than expected (%llu GB)\n",
                (u64)bar_len / (1024 * 1024 * 1024),
                QDMA_V80_HBM_SIZE / (1024 * 1024 * 1024));
    }

    /* Register BAR as P2P memory provider */
    rv = pci_p2pdma_add_resource(pdev, hbm_bar, bar_len, QDMA_V80_HBM_OFFSET);
    if (rv) {
        pr_err("Failed to register P2P resource: %d\n", rv);
        return rv;
    }

    /* Store P2P configuration */
    xdev->p2p_enabled = true;
    xdev->hbm_bar_num = hbm_bar;
    xdev->hbm_bar_start = bar_start;
    xdev->hbm_bar_len = bar_len;

    pr_info("P2P provider enabled: HBM BAR %d, %llu GB available\n",
            hbm_bar, (u64)bar_len / (1024 * 1024 * 1024));

    return 0;
}

void qdma_p2p_cleanup(struct xlnx_dma_dev *xdev)
{
    if (!xdev)
        return;

    if (xdev->p2p_enabled) {
        /* P2P resources are cleaned up automatically on device removal */
        xdev->p2p_enabled = false;
        xdev->hbm_bar_num = -1;
        xdev->hbm_bar_start = 0;
        xdev->hbm_bar_len = 0;
        pr_info("P2P provider disabled\n");
    }
}

enum qdma_p2p_status qdma_p2p_get_status(struct xlnx_dma_dev *xdev)
{
    if (!xdev)
        return QDMA_P2P_ERROR;

    return xdev->p2p_enabled ? QDMA_P2P_ENABLED : QDMA_P2P_DISABLED;
}

int qdma_p2p_get_hbm_info(struct xlnx_dma_dev *xdev,
                          int *bar_num,
                          resource_size_t *base_addr,
                          resource_size_t *size)
{
    if (!xdev || !xdev->p2p_enabled)
        return -ENODEV;

    if (bar_num)
        *bar_num = xdev->hbm_bar_num;
    if (base_addr)
        *base_addr = xdev->hbm_bar_start;
    if (size)
        *size = xdev->hbm_bar_len;

    return 0;
}

#else /* !QDMA_P2P_SUPPORT */

/* Stub implementations for old kernels */
int qdma_p2p_init(struct xlnx_dma_dev *xdev, int hbm_bar)
{
    pr_warn("P2P not supported: kernel version too old (requires 4.20+)\n");
    return -ENOTSUPP;
}

void qdma_p2p_cleanup(struct xlnx_dma_dev *xdev) {}

enum qdma_p2p_status qdma_p2p_get_status(struct xlnx_dma_dev *xdev)
{
    return QDMA_P2P_UNSUPPORTED;
}

int qdma_p2p_get_hbm_info(struct xlnx_dma_dev *xdev,
                          int *bar_num,
                          resource_size_t *base_addr,
                          resource_size_t *size)
{
    return -ENOTSUPP;
}

#endif /* QDMA_P2P_SUPPORT */
```

---

### Phase 3: Driver Integration

#### 3.1 Device Probe Integration

**File:** `QDMA/linux-kernel/driver/src/qdma_mod.c`

Modify `probe_one()` to initialize P2P:

```c
static int probe_one(struct pci_dev *pdev, const struct pci_device_id *id)
{
    /* ... existing code ... */

    /* Extract HBM BAR parameter for P2P */
    conf.bar_num_hbm = extract_hbm_bar_param(pdev);
    if (conf.bar_num_hbm >= 0) {
        conf.p2p_provider_enabled = true;
        pr_info("P2P provider mode: HBM BAR %d\n", conf.bar_num_hbm);
    }

    rv = qdma_device_open(DRV_MODULE_NAME, &conf, &dev_hndl);
    if (rv < 0)
        return rv;

    /* ... rest of existing code ... */
}
```

#### 3.2 Device Open Integration

**File:** `QDMA/linux-kernel/driver/libqdma/libqdma_export.c`

Modify `qdma_device_open()` to initialize P2P:

```c
int qdma_device_open(...)
{
    /* ... existing initialization ... */

    /* Initialize P2P provider if configured */
    if (conf->p2p_provider_enabled && conf->bar_num_hbm >= 0) {
        rv = qdma_p2p_init(xdev, conf->bar_num_hbm);
        if (rv < 0) {
            pr_warn("P2P initialization failed: %d (continuing without P2P)\n", rv);
            /* Non-fatal - continue without P2P */
        }
    }

    /* ... rest of initialization ... */
}
```

#### 3.3 Device Close Integration

**File:** `QDMA/linux-kernel/driver/libqdma/libqdma_export.c`

Modify `qdma_device_close()` to cleanup P2P:

```c
void qdma_device_close(...)
{
    /* ... existing cleanup ... */

    /* Cleanup P2P provider */
    qdma_p2p_cleanup(xdev);

    /* ... rest of cleanup ... */
}
```

---

### Phase 4: Sysfs Interface

#### 4.1 P2P Status Attributes

**File:** `QDMA/linux-kernel/driver/src/qdma_mod.c`

Add sysfs attributes for P2P status:

```c
static ssize_t show_p2p_status(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct xlnx_pci_dev *xpdev;
    struct xlnx_dma_dev *xdev;
    int len = 0;

    xpdev = (struct xlnx_pci_dev *)dev_get_drvdata(dev);
    if (!xpdev)
        return -EINVAL;

    xdev = (struct xlnx_dma_dev *)(xpdev->dev_hndl);

    len = scnprintf(buf, PAGE_SIZE,
                    "P2P Enabled: %s\n"
                    "HBM BAR: %d\n"
                    "HBM Base: 0x%llx\n"
                    "HBM Size: %llu GB\n",
                    xdev->p2p_enabled ? "yes" : "no",
                    xdev->hbm_bar_num,
                    (u64)xdev->hbm_bar_start,
                    (u64)xdev->hbm_bar_len / (1024 * 1024 * 1024));

    return len;
}

static DEVICE_ATTR(p2p_status, S_IRUGO, show_p2p_status, NULL);
```

Add to attribute group:

```c
static struct attribute *pci_device_attrs[] = {
    &dev_attr_qmax.attr,
    &dev_attr_intr_rngsz.attr,
    &dev_attr_p2p_status.attr,  /* New P2P status */
    NULL,
};
```

---

### Phase 5: Build System Changes

#### 5.1 Makefile Updates

**File:** `QDMA/linux-kernel/driver/libqdma/Makefile`

Add P2P source file:

```makefile
QDMA_LIBQDMA_OBJS := \
    libqdma_export.o \
    libqdma_config.o \
    qdma_descq.o \
    ...
    qdma_p2p.o            # Add P2P module
```

**File:** `QDMA/linux-kernel/driver/Makefile`

Add P2P configuration option:

```makefile
# P2P Provider Support (requires kernel 4.20+)
CONFIG_QDMA_P2P_PROVIDER ?= y

ifeq ($(CONFIG_QDMA_P2P_PROVIDER),y)
    EXTRA_CFLAGS += -DCONFIG_QDMA_P2P_PROVIDER
endif
```

---

### Phase 6: Documentation

#### 6.1 Module Parameter Documentation

Update `QDMA/linux-kernel/docs/README` with:

```
P2P Provider Mode (V80 HBM)
---------------------------
The QDMA driver supports P2P (Peer-to-Peer) DMA provider mode for
Versal V80 devices with HBM (High Bandwidth Memory).

Module Parameter:
  hbm_bar=<bus_num>:<pf_num>:<bar_num>

Example:
  # Load driver with HBM on BAR 4 for device 0000:01:00.0
  modprobe qdma hbm_bar=1:0:4

When enabled, the HBM memory (42GB) is registered as P2P memory,
allowing peer devices (GPUs, NVMe SSDs) to DMA directly to/from
the V80 HBM without CPU involvement.

Requirements:
  - Linux kernel 4.20 or later
  - Peer devices with P2P DMA support
  - Both devices under same PCIe root complex (recommended)

Checking P2P Status:
  cat /sys/bus/pci/devices/<bdf>/qdma/p2p_status
```

---

## Files Summary

### New Files
| File | Description |
|------|-------------|
| `driver/libqdma/qdma_p2p.h` | P2P provider API header |
| `driver/libqdma/qdma_p2p.c` | P2P provider implementation |

### Modified Files
| File | Changes |
|------|---------|
| `driver/src/qdma_mod.c` | Add `hbm_bar` module param, P2P sysfs, probe integration |
| `driver/libqdma/xdev.h` | Add P2P fields to `xlnx_dma_dev` |
| `driver/libqdma/libqdma_export.h` | Add P2P fields to `qdma_dev_conf` |
| `driver/libqdma/libqdma_export.c` | Call P2P init/cleanup in device open/close |
| `driver/libqdma/Makefile` | Add `qdma_p2p.o` |
| `driver/Makefile` | Add `CONFIG_QDMA_P2P_PROVIDER` |

---

## Testing Strategy

### Unit Tests
1. **Module parameter parsing**
   - Valid BAR numbers (0-5)
   - Invalid BAR numbers (negative, >5)
   - Multiple device configurations

2. **P2P initialization**
   - Valid HBM BAR
   - Non-existent BAR
   - Kernel without P2P support (<4.20)

### Integration Tests
1. **P2P with NVIDIA GPU**
   ```bash
   # Check P2P capability
   nvidia-smi topo -p2p r

   # Use CUDA P2P sample to transfer to/from V80 HBM
   ./p2pBandwidthLatencyTest
   ```

2. **P2P with NVMe SSD (if supported)**
   - Use nvme-cli with P2P options

3. **Sysfs verification**
   ```bash
   cat /sys/bus/pci/devices/0000:01:00.0/qdma/p2p_status
   ```

### Performance Tests
1. **Bandwidth measurement**
   - GPU -> V80 HBM
   - V80 HBM -> GPU
   - Compare with host-bounce baseline

2. **Latency measurement**
   - Round-trip latency
   - Compare with non-P2P path

---

## Considerations and Limitations

### Hardware Requirements
- Versal V80 with HBM
- PCIe Gen4/Gen5 for optimal bandwidth
- Peer devices must support P2P DMA

### Software Requirements
- Linux kernel 4.20+ (P2P DMA APIs)
- Peer device drivers with P2P support (NVIDIA GPU driver, etc.)

### Known Limitations
1. **IOMMU Compatibility**: P2P may require IOMMU bypass or special configuration
2. **ACS (Access Control Services)**: May need to be disabled for P2P to work
3. **Root Complex**: P2P typically requires devices under same root complex
4. **No Outbound**: V80 cannot initiate transfers - only expose memory

### Error Handling
- P2P initialization failure is non-fatal
- Driver continues operation without P2P if initialization fails
- Clear error messages in dmesg for debugging

---

## Implementation Timeline

| Phase | Description | Effort |
|-------|-------------|--------|
| Phase 1 | Module parameter + device structure | 1-2 days |
| Phase 2 | P2P core implementation | 2-3 days |
| Phase 3 | Driver integration | 1-2 days |
| Phase 4 | Sysfs interface | 1 day |
| Phase 5 | Build system | 0.5 day |
| Phase 6 | Documentation | 0.5 day |
| Testing | Unit + integration tests | 2-3 days |

**Total Estimate:** 8-12 days

---

## Usage Example

```bash
# Load driver with P2P enabled on BAR 4
modprobe qdma hbm_bar=1:0:4

# Verify P2P status
cat /sys/bus/pci/devices/0000:01:00.0/qdma/p2p_status
# Output:
# P2P Enabled: yes
# HBM BAR: 4
# HBM Base: 0x380000000000
# HBM Size: 42 GB

# Check dmesg for P2P info
dmesg | grep -i p2p
# [  123.456789] qdma:qdma_p2p_init: HBM BAR 4: start=0x380000000000, len=0xa80000000 (42 GB)
# [  123.456790] qdma:qdma_p2p_init: P2P provider enabled: HBM BAR 4, 42 GB available

# Peer device (GPU) can now DMA directly to V80 HBM
# using the peer device's P2P APIs
```
