/*
 * UEFI-DOS native SDK -- tiny freestanding runtime helpers.
 *
 * Everything here is header-only and built on the DOS service table, so a
 * native app needs no CRT: it prints through INT 21h-equivalent calls, finds
 * files, reads the clock and can even downcall into a 16-bit .COM.
 */
#ifndef UEFIDOS_SDK_RUNTIME_H
#define UEFIDOS_SDK_RUNTIME_H

#include "dosapi.h"

static const uefidos_exports *g_api;

/* ---- console ------------------------------------------------------------ */
/* DOS has no "write to console" file service in this API, so the runtime uses
 * a scratch file descriptor bound to handle 1 (STDOUT), which the kernel keeps
 * mapped to the console for every process. */
static void __attribute__((unused)) uefidos_init(const uefidos_exports *api) { g_api = api; }

static __attribute__((unused)) u16 rt_write_stdout(const void *buf, u32 len)
{
    u32 put = 0;
    return g_api->svc->write(1, buf, len, &put);
}

static __attribute__((unused)) void rt_putc(char c) { rt_write_stdout(&c, 1); }

static __attribute__((unused)) void rt_puts(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    if (n) rt_write_stdout(s, n);
}

static __attribute__((unused)) void rt_putu(u32 v)
{
    char t[12];
    int n = 0;
    if (!v) { rt_putc('0'); return; }
    while (v) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) rt_putc(t[--n]);
}

static __attribute__((unused)) void rt_putx(u32 v, int digits)
{
    static const char *hex = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) rt_putc(hex[(v >> i) & 0xF]);
}

/* ---- helpers ------------------------------------------------------------ */
static __attribute__((unused)) u32 rt_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static __attribute__((unused)) int rt_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static __attribute__((unused)) void rt_memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

static __attribute__((unused)) void rt_memset(void *dst, int v, u32 n)
{
    u8 *d = (u8 *)dst;
    while (n--) *d++ = (u8)v;
}

#endif
