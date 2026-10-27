/*
 * Resumable UART update protocol (device side). Transport-independent: the
 * caller feeds decoded frames and sends back the response frame.
 * See docs/protocol.md.
 */
#ifndef SB_PROTO_H
#define SB_PROTO_H

#include <stdint.h>

#include "sb/image.h"
#include "sb/journal.h"

enum sb_cmd {
    SB_CMD_BEGIN_UPDATE = 0x01,   /* session u32 */
    SB_CMD_IMAGE_INFO = 0x02,     /* 512 B header */
    SB_CMD_DATA = 0x03,           /* offset u32, crc32 u32, data[4..1024] */
    SB_CMD_QUERY_PROGRESS = 0x04, /* - */
    SB_CMD_FINALIZE = 0x05,       /* total u32 */
    SB_CMD_VERIFY = 0x06,         /* activate u8 */
    SB_CMD_REBOOT = 0x07,         /* - */
    SB_CMD_SET_BAUD = 0x08,       /* baud u32; handled by the transport, not the core:
                                   * reply at the old rate (a = actual baud), then switch */
    SB_RESP_FLAG = 0x80,
};

enum sb_status {
    SB_S_OK = 0,
    SB_S_ERR_SEQ = 1,       /* offset != next_offset; resend from next_offset */
    SB_S_ERR_CRC = 2,
    SB_S_ERR_STATE = 3,     /* command not valid now */
    SB_S_ERR_HDR = 4,
    SB_S_ERR_VERSION = 5,   /* rejected by anti-rollback */
    SB_S_ERR_RANGE = 6,
    SB_S_ERR_FLASH = 7,
    SB_S_ERR_VERIFY = 8,
    SB_S_RESTART = 9,       /* resend IMAGE_INFO and start from offset 0 */
    SB_S_ERR_LEN = 10,
    SB_S_ERR_CMD = 11,
};

#define SB_DATA_MAX 1024u
#define SB_CKPT_INTERVAL 0x4000u   /* journal checkpoint every 16 KB */
#define SB_RESP_LEN 16u

/* Response payload (16 bytes):
 *   status u8, slot u8, slot_state u8, rsv u8, next_offset u32, a u32, b u32
 * BEGIN: a = max payload, b = session.  QUERY: a = version of slot,
 * b = active session or 0.  VERIFY: a = verify result (int32), b = version. */

typedef struct {
    sb_journal_t *j;
    const sb_prov_t *prov;
    sb_mark_fn mark;
    uint8_t open;
    uint8_t hdr_ok;
    uint8_t slot;
    uint8_t reboot;          /* set by REBOOT; caller resets after replying */
    uint32_t session;
    uint32_t image_size;
    uint32_t next_offset;    /* payload offset expected next */
    uint32_t erased_end;     /* flash address up to which this session erased */
    uint32_t ckpt;           /* last journaled rx_offset */
    uint8_t have_last;
    uint8_t last_type;
    uint16_t last_seq;
    uint8_t last_resp[SB_RESP_LEN];
} sb_proto_t;

void sb_proto_init(sb_proto_t *p, sb_journal_t *j, const sb_prov_t *prov, sb_mark_fn mark);

/* Handle one request; writes SB_RESP_LEN bytes to resp. A retransmitted
 * request (same type and seq as the previous one) gets the cached reply. */
void sb_proto_handle(sb_proto_t *p, uint8_t type, uint16_t seq, const uint8_t *payload,
                     uint16_t len, uint8_t resp[SB_RESP_LEN]);

#endif
