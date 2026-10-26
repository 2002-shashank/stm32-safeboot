#ifndef SB_CRC32_H
#define SB_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32/ISO-HDLC (same as zlib.crc32). Pass 0 as crc to start. */
uint32_t sb_crc32(uint32_t crc, const void *data, size_t len);

#endif
