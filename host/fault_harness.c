/*
 * Power-cut fault harness.
 *
 * Each scenario is a restartable "driver": a loop of reboot -> bootloader
 * decision -> host/app action until a goal state is reached. A reference run
 * records every flash op and every journal state it commits. Then, for each
 * op k and each cut variant (op not done / done / torn), the scenario is
 * replayed from the same initial flash with power lost at op k, followed by:
 *   1. the journal must scan to exactly the state before or after op k;
 *   2. every boot decision must pick an image that verifies, passes
 *      anti-rollback and is allowed for the scenario (checked on every reboot);
 *   3. min_version never decreases;
 *   4. the driver, restarted with no more cuts, must reach the same goal;
 *   5. no word is ever programmed over non-erased flash;
 *   6. liveness: an image the host sent correctly never fails VERIFY (a
 *      failure means resume wrote or skipped data incorrectly).
 *
 * Usage: fault_harness [--quick] [--csv path]
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flash_sim.h"
#include "sb/boot.h"
#include "sb/crc32.h"
#include "sb/flash.h"
#include "sb/frame.h"
#include "sb/journal.h"
#include "sb/proto.h"
#include "testutil.h"

#define NUM_VARIANTS FSIM_NUM_VARIANTS
#define MAX_REBOOTS 12

static sb_journal_t J;
static const sb_prov_t *PROV;
static uint8_t snapshot[SB_FLASH_SIZE];
static jmp_buf cut_env;
static const char *g_err;
static uint32_t g_min_seen;
static unsigned g_allowed;   /* bitmask of allowed boot targets */

/* ---------------- reference-run log ---------------- */

typedef struct {
    long idx;
    int have;
    sb_record_t rec;
} jstate_t;

typedef struct {
    uint32_t addr;
    char kind;
} op_t;

static jstate_t *jlog;
static size_t jlog_n, jlog_cap;
static op_t *oplog;
static size_t oplog_n, oplog_cap;
static int recording;

static int same_jstate(const jstate_t *a, int have, const sb_record_t *r)
{
    if (a->have != have)
        return 0;
    return !have || sb_record_same_state(&a->rec, r);
}

static void log_jstate(long idx)
{
    sb_journal_t t;
    sb_journal_scan(&t);
    if (jlog_n && same_jstate(&jlog[jlog_n - 1], t.have_cur, &t.cur))
        return;
    if (jlog_n == jlog_cap) {
        jlog_cap = jlog_cap ? 2 * jlog_cap : 256;
        jlog = realloc(jlog, jlog_cap * sizeof *jlog);
    }
    jlog[jlog_n].idx = idx;
    jlog[jlog_n].have = t.have_cur;
    jlog[jlog_n].rec = t.cur;
    jlog_n++;
}

static int in_journal(uint32_t addr)
{
    return addr >= sb_sector_addr(SB_JOURNAL_SECTOR0) &&
           addr < sb_sector_addr(SB_JOURNAL_SECTOR1) + SB_JOURNAL_PAGE_SIZE;
}

static void op_hook(long idx, uint32_t addr, char kind)
{
    if (!recording)
        return;
    if (oplog_n == oplog_cap) {
        oplog_cap = oplog_cap ? 2 * oplog_cap : 4096;
        oplog = realloc(oplog, oplog_cap * sizeof *oplog);
    }
    oplog[oplog_n].addr = addr;
    oplog[oplog_n].kind = kind;
    oplog_n++;
    if (in_journal(addr))
        log_jstate(idx);
}

/* Journal state after all ops with index <= k completed (k = -1: initial). */
static const jstate_t *jstate_at(long k)
{
    const jstate_t *s = &jlog[0];
    for (size_t i = 0; i < jlog_n && jlog[i].idx <= k; i++)
        s = &jlog[i];
    return s;
}

/* ---------------- device/host model ---------------- */

static void fail(const char *why)
{
    if (!g_err)
        g_err = why;
}

static void reboot(sb_boot_decision_t *d)
{
    sb_boot_decide(&J, PROV, NULL, d);
    if (J.have_cur) {
        if (J.cur.min_version < g_min_seen)
            fail("min_version decreased");
        g_min_seen = J.cur.min_version;
    }
    if (!(g_allowed & (1u << d->target)))
        fail("boot target not allowed in this scenario");
    if (d->target == SB_BOOT_UPDATE_MODE)
        return;
    uint32_t size = d->target == SB_BOOT_RECOVERY_IMAGE ? SB_RECOVERY_SIZE : SB_SLOT_SIZE;
    if (sb_image_verify(d->region_addr, size, PROV, NULL) != SB_V_OK)
        fail("booted an image that does not verify");
    if (d->target != SB_BOOT_RECOVERY_IMAGE && d->version < J.cur.min_version)
        fail("booted an image below min_version");
}

static sb_proto_t P;
static uint16_t seqno;

static int cmd(uint8_t type, const void *pl, uint16_t len, uint32_t *next)
{
    uint8_t resp[SB_RESP_LEN];
    sb_proto_handle(&P, type, seqno++, pl, len, resp);
    if (next)
        *next = sb_get_le32(resp + 4);
    return resp[0];
}

/* Host side of a resumable update, as tools/sbtool.py does it. */
static int host_update(const uint8_t *img, uint32_t size, uint32_t session, int activate)
{
    static uint8_t pl[8 + SB_DATA_MAX];
    uint8_t s[4];
    uint32_t next = 0;
    sb_journal_scan(&J);
    sb_proto_init(&P, &J, PROV, NULL);
    sb_put_le32(s, session);
    if (cmd(SB_CMD_BEGIN_UPDATE, s, 4, &next) != SB_S_OK)
        return -1;
    for (int restarts = 0;; restarts++) {
        if (restarts > 4)
            return -2;
        if (cmd(SB_CMD_IMAGE_INFO, img, SB_HDR_SIZE, &next) != SB_S_OK)
            return -3;
        int st = SB_S_OK;
        while (next < size) {
            uint32_t n = size - next < SB_DATA_MAX ? size - next : SB_DATA_MAX;
            sb_put_le32(pl, next);
            sb_put_le32(pl + 4, sb_crc32(0, img + SB_HDR_SIZE + next, n));
            memcpy(pl + 8, img + SB_HDR_SIZE + next, n);
            st = cmd(SB_CMD_DATA, pl, (uint16_t)(8 + n), &next);
            if (st == SB_S_RESTART)
                break;
            if (st != SB_S_OK && st != SB_S_ERR_SEQ)
                return -4;
        }
        if (st != SB_S_RESTART)
            break;
    }
    sb_put_le32(s, size);
    if (cmd(SB_CMD_FINALIZE, s, 4, NULL) != SB_S_OK)
        return -5;
    uint8_t act = (uint8_t)activate;
    if (cmd(SB_CMD_VERIFY, &act, 1, NULL) != SB_S_OK) {
        fail("VERIFY failed for a correctly transferred image");
        return -6;
    }
    return 0;
}

/* ---------------- scenarios ---------------- */

static uint8_t img_a[SB_SLOT_SIZE], img_b[SB_SLOT_SIZE];
static uint32_t size_a, size_b;
static unsigned compact_at;   /* if nonzero, the scenario's nth journal append compacts */

/* Append copies of the current state until the active page has exactly
 * compact_at - 1 free record slots, so the compaction lands where wanted. */
static void fill_journal(void)
{
    sb_journal_scan(&J);
    while (compact_at && (SB_JOURNAL_PAGE_SIZE - J.next_off) / SB_REC_SIZE > compact_at - 1)
        sb_journal_append(&J, &J.cur);
}

static void setup_confirmed_a(uint32_t payload_a, uint32_t payload_b)
{
    sb_boot_decision_t d;
    tu_factory_reset();
    PROV = sb_prov_get();
    size_a = payload_a;
    size_b = payload_b;
    tu_flash_image(SB_SLOT_A_ADDR, 1, size_a);
    tu_make_image(img_b, SB_SLOT_B_ADDR, 2, size_b, 0);
    sb_boot_decide(&J, PROV, NULL, &d);   /* adopt A */
}

static void setup_update(uint32_t payload_a, uint32_t payload_b)
{
    setup_confirmed_a(payload_a, payload_b);
    fill_journal();
}

/* Goal: B (v2) confirmed. The app always confirms after a trial boot. */
static int drive_update(void)
{
    sb_boot_decision_t d;
    for (int i = 0; i < MAX_REBOOTS; i++) {
        reboot(&d);
        const sb_record_t *r = &J.cur;
        if (r->active == 1 && r->state[1] == SB_ST_CONFIRMED)
            return 0;
        if (d.target == SB_BOOT_SLOT_B && d.trial) {
            sb_boot_confirm(&J, 1);
            continue;
        }
        if (!(r->candidate == 1 && (r->state[1] == SB_ST_PENDING || r->state[1] == SB_ST_TRIAL)))
            host_update(img_b, size_b, 0xB2, 1);
    }
    return -1;
}

static int goal_update(void)
{
    sb_journal_scan(&J);
    return J.have_cur && J.cur.active == 1 && J.cur.state[1] == SB_ST_CONFIRMED &&
           J.cur.version[1] == 2 && J.cur.min_version == 2 && J.cur.state[0] == SB_ST_VALID &&
           !(J.cur.flags & SB_RF_RX);
}

static void setup_pending_b(uint32_t payload_a, uint32_t payload_b)
{
    setup_confirmed_a(payload_a, payload_b);
    host_update(img_b, size_b, 0xB2, 1);
    fill_journal();
}

/* Goal: B never confirms; after max_attempts it is rolled back to A. */
static int drive_rollback(void)
{
    sb_boot_decision_t d;
    for (int i = 0; i < MAX_REBOOTS; i++) {
        reboot(&d);
        if (J.cur.state[1] == SB_ST_ROLLBACK && d.target == SB_BOOT_SLOT_A)
            return 0;
    }
    return -1;
}

static int goal_rollback(void)
{
    sb_journal_scan(&J);
    return J.have_cur && J.cur.active == 0 && J.cur.state[0] == SB_ST_CONFIRMED &&
           J.cur.state[1] == SB_ST_ROLLBACK && J.cur.candidate == SB_SLOT_NONE &&
           J.cur.min_version == 1;
}

static void setup_empty(uint32_t payload_a, uint32_t unused)
{
    (void)unused;
    tu_factory_reset();
    PROV = sb_prov_get();
    size_a = payload_a;
    tu_make_image(img_a, SB_SLOT_A_ADDR, 1, size_a, 0);
}

/* Goal: first install into an empty device over UART, then confirm. */
static int drive_first_install(void)
{
    sb_boot_decision_t d;
    for (int i = 0; i < MAX_REBOOTS; i++) {
        reboot(&d);
        const sb_record_t *r = &J.cur;
        if (J.have_cur && r->active == 0 && r->state[0] == SB_ST_CONFIRMED)
            return 0;
        if (d.target == SB_BOOT_SLOT_A && d.trial) {
            sb_boot_confirm(&J, 0);
            continue;
        }
        if (!(J.have_cur && r->candidate == 0 &&
              (r->state[0] == SB_ST_PENDING || r->state[0] == SB_ST_TRIAL)))
            host_update(img_a, size_a, 0xA1, 1);
    }
    return -1;
}

static int goal_first_install(void)
{
    sb_journal_scan(&J);
    return J.have_cur && J.cur.active == 0 && J.cur.state[0] == SB_ST_CONFIRMED &&
           J.cur.min_version == 1;
}

/* An earlier transfer of B was interrupted with a torn word at payload
 * offset torn_off (after the last checkpoint) and power lost; the scenario
 * then resumes it. Exercises compare/skip and the sector rewind. */
static uint32_t torn_off;

static void setup_torn_transfer(uint32_t payload_a, uint32_t payload_b)
{
    static uint8_t pl[8 + SB_DATA_MAX];
    uint8_t s[4];
    uint32_t next = 0;
    setup_confirmed_a(payload_a, payload_b);
    sb_journal_scan(&J);
    sb_proto_init(&P, &J, PROV, NULL);
    sb_put_le32(s, 0xB2);
    cmd(SB_CMD_BEGIN_UPDATE, s, 4, &next);
    cmd(SB_CMD_IMAGE_INFO, img_b, SB_HDR_SIZE, &next);
    while (next < torn_off) {
        uint32_t n = torn_off - next < SB_DATA_MAX ? torn_off - next : SB_DATA_MAX;
        sb_put_le32(pl, next);
        sb_put_le32(pl + 4, sb_crc32(0, img_b + SB_HDR_SIZE + next, n));
        memcpy(pl + 8, img_b + SB_HDR_SIZE + next, n);
        cmd(SB_CMD_DATA, pl, (uint16_t)(8 + n), &next);
    }
    /* The word at torn_off was being programmed when power failed. */
    uint32_t w;
    memcpy(&w, img_b + SB_HDR_SIZE + torn_off, 4);
    w |= 0x00F0F000u;   /* some of its 0-bits never got programmed */
    if (w == 0xFFFFFFFFu)
        w = 0x7FFFFFFFu;
    sb_flash_program(sb_slot_addr(1) + SB_HDR_SIZE + torn_off, &w, 4);
}

typedef struct {
    char name[48];
    void (*setup)(uint32_t, uint32_t);
    int (*drive)(void);
    int (*goal)(void);
    unsigned allowed;
    uint32_t payload_a, payload_b;
    unsigned compact_at;
    long data_sample;   /* 0: every op; otherwise every non-data op + ~N data ops */
    uint32_t torn_off;
} scenario_t;

#define T_A (1u << SB_BOOT_SLOT_A)
#define T_B (1u << SB_BOOT_SLOT_B)
#define T_UPD (1u << SB_BOOT_UPDATE_MODE)

/* ---------------- runner ---------------- */

typedef struct {
    long ops, points, runs, passed;
    double secs;
    const char *mode;
} result_t;

static int is_data_op(const op_t *o)
{
    for (unsigned s = 0; s < SB_NUM_SLOTS; s++) {
        uint32_t base = sb_slot_addr(s);
        if (o->kind == 'P' && o->addr >= base + SB_HDR_SIZE && o->addr < base + SB_SLOT_SIZE)
            return 1;
    }
    return 0;
}

static int interesting_data_op(const op_t *o)
{
    for (unsigned s = 0; s < SB_NUM_SLOTS; s++) {
        uint32_t base = sb_slot_addr(s);
        if (o->addr < base || o->addr >= base + SB_SLOT_SIZE)
            continue;
        uint32_t off = o->addr - base - SB_HDR_SIZE;
        uint32_t in_ckpt = off % SB_CKPT_INTERVAL;
        if (in_ckpt < 32 || in_ckpt >= SB_CKPT_INTERVAL - 32)
            return 1;   /* around journal checkpoints */
        int sec = sb_sector_of(o->addr);
        uint32_t in_sec = o->addr - sb_sector_addr((unsigned)sec);
        if (in_sec < 32 || in_sec >= sb_sector_size((unsigned)sec) - 32)
            return 1;   /* around sector boundaries */
    }
    return 0;
}

static int run_scenario(const scenario_t *sc, FILE *errlog, result_t *res)
{
    clock_t t0 = clock();
    memset(res, 0, sizeof *res);
    compact_at = sc->compact_at;
    torn_off = sc->torn_off;
    g_allowed = sc->allowed;
    sc->setup(sc->payload_a, sc->payload_b);
    memcpy(snapshot, fsim_mem(), SB_FLASH_SIZE);

    /* Reference run. */
    jlog_n = oplog_n = 0;
    log_jstate(-1);
    g_err = NULL;
    g_min_seen = 0;
    seqno = 1;
    fsim_reset_counters();
    recording = 1;
    int rc = sc->drive();
    recording = 0;
    if (rc != 0 || !sc->goal() || g_err) {
        fprintf(stderr, "%s: reference run failed (%s)\n", sc->name, g_err ? g_err : "goal");
        return -1;
    }
    long n = fsim_stats()->ops;
    res->ops = n;
    if (sc->compact_at) {
        int compacted = 0;
        for (long k = 0; k < n; k++)
            compacted |= oplog[k].kind == 'E' && in_journal(oplog[k].addr);
        if (!compacted) {
            fprintf(stderr, "%s: reference run never compacted the journal\n", sc->name);
            return -1;
        }
    }
    res->mode = sc->data_sample ? "sampled-data" : "exhaustive";

    long data_ops = 0;
    for (long k = 0; k < n; k++)
        data_ops += is_data_op(&oplog[k]);
    uint32_t rng = 12345;

    for (long k = 0; k < n; k++) {
        if (sc->data_sample && is_data_op(&oplog[k]) && !interesting_data_op(&oplog[k])) {
            rng = rng * 1103515245u + 12345u;
            if ((long)((rng >> 8) % (uint32_t)data_ops) >= sc->data_sample)
                continue;
        }
        res->points++;
        const jstate_t *before = jstate_at(k - 1), *after = jstate_at(k);
        for (int v = 0; v < NUM_VARIANTS; v++) {
            res->runs++;
            memcpy(fsim_mem(), snapshot, SB_FLASH_SIZE);
            fsim_reset_counters();
            g_err = NULL;
            g_min_seen = 0;
            seqno = 1;
            fsim_arm_cut(k, v, (uint32_t)(k * NUM_VARIANTS + v + 1), &cut_env);
            if (setjmp(cut_env) == 0) {
                sc->drive();
                fsim_arm_cut(-1, 0, 0, NULL);
                fail("cut point not reached (non-deterministic replay)");
            } else {
                sb_journal_scan(&J);
                if (!same_jstate(before, J.have_cur, &J.cur) && !same_jstate(after, J.have_cur, &J.cur))
                    fail("journal state is neither before nor after the interrupted op");
                if (sc->drive() != 0)
                    fail("did not converge after the cut");
                else if (!sc->goal())
                    fail("converged to the wrong final state");
            }
            if (fsim_stats()->overwrite_violations)
                fail("programmed over non-erased flash");
            if (g_err) {
                fprintf(errlog, "%s: op %ld (%c @0x%08X) variant %d: %s\n", sc->name, k,
                        oplog[k].kind, oplog[k].addr, v, g_err);
            } else {
                res->passed++;
            }
        }
    }
    res->secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    return 0;
}

int main(int argc, char **argv)
{
    int quick = 0;
    const char *csv = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick"))
            quick = 1;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc)
            csv = argv[++i];
    }
    fsim_set_op_hook(op_hook);

    scenario_t sc[32];
    int ns = 0;
    sc[ns++] = (scenario_t){"update_small", setup_update, drive_update, goal_update,
                            T_A | T_B, 6000, 6000, 0, 0};
    sc[ns++] = (scenario_t){"update_large_200k", setup_update, drive_update, goal_update,
                            T_A | T_B, 6000, 200000, 0, quick ? 300 : 3000};
    sc[ns++] = (scenario_t){"rollback", setup_pending_b, drive_rollback, goal_rollback,
                            T_A | T_B, 6000, 6000, 0, 0};
    sc[ns++] = (scenario_t){"first_install", setup_empty, drive_first_install, goal_first_install,
                            T_A | T_UPD, 6000, 0, 0, 0};
    /* An update appends 7 records (BEGIN, 2 checkpoints, FINALIZE, VERIFY,
     * TRIAL, CONFIRM): compact at each of them. */
    for (unsigned m = 1; m <= (quick ? 3u : 7u); m++) {
        scenario_t s = {"", setup_update, drive_update, goal_update, T_A | T_B, 6000, 40000,
                        m, quick ? 50 : 400};
        snprintf(s.name, sizeof s.name, "update_compaction_at_%u", m);
        sc[ns++] = s;
    }
    /* A rollback appends 4 (3 TRIAL, 1 ROLLBACK): compact at each. */
    for (unsigned m = 1; m <= (quick ? 2u : 4u); m++) {
        scenario_t s = {"", setup_pending_b, drive_rollback, goal_rollback, T_A | T_B, 6000, 6000,
                        m, 0};
        snprintf(s.name, sizeof s.name, "rollback_compaction_at_%u", m);
        sc[ns++] = s;
    }

    /* Resume over a torn word: in the slot's first sector, which also holds
     * the header (forces a RESTART from offset 0), and in its second sector
     * (rewind: checkpoint lowered, that sector re-erased and resent). */
    sc[ns++] = (scenario_t){"resume_torn_word_sector1", setup_torn_transfer, drive_update,
                            goal_update, T_A | T_B, 6000, 40000, 0, quick ? 300 : 0,
                            20 * 1024 + 8};
    sc[ns++] = (scenario_t){"resume_torn_word_sector2", setup_torn_transfer, drive_update,
                            goal_update, T_A | T_B, 6000, 140000, 0, quick ? 300 : 3000,
                            131 * 1024 + 8};

    FILE *errlog = stderr;
    FILE *out = csv ? fopen(csv, "w") : NULL;
    if (out)
        fprintf(out, "scenario,mode,flash_ops,cut_points,variants,runs,passed,failed,seconds\n");
    printf("%-28s %-13s %8s %8s %8s %8s %7s\n", "scenario", "mode", "ops", "points", "runs",
           "passed", "secs");
    long tot_runs = 0, tot_pass = 0;
    int bad = 0;
    for (int i = 0; i < ns; i++) {
        result_t r;
        if (run_scenario(&sc[i], errlog, &r) != 0) {
            bad = 1;
            continue;
        }
        printf("%-28s %-13s %8ld %8ld %8ld %8ld %7.1f\n", sc[i].name, r.mode, r.ops, r.points,
               r.runs, r.passed, r.secs);
        if (out)
            fprintf(out, "%s,%s,%ld,%ld,%d,%ld,%ld,%ld,%.1f\n", sc[i].name, r.mode, r.ops,
                    r.points, NUM_VARIANTS, r.runs, r.passed, r.runs - r.passed, r.secs);
        tot_runs += r.runs;
        tot_pass += r.passed;
    }
    printf("TOTAL %ld/%ld runs passed\n", tot_pass, tot_runs);
    if (out)
        fclose(out);
    return bad || tot_pass != tot_runs;
}
