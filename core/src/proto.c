#include "sb/proto.h"

#include <string.h>

#include "sb/boot.h"
#include "sb/crc32.h"
#include "sb/flash.h"
#include "sb/frame.h"
#include "sb/layout.h"

void sb_proto_init(sb_proto_t *p, sb_journal_t *j, const sb_prov_t *prov, sb_mark_fn mark)
{
    memset(p, 0, sizeof *p);
    p->j = j;
    p->prov = prov;
    p->mark = mark;
    p->slot = SB_SLOT_NONE;
}

static sb_record_t current(const sb_proto_t *p)
{
    sb_record_t r;
    if (p->j->have_cur)
        return p->j->cur;
    sb_record_init(&r);
    return r;
}

/* Updates always target the slot that is not running the confirmed image. */
static unsigned target_slot(const sb_record_t *r)
{
    return r->active < SB_NUM_SLOTS ? r->active ^ 1u : 0u;
}

static uint32_t slot_base(const sb_proto_t *p)
{
    return sb_slot_addr(p->slot);
}

static uint32_t payload_addr(const sb_proto_t *p, uint32_t off)
{
    return slot_base(p) + SB_HDR_SIZE + off;
}

static int checkpoint(sb_proto_t *p, uint32_t off)
{
    sb_record_t r = current(p);
    r.rx_offset = off;
    if (sb_journal_append(p->j, &r) != 0)
        return -1;
    p->ckpt = off;
    return 0;
}

static int erase_sector_at(sb_proto_t *p, uint32_t addr)
{
    int sec = sb_sector_of(addr);
    if (sec < 0 || sb_flash_erase_sector((unsigned)sec) != 0)
        return -1;
    p->erased_end = sb_sector_addr((unsigned)sec) + sb_sector_size((unsigned)sec);
    return 0;
}

static int begin_update(sb_proto_t *p, uint32_t session, uint32_t *a)
{
    sb_record_t r = current(p);
    unsigned t = target_slot(&r);
    *a = SB_SLOT_MAX_PAYLOAD;
    if (p->open && p->session == session && p->slot == t)
        return SB_S_OK;   /* host reconnected mid-session: keep RAM progress */
    p->slot = (uint8_t)t;
    p->session = session;
    p->hdr_ok = 0;
    p->open = 1;

    if ((r.flags & SB_RF_RX) && r.rx_session == session && r.rx_slot == t && r.rx_offset > 0) {
        /* Resume. The header is in flash from the interrupted session and
         * must match the IMAGE_INFO the host sends next. */
        const sb_image_hdr_t *h = (const sb_image_hdr_t *)sb_flash_ptr(slot_base(p));
        if (sb_image_check_header(h, slot_base(p), SB_SLOT_SIZE) == SB_V_OK &&
            r.rx_offset <= h->image_size) {
            p->image_size = h->image_size;
            p->next_offset = p->ckpt = r.rx_offset;
            uint32_t pos = payload_addr(p, r.rx_offset);
            int sec = sb_sector_of(pos);
            /* The sector holding pos was erased by this session unless pos
             * is exactly its first byte. */
            p->erased_end = sb_sector_addr((unsigned)sec) == pos
                                ? pos
                                : sb_sector_addr((unsigned)sec) + sb_sector_size((unsigned)sec);
            return SB_S_OK;
        }
    }

    /* New session: record intent (slot t is no longer an image) before any
     * erase touches it. */
    r.flags |= SB_RF_RX;
    r.rx_session = session;
    r.rx_slot = (uint8_t)t;
    r.rx_offset = 0;
    r.state[t] = SB_ST_EMPTY;
    r.version[t] = 0;
    if (r.candidate == t) {
        r.candidate = SB_SLOT_NONE;
        r.attempts = 0;
    }
    if (sb_journal_append(p->j, &r) != 0) {
        p->open = 0;
        return SB_S_ERR_FLASH;
    }
    p->next_offset = p->ckpt = 0;
    p->image_size = 0;
    p->erased_end = slot_base(p);
    return SB_S_OK;
}

static int image_info(sb_proto_t *p, const uint8_t *pl, uint16_t len)
{
    if (!p->open)
        return SB_S_ERR_STATE;
    if (len != SB_HDR_SIZE)
        return SB_S_ERR_LEN;
    sb_image_hdr_t h;
    memcpy(&h, pl, sizeof h);
    if (sb_image_check_header(&h, slot_base(p), SB_SLOT_SIZE) != SB_V_OK)
        return SB_S_ERR_HDR;
    if (p->prov == NULL || h.key_id != p->prov->key_id)
        return SB_S_ERR_HDR;
    sb_record_t r = current(p);
    if (!sb_version_installable(&r, h.image_version))
        return SB_S_ERR_VERSION;

    if (p->next_offset > 0) {
        if (memcmp(sb_flash_ptr(slot_base(p)), &h, sizeof h) == 0) {
            p->hdr_ok = 1;
            return SB_S_OK;
        }
        /* Different image under the same session id: start over. */
        if (checkpoint(p, 0) != 0)
            return SB_S_ERR_FLASH;
        p->next_offset = 0;
    }
    if (erase_sector_at(p, slot_base(p)) != 0)
        return SB_S_ERR_FLASH;
    if (sb_flash_program(slot_base(p), &h, sizeof h) != 0 ||
        memcmp(sb_flash_ptr(slot_base(p)), &h, sizeof h) != 0)
        return SB_S_ERR_FLASH;
    p->image_size = h.image_size;
    p->hdr_ok = 1;
    return SB_S_OK;
}

/* Flash under a resumed region did not match the incoming data, so the
 * sector must be erased and resent. Lower the journal checkpoint first so a
 * reboot never resumes past erased flash. */
static int rewind_sector(sb_proto_t *p, uint32_t addr)
{
    int sec = sb_sector_of(addr);
    uint32_t start = sb_sector_addr((unsigned)sec);
    if (start <= slot_base(p)) {
        if (checkpoint(p, 0) != 0)
            return SB_S_ERR_FLASH;
        p->next_offset = 0;
        p->hdr_ok = 0;
        p->erased_end = slot_base(p);
        return SB_S_RESTART;
    }
    uint32_t off = start - slot_base(p) - SB_HDR_SIZE;
    if (checkpoint(p, off) != 0 || erase_sector_at(p, start) != 0)
        return SB_S_ERR_FLASH;
    p->next_offset = off;
    return SB_S_ERR_SEQ;
}

static int data(sb_proto_t *p, const uint8_t *pl, uint16_t len)
{
    if (!p->open || !p->hdr_ok)
        return SB_S_ERR_STATE;
    if (len < 12 || len > 8 + SB_DATA_MAX || (len & 3u) != 0)
        return SB_S_ERR_LEN;
    uint32_t off = sb_get_le32(pl);
    uint32_t crc = sb_get_le32(pl + 4);
    const uint8_t *d = pl + 8;
    uint32_t n = len - 8u;
    if (sb_crc32(0, d, n) != crc)
        return SB_S_ERR_CRC;
    if (off + n <= p->next_offset)
        return SB_S_OK;   /* duplicate: already written, just re-ACK */
    if (off != p->next_offset)
        return SB_S_ERR_SEQ;
    if (off + n > p->image_size)
        return SB_S_ERR_RANGE;

    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t addr = payload_addr(p, off + i);
        if (addr >= p->erased_end && erase_sector_at(p, addr) != 0)
            return SB_S_ERR_FLASH;
        uint32_t w = sb_get_le32(d + i);
        uint32_t fw = sb_flash_word(addr);
        if (fw == w)
            continue;   /* already written before an interruption */
        if (fw != SB_FLASH_ERASED_WORD)
            return rewind_sector(p, addr);
        if (sb_flash_program(addr, d + i, 4) != 0 || sb_flash_word(addr) != w)
            return rewind_sector(p, addr);
    }
    p->next_offset = off + n;
    if (p->next_offset / SB_CKPT_INTERVAL > p->ckpt / SB_CKPT_INTERVAL &&
        checkpoint(p, p->next_offset) != 0)
        return SB_S_ERR_FLASH;
    return SB_S_OK;
}

static int finalize(sb_proto_t *p, const uint8_t *pl, uint16_t len)
{
    if (!p->open || !p->hdr_ok)
        return SB_S_ERR_STATE;
    if (len != 4)
        return SB_S_ERR_LEN;
    if (sb_get_le32(pl) != p->image_size || p->next_offset != p->image_size)
        return SB_S_ERR_RANGE;
    if (p->ckpt != p->image_size && checkpoint(p, p->image_size) != 0)
        return SB_S_ERR_FLASH;
    return SB_S_OK;
}

static int verify(sb_proto_t *p, const uint8_t *pl, uint16_t len, uint32_t *a, uint32_t *b)
{
    if (!p->open || !p->hdr_ok || p->next_offset != p->image_size)
        return SB_S_ERR_STATE;
    if (len != 1)
        return SB_S_ERR_LEN;
    int rc = sb_image_verify(slot_base(p), SB_SLOT_SIZE, p->prov, p->mark);
    const sb_image_hdr_t *h = (const sb_image_hdr_t *)sb_flash_ptr(slot_base(p));
    sb_record_t r = current(p);
    *a = (uint32_t)rc;
    *b = h->image_version;
    if (rc == SB_V_OK && !sb_version_installable(&r, h->image_version))
        rc = SB_V_BAD_LAYOUT;
    r.flags &= (uint8_t)~SB_RF_RX;
    r.rx_session = 0;
    r.rx_offset = 0;
    r.rx_slot = SB_SLOT_NONE;
    if (rc == SB_V_OK) {
        r.state[p->slot] = SB_ST_VALID;
        r.version[p->slot] = h->image_version;
        if (pl[0]) {
            r.state[p->slot] = SB_ST_PENDING;
            r.candidate = p->slot;
            r.attempts = 0;
        }
    } else {
        r.state[p->slot] = SB_ST_INVALID;
    }
    if (sb_journal_append(p->j, &r) != 0)
        return SB_S_ERR_FLASH;
    p->open = 0;
    return rc == SB_V_OK ? SB_S_OK : SB_S_ERR_VERIFY;
}

void sb_proto_handle(sb_proto_t *p, uint8_t type, uint16_t seq, const uint8_t *payload,
                     uint16_t len, uint8_t resp[SB_RESP_LEN])
{
    if (p->have_last && p->last_type == type && p->last_seq == seq) {
        memcpy(resp, p->last_resp, SB_RESP_LEN);
        return;
    }
    uint32_t a = 0, b = 0;
    int st;
    switch (type) {
    case SB_CMD_BEGIN_UPDATE:
        st = len == 4 ? begin_update(p, sb_get_le32(payload), &a) : SB_S_ERR_LEN;
        b = p->session;
        break;
    case SB_CMD_IMAGE_INFO:
        st = image_info(p, payload, len);
        break;
    case SB_CMD_DATA:
        st = data(p, payload, len);
        break;
    case SB_CMD_QUERY_PROGRESS: {
        sb_record_t r = current(p);
        if (!p->open) {
            p->slot = (uint8_t)target_slot(&r);
            p->next_offset = (r.flags & SB_RF_RX) && r.rx_slot == p->slot ? r.rx_offset : 0;
        }
        a = r.version[p->slot];
        b = p->open ? p->session : (r.flags & SB_RF_RX) ? r.rx_session : 0;
        st = SB_S_OK;
        break;
    }
    case SB_CMD_FINALIZE:
        st = finalize(p, payload, len);
        break;
    case SB_CMD_VERIFY:
        st = verify(p, payload, len, &a, &b);
        break;
    case SB_CMD_REBOOT:
        p->reboot = 1;
        st = SB_S_OK;
        break;
    default:
        st = SB_S_ERR_CMD;
        break;
    }

    sb_record_t r = current(p);
    resp[0] = (uint8_t)st;
    resp[1] = p->slot;
    resp[2] = p->slot < SB_NUM_SLOTS ? r.state[p->slot] : 0;
    resp[3] = 0;
    sb_put_le32(resp + 4, p->next_offset);
    sb_put_le32(resp + 8, a);
    sb_put_le32(resp + 12, b);
    p->have_last = 1;
    p->last_type = type;
    p->last_seq = seq;
    memcpy(p->last_resp, resp, SB_RESP_LEN);
}
