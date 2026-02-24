/*
 * This file is part of the QDMA userspace application
 * to enable the user to execute the QDMA functionality
 *
 * Copyright (c) 2018-2022, Xilinx, Inc. All rights reserved.
 * Copyright (c) 2022-2024, Advanced Micro Devices, Inc. All rights reserved.
 *
 * This source code is licensed under BSD-style license (found in the
 * LICENSE file in the root directory of this source tree)
 */

/**
 * @file dma_p2p_nvme.c
 * @brief User-space utility for true P2P NVMe to HBM transfers
 *
 * This application uses the qdma_p2p_nvme kernel module to perform
 * zero-copy peer-to-peer DMA transfers from NVMe storage directly
 * to V80 HBM memory.
 *
 * Data path: NVMe --DMA--> PCIe fabric ---> V80 HBM (no CPU memory)
 *
 * Usage:
 *   dma-p2p-nvme -f /mnt/nvme/file.bin           # Transfer file to HBM
 *   dma-p2p-nvme -d /dev/nvme0n1 -s 0 -n 2048    # Transfer raw sectors
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#include "qdma_p2p_nvme.h"

#define P2P_NVME_DEV "/dev/qdma_p2p_nvme"

/* Maximum extents to handle */
#define MAX_EXTENTS 1024

static int verbose;

static struct option const long_opts[] = {
	{"device", required_argument, NULL, 'd'},
	{"sector", required_argument, NULL, 's'},
	{"count", required_argument, NULL, 'n'},
	{"offset", required_argument, NULL, 'o'},
	{"file", required_argument, NULL, 'f'},
	{"info", no_argument, NULL, 'i'},
	{"status", no_argument, NULL, 'S'},
	{"help", no_argument, NULL, 'h'},
	{"verbose", no_argument, NULL, 'v'},
	{0, 0, 0, 0}
};

static void usage(const char *name)
{
	fprintf(stdout, "%s - True P2P NVMe to V80 HBM Transfer\n\n", name);
	fprintf(stdout, "Usage: %s [OPTIONS]\n\n", name);
	fprintf(stdout,
		"Transfer data from NVMe directly to V80 HBM via P2P DMA.\n"
		"No CPU memory involved - true zero-copy transfer.\n\n");

	fprintf(stdout, "File transfer (recommended):\n");
	fprintf(stdout, "  -f, --file <path>     File on NVMe to transfer to HBM\n");
	fprintf(stdout, "  -o, --offset <bytes>  HBM offset (default: 0)\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "Raw sector transfer:\n");
	fprintf(stdout, "  -d, --device <dev>    NVMe device (e.g., /dev/nvme0n1)\n");
	fprintf(stdout, "  -s, --sector <num>    Starting sector (512-byte sectors)\n");
	fprintf(stdout, "  -n, --count <num>     Number of sectors to transfer\n");
	fprintf(stdout, "  -o, --offset <bytes>  HBM offset (default: 0)\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "Info options:\n");
	fprintf(stdout, "  -i, --info            Show P2P device information\n");
	fprintf(stdout, "  -S, --status          Show last transfer status\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "General options:\n");
	fprintf(stdout, "  -v, --verbose         Verbose output\n");
	fprintf(stdout, "  -h, --help            Show this help message\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "Examples:\n");
	fprintf(stdout, "  # Transfer file from mounted NVMe to HBM (auto-detects device)\n");
	fprintf(stdout, "  %s -f /mnt/nvme/data.bin\n\n", name);
	fprintf(stdout, "  # Transfer file to specific HBM offset\n");
	fprintf(stdout, "  %s -f /mnt/nvme/data.bin -o 0x100000\n\n", name);
	fprintf(stdout, "  # Transfer raw sectors (1MB from sector 0)\n");
	fprintf(stdout, "  %s -d /dev/nvme0n1 -s 0 -n 2048\n\n", name);
	fprintf(stdout, "  # Show P2P device info\n");
	fprintf(stdout, "  %s -i\n\n", name);
	fprintf(stdout, "Prerequisites:\n");
	fprintf(stdout, "  1. QDMA driver loaded with hbm_bar parameter\n");
	fprintf(stdout, "  2. qdma_p2p_nvme kernel module loaded\n");
	fprintf(stdout, "  3. NVMe and V80 on same PCIe root/switch (for P2P)\n");
}

static int show_info(int fd)
{
	struct qdma_p2p_nvme_info info;
	int rv;

	rv = ioctl(fd, QDMA_P2P_NVME_GET_INFO, &info);
	if (rv < 0) {
		fprintf(stderr, "IOCTL GET_INFO failed: %s\n", strerror(errno));
		if (errno == ENODEV) {
			fprintf(stderr, "\nNo V80 device with P2P memory found.\n");
			fprintf(stderr, "Ensure QDMA driver is loaded with hbm_bar parameter.\n");
		}
		return -1;
	}

	printf("P2P Device Information\n");
	printf("======================\n");
	printf("V80 BDF:       %02x:%02x.%x\n",
	       (info.qdma_bdf >> 8) & 0xFF,
	       (info.qdma_bdf >> 3) & 0x1F,
	       info.qdma_bdf & 0x7);
	printf("HBM BAR:       %u\n", info.hbm_bar);
	printf("HBM Size:      %llu bytes (%.2f GB)\n",
	       (unsigned long long)info.hbm_size,
	       (double)info.hbm_size / (1024.0 * 1024 * 1024));
	printf("HBM Physical:  0x%llx\n", (unsigned long long)info.hbm_phys);
	printf("P2P Enabled:   %s\n", info.p2p_enabled ? "Yes" : "No");

	return 0;
}

static int show_status(int fd)
{
	struct qdma_p2p_nvme_status status;
	int rv;

	rv = ioctl(fd, QDMA_P2P_NVME_GET_STATUS, &status);
	if (rv < 0) {
		fprintf(stderr, "IOCTL GET_STATUS failed: %s\n", strerror(errno));
		return -1;
	}

	printf("Last Transfer Status\n");
	printf("====================\n");

	if (status.error_code == 0) {
		double elapsed_ms = status.elapsed_ns / 1000000.0;
		double bandwidth_mbps = 0;

		if (status.elapsed_ns > 0) {
			bandwidth_mbps = (status.bytes_transferred * 1000.0) /
					 (status.elapsed_ns / 1000000.0);
		}

		printf("Status:        SUCCESS\n");
		printf("Bytes:         %llu (%.2f MB)\n",
		       (unsigned long long)status.bytes_transferred,
		       status.bytes_transferred / (1024.0 * 1024));
		printf("Time:          %.3f ms\n", elapsed_ms);
		printf("Bandwidth:     %.2f MB/s\n", bandwidth_mbps);
		printf("P2P Used:      %s\n", status.p2p_enabled ? "Yes" : "No");
	} else {
		printf("Status:        FAILED (error %d)\n", status.error_code);
		printf("Error:         %s\n", status.error_msg);
	}

	return 0;
}

/**
 * get_block_device_from_file() - Get the block device path for a file
 * @filepath: Path to the file
 * @blkdev: Output buffer for block device path
 * @blkdev_size: Size of output buffer
 *
 * Returns 0 on success, -1 on error.
 */
static int get_block_device_from_file(const char *filepath, char *blkdev,
				      size_t blkdev_size)
{
	struct stat file_st, dev_st;
	FILE *fp;
	char line[512];
	char dev_path[256];
	dev_t file_dev;
	int found = 0;

	if (stat(filepath, &file_st) < 0) {
		fprintf(stderr, "Cannot stat %s: %s\n", filepath, strerror(errno));
		return -1;
	}

	file_dev = file_st.st_dev;

	/* Read /proc/mounts to find the mount point and device */
	fp = fopen("/proc/mounts", "r");
	if (!fp) {
		fprintf(stderr, "Cannot open /proc/mounts: %s\n", strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char device[256], mountpoint[256], fstype[64];

		if (sscanf(line, "%255s %255s %63s", device, mountpoint, fstype) >= 2) {
			/* Check if this is an NVMe device */
			if (strncmp(device, "/dev/nvme", 9) != 0)
				continue;

			if (stat(device, &dev_st) == 0) {
				/* Check if this device matches the file's device */
				if (dev_st.st_rdev == file_dev ||
				    major(dev_st.st_rdev) == major(file_dev)) {
					/* Found it - get the base NVMe device (without partition) */
					strncpy(blkdev, device, blkdev_size - 1);
					blkdev[blkdev_size - 1] = '\0';

					/* Strip partition number if present (e.g., nvme0n1p1 -> nvme0n1) */
					char *p = strstr(blkdev, "nvme");
					if (p) {
						/* Find 'p' followed by digit (partition) */
						char *part = strchr(p + 4, 'p');
						if (part && part[1] >= '0' && part[1] <= '9') {
							*part = '\0';
						}
					}
					found = 1;
					break;
				}
			}
		}
	}

	fclose(fp);

	if (!found) {
		fprintf(stderr, "Could not find NVMe device for %s\n", filepath);
		fprintf(stderr, "Make sure the file is on an NVMe filesystem\n");
		return -1;
	}

	return 0;
}

/**
 * struct file_extent - A contiguous file extent on disk
 */
struct file_extent {
	uint64_t logical_offset;   /* Offset within file */
	uint64_t physical_sector;  /* Physical sector on device */
	uint64_t length_sectors;   /* Length in sectors */
};

/**
 * get_file_extents() - Get physical extents of a file using FIEMAP
 * @filepath: Path to the file
 * @extents: Output array of extents
 * @max_extents: Maximum number of extents to return
 * @num_extents: Output - number of extents found
 *
 * Returns 0 on success, -1 on error.
 */
static int get_file_extents(const char *filepath, struct file_extent *extents,
			    int max_extents, int *num_extents)
{
	int fd;
	struct stat st;
	struct fiemap *fiemap;
	struct fiemap_extent *fm_ext;
	size_t fiemap_size;
	int i, rv = 0;

	fd = open(filepath, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", filepath, strerror(errno));
		return -1;
	}

	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "Cannot stat %s: %s\n", filepath, strerror(errno));
		close(fd);
		return -1;
	}

	if (st.st_size == 0) {
		fprintf(stderr, "File %s is empty\n", filepath);
		close(fd);
		return -1;
	}

	/* Allocate fiemap structure */
	fiemap_size = sizeof(struct fiemap) + max_extents * sizeof(struct fiemap_extent);
	fiemap = calloc(1, fiemap_size);
	if (!fiemap) {
		fprintf(stderr, "Memory allocation failed\n");
		close(fd);
		return -1;
	}

	/* Request all extents */
	fiemap->fm_start = 0;
	fiemap->fm_length = st.st_size;
	fiemap->fm_flags = FIEMAP_FLAG_SYNC;
	fiemap->fm_extent_count = max_extents;

	/* Get extent map */
	if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
		fprintf(stderr, "FIEMAP ioctl failed: %s\n", strerror(errno));
		fprintf(stderr, "The filesystem may not support FIEMAP\n");
		rv = -1;
		goto out;
	}

	if (fiemap->fm_mapped_extents == 0) {
		fprintf(stderr, "No extents found for file (sparse file?)\n");
		rv = -1;
		goto out;
	}

	/* Convert to our extent format */
	*num_extents = 0;
	for (i = 0; i < fiemap->fm_mapped_extents && i < max_extents; i++) {
		fm_ext = &fiemap->fm_extents[i];

		/* Skip unwritten/delalloc extents */
		if (fm_ext->fe_flags & (FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_DELALLOC)) {
			if (verbose) {
				printf("  Skipping unwritten extent %d\n", i);
			}
			continue;
		}

		extents[*num_extents].logical_offset = fm_ext->fe_logical;
		extents[*num_extents].physical_sector = fm_ext->fe_physical / 512;
		extents[*num_extents].length_sectors = fm_ext->fe_length / 512;

		if (verbose) {
			printf("  Extent %d: file_off=%llu phys_sector=%llu len=%llu sectors\n",
			       *num_extents,
			       (unsigned long long)fm_ext->fe_logical,
			       (unsigned long long)(fm_ext->fe_physical / 512),
			       (unsigned long long)(fm_ext->fe_length / 512));
		}

		(*num_extents)++;
	}

	if (*num_extents == 0) {
		fprintf(stderr, "No valid extents found\n");
		rv = -1;
	}

out:
	free(fiemap);
	close(fd);
	return rv;
}

/**
 * do_transfer() - Execute a single P2P transfer
 */
static int do_transfer(int p2p_fd, const char *nvme_dev, uint64_t start_sector,
		       uint64_t num_sectors, uint64_t hbm_offset)
{
	struct qdma_p2p_nvme_xfer xfer;
	struct qdma_p2p_nvme_status status;
	int rv;

	memset(&xfer, 0, sizeof(xfer));
	strncpy(xfer.nvme_dev, nvme_dev, sizeof(xfer.nvme_dev) - 1);
	xfer.start_sector = start_sector;
	xfer.num_sectors = num_sectors;
	xfer.hbm_offset = hbm_offset;
	xfer.qdma_bdf = -1;
	xfer.flags = QDMA_P2P_NVME_FLAG_SYNC;

	rv = ioctl(p2p_fd, QDMA_P2P_NVME_XFER, &xfer);
	if (rv < 0) {
		ioctl(p2p_fd, QDMA_P2P_NVME_GET_STATUS, &status);
		fprintf(stderr, "Transfer failed: %s\n", status.error_msg);
		return -1;
	}

	return 0;
}

/**
 * transfer_file() - Transfer a file from NVMe to HBM using P2P DMA
 * @p2p_fd: File descriptor for P2P device
 * @filepath: Path to file on NVMe
 * @hbm_offset: Starting offset in HBM
 *
 * This function:
 * 1. Determines which NVMe device the file is on
 * 2. Uses FIEMAP to get physical extents of the file
 * 3. Transfers each extent via P2P DMA
 */
static int transfer_file(int p2p_fd, const char *filepath, uint64_t hbm_offset)
{
	char nvme_dev[256];
	struct file_extent extents[MAX_EXTENTS];
	struct stat st;
	int num_extents = 0;
	int i, rv;
	uint64_t total_bytes = 0;
	uint64_t current_hbm_offset = hbm_offset;
	struct qdma_p2p_nvme_status status;
	uint64_t total_elapsed_ns = 0;

	printf("P2P File Transfer\n");
	printf("=================\n");
	printf("File: %s\n", filepath);

	/* Get file size */
	if (stat(filepath, &st) < 0) {
		fprintf(stderr, "Cannot stat %s: %s\n", filepath, strerror(errno));
		return -1;
	}
	printf("Size: %llu bytes (%.2f MB)\n",
	       (unsigned long long)st.st_size,
	       st.st_size / (1024.0 * 1024));

	/* Find the NVMe device for this file */
	rv = get_block_device_from_file(filepath, nvme_dev, sizeof(nvme_dev));
	if (rv < 0) {
		return -1;
	}
	printf("NVMe Device: %s\n", nvme_dev);

	/* Get file extents using FIEMAP */
	printf("\nMapping file extents...\n");
	rv = get_file_extents(filepath, extents, MAX_EXTENTS, &num_extents);
	if (rv < 0) {
		return -1;
	}
	printf("Found %d extent(s)\n", num_extents);

	/* Check for fragmentation */
	if (num_extents > 1) {
		printf("\nNote: File has %d extents (fragmented). Each extent will\n", num_extents);
		printf("      be transferred separately via P2P DMA.\n");
	}

	printf("\nTransferring to HBM offset 0x%llx...\n",
	       (unsigned long long)hbm_offset);

	/* Transfer each extent */
	for (i = 0; i < num_extents; i++) {
		uint64_t bytes = extents[i].length_sectors * 512;

		if (verbose) {
			printf("\n  Extent %d/%d:\n", i + 1, num_extents);
			printf("    Sector: %llu, Length: %llu sectors (%llu bytes)\n",
			       (unsigned long long)extents[i].physical_sector,
			       (unsigned long long)extents[i].length_sectors,
			       (unsigned long long)bytes);
			printf("    HBM offset: 0x%llx\n",
			       (unsigned long long)current_hbm_offset);
		}

		rv = do_transfer(p2p_fd, nvme_dev,
				 extents[i].physical_sector,
				 extents[i].length_sectors,
				 current_hbm_offset);
		if (rv < 0) {
			fprintf(stderr, "Failed to transfer extent %d\n", i);
			return -1;
		}

		/* Get status for timing */
		ioctl(p2p_fd, QDMA_P2P_NVME_GET_STATUS, &status);
		total_elapsed_ns += status.elapsed_ns;

		total_bytes += bytes;
		current_hbm_offset += bytes;

		if (!verbose) {
			printf("  Extent %d/%d: %llu bytes transferred\n",
			       i + 1, num_extents, (unsigned long long)bytes);
		}
	}

	/* Summary */
	printf("\nTransfer Complete!\n");
	printf("------------------\n");
	printf("Total Bytes:   %llu (%.2f MB)\n",
	       (unsigned long long)total_bytes,
	       total_bytes / (1024.0 * 1024));
	printf("Extents:       %d\n", num_extents);

	if (total_elapsed_ns > 0) {
		double elapsed_ms = total_elapsed_ns / 1000000.0;
		double bandwidth_mbps = (total_bytes * 1000.0) / (total_elapsed_ns / 1000000.0);
		printf("Elapsed Time:  %.3f ms\n", elapsed_ms);
		printf("Bandwidth:     %.2f MB/s\n", bandwidth_mbps);
	}

	printf("HBM Location:  0x%llx - 0x%llx\n",
	       (unsigned long long)hbm_offset,
	       (unsigned long long)(hbm_offset + total_bytes - 1));

	return 0;
}

/**
 * transfer_sectors() - Transfer raw sectors from NVMe to HBM
 */
static int transfer_sectors(int p2p_fd, const char *nvme_dev,
			    uint64_t start_sector, uint64_t num_sectors,
			    uint64_t hbm_offset)
{
	struct qdma_p2p_nvme_status status;
	int rv;
	double elapsed_ms, bandwidth_mbps;
	uint64_t transfer_size = num_sectors * 512;

	printf("P2P Sector Transfer\n");
	printf("===================\n");
	printf("NVMe Device:   %s\n", nvme_dev);
	printf("Start Sector:  %llu\n", (unsigned long long)start_sector);
	printf("Num Sectors:   %llu\n", (unsigned long long)num_sectors);
	printf("Transfer Size: %llu bytes (%.2f MB)\n",
	       (unsigned long long)transfer_size,
	       transfer_size / (1024.0 * 1024));
	printf("HBM Offset:    0x%llx\n", (unsigned long long)hbm_offset);
	printf("\n");

	if (verbose)
		printf("Submitting P2P transfer...\n");

	rv = do_transfer(p2p_fd, nvme_dev, start_sector, num_sectors, hbm_offset);
	if (rv < 0) {
		return -1;
	}

	/* Get status */
	ioctl(p2p_fd, QDMA_P2P_NVME_GET_STATUS, &status);

	elapsed_ms = status.elapsed_ns / 1000000.0;
	bandwidth_mbps = 0;
	if (status.elapsed_ns > 0) {
		bandwidth_mbps = (status.bytes_transferred * 1000.0) /
				 (status.elapsed_ns / 1000000.0);
	}

	printf("Transfer Complete!\n");
	printf("------------------\n");
	printf("Bytes Transferred: %llu\n",
	       (unsigned long long)status.bytes_transferred);
	printf("Elapsed Time:      %.3f ms\n", elapsed_ms);
	printf("Bandwidth:         %.2f MB/s\n", bandwidth_mbps);
	printf("P2P Path Used:     %s\n", status.p2p_enabled ? "Yes" : "No");
	printf("\nData is now in V80 HBM at offset 0x%llx\n",
	       (unsigned long long)hbm_offset);

	return 0;
}

int main(int argc, char *argv[])
{
	int cmd_opt;
	int fd;
	int rv = 0;
	int do_info = 0;
	int do_status_only = 0;
	char *nvme_dev = NULL;
	char *filepath = NULL;
	uint64_t start_sector = 0;
	uint64_t num_sectors = 0;
	uint64_t hbm_offset = 0;
	int have_sector = 0;
	int have_count = 0;

	while ((cmd_opt = getopt_long(argc, argv, "d:s:n:o:f:iShv",
				      long_opts, NULL)) != -1) {
		switch (cmd_opt) {
		case 'd':
			nvme_dev = strdup(optarg);
			break;
		case 's':
			start_sector = strtoull(optarg, NULL, 0);
			have_sector = 1;
			break;
		case 'n':
			num_sectors = strtoull(optarg, NULL, 0);
			have_count = 1;
			break;
		case 'o':
			hbm_offset = strtoull(optarg, NULL, 0);
			break;
		case 'f':
			filepath = strdup(optarg);
			break;
		case 'i':
			do_info = 1;
			break;
		case 'S':
			do_status_only = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (cmd_opt == 'h') ? 0 : 1;
		}
	}

	/* Open the P2P NVMe device */
	fd = open(P2P_NVME_DEV, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n",
			P2P_NVME_DEV, strerror(errno));
		if (errno == ENOENT) {
			fprintf(stderr, "\nThe qdma_p2p_nvme kernel module is not loaded.\n");
			fprintf(stderr, "Load it with: insmod qdma_p2p_nvme.ko\n");
		}
		return 1;
	}

	/* Handle info request */
	if (do_info) {
		rv = show_info(fd);
		close(fd);
		return rv < 0 ? 1 : 0;
	}

	/* Handle status request */
	if (do_status_only) {
		rv = show_status(fd);
		close(fd);
		return rv < 0 ? 1 : 0;
	}

	/* File mode - transfer file using FIEMAP */
	if (filepath) {
		rv = transfer_file(fd, filepath, hbm_offset);
		close(fd);
		free(filepath);
		free(nvme_dev);
		return rv < 0 ? 1 : 0;
	}

	/* Sector mode - need device, sector, and count */
	if (!nvme_dev) {
		fprintf(stderr, "Error: Need either -f <file> or -d <device> -s <sector> -n <count>\n\n");
		usage(argv[0]);
		close(fd);
		return 1;
	}

	if (!have_sector || !have_count) {
		fprintf(stderr, "Error: Need -s <sector> and -n <count> for raw sector mode\n\n");
		usage(argv[0]);
		close(fd);
		free(nvme_dev);
		return 1;
	}

	if (num_sectors == 0) {
		fprintf(stderr, "Error: Number of sectors must be > 0\n");
		close(fd);
		free(nvme_dev);
		return 1;
	}

	/* Do sector transfer */
	rv = transfer_sectors(fd, nvme_dev, start_sector, num_sectors, hbm_offset);

	close(fd);
	free(nvme_dev);

	return rv < 0 ? 1 : 0;
}
