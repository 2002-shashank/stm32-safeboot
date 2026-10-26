#include "sb/boot.h"

#include <string.h>

#include "sb/flash.h"
#include "sb/layout.h"

static int verify_slot(unsigned slot, const sb_prov_t *prov, sb_mark_fn mark, uint32_t *version)
{
    int rc = sb_image_verify(sb_slot_addr(slot), SB_SLOT_SIZE, prov, mark);
    if (rc == SB_V_OK)
        *version = ((const sb_image_hdr_t *)sb_flash_ptr(sb_slot_addr(slot)))->image_version;
    return rc;
}

static void choose(sb_boot_decision_t *out, unsigned slot, uint8_t reason, uint32_t version)
{
    out->target = (uint8_t)slot;
    out->reason = reason;
    out->region_addr = sb_slot_addr(slot);
    out->version = version;
}

/* Empty journal (factory state): adopt the newest verified image, if any. */
static int adopt(sb_journal_t *j, const sb_prov_t *prov, sb_mark_fn mark, sb_boot_decision_t *out)
{
    sb_record_t r;
    sb_record_init(&r);
    uint32_t ver[2];
    int ok[2];
    for (unsigned s = 0; s < SB_NUM_SLOTS; s++) {
        ok[s] = verify_slot(s, prov, mark, &ver[s]) == SB_V_OK;
        if (ok[s]) {
            r.state[s] = SB_ST_VALID;
            r.version[s] = ver[s];
        }
    }
    if (!ok[0] && !ok[1])
        return 0;
    unsigned best = ok[0] && (!ok[1] || ver[0] >= ver[1]) ? 0 : 1;
    r.state[best] = SB_ST_CONFIRMED;
    r.active = (uint8_t)best;
    r.min_version = ver[best];
    if (sb_journal_append(j, &r) != 0) {
        out->reason = SB_R_JOURNAL_ERROR;
        return 0;
    }
    choose(out, best, SB_R_ADOPTED, ver[best]);
    return 1;
}

static int boot_recovery_image(const sb_prov_t *prov, sb_mark_fn mark, sb_boot_decision_t *out)
{
    int rc = sb_image_verify(SB_RECOVERY_ADDR, SB_RECOVERY_SIZE, prov, mark);
    if (rc != SB_V_OK)
        return 0;
    out->target = SB_BOOT_RECOVERY_IMAGE;
    out->reason = SB_R_RECOVERY;
    out->region_addr = SB_RECOVERY_ADDR;
    out->version = ((const sb_image_hdr_t *)sb_flash_ptr(SB_RECOVERY_ADDR))->image_version;
    return 1;
}

void sb_boot_decide(sb_journal_t *j, const sb_prov_t *prov, sb_mark_fn mark,
                    sb_boot_decision_t *out)
{
    memset(out, 0, sizeof *out);
    out->target = SB_BOOT_UPDATE_MODE;
    out->reason = SB_R_NOTHING;

    sb_journal_scan(j);
    if (!j->have_cur) {
        if (adopt(j, prov, mark, out))
            return;
        if (out->reason != SB_R_JOURNAL_ERROR)
            boot_recovery_image(prov, mark, out);
        return;
    }

    /* Each pass either boots or persists a state change that removes the
     * option it just rejected, so the loop runs at most a few times. */
    for (int pass = 0; pass < 4; pass++) {
        sb_record_t r = j->cur;
        unsigned c = r.candidate;
        uint32_t ver;

        if (c < SB_NUM_SLOTS && (r.state[c] == SB_ST_PENDING || r.state[c] == SB_ST_TRIAL)) {
            if (r.attempts >= r.max_attempts) {
                /* Out of attempts: the candidate never confirmed. */
                r.state[c] = SB_ST_ROLLBACK;
                r.candidate = SB_SLOT_NONE;
                r.attempts = 0;
                if (sb_journal_append(j, &r) != 0)
                    goto journal_error;
                out->reason = SB_R_ROLLBACK;
                continue;
            }
            int rc = verify_slot(c, prov, mark, &ver);
            if (rc != SB_V_OK || ver != r.version[c] || ver < r.min_version) {
                out->last_verify = rc;
                r.state[c] = SB_ST_INVALID;
                r.candidate = SB_SLOT_NONE;
                r.attempts = 0;
                if (sb_journal_append(j, &r) != 0)
                    goto journal_error;
                continue;
            }
            /* Count the attempt before jumping so a crash or hang counts. */
            r.state[c] = SB_ST_TRIAL;
            r.attempts++;
            if (sb_journal_append(j, &r) != 0)
                goto journal_error;
            uint8_t reason = out->reason == SB_R_PROMOTED ? SB_R_PROMOTED : SB_R_TRIAL;
            choose(out, c, reason, ver);
            out->trial = 1;
            out->attempts = r.attempts;
            return;
        }

        unsigned a = r.active;
        if (a < SB_NUM_SLOTS && r.state[a] == SB_ST_CONFIRMED) {
            int rc = verify_slot(a, prov, mark, &ver);
            if (rc == SB_V_OK && ver == r.version[a] && ver >= r.min_version) {
                uint8_t reason = out->reason == SB_R_ROLLBACK ? SB_R_ROLLBACK : SB_R_CONFIRMED;
                choose(out, a, reason, ver);
                return;
            }
            out->last_verify = rc;
            r.state[a] = SB_ST_INVALID;
            r.active = SB_SLOT_NONE;
            /* A verified, not-yet-activated image in the other slot that
             * still passes anti-rollback gets a trial instead. */
            unsigned o = a ^ 1u;
            if (r.state[o] == SB_ST_VALID && r.version[o] >= r.min_version) {
                r.state[o] = SB_ST_PENDING;
                r.candidate = (uint8_t)o;
                r.attempts = 0;
                out->reason = SB_R_PROMOTED;
            }
            if (sb_journal_append(j, &r) != 0)
                goto journal_error;
            continue;
        }
        break;
    }

    out->target = SB_BOOT_UPDATE_MODE;
    out->reason = SB_R_NOTHING;
    boot_recovery_image(prov, mark, out);
    return;

journal_error:
    /* Without a working journal nothing can be counted, so never boot a
     * trial image; prefer UART recovery. */
    out->target = SB_BOOT_UPDATE_MODE;
    out->reason = SB_R_JOURNAL_ERROR;
}

int sb_boot_confirm(sb_journal_t *j, unsigned booted_slot)
{
    sb_journal_scan(j);
    if (!j->have_cur || booted_slot >= SB_NUM_SLOTS)
        return -1;
    sb_record_t r = j->cur;
    if (r.active == booted_slot && r.state[booted_slot] == SB_ST_CONFIRMED)
        return 0;   /* already confirmed: no flash write */
    if (r.candidate != booted_slot || r.state[booted_slot] != SB_ST_TRIAL)
        return -1;
    if (r.active < SB_NUM_SLOTS && r.state[r.active] == SB_ST_CONFIRMED)
        r.state[r.active] = SB_ST_VALID;   /* stays as an image, below the floor */
    r.state[booted_slot] = SB_ST_CONFIRMED;
    r.active = (uint8_t)booted_slot;
    r.candidate = SB_SLOT_NONE;
    r.attempts = 0;
    if (r.version[booted_slot] > r.min_version)
        r.min_version = r.version[booted_slot];
    return sb_journal_append(j, &r);
}
