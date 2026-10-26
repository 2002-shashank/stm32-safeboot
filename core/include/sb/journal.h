/*
 * Power-fail-atomic boot journal: append-only 64-byte records in two
 * ping-pong flash sectors. The valid record with the highest seq is the
 * current state. See docs/journal.md.
 */
#ifndef SB_JOURNAL_H
#define SB_JOURNAL_H

#include <stdint.h>

#define SB_REC_MAGIC   0x4A425321u   /* "!SBJ" */
#define SB_REC_COMMIT  0x5AFEC0DEu
#define SB_REC_SIZE    64u

enum sb_slot_state {
    SB_ST_EMPTY = 1,
    SB_ST_VALID = 2,
    SB_ST_PENDING = 3,
    SB_ST_TRIAL = 4,
    SB_ST_CONFIRMED = 5,
    SB_ST_INVALID = 6,
    SB_ST_ROLLBACK = 7,
};

/* flags */
#define SB_RF_RX 0x01u   /* a UART transfer into rx_slot is in progress */

#define SB_DEFAULT_MAX_ATTEMPTS 3u

typedef struct {
    uint32_t magic;          /*  0 */
    uint32_t seq;            /*  4 */
    uint8_t state[2];        /*  8 */
    uint8_t active;          /* 10  SB_SLOT_NONE if none */
    uint8_t candidate;       /* 11  SB_SLOT_NONE if none */
    uint32_t version[2];     /* 12 */
    uint32_t min_version;    /* 20  anti-rollback floor, never decreases */
    uint8_t attempts;        /* 24  trial boots of candidate so far */
    uint8_t max_attempts;    /* 25 */
    uint8_t rx_slot;         /* 26 */
    uint8_t flags;           /* 27 */
    uint32_t rx_session;     /* 28 */
    uint32_t rx_offset;      /* 32  payload bytes received and verified */
    uint32_t reserved[5];    /* 36 */
    uint32_t crc;            /* 56  CRC32 of bytes 0..55 */
    uint32_t commit;         /* 60  SB_REC_COMMIT, programmed last */
} sb_record_t;

_Static_assert(sizeof(sb_record_t) == SB_REC_SIZE, "record must be 64 bytes");

typedef struct {
    sb_record_t cur;     /* current state; valid only if have_cur */
    int have_cur;
    unsigned page;       /* page (0/1) the next append goes to */
    uint32_t next_off;   /* offset in that page after the last non-erased record */
} sb_journal_t;

/* Fills a fresh logical state (no images, no active slot). */
void sb_record_init(sb_record_t *r);

/* Scan both pages and load the current state. Always succeeds; have_cur is 0
 * if no valid record exists. */
void sb_journal_scan(sb_journal_t *j);

/* Append rec as the new current state (seq/crc/commit are filled in).
 * Compacts into the other page when the current one is full. Returns 0 on
 * success, negative on persistent flash failure. */
int sb_journal_append(sb_journal_t *j, const sb_record_t *rec);

/* Logical equality: all fields except seq/crc/commit/magic. */
int sb_record_same_state(const sb_record_t *a, const sb_record_t *b);

#endif
