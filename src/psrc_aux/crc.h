#ifndef AUX_CRC_H
#define AUX_CRC_H

#include <stdint.h>
#include <stddef.h>

uint32_t crc32(const uint8_t*, size_t);
uint64_t crc64(const uint8_t*, size_t);
uint32_t strcrc32(const char*);
uint64_t strcrc64(const char*);

#endif
