#include <sys/types.h>

ssize_t write_bits_from_host_to_device(int fd, char *buffer, uint64_t size);

ssize_t read_bits_from_device_to_host(int fd, char *buffer, uint64_t size);