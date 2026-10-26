#include "sb/crash.h"

#include "sb/crc32.h"

void sb_crash_seal(sb_crash_t *c)
{
    c->magic = SB_CRASH_MAGIC;
    c->crc = sb_crc32(0, c, sb_crash_crc_len());
}

int sb_crash_valid(const sb_crash_t *c)
{
    return c->magic == SB_CRASH_MAGIC && c->crc == sb_crc32(0, c, sb_crash_crc_len());
}

/* Tiny formatter: the bootloader cannot afford printf. */
typedef struct {
    char *p;
    size_t n, cap;
} out_t;

static void put_s(out_t *o, const char *s)
{
    while (*s) {
        if (o->n + 1 < o->cap)
            o->p[o->n] = *s;
        o->n++;
        s++;
    }
    if (o->cap)
        o->p[o->n < o->cap ? o->n : o->cap - 1] = '\0';
}

static void put_x(out_t *o, uint32_t v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++)
        b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xFu];
    b[10] = '\0';
    put_s(o, b);
}

static void put_u(out_t *o, uint32_t v)
{
    char b[11];
    int i = 10;
    b[i] = '\0';
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put_s(o, b + i);
}

static void field(out_t *o, const char *name, uint32_t v)
{
    put_s(o, name);
    put_s(o, "=");
    put_x(o, v);
    put_s(o, " ");
}

static const char *const cfsr_bits[32] = {
    [0] = "IACCVIOL", [1] = "DACCVIOL", [3] = "MUNSTKERR", [4] = "MSTKERR", [5] = "MLSPERR",
    [7] = "MMARVALID", [8] = "IBUSERR", [9] = "PRECISERR", [10] = "IMPRECISERR",
    [11] = "UNSTKERR", [12] = "STKERR", [13] = "LSPERR", [15] = "BFARVALID",
    [16] = "UNDEFINSTR", [17] = "INVSTATE", [18] = "INVPC", [19] = "NOCP",
    [24] = "UNALIGNED", [25] = "DIVBYZERO",
};

static const char *fault_name(uint32_t t)
{
    switch (t) {
    case SB_FAULT_HARD: return "HardFault";
    case SB_FAULT_MEM: return "MemManage";
    case SB_FAULT_BUS: return "BusFault";
    case SB_FAULT_USAGE: return "UsageFault";
    case SB_FAULT_WDG: return "WatchdogEarlyWarning";
    case SB_FAULT_APP: return "AppAssert";
    default: return "Unknown";
    }
}

size_t sb_crash_format(const sb_crash_t *c, char *buf, size_t cap)
{
    out_t o = {buf, 0, cap};
    static const char *const rst[7] = {"BOR", "PIN", "POR", "SFT", "IWDG", "WWDG", "LPWR"};

    put_s(&o, "CRASH #");
    put_u(&o, c->count);
    put_s(&o, " ");
    put_s(&o, fault_name(c->fault_type));
    put_s(&o, " slot=");
    put_u(&o, c->slot);
    put_s(&o, " version=");
    put_u(&o, c->image_version);
    put_s(&o, "\r\n  ");
    field(&o, "PC", c->pc);
    field(&o, "LR", c->lr);
    field(&o, "xPSR", c->xpsr);
    field(&o, "SP", c->sp);
    field(&o, "EXC_RETURN", c->exc_return);
    put_s(&o, "\r\n  ");
    field(&o, "CFSR", c->cfsr);
    field(&o, "HFSR", c->hfsr);
    field(&o, "MMFAR", c->mmfar);
    field(&o, "BFAR", c->bfar);
    put_s(&o, "\r\n  CFSR:");
    for (int i = 0; i < 32; i++)
        if ((c->cfsr >> i & 1u) && cfsr_bits[i]) {
            put_s(&o, " ");
            put_s(&o, cfsr_bits[i]);
        }
    if (c->hfsr & (1u << 30))
        put_s(&o, " HFSR:FORCED");
    if (c->hfsr & (1u << 1))
        put_s(&o, " HFSR:VECTTBL");
    if (c->cfsr & (1u << 7))
        field(&o, " fault_addr", c->mmfar);
    else if (c->cfsr & (1u << 15))
        field(&o, " fault_addr", c->bfar);
    put_s(&o, "\r\n  regs: ");
    field(&o, "R0", c->r0);
    field(&o, "R1", c->r1);
    field(&o, "R2", c->r2);
    field(&o, "R3", c->r3);
    field(&o, "R12", c->r12);
    put_s(&o, "\r\n  reset:");
    for (int i = 0; i < 7; i++)
        if (c->reset_reason >> i & 1u) {
            put_s(&o, " ");
            put_s(&o, rst[i]);
        }
    put_s(&o, "\r\n");
    return o.n;
}
