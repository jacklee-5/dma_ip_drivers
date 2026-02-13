/*
 * This file is part of the Xilinx DMA IP Core driver for Linux
 *
 * Copyright (c) 2017-2022, Xilinx, Inc. All rights reserved.
 * Copyright (c) 2022-2024, Advanced Micro Devices, Inc. All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#define pr_fmt(fmt) KBUILD_MODNAME ":%s: " fmt, __func__

#include "qdma_p2p.h"
#include "xdev.h"

#include <linux/pci.h>
#include <linux/kernel.h>

#ifdef QDMA_P2P_SUPPORT

/**
 * qdma_p2p_init() - Initialize P2P provider for device
 *
 * Registers the HBM BAR as P2P memory using pci_p2pdma_add_resource().
 * This allows peer devices (GPUs, NVMe, etc.) to DMA directly to/from
 * the V80's HBM memory without CPU involvement.
 */
int qdma_p2p_init(struct xlnx_dma_dev *xdev, int hbm_bar)
{
	struct pci_dev *pdev;
	resource_size_t bar_start, bar_len;
	int rv;

	if (!xdev) {
		pr_err("Invalid device handle\n");
		return -EINVAL;
	}

	if (!xdev->conf.pdev) {
		pr_err("PCI device not available\n");
		return -EINVAL;
	}

	pdev = xdev->conf.pdev;

	/* Validate BAR number */
	if (hbm_bar < 0 || hbm_bar >= QDMA_BAR_NUM) {
		pr_err("%s: Invalid HBM BAR number: %d (valid: 0-%d)\n",
		       dev_name(&pdev->dev), hbm_bar, QDMA_BAR_NUM - 1);
		return -EINVAL;
	}

	/* Check if BAR exists and get its properties */
	bar_start = pci_resource_start(pdev, hbm_bar);
	bar_len = pci_resource_len(pdev, hbm_bar);

	if (!bar_start || !bar_len) {
		pr_err("%s: HBM BAR %d not available or has zero size\n",
		       dev_name(&pdev->dev), hbm_bar);
		return -ENODEV;
	}

	pr_info("%s: HBM BAR %d: phys_addr=0x%llx, size=0x%llx (%llu GB)\n",
		dev_name(&pdev->dev), hbm_bar,
		(unsigned long long)bar_start,
		(unsigned long long)bar_len,
		(unsigned long long)bar_len / (1024ULL * 1024 * 1024));

	/* Verify BAR size against expected HBM size */
	if (bar_len < QDMA_V80_HBM_SIZE) {
		pr_warn("%s: HBM BAR size (%llu GB) is less than expected V80 HBM size (%llu GB)\n",
			dev_name(&pdev->dev),
			(unsigned long long)bar_len / (1024ULL * 1024 * 1024),
			QDMA_V80_HBM_SIZE / (1024ULL * 1024 * 1024));
		/* Continue anyway - use actual BAR size */
	}

	/*
	 * Register BAR as P2P memory provider.
	 * This makes the memory accessible to peer PCIe devices.
	 */
	rv = pci_p2pdma_add_resource(pdev, hbm_bar, bar_len, QDMA_V80_HBM_OFFSET);
	if (rv) {
		pr_err("%s: Failed to register P2P resource on BAR %d: %d\n",
		       dev_name(&pdev->dev), hbm_bar, rv);
		pr_err("%s: Possible causes: kernel config, IOMMU, ACS settings\n",
		       dev_name(&pdev->dev));
		return rv;
	}

	/* Store P2P configuration in device structure */
	xdev->p2p_enabled = true;
	xdev->hbm_bar_num = hbm_bar;
	xdev->hbm_bar_start = bar_start;
	xdev->hbm_bar_len = bar_len;

	pr_info("%s: P2P provider enabled successfully\n", dev_name(&pdev->dev));
	pr_info("%s: HBM BAR %d, %llu GB available for peer device access\n",
		dev_name(&pdev->dev), hbm_bar,
		(unsigned long long)bar_len / (1024ULL * 1024 * 1024));

	return 0;
}

/**
 * qdma_p2p_cleanup() - Cleanup P2P provider resources
 *
 * P2P memory regions registered with pci_p2pdma_add_resource() are
 * automatically cleaned up when the PCI device is removed. This function
 * resets the driver's internal state.
 */
void qdma_p2p_cleanup(struct xlnx_dma_dev *xdev)
{
	if (!xdev)
		return;

	if (xdev->p2p_enabled) {
		pr_info("%s: P2P provider disabled\n",
			xdev->conf.pdev ? dev_name(&xdev->conf.pdev->dev) : "unknown");

		xdev->p2p_enabled = false;
		xdev->hbm_bar_num = -1;
		xdev->hbm_bar_start = 0;
		xdev->hbm_bar_len = 0;
	}
}

/**
 * qdma_p2p_get_status() - Get P2P provider status
 */
enum qdma_p2p_status qdma_p2p_get_status(struct xlnx_dma_dev *xdev)
{
	if (!xdev)
		return QDMA_P2P_ERROR;

	return xdev->p2p_enabled ? QDMA_P2P_ENABLED : QDMA_P2P_DISABLED;
}

/**
 * qdma_p2p_get_hbm_info() - Get HBM memory information
 */
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

/**
 * qdma_p2p_dump_info() - Dump P2P information to buffer
 */
int qdma_p2p_dump_info(struct xlnx_dma_dev *xdev, char *buf, int buflen)
{
	int len = 0;

	if (!xdev || !buf || buflen <= 0)
		return 0;

	len += scnprintf(buf + len, buflen - len,
			 "P2P Provider Status\n");
	len += scnprintf(buf + len, buflen - len,
			 "-------------------\n");

	if (xdev->p2p_enabled) {
		len += scnprintf(buf + len, buflen - len,
				 "Status:     Enabled\n");
		len += scnprintf(buf + len, buflen - len,
				 "HBM BAR:    %d\n", xdev->hbm_bar_num);
		len += scnprintf(buf + len, buflen - len,
				 "HBM Base:   0x%llx\n",
				 (unsigned long long)xdev->hbm_bar_start);
		len += scnprintf(buf + len, buflen - len,
				 "HBM Size:   %llu bytes (%llu GB)\n",
				 (unsigned long long)xdev->hbm_bar_len,
				 (unsigned long long)xdev->hbm_bar_len /
				 (1024ULL * 1024 * 1024));
	} else {
		len += scnprintf(buf + len, buflen - len,
				 "Status:     Disabled\n");
		len += scnprintf(buf + len, buflen - len,
				 "HBM BAR:    Not configured\n");
	}

	return len;
}

#else /* !QDMA_P2P_SUPPORT */

/*
 * Stub implementations for kernels without P2P support (< 4.20)
 */

int qdma_p2p_init(struct xlnx_dma_dev *xdev, int hbm_bar)
{
	pr_warn("P2P DMA not supported: kernel version too old (requires 4.20+)\n");
	pr_warn("Build with kernel 4.20+ to enable P2P provider mode\n");
	return -ENOTSUPP;
}

void qdma_p2p_cleanup(struct xlnx_dma_dev *xdev)
{
	/* Nothing to clean up */
}

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

int qdma_p2p_dump_info(struct xlnx_dma_dev *xdev, char *buf, int buflen)
{
	int len = 0;

	if (!buf || buflen <= 0)
		return 0;

	len += scnprintf(buf + len, buflen - len,
			 "P2P Provider Status\n");
	len += scnprintf(buf + len, buflen - len,
			 "-------------------\n");
	len += scnprintf(buf + len, buflen - len,
			 "Status:     Not Supported (kernel < 4.20)\n");

	return len;
}

#endif /* QDMA_P2P_SUPPORT */
