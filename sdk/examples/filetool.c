/*
 * UEFI-DOS native SDK example -- FILETOOL.
 *
 * A small native (32/64-bit) utility that shows what the SDK can do without a
 * CRT.  It is also the reference "do real work in native mode" sample: long
 * file names, whole-file I/O, 32-bit file offsets and a 16-bit downcall.
 *
 *   C:\> FILETOOL                  list the current directory
 *   C:\> FILETOOL -l *.TXT         list matching files
 *   C:\> FILETOOL -r HELLO.TXT     hex dump the first 128 bytes
 *   C:\> FILETOOL -s HELLO.TXT     size + short/long names
 *   C:\> FILETOOL -w OUT.BIN 4096  write a 4096-byte file, then verify it
 *   C:\> FILETOOL -d               drives and file-system types
 *   C:\> FILETOOL -x HELLO.COM     downcall a 16-bit .COM and report its exit
 *
 * Build: make -C sdk   (or sdk\build_sdk.ps1 on Windows)
 */
#include "uefidos/dosapi.h"
#include "uefidos/runtime.h"

static void skip_spaces(const char **p) { while (**p == ' ') (*p)++; }

/* Split "VERB ARG1 ARG2" into up to four NUL-terminated tokens. */
static int tokenize(const char *args, char *verb, int vmax, char *a1, int a1max,
                    char *a2, int a2max)
{
    const char *p = args;
    char *dst[3];
    int max[3];
    dst[0] = verb; max[0] = vmax;
    dst[1] = a1;   max[1] = a1max;
    dst[2] = a2;   max[2] = a2max;
    int n = 0;
    skip_spaces(&p);
    while (*p && n < 3 && max[n] > 0) {
        int k = 0;
        while (*p && *p != ' ' && k < max[n] - 1) dst[n][k++] = *p++;
        dst[n][k] = 0;
        skip_spaces(&p);
        n++;
    }
    for (int i = n; i < 3; i++) dst[i][0] = 0;
    return n;
}

static u32 parse_u32(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (u32)(*s++ - '0');
    return v;
}

static void show_drives(void)
{
    int cur = (int)g_api->svc->get_drive();
    rt_puts("drive  fs      status\n");
    for (int d = 0; d < 26; d++) {
        int fs = g_api->svc->fs_type((u8)d);
        if (!fs) continue;
        rt_putc((char)('A' + d));
        rt_puts(":     FAT");
        rt_putu((u32)fs);
        rt_puts(fs < 10 ? "     " : "    ");
        rt_puts(d == cur ? "current\n" : "ok\n");
    }
}

static void show_file(const char *path)
{
    u32 size = rt_file_size(path);
    char lname[DOSAPI_LFN_MAX];
    lname[0] = 0;
    g_api->svc->longname(path, lname, sizeof(lname));
    rt_puts("name  : ");
    rt_puts(path);
    rt_puts("\nlong  : ");
    rt_puts(lname[0] ? lname : "(none)");
    rt_puts("\nsize  : ");
    rt_putu(size);
    rt_puts(" bytes\n");
}

static void hexdump(const char *path, u32 want)
{
    static u8 buf[512];
    u32 got = 0;
    if (want > sizeof(buf)) want = sizeof(buf);
    u16 rc = rt_read_file(path, buf, want, &got);
    if (rc) {
        rt_puts("read failed, DOS error ");
        rt_putu(rc);
        rt_putc('\n');
        return;
    }
    for (u32 off = 0; off < got; off += 16) {
        rt_putx(off, 4);
        rt_puts(": ");
        for (u32 i = 0; i < 16; i++) {
            if (off + i < got) rt_putx(buf[off + i], 2);
            else rt_puts("  ");
            rt_putc(' ');
        }
        rt_putc(' ');
        for (u32 i = 0; i < 16 && off + i < got; i++) {
            u8 c = buf[off + i];
            rt_putc((c >= 32 && c < 127) ? (char)c : '.');
        }
        rt_putc('\n');
    }
    rt_puts("bytes read: ");
    rt_putu(got);
    rt_putc('\n');
}

/* Write a pattern file and read it back: proves 32-bit offsets and sizes in
 * both directions.  Files this big do not fit a 16-bit DOS buffer. */
static void write_pattern(const char *path, u32 size)
{
    static u8 buf[4096];
    for (u32 i = 0; i < sizeof(buf); i++) buf[i] = (u8)(i & 0xFF);
    u16 h = 0;
    u16 rc = g_api->svc->create(path, DOS_ATTR_ARCHIVE, &h);
    if (rc) { rt_puts("create failed\n"); return; }
    u32 written = 0;
    while (written < size) {
        u32 chunk = size - written;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        u32 put = 0;
        rc = g_api->svc->write(h, buf, chunk, &put);
        if (rc || !put) break;
        written += put;
    }
    g_api->svc->close(h);
    rt_puts("wrote ");
    rt_putu(written);
    rt_puts(" bytes to ");
    rt_puts(path);
    rt_puts("\n");

    /* verify: read the whole thing back and check the pattern */
    u16 hv = 0;
    if (g_api->svc->open(path, DOS_OPEN_READ, &hv)) { rt_puts("reopen failed\n"); return; }
    u32 total = 0, bad = 0;
    for (;;) {
        u32 n = 0;
        if (g_api->svc->read(hv, buf, sizeof(buf), &n) || !n) break;
        for (u32 i = 0; i < n; i++) {
            if (buf[i] != (u8)((total + i) & 0xFF)) bad++;
        }
        total += n;
    }
    g_api->svc->close(hv);
    rt_puts("read back ");
    rt_putu(total);
    rt_puts(" bytes, mismatches: ");
    rt_putu(bad);
    rt_putc('\n');
    rt_puts(bad == 0 && total == size ? "FILEIO-OK\n" : "FILEIO-FAIL\n");
}

int uefidos_main(const uefidos_exports *api, const char *args)
{
    uefidos_init(api, args);

    char verb[8], a1[64], a2[16];
    int n = tokenize(args, verb, sizeof(verb), a1, sizeof(a1), a2, sizeof(a2));

    rt_puts("FILETOOL (native ");
    rt_putu(api->pointer_size * 8);
    rt_puts("-bit, API ");
    rt_putx(api->version, 4);
    rt_puts(")\n");

    if (!n || !verb[0]) {
        rt_list("*.*");
        return 0;
    }
    if (rt_streq(verb, "-l")) {
        rt_list(a1[0] ? a1 : "*.*");
    } else if (rt_streq(verb, "-r")) {
        if (!a1[0]) { rt_puts("usage: FILETOOL -r FILE\n"); return 1; }
        hexdump(a1, 128);
    } else if (rt_streq(verb, "-s")) {
        if (!a1[0]) { rt_puts("usage: FILETOOL -s FILE\n"); return 1; }
        show_file(a1);
    } else if (rt_streq(verb, "-w")) {
        if (!a1[0]) { rt_puts("usage: FILETOOL -w FILE [SIZE]\n"); return 1; }
        u32 size = a2[0] ? parse_u32(a2) : 4096u;
        write_pattern(a1, size);
    } else if (rt_streq(verb, "-d")) {
        show_drives();
    } else if (rt_streq(verb, "-x")) {
        if (!a1[0]) { rt_puts("usage: FILETOOL -x PROG.COM\n"); return 1; }
        i16 code = 0;
        u16 rc = rt_exec16(a1, "", &code);
        if (rc) {
            rt_puts("exec16 failed, DOS error ");
            rt_putu(rc);
            rt_putc('\n');
            return 1;
        }
        rt_puts("child exit code: ");
        rt_putu((u32)(u16)code);
        rt_puts("  -> EXEC16-OK\n");
    } else {
        rt_puts("unknown option: ");
        rt_puts(verb);
        rt_puts("\n");
        return 1;
    }
    rt_puts("FILETOOL-DONE\n");
    return 0;
}
