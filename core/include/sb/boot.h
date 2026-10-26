/*
 * Boot decision state machine (EMPTY/VALID/PENDING/TRIAL/CONFIRMED/INVALID/
 * ROLLBACK). Pure policy on top of the journal and image verification; the
 * target code only performs the jump the decision asks for.
 */
#ifndef SB_BOOT_H
#define SB_BOOT_H

#include <stdint.h>

#include "sb/image.h"
#include "sb/journal.h"

enum sb_boot_target {
    SB_BOOT_SLOT_A = 0,
    SB_BOOT_SLOT_B = 1,
    SB_BOOT_RECOVERY_IMAGE = 2,
    SB_BOOT_UPDATE_MODE = 3,   /* nothing bootable: stay in UART recovery */
};

enum sb_boot_reason {
    SB_R_CONFIRMED = 1,   /* normal boot of the confirmed image */
    SB_R_TRIAL,           /* trial boot of a candidate */
    SB_R_ROLLBACK,        /* candidate exhausted attempts; booted previous */
    SB_R_ADOPTED,         /* empty journal; adopted a factory image */
    SB_R_PROMOTED,        /* active failed verification; other slot on trial */
    SB_R_RECOVERY,        /* both slots unusable; golden image */
    SB_R_NOTHING,         /* nothing bootable */
    SB_R_JOURNAL_ERROR,   /* journal could not be written */
};

typedef struct {
    uint8_t target;       /* enum sb_boot_target */
    uint8_t reason;       /* enum sb_boot_reason */
    uint8_t trial;        /* 1 if this is a trial boot (start IWDG) */
    uint8_t attempts;     /* trial attempt number */
    uint32_t region_addr; /* header address of the image to boot */
    uint32_t version;
    int last_verify;      /* verify result of the last image rejected, if any */
} sb_boot_decision_t;

/* Scan the journal, apply the policy, persist any state change, and return
 * where to boot. Every image is fully verified before it is chosen. */
void sb_boot_decide(sb_journal_t *j, const sb_prov_t *prov, sb_mark_fn mark,
                    sb_boot_decision_t *out);

/* Called (through the API table) by an app running from booted_slot after it
 * initialized successfully. Returns 0 if the slot is now CONFIRMED. */
int sb_boot_confirm(sb_journal_t *j, unsigned booted_slot);

/* Accept a new candidate image only if its version passes anti-rollback. */
static inline int sb_version_installable(const sb_record_t *r, uint32_t version)
{
    return version > r->min_version;
}

#endif
