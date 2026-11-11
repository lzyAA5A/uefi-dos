/*
 * UEFI-DOS native SDK -- tiny freestanding runtime helpers.
 *
 * Everything here is header-only and built on the DOS service table, so a
 * native app needs no CRT: it prints through the console handle, walks
 * directories, reads and writes whole files, formats numbers, and can even
 * downcall into a 16-bit .COM/.EXE.
 *
 * All helpers are `static` and marked unused, so an app pays only for what it
 * calls and an unused helper never warns.
 */
#ifndef UEFIDOS_SDK_RUNTIME_H
#define UEFIDOS_SDK_RUNTIME_H

#include "dosapi.h"

static const uefidos_exports *g_api __attribute__((unused));
static const char *g_args __attribute__((unused));

/* ---- init --------------------------------------------------------------- */
/* Call this first.  g_args then holds the raw command tail (never NULL). */
static void __attribute__((unused)) uefidos_init(const uefidos_exports *api, const char *args)
{
    g_api = api;
    g_args = args ? args : "";
}

/* ---- console ------------------------------------------------------------ */
/* The kernel keeps handle 1 mapped to the console for every process, so
 * "write to handle 1" is the native equivalent of INT 21h AH=40h on STDOUT. */
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

/* Right-aligned number, so column output stays readable. */
static __attribute__((unused)) void rt_putu_pad(u32 v, int width)
{
    u32 t = v, digits = 1;
    while (t >= 10u) { t /= 10u; digits++; }
    for (u32 i = digits; i < (u32)width; i++) rt_putc(' ');
    rt_putu(v);
}

/* ---- strings / memory --------------------------------------------------- */
static __attribute__((unused)) u32 rt_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static __attribute__((unused)) int rt_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static __attribute__((unused)) int rt_strncmp(const char *a, const char *b, u32 n)
{
    while (n--) {
        if (*a != *b) return (int)(u8)*a - (int)(u8)*b;
        if (!*a) return 0;
        a++; b++;
    }
    return 0;
}

static __attribute__((unused)) char *rt_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) { }
    return dst;
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

/* ---- whole-file helpers ------------------------------------------------- */
/* Read up to `max` bytes.  Returns 0 on success (a DOS error code otherwise)
 * and stores the byte count in *got. */
static __attribute__((unused)) u16 rt_read_file(const char *path, void *buf, u32 max, u32 *got)
{
    u16 h = 0;
    *got = 0;
    u16 rc = g_api->svc->open(path, DOS_OPEN_READ, &h);
    if (rc) return rc;
    u32 total = 0;
    while (total < max) {
        u32 chunk = max - total, n = 0;
        rc = g_api->svc->read(h, (u8 *)buf + total, chunk, &n);
        if (rc) break;
        total += n;
        if (n < chunk) break;
    }
    g_api->svc->close(h);
    *got = total;
    return rc;
}

/* Create (or truncate) a file and write `len` bytes. */
static __attribute__((unused)) u16 rt_write_file(const char *path, const void *buf, u32 len)
{
    u16 h = 0;
    u16 rc = g_api->svc->create(path, DOS_ATTR_ARCHIVE, &h);
    if (rc) return rc;
    u32 total = 0;
    while (total < len) {
        u32 n = 0;
        rc = g_api->svc->write(h, (const u8 *)buf + total, len - total, &n);
        if (rc) break;
        if (!n) break;
        total += n;
    }
    g_api->svc->close(h);
    return rc;
}

/* File size, or 0 when the file does not exist. */
static __attribute__((unused)) u32 rt_file_size(const char *path)
{
    u16 h = 0;
    if (g_api->svc->open(path, DOS_OPEN_READ, &h)) return 0;
    u32 size = 0;
    g_api->svc->seek(h, 0, DOS_SEEK_END, &size);
    g_api->svc->close(h);
    return size;
}

/* ---- directories -------------------------------------------------------- */
/* Print one line per match, "<8.3 name>  <size>  <long name>".
 * `pattern` is an 8.3 wildcard such as "*.TXT" or "*.*". */
static __attribute__((unused)) u32 rt_list(const char *pattern)
{
    dosapi_findinfo fi;
    u32 count = 0;
    if (g_api->svc->find_first(pattern, DOS_ATTR_ARCHIVE | DOS_ATTR_DIR, &fi)) {
        rt_puts("(no files)\n");
        return 0;
    }
    do {
        char nm[14];
        rt_memcpy(nm, fi.name, 13);
        nm[13] = 0;
        rt_puts(nm);
        for (u32 i = rt_strlen(nm); i < 13; i++) rt_putc(' ');
        if (fi.attr & DOS_ATTR_DIR) {
            rt_puts("   <DIR>");
        } else {
            rt_putu_pad(fi.size, 9);
        }
        if (fi.long_name[0]) {
            rt_puts("  ");
            rt_puts(fi.long_name);
        }
        rt_putc('\n');
        count++;
    } while (g_api->svc->find_next(&fi) == 0);
    return count;
}

/* ---- downcall into 16-bit code ------------------------------------------ */
static __attribute__((unused)) u16 rt_exec16(const char *prog, const char *args, i16 *code)
{
    return g_api->svc->exec16(prog, args ? args : "", code);
}

#endif
