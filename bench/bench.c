/*
 * UEFI-DOS benchmark -- the 32-bit and 64-bit native halves.
 *
 * Identical workloads to bench/make_bench16.py's BENCH16.COM; the sizes come
 * from bench/bench_sizes.h, which the generator writes, so all three worlds do
 * exactly the same work.  Build with -m32 (PE32) and -m64 (PE32+):
 *
 *   gcc -m64 -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
 *       -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -fno-pic \
 *       -Isdk/include -nostdlib -Wl,--subsystem,10 -Wl,-e,uefidos_main \
 *       -Wl,--enable-reloc-section -Wl,--no-insert-timestamp \
 *       -o BENCH64.EXE bench/bench.c
 *
 * Timing: the native worlds run on the real CPU, so they read RDTSC.  The
 * native API's get_ticks() cannot be used here -- it is maintained by the
 * interpreter, which is not running while native code executes -- and the
 * driver converts cycles to microseconds with the measured TSC frequency.
 *
 * Workloads are written so the compiler cannot delete them: every buffer is
 * `volatile` and every loop folds something into a checksum that is printed.
 * The same shape is what the 16-bit version does, byte at a time, which is the
 * point of the comparison.
 */
#include "uefidos/dosapi.h"
#include "uefidos/runtime.h"
#include "bench_sizes.h"

static volatile u8 buf[BENCH_BUF];
static u8 chunk[BENCH_CHUNK];
static const char *g_world;

static u64 rdtsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

/* ---- output ------------------------------------------------------------- */
static void put_u64(u64 v)
{
    char t[24];
    int n = 0;
    if (!v) { rt_putc('0'); return; }
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) rt_putc(t[--n]);
}

static void put_hex16(u16 v)
{
    static const char *hex = "0123456789ABCDEF";
    for (int i = 12; i >= 0; i -= 4) rt_putc(hex[(v >> i) & 0xF]);
}

static void report(const char *name, u16 cs, u64 cycles)
{
    rt_puts("BENCH ");
    rt_puts(name);
    rt_puts(" cycles=");
    put_u64(cycles);
    rt_puts(" cs=");
    put_hex16(cs);
    rt_putc('\r');
    rt_putc('\n');
}

/* ---- workloads ---------------------------------------------------------- */
static u16 w_alu(void)
{
    u16 x = 1, cs = 0;
    u32 n = BENCH_ALU_N;
    while (n--) {
        x = (u16)(x * 1103u + 12345u);
        cs = (u16)(cs ^ x);
    }
    return (u16)(cs ^ x);           /* the 16-bit version folds the final x in too */
}

static u16 w_fill(void)
{
    for (u32 r = 0; r < BENCH_FILL_R; r++) {
        u8 v = 0;
        for (u32 i = 0; i < BENCH_BUF; i++) {
            buf[i] = v;
            v = (u8)(v + 1);
        }
    }
    return (u16)(u16)(buf[0] ^ buf[BENCH_BUF - 1]);
}

static u16 w_copy(void)
{
    for (u32 r = 0; r < BENCH_COPY_R; r++)
        for (u32 i = 0; i < BENCH_HALF; i++)
            buf[BENCH_HALF + i] = buf[i];
    return (u16)(u16)(buf[BENCH_HALF] ^ buf[BENCH_BUF - 1]);
}

static u16 w_scan(void)
{
    u32 sum = 0;
    for (u32 r = 0; r < BENCH_SCAN_R; r++)
        for (u32 i = 0; i < BENCH_BUF; i++)
            sum += buf[i];
    return (u16)((sum & 0xFFFFu) ^ (sum >> 16));
}

static u16 w_api(void)
{
    u16 cs = 0;
    for (u32 i = 0; i < BENCH_API_N; i++)
        cs = (u16)(cs + g_api->svc->get_version());
    return cs;
}

static u16 w_text(void)
{
    for (u32 i = 0; i < BENCH_TEXT_N; i++)
        rt_putc('x');
    return (u16)(BENCH_TEXT_N & 0xFFFF);
}

static u16 w_file(void)
{
    u16 h = 0, rc, cs = 0;
    u32 got = 0, pos = 0;
    u32 blocks = BENCH_FILE_N / BENCH_CHUNK;

    rc = g_api->svc->create("BENCH.DAT", 0, &h);
    if (rc) { rt_puts("BENCH file: create failed\r\n"); return 0; }
    for (u32 i = 0; i < blocks; i++) {
        rc = g_api->svc->write(h, chunk, BENCH_CHUNK, &got);
        if (rc) break;
    }
    g_api->svc->close(h);
    rc = g_api->svc->open("BENCH.DAT", DOS_OPEN_READ, &h);
    if (rc == 0) {
        for (u32 i = 0; i < blocks; i++) {
            rc = g_api->svc->read(h, chunk, BENCH_CHUNK, &got);
            cs = (u16)(cs + (u16)got);
            if (rc) break;
        }
        g_api->svc->close(h);
    }
    pos = 0;
    g_api->svc->seek(0, 0, DOS_SEEK_SET, &pos);     /* no-op, keeps the API honest */
    g_api->svc->unlink("BENCH.DAT");
    return cs;
}

static u16 w_find(void)
{
    static dosapi_findinfo fi;
    u16 count = 0, rc;
    g_api->svc->chdir("\\BDATA");
    for (u32 r = 0; r < BENCH_FIND_R; r++) {
        rc = g_api->svc->find_first("*.*", 0x3F, &fi);
        while (rc == 0) {
            count++;
            rc = g_api->svc->find_next(&fi);
        }
    }
    g_api->svc->chdir("\\");
    return count;
}

/* ---- driver ------------------------------------------------------------- */
static void run(const char *name, u16 (*fn)(void))
{
    u64 t0 = rdtsc();
    u16 cs = fn();
    u64 t1 = rdtsc();
    report(name, cs, t1 - t0);
}

int uefidos_main(const uefidos_exports *api, const char *args)
{
    uefidos_init(api, args);
    g_world = (api->pointer_size == 8) ? "native64" : "native32";
    rt_puts("BENCH world=");
    rt_puts(g_world);
    rt_puts(" pointers=");
    put_u64(api->pointer_size);
    rt_putc('\r');
    rt_putc('\n');

    run("alu", w_alu);
    run("fill", w_fill);
    run("copy", w_copy);
    run("scan", w_scan);
    run("api", w_api);
    run("text", w_text);
    run("file", w_file);
    run("find", w_find);

    rt_puts("BENCH done world=");
    rt_puts(g_world);
    rt_putc('\r');
    rt_putc('\n');
    return 0;
}
