#include "rtt.h"

#include <stdarg.h>
#include <string.h>

#include "sb/layout.h"

#define UP_SIZE   2560u
#define DOWN_SIZE 64u

typedef struct {
    const char *name;
    char *buf;
    uint32_t size;
    volatile uint32_t wr;
    volatile uint32_t rd;
    uint32_t flags;      /* 0 = skip when full */
} rtt_ring_t;

typedef struct {
    char id[16];
    int32_t max_up;
    int32_t max_down;
    rtt_ring_t up[1];
    rtt_ring_t down[1];
    char up_buf[UP_SIZE];
    char down_buf[DOWN_SIZE];
} rtt_cb_t;

_Static_assert(sizeof(rtt_cb_t) <= SB_RTT_SIZE, "RTT block exceeds reserved RAM");

#define CB ((rtt_cb_t *)SB_RTT_ADDR)

static const char id_str[16] = "SEGGER RTT";

void sb_rtt_init(void)
{
    rtt_cb_t *cb = CB;
    if (memcmp(cb->id, id_str, sizeof id_str) == 0 && cb->max_up == 1 && cb->max_down == 1 &&
        cb->up[0].buf == cb->up_buf && cb->up[0].size == UP_SIZE && cb->up[0].wr < UP_SIZE &&
        cb->up[0].rd < UP_SIZE && cb->down[0].buf == cb->down_buf)
        return;   /* intact block from the previous stage */

    memset(cb->id, 0, sizeof cb->id);   /* hide the block while it is rebuilt */
    cb->max_up = 1;
    cb->max_down = 1;
    cb->up[0] = (rtt_ring_t){"log", cb->up_buf, UP_SIZE, 0, 0, 0};
    cb->down[0] = (rtt_ring_t){"cmd", cb->down_buf, DOWN_SIZE, 0, 0, 0};
    __asm volatile("dmb" ::: "memory");
    memcpy(cb->id, id_str, sizeof id_str);   /* id last: block is now valid */
    __asm volatile("dmb" ::: "memory");
}

size_t sb_rtt_write(const void *data, size_t len)
{
    rtt_ring_t *r = &CB->up[0];
    const char *p = data;
    uint32_t wr = r->wr, rd = r->rd;
    uint32_t avail = rd > wr ? rd - wr - 1 : r->size - (wr - rd) - 1;
    if (len > avail)
        len = avail;
    for (size_t i = 0; i < len; i++) {
        r->buf[wr] = p[i];
        if (++wr == r->size)
            wr = 0;
    }
    __asm volatile("dmb" ::: "memory");
    r->wr = wr;
    return len;
}

size_t sb_rtt_read(void *buf, size_t cap)
{
    rtt_ring_t *r = &CB->down[0];
    char *p = buf;
    uint32_t rd = r->rd, wr = r->wr;
    size_t n = 0;
    while (rd != wr && n < cap) {
        p[n++] = r->buf[rd];
        if (++rd == r->size)
            rd = 0;
    }
    r->rd = rd;
    return n;
}

static void out_num(uint32_t v, int base, int width, char pad, int neg, int upper)
{
    char t[12];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        t[n++] = digits[v % (uint32_t)base];
        v /= (uint32_t)base;
    } while (v);
    if (neg)
        t[n++] = '-';
    while (n < width && n < (int)sizeof t)
        t[n++] = pad;
    char o[12];
    for (int i = 0; i < n; i++)
        o[i] = t[n - 1 - i];
    sb_rtt_write(o, (size_t)n);
}

void sb_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            const char *s = f;
            while (f[1] && f[1] != '%')
                f++;
            sb_rtt_write(s, (size_t)(f - s + 1));
            continue;
        }
        f++;
        char pad = ' ';
        int width = 0;
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            sb_rtt_write(s, strlen(s));
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            sb_rtt_write(&c, 1);
            break;
        }
        case 'u':
            out_num(va_arg(ap, uint32_t), 10, width, pad, 0, 0);
            break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            out_num(v < 0 ? (uint32_t)-v : (uint32_t)v, 10, width, pad, v < 0, 0);
            break;
        }
        case 'x':
        case 'X':
            out_num(va_arg(ap, uint32_t), 16, width, pad, 0, *f == 'X');
            break;
        case '%':
            sb_rtt_write("%", 1);
            break;
        default:
            if (!*f)
                f--;
            break;
        }
    }
    va_end(ap);
}
