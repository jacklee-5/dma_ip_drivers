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
 *   dma-p2p-nvme -d /dev/nvme0n1 -s <start_sector> -n <num_sectors>
 */

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

#include "qdma_p2p_nvme.h"

#define P2P_NVME_DEV "/dev/qdma_p2p_nvme"

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

	fprintf(stdout, "Transfer options:\n");
	fprintf(stdout, "  -d, --device <dev>    NVMe device (e.g., /dev/nvme0n1)\n");
	fprintf(stdout, "  -s, --sector <num>    Starting sector (512-byte sectors)\n");
	fprintf(stdout, "  -n, --count <num>     Number of sectors to transfer\n");
	fprintf(stdout, "  -o, --offset <bytes>  HBM offset (default: 0)\n");
	fprintf(stdout, "  -f, --file <path>     Transfer file (calculates sectors)\n");
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
	fprintf(stdout, "  # Transfer 1MB (2048 sectors) from sector 0\n");
	fprintf(stdout, "  %s -d /dev/nvme0n1 -s 0 -n 2048\n\n", name);
	fprintf(stdout, "  # Transfer file contents to HBM\n");
	fprintf(stdout, "  %s -d /dev/nvme0n1 -f /mnt/nvme/data.bin\n\n", name);
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

static int do_transfer(int fd, const char *nvme_dev, uint64_t start_sector,
		       uint64_t num_sectors, uint64_t hbm_offset)
{
	struct qdma_p2p_nvme_xfer xfer;
	struct qdma_p2p_nvme_status status;
	int rv;
	double elapsed_ms, bandwidth_mbps;
	uint64_t transfer_size;

	memset(&xfer, 0, sizeof(xfer));
	strncpy(xfer.nvme_dev, nvme_dev, sizeof(xfer.nvme_dev) - 1);
	xfer.start_sector = start_sector;
	xfer.num_sectors = num_sectors;
	xfer.hbm_offset = hbm_offset;
	xfer.qdma_bdf = -1;  /* Auto-detect */
	xfer.flags = QDMA_P2P_NVME_FLAG_SYNC;

	transfer_size = num_sectors * 512;

	printf("P2P Transfer Request\n");
	printf("====================\n");
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

	rv = ioctl(fd, QDMA_P2P_NVME_XFER, &xfer);

	/* Get status regardless of return value */
	ioctl(fd, QDMA_P2P_NVME_GET_STATUS, &status);

	if (rv < 0) {
		fprintf(stderr, "P2P Transfer FAILED\n");
		fprintf(stderr, "Error code: %d\n", status.error_code);
		fprintf(stderr, "Error msg:  %s\n", status.error_msg);
		return -1;
	}

	/* Success */
	elapsed_ms = status.elapsed_ns / 1000000.0;
	bandwidth_mbps = 0;
	if (status.elapsed_ns > 0) {
		bandwidth_mbps = (status.bytes_transferred * 1000.0) /
				 (status.elapsed_ns / 1000000.0);
	}

	printf("P2P Transfer Complete!\n");
	printf("----------------------\n");
	printf("Bytes Transferred: %llu\n",
	       (unsigned long long)status.bytes_transferred);
	printf("Elapsed Time:      %.3f ms\n", elapsed_ms);
	printf("Bandwidth:         %.2f MB/s\n", bandwidth_mbps);
	printf("P2P Path Used:     %s\n", status.p2p_enabled ? "Yes" : "No");
	printf("\nData is now in V80 HBM at offset 0x%llx\n",
	       (unsigned long long)hbm_offset);

	return 0;
}

static int get_file_sectors(const char *nvme_dev, const char *filepath,
			    uint64_t *start_sector, uint64_t *num_sectors)
{
	struct stat st;
	int nvme_fd;
	char nvme_path[256];
	off_t file_offset;

	/*
	 * For this to work, the file must be on the NVMe device and we need
	 * to find its physical location. This is simplified - in practice
	 * you'd use FIEMAP or FIBMAP ioctls to get physical extents.
	 *
	 * For now, we just calculate based on file size.
	 */
	if (stat(filepath, &st) < 0) {
		fprintf(stderr, "Cannot stat file %s: %s\n",
			filepath, strerror(errno));
		return -1;
	}

	if (st.st_size == 0) {
		fprintf(stderr, "File %s is empty\n", filepath);
		return -1;
	}

	/* For simplicity, assume file starts at sector 0 */
	/* In a real implementation, use FIEMAP/FIBMAP */
	*start_sector = 0;
	*num_sectors = (st.st_size + 511) / 512;  /* Round up */

	printf("File: %s\n", filepath);
	printf("Size: %llu bytes -> %llu sectors\n",
	       (unsigned long long)st.st_size,
	       (unsigned long long)*num_sectors);
	printf("\nNote: Using sector 0 as start. For actual file location,\n");
	printf("      use FIEMAP/FIBMAP to get physical extents.\n\n");

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

	/* Transfer mode - need NVMe device */
	if (!nvme_dev) {
		fprintf(stderr, "Error: NVMe device (-d) is required for transfer\n\n");
		usage(argv[0]);
		close(fd);
		return 1;
	}

	/* If file is specified, get sectors from file */
	if (filepath) {
		if (get_file_sectors(nvme_dev, filepath,
				     &start_sector, &num_sectors) < 0) {
			close(fd);
			return 1;
		}
	} else {
		/* Need sector and count */
		if (!have_sector || !have_count) {
			fprintf(stderr, "Error: Need -s and -n options, or -f for file\n\n");
			usage(argv[0]);
			close(fd);
			return 1;
		}
	}

	/* Validate */
	if (num_sectors == 0) {
		fprintf(stderr, "Error: Number of sectors must be > 0\n");
		close(fd);
		return 1;
	}

	/* Do the transfer */
	rv = do_transfer(fd, nvme_dev, start_sector, num_sectors, hbm_offset);

	close(fd);
	free(nvme_dev);
	free(filepath);

	return rv < 0 ? 1 : 0;
}
