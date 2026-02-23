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

/**
 * @file qdma_p2p_nvme.c
 * @brief P2P NVMe to HBM DMA kernel module
 *
 * This module enables true peer-to-peer DMA transfers from NVMe storage
 * directly to V80 HBM memory, bypassing CPU memory entirely.
 *
 * Data flow:
 *   NVMe Controller --DMA--> PCIe Switch/Fabric ---> V80 HBM BAR
 *
 * No CPU memory involved - true zero-copy P2P transfer.
 *
 * Prerequisites:
 *   - QDMA driver must be loaded with hbm_bar parameter to register P2P memory
 *   - Kernel 5.1+ for full P2P bio support
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/version.h>
#include <linux/ktime.h>
#include <linux/completion.h>

#include "qdma_p2p_nvme.h"

/* P2P DMA support requires kernel 5.1+ for full bio P2P support */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#define QDMA_P2P_NVME_SUPPORT
#include <linux/pci-p2pdma.h>
#endif

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("AMD/Xilinx");
MODULE_DESCRIPTION("P2P NVMe to QDMA HBM DMA Module");
MODULE_VERSION("1.0");

/* AMD/Xilinx Vendor ID */
#define PCI_VENDOR_ID_XILINX    0x10EE
#define PCI_VENDOR_ID_AMD       0x1022

/* V80 Device IDs - add your specific device IDs here */
static const struct pci_device_id v80_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_ANY_ID) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_ANY_ID) },
	{ 0, }
};

/* Module parameters */
static int v80_vendor = PCI_VENDOR_ID_XILINX;
module_param(v80_vendor, int, 0444);
MODULE_PARM_DESC(v80_vendor, "V80 PCI vendor ID (default: 0x10EE for Xilinx)");

static int v80_device = -1;  /* -1 means any device from vendor */
module_param(v80_device, int, 0444);
MODULE_PARM_DESC(v80_device, "V80 PCI device ID (-1 for any from vendor)");

/* Device class and major number */
static struct class *p2p_nvme_class;
static dev_t p2p_nvme_devno;
static struct cdev p2p_nvme_cdev;
static struct device *p2p_nvme_device;

/* Cached V80 device */
static struct pci_dev *v80_pdev;
static DEFINE_MUTEX(v80_lock);

/* Last transfer status */
static struct qdma_p2p_nvme_status last_status;
static DEFINE_MUTEX(status_lock);

#ifdef QDMA_P2P_NVME_SUPPORT

/**
 * struct p2p_bio_ctx - Context for async bio completion
 */
struct p2p_bio_ctx {
	struct completion done;
	int error;
};

/**
 * p2p_bio_end_io() - Bio completion callback
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
static void p2p_bio_end_io(struct bio *bio)
{
	struct p2p_bio_ctx *ctx = bio->bi_private;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	ctx->error = blk_status_to_errno(bio->bi_status);
#else
	ctx->error = bio->bi_error;
#endif
	complete(&ctx->done);
}
#else
static void p2p_bio_end_io(struct bio *bio, int error)
{
	struct p2p_bio_ctx *ctx = bio->bi_private;

	ctx->error = error;
	complete(&ctx->done);
}
#endif

/**
 * find_v80_device() - Find V80 PCI device with P2P memory registered
 *
 * Returns PCI device with reference taken, or NULL if not found.
 * Caller must call pci_dev_put() when done.
 */
static struct pci_dev *find_v80_device(void)
{
	struct pci_dev *pdev = NULL;

	mutex_lock(&v80_lock);

	/* Return cached device if still valid */
	if (v80_pdev && pci_dev_get(v80_pdev)) {
		/* Check if P2P is still available */
		if (pci_p2pdma_distance_many(v80_pdev, NULL, 0, false) >= 0) {
			mutex_unlock(&v80_lock);
			return v80_pdev;
		}
		pci_dev_put(v80_pdev);
		v80_pdev = NULL;
	}

	/* Search for V80 device with P2P memory */
	while ((pdev = pci_get_device(v80_vendor,
				      v80_device < 0 ? PCI_ANY_ID : v80_device,
				      pdev)) != NULL) {
		/*
		 * Check if this device has P2P memory registered.
		 * pci_p2pdma_distance_many() returns >= 0 if P2P is available.
		 */
		if (pci_p2pdma_distance_many(pdev, NULL, 0, false) >= 0) {
			pr_info("Found V80 device %s with P2P memory\n",
				pci_name(pdev));
			v80_pdev = pdev;
			pci_dev_get(pdev);  /* Keep reference */
			mutex_unlock(&v80_lock);
			return pdev;
		}
	}

	mutex_unlock(&v80_lock);
	return NULL;
}

/**
 * open_nvme_bdev() - Open NVMe block device
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static struct file *open_nvme_bdev(const char *path)
{
	return bdev_file_open_by_path(path, BLK_OPEN_READ, NULL, NULL);
}
static inline struct block_device *get_bdev(struct file *f) { return file_bdev(f); }
static inline void close_nvme_bdev(struct file *f) { fput(f); }
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
static struct bdev_handle *open_nvme_bdev(const char *path)
{
	return bdev_open_by_path(path, BLK_OPEN_READ, NULL, NULL);
}
static inline struct block_device *get_bdev(struct bdev_handle *h) { return h->bdev; }
static inline void close_nvme_bdev(struct bdev_handle *h) { bdev_release(h); }
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
static struct block_device *open_nvme_bdev(const char *path)
{
	return blkdev_get_by_path(path, FMODE_READ, NULL);
}
static inline struct block_device *get_bdev(struct block_device *b) { return b; }
static inline void close_nvme_bdev(struct block_device *b) { blkdev_put(b, FMODE_READ); }
#else
static struct block_device *open_nvme_bdev(const char *path)
{
	return blkdev_get_by_path(path, FMODE_READ, NULL);
}
static inline struct block_device *get_bdev(struct block_device *b) { return b; }
static inline void close_nvme_bdev(struct block_device *b) { blkdev_put(b, FMODE_READ); }
#endif

/**
 * get_nvme_pci_dev() - Get PCI device from block device
 */
static struct pci_dev *get_nvme_pci_dev(struct block_device *bdev)
{
	struct device *dev;
	struct pci_dev *pdev = NULL;

	if (!bdev || !bdev->bd_disk || !bdev->bd_disk->queue)
		return NULL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	dev = bdev->bd_disk->queue->backing_dev_info->dev;
#else
	dev = bdev->bd_disk->queue->backing_dev_info->dev;
#endif

	/* Walk up the device tree to find PCI device */
	while (dev) {
		if (dev_is_pci(dev)) {
			pdev = to_pci_dev(dev);
			break;
		}
		dev = dev->parent;
	}

	return pdev;
}

/**
 * check_p2p_distance() - Check if P2P is possible between devices
 */
static int check_p2p_distance(struct pci_dev *nvme_pdev,
			      struct pci_dev *v80_pdev_local)
{
	int distance;
	struct pci_dev *clients[] = { nvme_pdev, NULL };

	distance = pci_p2pdma_distance_many(v80_pdev_local, clients, 1, true);

	if (distance < 0) {
		pr_warn("P2P not possible between NVMe %s and V80 %s: %d\n",
			pci_name(nvme_pdev), pci_name(v80_pdev_local), distance);
		pr_warn("Check: IOMMU settings, ACS, PCIe topology\n");
		return distance;
	}

	pr_info("P2P distance: NVMe %s <-> V80 %s = %d\n",
		pci_name(nvme_pdev), pci_name(v80_pdev_local), distance);

	return 0;
}

/**
 * do_p2p_nvme_to_hbm() - Execute P2P transfer from NVMe to HBM
 */
static int do_p2p_nvme_to_hbm(struct qdma_p2p_nvme_xfer *xfer,
			      struct qdma_p2p_nvme_status *status)
{
	struct pci_dev *p2p_dev = NULL;
	struct pci_dev *nvme_pdev = NULL;
	struct block_device *bdev = NULL;
	void *p2p_mem = NULL;
	struct bio *bio = NULL;
	struct p2p_bio_ctx ctx;
	ktime_t start_time, end_time;
	size_t transfer_size;
	size_t remaining;
	size_t chunk_size;
	u64 current_sector;
	u64 total_transferred = 0;
	int rv = 0;
	int pages_added;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	struct file *bdev_file = NULL;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	struct bdev_handle *bdev_handle = NULL;
#endif
	void *bdev_ref = NULL;

	memset(status, 0, sizeof(*status));

	/* Validate transfer size */
	transfer_size = xfer->num_sectors * 512;
	if (transfer_size == 0 || transfer_size > QDMA_P2P_NVME_MAX_XFER_SIZE) {
		snprintf(status->error_msg, sizeof(status->error_msg),
			 "Invalid transfer size: %zu (max: %llu)",
			 transfer_size, QDMA_P2P_NVME_MAX_XFER_SIZE);
		status->error_code = -EINVAL;
		return -EINVAL;
	}

	/* Find V80 device with P2P memory */
	p2p_dev = find_v80_device();
	if (!p2p_dev) {
		snprintf(status->error_msg, sizeof(status->error_msg),
			 "No V80 device with P2P memory found. "
			 "Ensure QDMA driver is loaded with hbm_bar parameter.");
		status->error_code = -ENODEV;
		return -ENODEV;
	}

	/* Open NVMe block device */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	bdev_file = open_nvme_bdev(xfer->nvme_dev);
	if (IS_ERR(bdev_file)) {
		rv = PTR_ERR(bdev_file);
		bdev_file = NULL;
		goto err_open;
	}
	bdev_ref = bdev_file;
	bdev = get_bdev(bdev_file);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	bdev_handle = open_nvme_bdev(xfer->nvme_dev);
	if (IS_ERR(bdev_handle)) {
		rv = PTR_ERR(bdev_handle);
		bdev_handle = NULL;
		goto err_open;
	}
	bdev_ref = bdev_handle;
	bdev = get_bdev(bdev_handle);
#else
	bdev = open_nvme_bdev(xfer->nvme_dev);
	if (IS_ERR(bdev)) {
		rv = PTR_ERR(bdev);
		bdev = NULL;
		goto err_open;
	}
	bdev_ref = bdev;
#endif

	/* Get NVMe PCI device for P2P path check */
	nvme_pdev = get_nvme_pci_dev(bdev);
	if (nvme_pdev) {
		rv = check_p2p_distance(nvme_pdev, p2p_dev);
		if (rv < 0) {
			snprintf(status->error_msg, sizeof(status->error_msg),
				 "P2P path not available between NVMe and V80");
			status->error_code = rv;
			goto err_close;
		}
	} else {
		pr_warn("Could not get NVMe PCI device, P2P path not verified\n");
	}

	pr_info("P2P transfer: %s sector %llu -> V80 HBM, size %zu bytes\n",
		xfer->nvme_dev, xfer->start_sector, transfer_size);

	start_time = ktime_get();

	/* Transfer in chunks */
	remaining = transfer_size;
	current_sector = xfer->start_sector;

	while (remaining > 0) {
		chunk_size = min_t(size_t, remaining, QDMA_P2P_NVME_CHUNK_SIZE);

		/* Allocate P2P memory from V80 HBM */
		p2p_mem = pci_alloc_p2pmem(p2p_dev, chunk_size);
		if (!p2p_mem) {
			snprintf(status->error_msg, sizeof(status->error_msg),
				 "Failed to allocate %zu bytes P2P memory from V80",
				 chunk_size);
			status->error_code = -ENOMEM;
			rv = -ENOMEM;
			goto err_close;
		}

		/* Create bio targeting P2P memory */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
		bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_KERNEL);
#else
		bio = bio_alloc(GFP_KERNEL, 1);
		if (bio) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
			bio_set_dev(bio, bdev);
			bio->bi_opf = REQ_OP_READ;
#else
			bio->bi_bdev = bdev;
			bio_set_op_attrs(bio, REQ_OP_READ, 0);
#endif
		}
#endif

		if (!bio) {
			pci_free_p2pmem(p2p_dev, p2p_mem, chunk_size);
			snprintf(status->error_msg, sizeof(status->error_msg),
				 "Failed to allocate bio");
			status->error_code = -ENOMEM;
			rv = -ENOMEM;
			goto err_close;
		}

		bio->bi_iter.bi_sector = current_sector;
		bio->bi_private = &ctx;
		bio->bi_end_io = p2p_bio_end_io;

		/*
		 * Add P2P pages to bio.
		 * The P2P memory is backed by the V80 HBM BAR, so when
		 * NVMe does DMA, it writes directly to HBM!
		 */
		pages_added = bio_add_page(bio, virt_to_page(p2p_mem),
					   chunk_size, offset_in_page(p2p_mem));
		if (pages_added != chunk_size) {
			pr_err("bio_add_page: added %d of %zu bytes\n",
			       pages_added, chunk_size);
			bio_put(bio);
			pci_free_p2pmem(p2p_dev, p2p_mem, chunk_size);
			snprintf(status->error_msg, sizeof(status->error_msg),
				 "bio_add_page failed (%d/%zu)", pages_added, chunk_size);
			status->error_code = -EIO;
			rv = -EIO;
			goto err_close;
		}

		/* Submit bio and wait */
		init_completion(&ctx.done);
		ctx.error = 0;

		submit_bio(bio);
		wait_for_completion(&ctx.done);

		if (ctx.error) {
			pr_err("NVMe I/O error: %d\n", ctx.error);
			pci_free_p2pmem(p2p_dev, p2p_mem, chunk_size);
			snprintf(status->error_msg, sizeof(status->error_msg),
				 "NVMe read I/O error: %d", ctx.error);
			status->error_code = ctx.error;
			rv = ctx.error;
			goto err_close;
		}

		/*
		 * SUCCESS! At this point:
		 * - NVMe controller DMA'd data directly to V80 HBM BAR
		 * - Data is now in HBM without ever touching CPU memory
		 * - True zero-copy P2P transfer achieved
		 */

		total_transferred += chunk_size;
		current_sector += chunk_size / 512;
		remaining -= chunk_size;

		/* Free P2P allocation (data remains in HBM) */
		pci_free_p2pmem(p2p_dev, p2p_mem, chunk_size);
		p2p_mem = NULL;
	}

	end_time = ktime_get();

	/* Fill success status */
	status->bytes_transferred = total_transferred;
	status->elapsed_ns = ktime_to_ns(ktime_sub(end_time, start_time));
	status->p2p_enabled = 1;
	status->error_code = 0;

	if (status->elapsed_ns > 0) {
		u64 bandwidth_mbps = (total_transferred * 1000ULL) /
				     (status->elapsed_ns / 1000000ULL);
		pr_info("P2P complete: %llu bytes, %llu ns, %llu MB/s\n",
			total_transferred, status->elapsed_ns, bandwidth_mbps);
	}

	rv = 0;

err_close:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	if (bdev_file)
		close_nvme_bdev(bdev_file);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	if (bdev_handle)
		close_nvme_bdev(bdev_handle);
#else
	if (bdev)
		close_nvme_bdev(bdev);
#endif
	pci_dev_put(p2p_dev);
	return rv;

err_open:
	snprintf(status->error_msg, sizeof(status->error_msg),
		 "Failed to open NVMe device %s: %d", xfer->nvme_dev, rv);
	status->error_code = rv;
	pci_dev_put(p2p_dev);
	return rv;
}

/**
 * get_p2p_info() - Get P2P device information
 */
static int get_p2p_info(struct qdma_p2p_nvme_info *info)
{
	struct pci_dev *pdev;
	struct resource *res;
	int i;

	memset(info, 0, sizeof(*info));

	pdev = find_v80_device();
	if (!pdev) {
		pr_warn("No V80 device with P2P enabled\n");
		return -ENODEV;
	}

	info->qdma_bdf = PCI_DEVID(pdev->bus->number, pdev->devfn);
	info->p2p_enabled = 1;

	/* Find largest BAR (likely HBM) */
	for (i = 0; i < PCI_NUM_RESOURCES; i++) {
		res = &pdev->resource[i];
		if (resource_size(res) > info->hbm_size) {
			info->hbm_bar = i;
			info->hbm_size = resource_size(res);
			info->hbm_phys = res->start;
		}
	}

	pci_dev_put(pdev);
	return 0;
}

#else /* !QDMA_P2P_NVME_SUPPORT */

static int do_p2p_nvme_to_hbm(struct qdma_p2p_nvme_xfer *xfer,
			      struct qdma_p2p_nvme_status *status)
{
	memset(status, 0, sizeof(*status));
	snprintf(status->error_msg, sizeof(status->error_msg),
		 "P2P NVMe not supported: requires kernel 5.1+");
	status->error_code = -ENOTSUPP;
	return -ENOTSUPP;
}

static int get_p2p_info(struct qdma_p2p_nvme_info *info)
{
	memset(info, 0, sizeof(*info));
	return -ENOTSUPP;
}

#endif /* QDMA_P2P_NVME_SUPPORT */

/* Character device operations */

static int p2p_nvme_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int p2p_nvme_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long p2p_nvme_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	int rv = 0;

	switch (cmd) {
	case QDMA_P2P_NVME_XFER: {
		struct qdma_p2p_nvme_xfer xfer;
		struct qdma_p2p_nvme_status status;

		if (copy_from_user(&xfer, (void __user *)arg, sizeof(xfer)))
			return -EFAULT;

		/* Ensure null-terminated device path */
		xfer.nvme_dev[sizeof(xfer.nvme_dev) - 1] = '\0';

		rv = do_p2p_nvme_to_hbm(&xfer, &status);

		/* Save last status */
		mutex_lock(&status_lock);
		memcpy(&last_status, &status, sizeof(last_status));
		mutex_unlock(&status_lock);

		return rv;
	}

	case QDMA_P2P_NVME_GET_STATUS: {
		mutex_lock(&status_lock);
		if (copy_to_user((void __user *)arg, &last_status,
				 sizeof(last_status))) {
			mutex_unlock(&status_lock);
			return -EFAULT;
		}
		mutex_unlock(&status_lock);
		return 0;
	}

	case QDMA_P2P_NVME_GET_INFO: {
		struct qdma_p2p_nvme_info info;

		rv = get_p2p_info(&info);
		if (rv)
			return rv;

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;

		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations p2p_nvme_fops = {
	.owner = THIS_MODULE,
	.open = p2p_nvme_open,
	.release = p2p_nvme_release,
	.unlocked_ioctl = p2p_nvme_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = p2p_nvme_ioctl,
#endif
};

/* Module init/exit */

static int __init qdma_p2p_nvme_init(void)
{
	int rv;

	pr_info("QDMA P2P NVMe module v1.0\n");
	pr_info("True zero-copy NVMe to V80 HBM transfers\n");

#ifndef QDMA_P2P_NVME_SUPPORT
	pr_warn("P2P support requires kernel 5.1+. Module loaded but transfers will fail.\n");
#endif

	/* Allocate character device number */
	rv = alloc_chrdev_region(&p2p_nvme_devno, 0, 1, QDMA_P2P_NVME_DEV_NAME);
	if (rv < 0) {
		pr_err("Failed to allocate chrdev region: %d\n", rv);
		return rv;
	}

	/* Create device class */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	p2p_nvme_class = class_create(QDMA_P2P_NVME_DEV_NAME);
#else
	p2p_nvme_class = class_create(THIS_MODULE, QDMA_P2P_NVME_DEV_NAME);
#endif
	if (IS_ERR(p2p_nvme_class)) {
		rv = PTR_ERR(p2p_nvme_class);
		pr_err("Failed to create device class: %d\n", rv);
		goto err_unregister;
	}

	/* Initialize and add cdev */
	cdev_init(&p2p_nvme_cdev, &p2p_nvme_fops);
	p2p_nvme_cdev.owner = THIS_MODULE;

	rv = cdev_add(&p2p_nvme_cdev, p2p_nvme_devno, 1);
	if (rv < 0) {
		pr_err("Failed to add cdev: %d\n", rv);
		goto err_class;
	}

	/* Create device node /dev/qdma_p2p_nvme */
	p2p_nvme_device = device_create(p2p_nvme_class, NULL, p2p_nvme_devno,
					NULL, QDMA_P2P_NVME_DEV_NAME);
	if (IS_ERR(p2p_nvme_device)) {
		rv = PTR_ERR(p2p_nvme_device);
		pr_err("Failed to create device: %d\n", rv);
		goto err_cdev;
	}

	pr_info("Device created: /dev/%s\n", QDMA_P2P_NVME_DEV_NAME);
	pr_info("Vendor filter: 0x%04x, Device filter: %s\n",
		v80_vendor, v80_device < 0 ? "any" : "specific");

	return 0;

err_cdev:
	cdev_del(&p2p_nvme_cdev);
err_class:
	class_destroy(p2p_nvme_class);
err_unregister:
	unregister_chrdev_region(p2p_nvme_devno, 1);
	return rv;
}

static void __exit qdma_p2p_nvme_exit(void)
{
	mutex_lock(&v80_lock);
	if (v80_pdev) {
		pci_dev_put(v80_pdev);
		v80_pdev = NULL;
	}
	mutex_unlock(&v80_lock);

	device_destroy(p2p_nvme_class, p2p_nvme_devno);
	cdev_del(&p2p_nvme_cdev);
	class_destroy(p2p_nvme_class);
	unregister_chrdev_region(p2p_nvme_devno, 1);

	pr_info("QDMA P2P NVMe module unloaded\n");
}

module_init(qdma_p2p_nvme_init);
module_exit(qdma_p2p_nvme_exit);
