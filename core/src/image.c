#include "sb/image.h"

#include <stddef.h>
#include <string.h>

#include "monocypher-ed25519.h"
#include "sb/crc32.h"
#include "sb/flash.h"
#include "sb/sha256.h"

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= a[i] ^ b[i];
    return d == 0;
}

int sb_image_check_header(const sb_image_hdr_t *h, uint32_t region_addr, uint32_t region_size)
{
    if (h->magic != SB_IMG_MAGIC || h->hdr_version != SB_IMG_HDR_VER || h->hdr_size != SB_HDR_SIZE)
        return SB_V_BAD_MAGIC;
    if (h->load_addr != region_addr || h->image_size == 0 || (h->image_size & 3u) != 0 ||
        h->image_size > region_size - SB_HDR_SIZE)
        return SB_V_BAD_LAYOUT;
    int is_recovery_region = region_addr == SB_RECOVERY_ADDR;
    if (((h->flags & SB_IMG_F_RECOVERY) != 0) != is_recovery_region)
        return SB_V_BAD_LAYOUT;
    if (h->flags & ~SB_IMG_F_RECOVERY)
        return SB_V_BAD_LAYOUT;
    return SB_V_OK;
}

static int vectors_sane(uint32_t payload_addr, uint32_t payload_size)
{
    uint32_t sp = sb_flash_word(payload_addr);
    uint32_t pc = sb_flash_word(payload_addr + 4);
    if (sp < SB_SRAM_BASE || sp > SB_SRAM_BASE + SB_APP_RAM_SIZE || (sp & 7u) != 0)
        return 0;
    if ((pc & 1u) == 0)
        return 0;
    pc &= ~1u;
    return pc >= payload_addr && pc < payload_addr + payload_size;
}

int sb_image_verify(uint32_t region_addr, uint32_t region_size, const sb_prov_t *prov,
                    sb_mark_fn mark)
{
    sb_image_hdr_t h;
    memcpy(&h, sb_flash_ptr(region_addr), sizeof h);

    int rc = sb_image_check_header(&h, region_addr, region_size);
    if (rc != SB_V_OK)
        return rc;
    if (prov == NULL || h.key_id != prov->key_id)
        return SB_V_BAD_KEY;

    if (mark)
        mark(SB_MARK_SHA_START);
    uint8_t digest[32];
    sb_sha256_ctx c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, sb_flash_ptr(region_addr + SB_HDR_SIZE), h.image_size);
    sb_sha256_final(&c, digest);
    if (mark)
        mark(SB_MARK_SHA_END);
    if (!ct_equal(digest, h.sha256, sizeof digest))
        return SB_V_BAD_HASH;

    int sig_ok = crypto_ed25519_check(h.signature, prov->pubkey, (const uint8_t *)&h,
                                      SB_HDR_SIG_OFF) == 0;
    if (mark)
        mark(SB_MARK_SIG_END);
    if (!sig_ok)
        return SB_V_BAD_SIG;

    /* Checked after the signature, so it only rejects signed-but-broken builds. */
    if (!vectors_sane(region_addr + SB_HDR_SIZE, h.image_size))
        return SB_V_BAD_VECTORS;
    return SB_V_OK;
}

const sb_prov_t *sb_prov_get(void)
{
    const sb_prov_t *p = (const sb_prov_t *)sb_flash_ptr(SB_PROV_ADDR);
    if (p->magic != SB_PROV_MAGIC || p->crc != sb_crc32(0, p, offsetof(sb_prov_t, crc)))
        return NULL;
    return p;
}
