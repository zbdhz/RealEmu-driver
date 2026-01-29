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
#include <sys/ioctl.h>

#include "../include/realemu_top.h"
#include "cdev_sgdma.h"
#include "dma_utils.h"

ssize_t write_bits_from_host_to_device(int fd, char *buffer, uint64_t size){
    //ssize_t write_from_buffer(char *fname, int fd, char *buffer, uint64_t size, uint64_t base)
    return write_from_buffer(DEVICE_H2C, fd, buffer, DATA_WIDTH_BYTE, DATA_BASE_ADDR);
}

ssize_t read_bits_from_device_to_host(int fd, char *buffer, uint64_t size){
    //ssize_t read_to_buffer(char *fname, int fd, char *buffer, uint64_t size, uint64_t base)
    return read_to_buffer(DEVICE_H2C, fd, buffer, DATA_WIDTH_BYTE, DATA_BASE_ADDR);

}