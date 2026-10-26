/*
 * Signed image format: a 512-byte header at the start of a slot, followed by
 * the payload (vector table first). The Ed25519 signature covers header bytes
 * [0, SB_HDR_SIG_OFF), which include the SHA-256 of the payload.
 */
#ifndef SB_IMAGE_H
#define SB_IMAGE_H

#include <stdint.h>

#include "sb/layout.h"

#define SB_IMG_MAGIC      0x4D494253u   /* "SBIM" */
#define SB_IMG_HDR_VER    1u
#define SB_HDR_SIG_OFF    (SB_HDR_SIZE - 64u)

#define SB_IMG_F_RECOVERY 0x00000001u   /* golden recovery image, S11 only */

typedef struct {
    uint32_t magic;          /*   0 */
    uint16_t hdr_version;    /*   4 */
    uint16_t hdr_size;       /*   6 */
    uint32_t image_version;  /*   8 */
    uint32_t image_size;     /*  12 payload bytes, multiple of 4 */
    uint32_t load_addr;      /*  16 address of this header (slot base) */
    uint32_t flags;          /*  20 */
    uint32_t key_id;         /*  24 */
    uint32_t reserved0;      /*  28 */
    uint8_t sha256[32];      /*  32 digest of payload */
    uint8_t reserved[SB_HDR_SIG_OFF - 64]; /* 64..447, must be 0xFF */
    uint8_t signature[64];   /* 448 */
} sb_image_hdr_t;

_Static_assert(sizeof(sb_image_hdr_t) == SB_HDR_SIZE, "header must be 512 bytes");

/* Provisioning block at the start of S4, written once by the programmer. */
#define SB_PROV_MAGIC 0x56525053u   /* "SPRV" */
typedef struct {
    uint32_t magic;
    uint32_t key_id;
    uint8_t pubkey[32];
    uint8_t device_id[16];
    uint32_t crc;            /* CRC32 of the preceding bytes */
} sb_prov_t;

enum sb_verify_result {
    SB_V_OK = 0,
    SB_V_BAD_MAGIC = -1,
    SB_V_BAD_LAYOUT = -2,    /* size/load_addr/flags inconsistent with the slot */
    SB_V_BAD_KEY = -3,
    SB_V_BAD_HASH = -4,
    SB_V_BAD_SIG = -5,
    SB_V_BAD_VECTORS = -6,
};

/* Timing hook for DWT checkpoints; may be NULL. */
typedef void (*sb_mark_fn)(int checkpoint);
enum { SB_MARK_SHA_START, SB_MARK_SHA_END, SB_MARK_SIG_END };

/* Header-only checks (no crypto): magic, version, size, alignment, that
 * load_addr == region_addr, and that the payload fits the region. */
int sb_image_check_header(const sb_image_hdr_t *h, uint32_t region_addr, uint32_t region_size);

/* Full verification of the image stored at region_addr: header checks,
 * SHA-256 of the payload, Ed25519 signature, and sane vector table. */
int sb_image_verify(uint32_t region_addr, uint32_t region_size, const sb_prov_t *prov,
                    sb_mark_fn mark);

/* Returns the provisioning block or NULL if missing/corrupt. */
const sb_prov_t *sb_prov_get(void);

#endif
