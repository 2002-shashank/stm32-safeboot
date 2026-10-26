#include "testutil.h"

#include <stddef.h>
#include <string.h>

#include "flash_sim.h"
#include "monocypher-ed25519.h"
#include "sb/crc32.h"
#include "sb/flash.h"
#include "sb/sha256.h"

int tu_failures;

void tu_keys(uint8_t sk[64], uint8_t pk[32])
{
    uint8_t seed[32];
    for (int i = 0; i < 32; i++)
        seed[i] = (uint8_t)(0xA0 + i);
    crypto_ed25519_key_pair(sk, pk, seed);   /* wipes seed */
}

uint32_t tu_make_image(uint8_t *buf, uint32_t region_addr, uint32_t version,
                       uint32_t payload_size, uint32_t flags)
{
    uint8_t sk[64], pk[32];
    tu_keys(sk, pk);

    uint8_t *pl = buf + SB_HDR_SIZE;
    uint32_t x = version * 2654435761u + 1;
    for (uint32_t i = 0; i < payload_size; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        pl[i] = (uint8_t)x;
    }
    uint32_t entry = region_addr + SB_HDR_SIZE + 0x200u + 1u;   /* Thumb */
    uint32_t sp = SB_SRAM_BASE + SB_APP_RAM_SIZE;
    memcpy(pl, &sp, 4);
    memcpy(pl + 4, &entry, 4);

    sb_image_hdr_t h;
    memset(&h, 0xFF, sizeof h);
    h.magic = SB_IMG_MAGIC;
    h.hdr_version = SB_IMG_HDR_VER;
    h.hdr_size = SB_HDR_SIZE;
    h.image_version = version;
    h.image_size = payload_size;
    h.load_addr = region_addr;
    h.flags = flags;
    h.key_id = TU_KEY_ID;
    h.reserved0 = 0xFFFFFFFFu;
    sb_sha256_ctx c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, pl, payload_size);
    sb_sha256_final(&c, h.sha256);
    crypto_ed25519_sign(h.signature, sk, (const uint8_t *)&h, SB_HDR_SIG_OFF);
    memcpy(buf, &h, sizeof h);
    return SB_HDR_SIZE + payload_size;
}

void tu_factory_reset(void)
{
    uint8_t sk[64];
    sb_prov_t p;
    memset(&p, 0, sizeof p);
    p.magic = SB_PROV_MAGIC;
    p.key_id = TU_KEY_ID;
    tu_keys(sk, p.pubkey);
    for (int i = 0; i < 16; i++)
        p.device_id[i] = (uint8_t)i;
    p.crc = sb_crc32(0, &p, offsetof(sb_prov_t, crc));
    fsim_erase_all();
    memcpy(fsim_mem() + (SB_PROV_ADDR - SB_FLASH_BASE), &p, sizeof p);
}

void tu_flash_image(uint32_t region_addr, uint32_t version, uint32_t payload_size)
{
    static uint8_t buf[SB_SLOT_SIZE];
    uint32_t n = tu_make_image(buf, region_addr, version, payload_size,
                               region_addr == SB_RECOVERY_ADDR ? SB_IMG_F_RECOVERY : 0);
    memcpy(fsim_mem() + (region_addr - SB_FLASH_BASE), buf, n);
}
