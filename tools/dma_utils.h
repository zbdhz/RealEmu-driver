/*
 * This file is part of the Xilinx DMA IP Core driver tools for Linux
 *
 * Copyright (c) 2016-present,  Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is licensed under BSD-style license (found in the
 * LICENSE file in the root directory of this source tree)
 */

#ifndef DMA_UTILS_H
#define DMA_UTILS_H

#include <stdint.h>
#include <sys/types.h>

#define RW_MAX_SIZE	0x7ffff000

extern int verbose;

uint64_t getopt_integer(char *optarg);

ssize_t read_to_buffer(char *fname, int fd, char *buffer, uint64_t size,
		     uint64_t base);

ssize_t write_from_buffer(char *fname, int fd, char *buffer, uint64_t size,
		      uint64_t base);

#endif /* DMA_UTILS_H */
