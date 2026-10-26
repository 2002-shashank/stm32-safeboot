/* Unit tests for the portable core, run on the host against flash_sim. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "flash_sim.h"
#include "sb/boot.h"
#include "sb/crash.h"
#include "sb/crc32.h"
#include "sb/flash.h"
#include "sb/frame.h"
#include "sb/journal.h"
#include "sb/layout.h"
#include "sb/proto.h"
#include "sb/sha256.h"
#include "testutil.h"

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void test_crc32(void)
{
    CHECK_EQ(sb_crc32(0, "123456789", 9), 0xCBF43926u);
    CHECK_EQ(sb_crc32(sb_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
}

static void sha_hex(const void *d, size_t n, char *out)
{
    uint8_t h[32];
    sb_sha256_ctx c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, d, n);
    sb_sha256_final(&c, h);
    hex(h, 32, out);
}

static void test_sha256(void)
{
    char out[65];
    sha_hex("", 0, out);
    CHECK(strcmp(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    sha_hex("abc", 3, out);
    CHECK(strcmp(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_hex(m, strlen(m), out);
    CHECK(strcmp(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    /* One million 'a', fed in odd-sized pieces to exercise buffering. */
    static uint8_t a[1000000];
    memset(a, 'a', sizeof a);
    uint8_t h[32];
    sb_sha256_ctx c;
    sb_sha256_init(&c);
    for (size_t off = 0; off < sizeof a;) {
        size_t n = (off % 97) + 1;
        if (n > sizeof a - off)
            n = sizeof a - off;
        sb_sha256_update(&c, a + off, n);
        off += n;
    }
    sb_sha256_final(&c, h);
    hex(h, 32, out);
    CHECK(strcmp(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_layout(void)
{
    /* Every region starts on a sector boundary and ends on one. */
    uint32_t starts[] = {SB_BOOT_ADDR, SB_PROV_ADDR, SB_SLOT_A_ADDR, SB_SLOT_B_ADDR, SB_RECOVERY_ADDR};
    uint32_t sizes[] = {SB_BOOT_SIZE, 0x10000u, SB_SLOT_SIZE, SB_SLOT_SIZE, SB_RECOVERY_SIZE};
    for (int i = 0; i < 5; i++) {
        int s = sb_sector_of(starts[i]);
        CHECK(s >= 0 && sb_sector_addr((unsigned)s) == starts[i]);
        int e = sb_sector_of(starts[i] + sizes[i] - 1);
        CHECK(e >= 0 && sb_sector_addr((unsigned)e) + sb_sector_size((unsigned)e) == starts[i] + sizes[i]);
    }
    CHECK_EQ(sb_sector_addr(SB_JOURNAL_SECTOR0), 0x08008000u);
    CHECK_EQ(sb_sector_addr(SB_JOURNAL_SECTOR1), 0x0800C000u);
    CHECK_EQ(sb_sector_size(SB_JOURNAL_SECTOR0), SB_JOURNAL_PAGE_SIZE);
    CHECK_EQ(sb_sector_addr(11) + sb_sector_size(11), SB_FLASH_BASE + SB_FLASH_SIZE);
    CHECK_EQ(SB_SLOT_MAX_PAYLOAD, 392704u);
    uint32_t total = 0;
    for (unsigned s = 0; s < SB_NUM_SECTORS; s++) {
        CHECK_EQ(sb_sector_of(sb_sector_addr(s)), s);
        total += sb_sector_size(s);
    }
    CHECK_EQ(total, SB_FLASH_SIZE);
    CHECK_EQ(sb_sector_of(SB_FLASH_BASE - 1), -1);
    CHECK_EQ(SB_API_ADDR & 0xFFu, 0);
}

static void test_journal(void)
{
    tu_factory_reset();
    sb_journal_t j;
    sb_journal_scan(&j);
    CHECK(!j.have_cur);

    sb_record_t r;
    sb_record_init(&r);
    r.min_version = 7;
    CHECK_EQ(sb_journal_append(&j, &r), 0);
    sb_journal_scan(&j);
    CHECK(j.have_cur);
    CHECK_EQ(j.cur.min_version, 7);
    CHECK_EQ(j.cur.seq, 1);

    /* Fill well past two pages to force several compactions. */
    uint32_t per_page = SB_JOURNAL_PAGE_SIZE / SB_REC_SIZE;
    for (uint32_t i = 0; i < 3 * per_page; i++) {
        r.min_version = 8 + i;
        CHECK_EQ(sb_journal_append(&j, &r), 0);
    }
    sb_journal_scan(&j);
    CHECK_EQ(j.cur.min_version, 8 + 3 * per_page - 1);
    CHECK_EQ(j.cur.seq, 1 + 3 * per_page);

    /* A torn record (no commit word) is ignored and never overwritten. */
    fsim_reset_counters();
    uint32_t before_seq = j.cur.seq;
    uint32_t addr = sb_sector_addr(j.page ? SB_JOURNAL_SECTOR1 : SB_JOURNAL_SECTOR0) + j.next_off;
    uint8_t junk[16];
    memset(junk, 0x11, sizeof junk);
    sb_flash_program(addr, junk, sizeof junk);
    sb_journal_scan(&j);
    CHECK_EQ(j.cur.seq, before_seq);
    r.min_version = 1000;
    CHECK_EQ(sb_journal_append(&j, &r), 0);
    sb_journal_scan(&j);
    CHECK_EQ(j.cur.min_version, 1000);
    CHECK_EQ(fsim_stats()->overwrite_violations, 0);
}

/* Each validity layer must reject on its own: the others are not allowed to
 * mask a missing check. Records are hand-built so only one layer is wrong. */
static void test_journal_layers(void)
{
    sb_journal_t j;
    sb_record_t good, r;
    uint32_t page0 = sb_sector_addr(SB_JOURNAL_SECTOR0);
    uint32_t page1 = sb_sector_addr(SB_JOURNAL_SECTOR1);

    tu_factory_reset();
    sb_journal_scan(&j);
    sb_record_init(&good);
    good.min_version = 5;
    CHECK_EQ(sb_journal_append(&j, &good), 0);
    good = j.cur;

    /* Candidate records in page 1 with a higher seq; each has one defect. */
    struct { const char *what; int field; } cases[] = {
        {"bad crc", 0}, {"no commit", 1}, {"bad magic", 2}, {"valid", 3},
    };
    for (unsigned i = 0; i < 4; i++) {
        memset(fsim_mem() + (page1 - SB_FLASH_BASE), 0xFF, SB_JOURNAL_PAGE_SIZE);
        r = good;
        r.seq = good.seq + 10;
        r.min_version = 99;
        r.crc = sb_crc32(0, &r, offsetof(sb_record_t, crc));
        if (cases[i].field == 0)
            r.crc ^= 1;
        if (cases[i].field == 1)
            r.commit = 0xFFFFFFFFu;
        if (cases[i].field == 2) {
            r.magic ^= 0x100;
            r.crc = sb_crc32(0, &r, offsetof(sb_record_t, crc));
        }
        memcpy(fsim_mem() + (page1 - SB_FLASH_BASE), &r, sizeof r);
        sb_journal_scan(&j);
        if (cases[i].field == 3) {
            CHECK_EQ(j.cur.min_version, 99);
            CHECK_EQ(j.page, 1);
        } else if (j.cur.min_version != 5) {
            fprintf(stderr, "journal accepted a record with %s\n", cases[i].what);
            tu_failures++;
        }
    }
    /* A partially erased old record whose seq bits went 0->1 (a higher seq)
     * must be rejected by the CRC even though magic and commit survived. */
    memset(fsim_mem() + (page1 - SB_FLASH_BASE), 0xFF, SB_JOURNAL_PAGE_SIZE);
    r = good;
    r.seq |= 0x00010000u;
    memcpy(fsim_mem() + (page1 - SB_FLASH_BASE), &r, sizeof r);
    sb_journal_scan(&j);
    CHECK_EQ(j.cur.seq, good.seq);
    (void)page0;
}

static void test_image_verify(void)
{
    tu_factory_reset();
    const sb_prov_t *prov = sb_prov_get();
    CHECK(prov != NULL);
    tu_flash_image(SB_SLOT_A_ADDR, 3, 8192);
    CHECK_EQ(sb_image_verify(SB_SLOT_A_ADDR, SB_SLOT_SIZE, prov, NULL), SB_V_OK);

    /* Same image, wrong slot: load_addr mismatch. */
    memcpy(fsim_mem() + (SB_SLOT_B_ADDR - SB_FLASH_BASE),
           fsim_mem() + (SB_SLOT_A_ADDR - SB_FLASH_BASE), SB_HDR_SIZE + 8192);
    CHECK_EQ(sb_image_verify(SB_SLOT_B_ADDR, SB_SLOT_SIZE, prov, NULL), SB_V_BAD_LAYOUT);

    /* Flip one payload bit (bits can only be cleared, as in real flash). */
    uint8_t *pl = fsim_mem() + (SB_SLOT_A_ADDR - SB_FLASH_BASE) + SB_HDR_SIZE;
    uint8_t saved = pl[5000];
    pl[5000] ^= 0x01;
    CHECK_EQ(sb_image_verify(SB_SLOT_A_ADDR, SB_SLOT_SIZE, prov, NULL), SB_V_BAD_HASH);
    pl[5000] = saved;

    /* Change the version in the header: signature must fail. */
    sb_image_hdr_t *h = (sb_image_hdr_t *)(fsim_mem() + (SB_SLOT_A_ADDR - SB_FLASH_BASE));
    h->image_version = 99;
    CHECK_EQ(sb_image_verify(SB_SLOT_A_ADDR, SB_SLOT_SIZE, prov, NULL), SB_V_BAD_SIG);
    h->image_version = 3;
    CHECK_EQ(sb_image_verify(SB_SLOT_A_ADDR, SB_SLOT_SIZE, prov, NULL), SB_V_OK);

    h->key_id ^= 1;
    CHECK_EQ(sb_image_verify(SB_SLOT_A_ADDR, SB_SLOT_SIZE, prov, NULL), SB_V_BAD_KEY);
    h->key_id ^= 1;

    /* Oversized image claims. */
    h->image_size = SB_SLOT_MAX_PAYLOAD + 4;
    CHECK_EQ(sb_image_verify(SB_SLOT_A_ADDR, SB_SLOT_SIZE, prov, NULL), SB_V_BAD_LAYOUT);
    h->image_size = 8192;

    /* A recovery-flagged image is only accepted in S11, and vice versa. */
    tu_flash_image(SB_RECOVERY_ADDR, 1, 4096);
    CHECK_EQ(sb_image_verify(SB_RECOVERY_ADDR, SB_RECOVERY_SIZE, prov, NULL), SB_V_OK);
    static uint8_t buf[SB_HDR_SIZE + 4096];
    tu_make_image(buf, SB_SLOT_B_ADDR, 4, 4096, SB_IMG_F_RECOVERY);
    CHECK_EQ(sb_image_check_header((const sb_image_hdr_t *)buf, SB_SLOT_B_ADDR, SB_SLOT_SIZE),
             SB_V_BAD_LAYOUT);

    /* Missing provisioning: nothing verifies. */
    memset(fsim_mem() + (SB_PROV_ADDR - SB_FLASH_BASE), 0xFF, 64);
    CHECK(sb_prov_get() == NULL);
    CHECK_EQ(sb_image_verify(SB_SLOT_A_ADDR, SB_SLOT_SIZE, NULL, NULL), SB_V_BAD_KEY);
}

/* --- protocol helpers --- */

static uint16_t seqno = 100;

static int send(sb_proto_t *p, uint8_t type, const void *pl, uint16_t len, uint32_t *next)
{
    uint8_t resp[SB_RESP_LEN];
    sb_proto_handle(p, type, seqno++, pl, len, resp);
    if (next)
        *next = sb_get_le32(resp + 4);
    return resp[0];
}

static int send_data(sb_proto_t *p, const uint8_t *payload, uint32_t off, uint32_t n, uint32_t *next)
{
    static uint8_t pl[8 + SB_DATA_MAX];
    sb_put_le32(pl, off);
    sb_put_le32(pl + 4, sb_crc32(0, payload + off, n));
    memcpy(pl + 8, payload + off, n);
    return send(p, SB_CMD_DATA, pl, (uint16_t)(8 + n), next);
}

/* Drive a full update of image `img` (hdr + payload) starting from the
 * device's next_offset; returns final status of VERIFY. */
static int run_update(sb_proto_t *p, const uint8_t *img, uint32_t payload_size, uint32_t session,
                      int activate)
{
    uint8_t s[4];
    uint32_t next = 0;
    sb_put_le32(s, session);
    if (send(p, SB_CMD_BEGIN_UPDATE, s, 4, &next) != SB_S_OK)
        return -1;
    int st = send(p, SB_CMD_IMAGE_INFO, img, SB_HDR_SIZE, &next);
    if (st != SB_S_OK)
        return -100 - st;
    const uint8_t *pl = img + SB_HDR_SIZE;
    while (next < payload_size) {
        uint32_t n = payload_size - next < SB_DATA_MAX ? payload_size - next : SB_DATA_MAX;
        st = send_data(p, pl, next, n, &next);
        if (st == SB_S_RESTART) {
            if (send(p, SB_CMD_IMAGE_INFO, img, SB_HDR_SIZE, &next) != SB_S_OK)
                return -2;
        } else if (st != SB_S_OK && st != SB_S_ERR_SEQ) {
            return -200 - st;
        }
    }
    sb_put_le32(s, payload_size);
    if (send(p, SB_CMD_FINALIZE, s, 4, NULL) != SB_S_OK)
        return -3;
    uint8_t act = (uint8_t)activate;
    return send(p, SB_CMD_VERIFY, &act, 1, NULL);
}

static void test_boot_flow(void)
{
    static uint8_t img[SB_SLOT_SIZE];
    sb_journal_t j;
    sb_boot_decision_t d;

    tu_factory_reset();
    const sb_prov_t *prov = sb_prov_get();

    /* Factory: nothing flashed -> update mode. */
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_UPDATE_MODE);

    /* Factory image in A gets adopted. */
    tu_flash_image(SB_SLOT_A_ADDR, 1, 20000);
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_SLOT_A);
    CHECK_EQ(d.reason, SB_R_ADOPTED);
    CHECK_EQ(j.cur.min_version, 1);
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.reason, SB_R_CONFIRMED);

    /* Update v2 into B via the protocol, crossing a 128 KB sector boundary. */
    uint32_t size2 = 150000;
    tu_make_image(img, SB_SLOT_B_ADDR, 2, size2, 0);
    sb_proto_t p;
    sb_journal_scan(&j);
    sb_proto_init(&p, &j, prov, NULL);
    CHECK_EQ(run_update(&p, img, size2, 0x1234, 1), SB_S_OK);
    CHECK_EQ(j.cur.state[1], SB_ST_PENDING);
    CHECK_EQ(j.cur.candidate, 1);

    /* Trial boot, then confirm. */
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_SLOT_B);
    CHECK_EQ(d.reason, SB_R_TRIAL);
    CHECK_EQ(d.trial, 1);
    CHECK_EQ(d.attempts, 1);
    /* Confirming A (still the confirmed active image) writes nothing and
     * leaves B on trial. */
    uint32_t seq0 = j.cur.seq;
    CHECK_EQ(sb_boot_confirm(&j, 0), 0);
    CHECK_EQ(j.cur.seq, seq0);
    CHECK_EQ(j.cur.state[1], SB_ST_TRIAL);
    CHECK_EQ(sb_boot_confirm(&j, 1), 0);
    CHECK_EQ(j.cur.min_version, 2);
    CHECK_EQ(j.cur.active, 1);
    CHECK_EQ(j.cur.state[0], SB_ST_VALID);
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_SLOT_B);
    CHECK_EQ(d.reason, SB_R_CONFIRMED);
    uint32_t seq = j.cur.seq;
    CHECK_EQ(sb_boot_confirm(&j, 1), 0);   /* idempotent, no write */
    CHECK_EQ(j.cur.seq, seq);

    /* Anti-rollback: v1 and v2 are both refused now. */
    tu_make_image(img, SB_SLOT_A_ADDR, 2, 8192, 0);
    sb_proto_init(&p, &j, prov, NULL);
    CHECK_EQ(run_update(&p, img, 8192, 0x2, 1), -100 - SB_S_ERR_VERSION);

    /* v3 that never confirms: 3 trials, then automatic rollback to v2. */
    tu_make_image(img, SB_SLOT_A_ADDR, 3, 30000, 0);
    sb_proto_init(&p, &j, prov, NULL);
    CHECK_EQ(run_update(&p, img, 30000, 0x3, 1), SB_S_OK);
    for (int i = 1; i <= 3; i++) {
        sb_boot_decide(&j, prov, NULL, &d);
        CHECK_EQ(d.target, SB_BOOT_SLOT_A);
        CHECK_EQ(d.attempts, i);
    }
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_SLOT_B);
    CHECK_EQ(d.reason, SB_R_ROLLBACK);
    CHECK_EQ(j.cur.state[0], SB_ST_ROLLBACK);
    CHECK_EQ(j.cur.min_version, 2);
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.reason, SB_R_CONFIRMED);

    /* v4 verified but not activated; then the active image gets corrupted:
     * v4 is promoted to a trial. */
    tu_make_image(img, SB_SLOT_A_ADDR, 4, 9000, 0);
    sb_proto_init(&p, &j, prov, NULL);
    CHECK_EQ(run_update(&p, img, 9000, 0x4, 0), SB_S_OK);
    CHECK_EQ(j.cur.state[0], SB_ST_VALID);
    fsim_mem()[SB_SLOT_B_ADDR - SB_FLASH_BASE + SB_HDR_SIZE + 100] ^= 0xFF;
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_SLOT_A);
    CHECK_EQ(d.reason, SB_R_PROMOTED);
    CHECK_EQ(d.trial, 1);
    CHECK_EQ(j.cur.state[1], SB_ST_INVALID);

    /* Candidate tampered after VERIFY: rejected at boot, nothing left. */
    fsim_mem()[SB_SLOT_A_ADDR - SB_FLASH_BASE + SB_HDR_SIZE + 100] ^= 0xFF;
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_UPDATE_MODE);

    /* ...unless a golden image is present. */
    tu_flash_image(SB_RECOVERY_ADDR, 1, 4096);
    sb_boot_decide(&j, prov, NULL, &d);
    CHECK_EQ(d.target, SB_BOOT_RECOVERY_IMAGE);
    CHECK_EQ(fsim_stats()->overwrite_violations, 0);
}

static void test_proto_edges(void)
{
    static uint8_t img[SB_SLOT_SIZE];
    sb_journal_t j;
    sb_boot_decision_t d;
    tu_factory_reset();
    const sb_prov_t *prov = sb_prov_get();
    tu_flash_image(SB_SLOT_A_ADDR, 1, 20000);
    sb_boot_decide(&j, prov, NULL, &d);

    uint32_t size = 140000, next = 0;
    tu_make_image(img, SB_SLOT_B_ADDR, 2, size, 0);
    const uint8_t *pl = img + SB_HDR_SIZE;
    sb_proto_t p;
    sb_proto_init(&p, &j, prov, NULL);
    uint8_t s[4];
    sb_put_le32(s, 77);

    /* Commands out of order are rejected. */
    CHECK_EQ(send_data(&p, pl, 0, 1024, &next), SB_S_ERR_STATE);
    CHECK_EQ(send(&p, SB_CMD_BEGIN_UPDATE, s, 4, &next), SB_S_OK);
    CHECK_EQ(send_data(&p, pl, 0, 1024, &next), SB_S_ERR_STATE);
    CHECK_EQ(send(&p, SB_CMD_IMAGE_INFO, img, SB_HDR_SIZE, &next), SB_S_OK);

    CHECK_EQ(send_data(&p, pl, 0, 1024, &next), SB_S_OK);
    CHECK_EQ(next, 1024);
    /* Duplicate: re-ACK, no write. */
    long progs = fsim_stats()->programs;
    CHECK_EQ(send_data(&p, pl, 0, 1024, &next), SB_S_OK);
    CHECK_EQ(next, 1024);
    CHECK_EQ(fsim_stats()->programs, progs);
    /* Gap: NACK with next_offset. */
    CHECK_EQ(send_data(&p, pl, 4096, 1024, &next), SB_S_ERR_SEQ);
    CHECK_EQ(next, 1024);
    /* Bad CRC. */
    static uint8_t bad[8 + 1024];
    sb_put_le32(bad, 1024);
    sb_put_le32(bad + 4, 0xDEADBEEF);
    memcpy(bad + 8, pl + 1024, 1024);
    CHECK_EQ(send(&p, SB_CMD_DATA, bad, sizeof bad, &next), SB_S_ERR_CRC);
    /* Retransmission with the same seq returns the cached reply. */
    uint8_t r1[SB_RESP_LEN], r2[SB_RESP_LEN];
    sb_proto_handle(&p, SB_CMD_QUERY_PROGRESS, 5000, NULL, 0, r1);
    sb_proto_handle(&p, SB_CMD_QUERY_PROGRESS, 5000, NULL, 0, r2);
    CHECK(memcmp(r1, r2, SB_RESP_LEN) == 0);
    /* Odd length is refused. */
    CHECK_EQ(send(&p, SB_CMD_DATA, bad, 8 + 6, &next), SB_S_ERR_LEN);

    /* Send 40 KB, then "reboot" (fresh proto + journal scan) and resume. */
    while (next < 40 * 1024)
        CHECK_EQ(send_data(&p, pl, next, 1024, &next), SB_S_OK);
    sb_journal_scan(&j);
    CHECK_EQ(j.cur.rx_offset, 32 * 1024);
    sb_proto_init(&p, &j, prov, NULL);
    CHECK_EQ(send(&p, SB_CMD_BEGIN_UPDATE, s, 4, &next), SB_S_OK);
    CHECK_EQ(next, 32 * 1024);
    CHECK_EQ(send(&p, SB_CMD_IMAGE_INFO, img, SB_HDR_SIZE, &next), SB_S_OK);
    CHECK_EQ(next, 32 * 1024);
    /* The 8 KB after the checkpoint is already in flash: compared, skipped. */
    progs = fsim_stats()->programs;
    CHECK_EQ(send_data(&p, pl, next, 1024, &next), SB_S_OK);
    CHECK_EQ(fsim_stats()->programs, progs);

    /* Corrupt a word ahead (as a torn write would), in the second slot
     * sector: the device rewinds to that sector's start. */
    uint32_t sec2 = SB_SLOT_B_ADDR + 0x20000u;
    uint32_t off2 = sec2 - SB_SLOT_B_ADDR - SB_HDR_SIZE;
    while (next < off2 + 2048) {
        uint32_t n = 1024;
        CHECK_EQ(send_data(&p, pl, next, n, &next), SB_S_OK);
    }
    sb_journal_scan(&j);
    sb_proto_init(&p, &j, prov, NULL);
    CHECK_EQ(send(&p, SB_CMD_BEGIN_UPDATE, s, 4, &next), SB_S_OK);
    CHECK_EQ(send(&p, SB_CMD_IMAGE_INFO, img, SB_HDR_SIZE, &next), SB_S_OK);
    uint32_t w = 0;
    uint32_t bad_addr = sb_slot_addr(1) + SB_HDR_SIZE + next + 8;
    if (bad_addr >= sec2) {
        memcpy(fsim_mem() + (bad_addr - SB_FLASH_BASE), &w, 4);
        int st = send_data(&p, pl, next, 1024, &next);
        CHECK_EQ(st, SB_S_ERR_SEQ);
        CHECK_EQ(next, off2);
    } else {
        CHECK(0 && "resume point expected in second sector");
    }
    /* Finish and verify. */
    while (next < size) {
        uint32_t n = size - next < 1024 ? size - next : 1024;
        CHECK_EQ(send_data(&p, pl, next, n, &next), SB_S_OK);
    }
    /* Wrong FINALIZE length. */
    sb_put_le32(s, size - 4);
    CHECK_EQ(send(&p, SB_CMD_FINALIZE, s, 4, NULL), SB_S_ERR_RANGE);
    sb_put_le32(s, size);
    CHECK_EQ(send(&p, SB_CMD_FINALIZE, s, 4, NULL), SB_S_OK);
    uint8_t act = 1;
    CHECK_EQ(send(&p, SB_CMD_VERIFY, &act, 1, NULL), SB_S_OK);
    CHECK_EQ(j.cur.state[1], SB_ST_PENDING);
    CHECK_EQ(j.cur.flags & SB_RF_RX, 0);

    /* Same session id but a different image: restarts from 0. */
    tu_make_image(img, SB_SLOT_A_ADDR, 3, 9000, 0);
    sb_boot_decide(&j, prov, NULL, &d);
    sb_boot_confirm(&j, 1);
    sb_proto_init(&p, &j, prov, NULL);
    CHECK_EQ(run_update(&p, img, 9000, 77, 1), SB_S_OK);

    /* A payload that does not match its signed hash fails VERIFY -> INVALID. */
    tu_make_image(img, SB_SLOT_A_ADDR, 5, 9000, 0);
    img[SB_HDR_SIZE + 600] ^= 1;
    sb_boot_decide(&j, prov, NULL, &d);
    sb_boot_confirm(&j, 0);
    sb_journal_scan(&j);
    sb_proto_init(&p, &j, prov, NULL);
    tu_make_image(img, SB_SLOT_B_ADDR, 5, 9000, 0);
    img[SB_HDR_SIZE + 600] ^= 1;
    CHECK_EQ(run_update(&p, img, 9000, 78, 1), SB_S_ERR_VERIFY);
    CHECK_EQ(j.cur.state[1], SB_ST_INVALID);
    CHECK_EQ(fsim_stats()->overwrite_violations, 0);
}

static void test_frame(void)
{
    uint8_t buf[64];
    uint8_t pl[5] = {1, 2, SB_SOF0, SB_SOF1, 5};
    uint32_t n = sb_frame_encode(buf, 0x42, 0x1234, pl, sizeof pl);
    sb_frame_parser_t p;
    memset(&p, 0, sizeof p);
    sb_frame_reset(&p);
    /* Leading noise including a false SOF. */
    uint8_t noise[] = {0x00, SB_SOF0, 0x13, SB_SOF0};
    int got = 0;
    for (size_t i = 0; i < sizeof noise; i++)
        got += sb_frame_feed(&p, noise[i]);
    for (uint32_t i = 0; i < n; i++)
        got += sb_frame_feed(&p, buf[i]);
    CHECK_EQ(got, 1);
    CHECK_EQ(p.f.type, 0x42);
    CHECK_EQ(p.f.seq, 0x1234);
    CHECK_EQ(p.f.len, 5);
    CHECK(memcmp(p.f.payload, pl, 5) == 0);
    buf[8] ^= 1;
    got = 0;
    for (uint32_t i = 0; i < n; i++)
        got += sb_frame_feed(&p, buf[i]);
    CHECK_EQ(got, 0);
    CHECK_EQ(p.crc_errors, 1);
}

static void test_crash_format(void)
{
    sb_crash_t c;
    memset(&c, 0, sizeof c);
    c.count = 2;
    c.fault_type = SB_FAULT_HARD;
    c.cfsr = (1u << 1) | (1u << 7) | (1u << 25);
    c.hfsr = 1u << 30;
    c.mmfar = 0x00000004;
    c.pc = 0x08020abc;
    c.image_version = 3;
    c.slot = 0;
    c.reset_reason = SB_RST_SFT | SB_RST_IWDG;
    sb_crash_seal(&c);
    CHECK(sb_crash_valid(&c));
    char out[600];
    size_t n = sb_crash_format(&c, out, sizeof out);
    CHECK(n < sizeof out);
    CHECK(strstr(out, "HardFault") != NULL);
    CHECK(strstr(out, "DACCVIOL") != NULL);
    CHECK(strstr(out, "DIVBYZERO") != NULL);
    CHECK(strstr(out, "PC=0x08020ABC") != NULL);
    CHECK(strstr(out, "fault_addr=0x00000004") != NULL);
    CHECK(strstr(out, "SFT IWDG") != NULL);
    /* reset_reason is filled after sealing, so it is outside the CRC. */
    c.reset_reason = 0;
    CHECK(sb_crash_valid(&c));
    c.pc++;
    CHECK(!sb_crash_valid(&c));
    /* Truncation stays NUL-terminated. */
    char small[10];
    sb_crash_format(&c, small, sizeof small);
    CHECK(strlen(small) == 9);
    CHECK_EQ(sb_reset_reason_from_csr(1u << 29), SB_RST_IWDG);
}

int main(void)
{
    test_crc32();
    test_sha256();
    test_layout();
    test_frame();
    test_journal();
    test_journal_layers();
    test_image_verify();
    test_boot_flow();
    test_proto_edges();
    test_crash_format();
    if (tu_failures) {
        fprintf(stderr, "test_core: %d check(s) FAILED\n", tu_failures);
        return 1;
    }
    printf("test_core: all checks passed\n");
    return 0;
}
