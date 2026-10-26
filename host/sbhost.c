/*
 * Host CLI around the real C core, for cross-language tests:
 *   sbhost verify <prov.bin> <image.sbimg>   verify like the bootloader does;
 *                                            prints the sb_verify_result
 *   sbhost journal <out.bin> <n>             append n records (min_version =
 *                                            1..n) and write both journal
 *                                            pages (32 KB) to out.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_sim.h"
#include "sb/image.h"
#include "sb/journal.h"
#include "sb/layout.h"

static long load(const char *path, uint8_t *dst, long cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    long n = (long)fread(dst, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    fsim_erase_all();
    if (argc == 4 && !strcmp(argv[1], "verify")) {
        uint8_t *m = fsim_mem();
        load(argv[2], m + (SB_PROV_ADDR - SB_FLASH_BASE), 64);
        static uint8_t img[SB_SLOT_SIZE];
        long n = load(argv[3], img, sizeof img);
        if (n < (long)SB_HDR_SIZE)
            return 2;
        const sb_image_hdr_t *h = (const sb_image_hdr_t *)img;
        uint32_t region = h->load_addr;
        uint32_t size = region == SB_RECOVERY_ADDR ? SB_RECOVERY_SIZE : SB_SLOT_SIZE;
        if (region != SB_SLOT_A_ADDR && region != SB_SLOT_B_ADDR && region != SB_RECOVERY_ADDR) {
            printf("%d\n", SB_V_BAD_LAYOUT);
            return 1;
        }
        memcpy(m + (region - SB_FLASH_BASE), img, (size_t)n);
        int rc = sb_image_verify(region, size, sb_prov_get(), NULL);
        printf("%d\n", rc);
        return rc == SB_V_OK ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[1], "journal")) {
        sb_journal_t j;
        sb_record_t r;
        sb_journal_scan(&j);
        sb_record_init(&r);
        r.state[0] = SB_ST_CONFIRMED;
        r.active = 0;
        r.version[0] = 7;
        int n = atoi(argv[3]);
        for (int i = 1; i <= n; i++) {
            r.min_version = (uint32_t)i;
            if (sb_journal_append(&j, &r) != 0)
                return 1;
        }
        FILE *f = fopen(argv[2], "wb");
        fwrite(fsim_mem() + (sb_sector_addr(SB_JOURNAL_SECTOR0) - SB_FLASH_BASE), 1,
               2 * SB_JOURNAL_PAGE_SIZE, f);
        fclose(f);
        return 0;
    }
    fprintf(stderr, "usage: sbhost verify <prov.bin> <image> | journal <out.bin> <n>\n");
    return 2;
}
