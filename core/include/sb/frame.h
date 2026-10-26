/*
 * UART framing: A5 5A | type u8 | seq u16 | len u16 | payload | crc32
 * All integers little-endian; crc32 covers type..payload. No byte stuffing:
 * a false SOF inside a payload is rejected by length bounds and the CRC.
 */
#ifndef SB_FRAME_H
#define SB_FRAME_H

#include <stdint.h>

#define SB_SOF0 0xA5u
#define SB_SOF1 0x5Au
#define SB_FRAME_MAX_PAYLOAD 1040u
#define SB_FRAME_OVERHEAD 11u

typedef struct {
    uint8_t type;
    uint16_t seq;
    uint16_t len;
    uint8_t payload[SB_FRAME_MAX_PAYLOAD];
} sb_frame_t;

typedef struct {
    uint8_t st;
    uint16_t pos;
    uint8_t hdr[5];
    uint8_t crcb[4];
    sb_frame_t f;
    uint32_t crc_errors;
} sb_frame_parser_t;

/* The parser must start zero-initialized; reset only drops a partial frame. */
void sb_frame_reset(sb_frame_parser_t *p);

/* Feed one byte; returns 1 when p->f holds a complete, CRC-valid frame. */
int sb_frame_feed(sb_frame_parser_t *p, uint8_t b);

/* Encode a frame into out (>= len + SB_FRAME_OVERHEAD bytes); returns size. */
uint32_t sb_frame_encode(uint8_t *out, uint8_t type, uint16_t seq, const uint8_t *payload,
                         uint16_t len);

static inline uint32_t sb_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void sb_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#endif
