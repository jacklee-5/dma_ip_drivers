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

#ifndef __QDMA_P2P_NVME_H__
#define __QDMA_P2P_NVME_H__

/**
 * @file
 * @brief P2P NVMe to HBM DMA module - True zero-copy transfers
 *
 * This kernel module enables true peer-to-peer DMA transfers from NVMe
 * storage directly to V80 HBM memory, bypassing CPU memory entirely.
 *
 * Data path: NVMe -> PCIe fabric -> V80 HBM BAR (no CPU memory involved)
 *
 * This module uses the Linux P2P DMA framework to:
 * 1. Allocate P2P pages from the V80 HBM BAR (registered by qdma driver)
 * 2. Submit block I/O targeting those P2P pages
 * 3. NVMe controller DMA's data directly to HBM
 */

#include <linux/ioctl.h>
#include <linux/types.h>

/* IOCTL magic number */
#define QDMA_P2P_NVME_MAGIC 'Q'

/**
 * struct qdma_p2p_nvme_xfer - P2P transfer request
 * @nvme_dev:      NVMe block device path (e.g., "/dev/nvme0n1")
 * @start_sector:  Starting sector on NVMe device
 * @num_sectors:   Number of 512-byte sectors to transfer
 * @hbm_offset:    Offset within HBM to write data (bytes)
 * @qdma_bdf:      QDMA device BDF (bus:dev:func) or -1 for auto
 * @flags:         Transfer flags (reserved)
 */
struct qdma_p2p_nvme_xfer {
	char nvme_dev[64];
	__u64 start_sector;
	__u64 num_sectors;
	__u64 hbm_offset;
	__s32 qdma_bdf;
	__u32 flags;
};

/**
 * struct qdma_p2p_nvme_status - P2P transfer status/result
 * @bytes_transferred: Number of bytes successfully transferred
 * @elapsed_ns:        Elapsed time in nanoseconds
 * @p2p_enabled:       Whether P2P path was used (vs fallback)
 * @error_code:        Error code (0 on success)
 * @error_msg:         Error message string
 */
struct qdma_p2p_nvme_status {
	__u64 bytes_transferred;
	__u64 elapsed_ns;
	__u32 p2p_enabled;
	__s32 error_code;
	char error_msg[128];
};

/**
 * struct qdma_p2p_nvme_info - P2P device information
 * @qdma_bdf:     QDMA device BDF
 * @hbm_bar:      BAR number mapped to HBM
 * @hbm_size:     HBM size in bytes
 * @hbm_phys:     HBM physical address
 * @p2p_enabled:  P2P provider status
 * @nvme_count:   Number of NVMe devices detected
 */
struct qdma_p2p_nvme_info {
	__u32 qdma_bdf;
	__u32 hbm_bar;
	__u64 hbm_size;
	__u64 hbm_phys;
	__u32 p2p_enabled;
	__u32 nvme_count;
};

/* IOCTL commands */

/**
 * QDMA_P2P_NVME_XFER - Execute P2P transfer from NVMe to HBM
 *
 * Reads data from NVMe device and writes directly to HBM using P2P DMA.
 * The NVMe controller DMA's data directly to the HBM BAR address space.
 */
#define QDMA_P2P_NVME_XFER \
	_IOWR(QDMA_P2P_NVME_MAGIC, 1, struct qdma_p2p_nvme_xfer)

/**
 * QDMA_P2P_NVME_GET_STATUS - Get status of last transfer
 */
#define QDMA_P2P_NVME_GET_STATUS \
	_IOR(QDMA_P2P_NVME_MAGIC, 2, struct qdma_p2p_nvme_status)

/**
 * QDMA_P2P_NVME_GET_INFO - Get P2P device information
 */
#define QDMA_P2P_NVME_GET_INFO \
	_IOR(QDMA_P2P_NVME_MAGIC, 3, struct qdma_p2p_nvme_info)

/**
 * QDMA_P2P_NVME_CHECK_PATH - Check if P2P path is available
 *
 * Checks P2P distance between NVMe and QDMA devices.
 * Returns 0 if P2P is possible, negative error if not.
 */
#define QDMA_P2P_NVME_CHECK_PATH \
	_IOW(QDMA_P2P_NVME_MAGIC, 4, struct qdma_p2p_nvme_xfer)

/* Transfer flags */
#define QDMA_P2P_NVME_FLAG_SYNC       0x0001  /* Synchronous transfer */
#define QDMA_P2P_NVME_FLAG_VERIFY     0x0002  /* Verify after transfer */

/* Device name */
#define QDMA_P2P_NVME_DEV_NAME "qdma_p2p_nvme"

/* Maximum transfer size per request (256MB) */
#define QDMA_P2P_NVME_MAX_XFER_SIZE (256ULL * 1024 * 1024)

/* Default chunk size for P2P allocation (4MB) */
#define QDMA_P2P_NVME_CHUNK_SIZE (4ULL * 1024 * 1024)

#endif /* __QDMA_P2P_NVME_H__ */
