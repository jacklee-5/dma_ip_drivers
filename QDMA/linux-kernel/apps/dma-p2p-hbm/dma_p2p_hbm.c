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
 * @file dma_p2p_hbm.c
 * @brief P2P DMA test application for writing files to V80 HBM
 *
 * This application reads a file (e.g., from NVMe storage) and writes it
 * to the V80 HBM memory via the QDMA MM (memory-mapped) interface.
 *
 * Usage:
 *   dma-p2p-hbm -d /dev/qdma01000-MM-0 -f /path/to/input/file [-a hbm_offset]
 *
 * The application demonstrates P2P DMA where:
 *   1. Data is read from storage (NVMe/SSD) into host memory
 *   2. Data is written to V80 HBM via QDMA MM device
 *
 * For true P2P (bypassing host memory), the peer device driver would need
 * to use the P2P DMA APIs to access the HBM BAR directly.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 500
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "dma_xfer_utils.c"

#define DEVICE_NAME_DEFAULT "/dev/qdma01000-MM-0"
#define HBM_OFFSET_DEFAULT  0
#define CHUNK_SIZE_DEFAULT  (4 * 1024 * 1024)  /* 4MB chunks */

static struct option const long_opts[] = {
	{"device", required_argument, NULL, 'd'},
	{"infile", required_argument, NULL, 'f'},
	{"address", required_argument, NULL, 'a'},
	{"chunk", required_argument, NULL, 'c'},
	{"verify", no_argument, NULL, 'V'},
	{"help", no_argument, NULL, 'h'},
	{"verbose", no_argument, NULL, 'v'},
	{0, 0, 0, 0}
};

static void usage(const char *name)
{
	fprintf(stdout, "%s - P2P DMA test: Write file to V80 HBM\n\n", name);
	fprintf(stdout, "Usage: %s [OPTIONS]\n\n", name);
	fprintf(stdout,
		"Read a file from storage (e.g., NVMe) and write to HBM via QDMA.\n\n");

	fprintf(stdout, "Options:\n");
	fprintf(stdout, "  -d, --device <dev>    QDMA device (default: %s)\n",
		DEVICE_NAME_DEFAULT);
	fprintf(stdout, "  -f, --infile <file>   Input file to read (REQUIRED)\n");
	fprintf(stdout, "  -a, --address <addr>  HBM offset address (default: 0x%x)\n",
		HBM_OFFSET_DEFAULT);
	fprintf(stdout, "  -c, --chunk <size>    Transfer chunk size (default: %d bytes)\n",
		CHUNK_SIZE_DEFAULT);
	fprintf(stdout, "  -V, --verify          Read back and verify data\n");
	fprintf(stdout, "  -v, --verbose         Verbose output\n");
	fprintf(stdout, "  -h, --help            Show this help message\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "Example:\n");
	fprintf(stdout, "  # Write a file from NVMe to HBM at offset 0\n");
	fprintf(stdout, "  %s -d /dev/qdma01000-MM-0 -f /mnt/nvme/data.bin\n", name);
	fprintf(stdout, "\n");
	fprintf(stdout, "  # Write with verification\n");
	fprintf(stdout, "  %s -d /dev/qdma01000-MM-0 -f /mnt/nvme/data.bin -V -v\n", name);
	fprintf(stdout, "\n");
}

static int get_file_size(const char *filename, uint64_t *size)
{
	struct stat st;

	if (stat(filename, &st) < 0) {
		fprintf(stderr, "Failed to stat file %s: %s\n",
			filename, strerror(errno));
		return -1;
	}

	*size = st.st_size;
	return 0;
}

static int write_file_to_hbm(const char *devname, const char *infile,
			     uint64_t hbm_addr, uint64_t chunk_size,
			     int do_verify)
{
	int rc = 0;
	int fpga_fd = -1;
	int infile_fd = -1;
	char *buffer = NULL;
	char *verify_buf = NULL;
	char *allocated = NULL;
	char *verify_allocated = NULL;
	uint64_t file_size = 0;
	uint64_t total_written = 0;
	uint64_t offset = 0;
	struct timespec ts_start, ts_end, ts_total_start, ts_total_end;
	double total_time = 0;
	double result;
	int chunk_count = 0;

	/* Get input file size */
	if (get_file_size(infile, &file_size) < 0) {
		return -1;
	}

	if (file_size == 0) {
		fprintf(stderr, "Input file is empty\n");
		return -1;
	}

	printf("P2P HBM Write Test\n");
	printf("==================\n");
	printf("Input file:   %s\n", infile);
	printf("File size:    %lu bytes (%.2f MB)\n",
	       file_size, (double)file_size / (1024 * 1024));
	printf("QDMA device:  %s\n", devname);
	printf("HBM address:  0x%lx\n", hbm_addr);
	printf("Chunk size:   %lu bytes (%.2f MB)\n",
	       chunk_size, (double)chunk_size / (1024 * 1024));
	printf("Verify:       %s\n", do_verify ? "yes" : "no");
	printf("\n");

	/* Open QDMA device */
	fpga_fd = open(devname, O_RDWR);
	if (fpga_fd < 0) {
		fprintf(stderr, "Failed to open QDMA device %s: %s\n",
			devname, strerror(errno));
		return -1;
	}

	/* Open input file */
	infile_fd = open(infile, O_RDONLY);
	if (infile_fd < 0) {
		fprintf(stderr, "Failed to open input file %s: %s\n",
			infile, strerror(errno));
		rc = -1;
		goto out;
	}

	/* Allocate aligned buffer for DMA */
	posix_memalign((void **)&allocated, 4096, chunk_size + 4096);
	if (!allocated) {
		fprintf(stderr, "Failed to allocate buffer: OOM\n");
		rc = -ENOMEM;
		goto out;
	}
	buffer = allocated;

	/* Allocate verify buffer if needed */
	if (do_verify) {
		posix_memalign((void **)&verify_allocated, 4096, chunk_size + 4096);
		if (!verify_allocated) {
			fprintf(stderr, "Failed to allocate verify buffer: OOM\n");
			rc = -ENOMEM;
			goto out;
		}
		verify_buf = verify_allocated;
	}

	if (verbose) {
		printf("Buffer allocated at %p (size: %lu)\n",
		       buffer, chunk_size + 4096);
	}

	/* Start total transfer timer */
	clock_gettime(CLOCK_MONOTONIC, &ts_total_start);

	/* Transfer file in chunks */
	printf("Transferring data to HBM...\n");
	while (total_written < file_size) {
		uint64_t bytes_to_read = chunk_size;
		ssize_t bytes_read;
		ssize_t bytes_written;
		uint64_t hbm_offset = hbm_addr + total_written;

		/* Adjust for last chunk */
		if (total_written + bytes_to_read > file_size) {
			bytes_to_read = file_size - total_written;
		}

		/* Read from input file */
		bytes_read = read_to_buffer((char *)infile, infile_fd, buffer,
					    bytes_to_read, offset);
		if (bytes_read < 0) {
			fprintf(stderr, "Failed to read from input file\n");
			rc = -1;
			goto out;
		}

		/* Write to HBM via QDMA */
		clock_gettime(CLOCK_MONOTONIC, &ts_start);

		bytes_written = write_from_buffer((char *)devname, fpga_fd,
						  buffer, bytes_read, hbm_offset);
		if (bytes_written < 0) {
			fprintf(stderr, "Failed to write to HBM at offset 0x%lx\n",
				hbm_offset);
			rc = -1;
			goto out;
		}

		clock_gettime(CLOCK_MONOTONIC, &ts_end);
		timespec_sub(&ts_end, &ts_start);
		total_time += (ts_end.tv_sec + ((double)ts_end.tv_nsec / 1000000000));

		total_written += bytes_written;
		offset += bytes_written;
		chunk_count++;

		if (verbose) {
			printf("  Chunk %d: wrote %zd bytes to HBM offset 0x%lx "
			       "(%.3f ms)\n",
			       chunk_count, bytes_written, hbm_offset,
			       (ts_end.tv_sec * 1000.0) +
			       (ts_end.tv_nsec / 1000000.0));
		}

		/* Verify if requested */
		if (do_verify) {
			ssize_t bytes_readback;

			bytes_readback = read_to_buffer((char *)devname, fpga_fd,
							verify_buf, bytes_written,
							hbm_offset);
			if (bytes_readback < 0) {
				fprintf(stderr, "Failed to read back from HBM\n");
				rc = -1;
				goto out;
			}

			if (memcmp(buffer, verify_buf, bytes_written) != 0) {
				fprintf(stderr, "VERIFICATION FAILED at offset 0x%lx!\n",
					hbm_offset);
				rc = -1;
				goto out;
			}

			if (verbose) {
				printf("  Chunk %d: verification passed\n", chunk_count);
			}
		}
	}

	/* Calculate total elapsed time */
	clock_gettime(CLOCK_MONOTONIC, &ts_total_end);
	timespec_sub(&ts_total_end, &ts_total_start);

	printf("\n");
	printf("Transfer Complete\n");
	printf("-----------------\n");
	printf("Total bytes written: %lu (%.2f MB)\n",
	       total_written, (double)total_written / (1024 * 1024));
	printf("Number of chunks:    %d\n", chunk_count);
	printf("Total time:          %.3f seconds\n",
	       ts_total_end.tv_sec + (double)ts_total_end.tv_nsec / 1000000000);
	printf("DMA write time:      %.3f seconds\n", total_time);

	/* Calculate and display bandwidth */
	if (total_time > 0) {
		result = (double)total_written / total_time;
		printf("Write bandwidth:     ");
		dump_throughput_result(total_written, result);
	}

	if (do_verify) {
		printf("Verification:        PASSED\n");
	}

	printf("\n");
	rc = 0;

out:
	if (fpga_fd >= 0)
		close(fpga_fd);
	if (infile_fd >= 0)
		close(infile_fd);
	if (allocated)
		free(allocated);
	if (verify_allocated)
		free(verify_allocated);

	return rc;
}

int main(int argc, char *argv[])
{
	int cmd_opt;
	char *device = DEVICE_NAME_DEFAULT;
	char *infile = NULL;
	uint64_t hbm_addr = HBM_OFFSET_DEFAULT;
	uint64_t chunk_size = CHUNK_SIZE_DEFAULT;
	int do_verify = 0;

	while ((cmd_opt = getopt_long(argc, argv, "vhVd:f:a:c:",
				      long_opts, NULL)) != -1) {
		switch (cmd_opt) {
		case 0:
			/* long option */
			break;
		case 'd':
			device = strdup(optarg);
			break;
		case 'f':
			infile = strdup(optarg);
			break;
		case 'a':
			hbm_addr = getopt_integer(optarg);
			break;
		case 'c':
			chunk_size = getopt_integer(optarg);
			break;
		case 'V':
			do_verify = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (cmd_opt == 'h') ? 0 : -1;
		}
	}

	/* Input file is required */
	if (!infile) {
		fprintf(stderr, "Error: Input file (-f) is required\n\n");
		usage(argv[0]);
		return -1;
	}

	/* Validate chunk size */
	if (chunk_size < 4096) {
		fprintf(stderr, "Warning: Chunk size increased to minimum 4096 bytes\n");
		chunk_size = 4096;
	}

	return write_file_to_hbm(device, infile, hbm_addr, chunk_size, do_verify);
}
