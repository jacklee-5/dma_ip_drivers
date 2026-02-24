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

#ifndef __QDMA_P2P_H__
#define __QDMA_P2P_H__
/**
 * @file
 * @brief This file contains the declarations for QDMA P2P DMA support
 *
 * QDMA P2P (Peer-to-Peer) DMA support allows the QDMA device to expose
 * its HBM (High Bandwidth Memory) as P2P memory that peer PCIe devices
 * (such as GPUs, NVMe SSDs) can directly access without CPU involvement.
 *
 * This implements the "provider" model where QDMA exposes memory,
 * not the "initiator" model (V80 does not support outbound P2P).
 */

#include <linux/pci.h>
#include <linux/version.h>

/* P2P DMA support requires kernel 4.20+ */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
#define QDMA_P2P_SUPPORT
#include <linux/pci-p2pdma.h>
#endif

/**
 * V80 HBM Configuration
 * - HBM Size: 42GB
 * - HBM Offset: 0 (start from beginning of BAR)
 */
#define QDMA_V80_HBM_SIZE	(42ULL * 1024 * 1024 * 1024)
#define QDMA_V80_HBM_OFFSET	0

/**
 * P2P provider status
 */
enum qdma_p2p_status {
	/** P2P provider is disabled */
	QDMA_P2P_DISABLED = 0,
	/** P2P provider is enabled and operational */
	QDMA_P2P_ENABLED,
	/** P2P initialization failed */
	QDMA_P2P_ERROR,
	/** P2P not supported (kernel too old) */
	QDMA_P2P_UNSUPPORTED,
};

/* Forward declaration */
struct xlnx_dma_dev;

/*****************************************************************************/
/**
 * qdma_p2p_init() - Initialize P2P provider for device
 *
 * @param[in]	xdev:		pointer to xlnx_dma_dev structure
 * @param[in]	hbm_bar:	BAR number mapped to HBM
 *
 * Registers the HBM BAR as P2P memory provider using the Linux kernel's
 * pci_p2pdma_add_resource() API. This allows peer devices to DMA directly
 * to/from the V80's HBM memory.
 *
 * @return	0 on success
 * @return	-EINVAL if parameters are invalid
 * @return	-ENODEV if BAR is not available
 * @return	-ENOTSUPP if kernel doesn't support P2P
 * @return	other negative error code on failure
 *****************************************************************************/
int qdma_p2p_init(struct xlnx_dma_dev *xdev, int hbm_bar);

/*****************************************************************************/
/**
 * qdma_p2p_cleanup() - Cleanup P2P provider resources
 *
 * @param[in]	xdev:	pointer to xlnx_dma_dev structure
 *
 * Cleans up P2P provider resources. Called during device removal.
 * P2P memory regions are automatically cleaned up by the kernel
 * when the PCI device is removed.
 *
 *****************************************************************************/
void qdma_p2p_cleanup(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * qdma_p2p_get_status() - Get P2P provider status
 *
 * @param[in]	xdev:	pointer to xlnx_dma_dev structure
 *
 * @return	P2P status enum value
 *****************************************************************************/
enum qdma_p2p_status qdma_p2p_get_status(struct xlnx_dma_dev *xdev);

/*****************************************************************************/
/**
 * qdma_p2p_get_hbm_info() - Get HBM memory information
 *
 * @param[in]	xdev:		pointer to xlnx_dma_dev structure
 * @param[out]	bar_num:	output BAR number (can be NULL)
 * @param[out]	base_addr:	output physical base address (can be NULL)
 * @param[out]	size:		output size in bytes (can be NULL)
 *
 * Retrieves information about the P2P HBM memory region.
 *
 * @return	0 on success
 * @return	-ENODEV if P2P is not enabled
 *****************************************************************************/
int qdma_p2p_get_hbm_info(struct xlnx_dma_dev *xdev,
			  int *bar_num,
			  resource_size_t *base_addr,
			  resource_size_t *size);

/*****************************************************************************/
/**
 * qdma_p2p_dump_info() - Dump P2P information to buffer
 *
 * @param[in]	xdev:	pointer to xlnx_dma_dev structure
 * @param[out]	buf:	output buffer
 * @param[in]	buflen:	buffer length
 *
 * Formats P2P status and HBM information into a string buffer.
 *
 * @return	number of bytes written to buffer
 *****************************************************************************/
int qdma_p2p_dump_info(struct xlnx_dma_dev *xdev, char *buf, int buflen);

#endif /* __QDMA_P2P_H__ */
