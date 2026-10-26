#include "sb/frame.h"

#include <string.h>

#include "sb/crc32.h"

enum { ST_SOF0, ST_SOF1, ST_HDR, ST_PAYLOAD, ST_CRC };

void sb_frame_reset(sb_frame_parser_t *p)
{
    p->st = ST_SOF0;
    p->pos = 0;
}

int sb_frame_feed(sb_frame_parser_t *p, uint8_t b)
{
    switch (p->st) {
    case ST_SOF0:
        if (b == SB_SOF0)
            p->st = ST_SOF1;
        return 0;
    case ST_SOF1:
        p->st = b == SB_SOF1 ? ST_HDR : b == SB_SOF0 ? ST_SOF1 : ST_SOF0;
        p->pos = 0;
        return 0;
    case ST_HDR:
        p->hdr[p->pos++] = b;
        if (p->pos < sizeof p->hdr)
            return 0;
        p->f.type = p->hdr[0];
        p->f.seq = (uint16_t)(p->hdr[1] | p->hdr[2] << 8);
        p->f.len = (uint16_t)(p->hdr[3] | p->hdr[4] << 8);
        p->pos = 0;
        if (p->f.len > SB_FRAME_MAX_PAYLOAD)
            p->st = ST_SOF0;
        else
            p->st = p->f.len ? ST_PAYLOAD : ST_CRC;
        return 0;
    case ST_PAYLOAD:
        p->f.payload[p->pos++] = b;
        if (p->pos == p->f.len) {
            p->pos = 0;
            p->st = ST_CRC;
        }
        return 0;
    case ST_CRC:
        p->crcb[p->pos++] = b;
        if (p->pos < 4)
            return 0;
        p->st = ST_SOF0;
        uint32_t crc = sb_crc32(sb_crc32(0, p->hdr, sizeof p->hdr), p->f.payload, p->f.len);
        if (crc == sb_get_le32(p->crcb))
            return 1;
        p->crc_errors++;
        return 0;
    }
    p->st = ST_SOF0;
    return 0;
}

uint32_t sb_frame_encode(uint8_t *out, uint8_t type, uint16_t seq, const uint8_t *payload,
                         uint16_t len)
{
    out[0] = SB_SOF0;
    out[1] = SB_SOF1;
    out[2] = type;
    out[3] = (uint8_t)seq;
    out[4] = (uint8_t)(seq >> 8);
    out[5] = (uint8_t)len;
    out[6] = (uint8_t)(len >> 8);
    if (len)
        memcpy(out + 7, payload, len);
    sb_put_le32(out + 7 + len, sb_crc32(0, out + 2, 5u + len));
    return SB_FRAME_OVERHEAD + len;
}
