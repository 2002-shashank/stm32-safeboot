#include "sb/journal.h"

#include <stddef.h>
#include <string.h>

#include "sb/crc32.h"
#include "sb/flash.h"
#include "sb/layout.h"

#define BODY_LEN offsetof(sb_record_t, crc)        /* bytes covered by crc */
#define PRE_COMMIT_LEN offsetof(sb_record_t, commit)

static uint32_t page_addr(unsigned page)
{
    return sb_sector_addr(page ? SB_JOURNAL_SECTOR1 : SB_JOURNAL_SECTOR0);
}

static unsigned page_sector(unsigned page)
{
    return page ? SB_JOURNAL_SECTOR1 : SB_JOURNAL_SECTOR0;
}

static int is_erased(uint32_t addr, uint32_t len)
{
    const uint8_t *p = sb_flash_ptr(addr);
    for (uint32_t i = 0; i < len; i++)
        if (p[i] != 0xFFu)
            return 0;
    return 1;
}

static int record_valid(const sb_record_t *r)
{
    return r->commit == SB_REC_COMMIT && r->magic == SB_REC_MAGIC &&
           r->crc == sb_crc32(0, r, BODY_LEN);
}

void sb_record_init(sb_record_t *r)
{
    memset(r, 0, sizeof *r);
    r->magic = SB_REC_MAGIC;
    r->state[0] = r->state[1] = SB_ST_EMPTY;
    r->active = r->candidate = SB_SLOT_NONE;
    r->max_attempts = SB_DEFAULT_MAX_ATTEMPTS;
    r->rx_slot = SB_SLOT_NONE;
    memset(r->reserved, 0xFF, sizeof r->reserved);
}

int sb_record_same_state(const sb_record_t *a, const sb_record_t *b)
{
    return memcmp((const uint8_t *)a + 8, (const uint8_t *)b + 8, BODY_LEN - 8) == 0;
}

/* Offset just past the last non-erased record slot in a page. */
static uint32_t page_end(unsigned page)
{
    uint32_t base = page_addr(page);
    for (uint32_t off = SB_JOURNAL_PAGE_SIZE; off > 0; off -= SB_REC_SIZE)
        if (!is_erased(base + off - SB_REC_SIZE, SB_REC_SIZE))
            return off;
    return 0;
}

void sb_journal_scan(sb_journal_t *j)
{
    j->have_cur = 0;
    j->page = 0;
    for (unsigned page = 0; page < 2; page++) {
        uint32_t base = page_addr(page);
        for (uint32_t off = 0; off < SB_JOURNAL_PAGE_SIZE; off += SB_REC_SIZE) {
            sb_record_t r;
            memcpy(&r, sb_flash_ptr(base + off), sizeof r);
            if (!record_valid(&r))
                continue;
            if (!j->have_cur || r.seq > j->cur.seq) {
                j->cur = r;
                j->have_cur = 1;
                j->page = page;
            }
        }
    }
    j->next_off = page_end(j->page);
}

/* Write one record into the erased slot at addr. Body first, commit word
 * last, each verified by read-back. Returns 0 if the record is now valid. */
static int write_record(uint32_t addr, const sb_record_t *r)
{
    if (sb_flash_program(addr, r, PRE_COMMIT_LEN) != 0)
        return -1;
    if (memcmp(sb_flash_ptr(addr), r, PRE_COMMIT_LEN) != 0)
        return -1;
    if (sb_flash_program(addr + PRE_COMMIT_LEN, &r->commit, sizeof r->commit) != 0)
        return -1;
    return memcmp(sb_flash_ptr(addr), r, sizeof *r) == 0 ? 0 : -1;
}

int sb_journal_append(sb_journal_t *j, const sb_record_t *rec)
{
    sb_record_t r = *rec;
    r.magic = SB_REC_MAGIC;
    r.seq = j->have_cur ? j->cur.seq + 1 : 1;
    r.crc = sb_crc32(0, &r, BODY_LEN);
    r.commit = SB_REC_COMMIT;

    /* Bounded: at most two compactions (one retry if the fresh page fails). */
    for (int compactions = 0; compactions <= 2;) {
        if (j->next_off + SB_REC_SIZE > SB_JOURNAL_PAGE_SIZE) {
            /* The current record is in j->page; erasing the other page
             * never touches it. */
            unsigned other = j->page ^ 1u;
            compactions++;
            if (sb_flash_erase_sector(page_sector(other)) != 0)
                continue;
            j->page = other;
            j->next_off = 0;
        }
        uint32_t addr = page_addr(j->page) + j->next_off;
        j->next_off += SB_REC_SIZE;
        /* Never program over non-erased space; torn records stay skipped. */
        if (!is_erased(addr, SB_REC_SIZE))
            continue;
        if (write_record(addr, &r) == 0) {
            j->cur = r;
            j->have_cur = 1;
            return 0;
        }
    }
    return -1;
}
