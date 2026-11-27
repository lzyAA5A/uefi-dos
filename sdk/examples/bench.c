/* bench.c -- BENCH32.EXE / BENCH64.EXE: the same performance probe as
 * AUTOBANC.COM (16-bit real mode under the emulator), but as a native UEFI-DOS
 * application, so the same machine can be measured three ways:
 *
 *   AUTOBANC.COM  16-bit x86 through the emulator (the Windows 1.x path)
 *   BENCH32.EXE   32-bit native (i386 code the firmware loader runs directly)
 *   BENCH64.EXE   64-bit native (x86-64)
 *
 * All three append their results to the same B<yymmdd>.TXT file, which is what
 * makes the three lines comparable on one machine.
 *
 * Clock: RDTSC, calibrated against the 18.2 Hz BIOS tick through the SDK
 * (get_ticks).  One second of TSC is therefore one second of wall-clock time,
 * and each window runs until it has spent exactly that much.
 *
 * Freestanding: no libc, no libm (the SDK links -nostdlib).  Everything used
 * here -- number printing, sqrt (Newton) -- is written below.
 *
 * Build: sdk/build_sdk.ps1 (targets BENCH32/BENCH64).
 */

#include "uefidos/dosapi.h"

#if __SIZEOF_POINTER__ == 8
#define BW "64"
#define BITS 64
typedef unsigned long long word_t;
#else
#define BW "32"
#define BITS 32
typedef unsigned int word_t;
#endif

static const dosapi_services *S;

/* ------------------------------------------------------------------ */
/* output: console (handle 1) and one shared result file               */
/* ------------------------------------------------------------------ */
static char conbuf[256];
static u32 conn;
static char fbuf[4096];
static u32 fn_;
static u16 fhandle = 0xFFFF;
static char fname[13];

static void con_flush(void)
{
    u32 put = 0;
    if (conn && S) S->write(1, conbuf, conn, &put);
    conn = 0;
}

static void file_flush(void)
{
    u32 put = 0;
    if (fn_ && fhandle != 0xFFFF) S->write(fhandle, fbuf, fn_, &put);
    fn_ = 0;
}

static void outc(char c)
{
    if (conn >= sizeof(conbuf)) con_flush();
    conbuf[conn++] = c;
    if (fn_ >= sizeof(fbuf)) file_flush();
    fbuf[fn_++] = c;
}

static void outs(const char *s)
{
    while (*s) outc(*s++);
}

/* 64-bit division by a small divisor with 32-bit arithmetic only: the SDK links
 * -nostdlib and this mingw toolchain ships no 32-bit libgcc, so __udivdi3 does
 * not exist for the i386 target.  Divisor must be below 2^31. */
static u64 udiv_small(u64 v, u32 d, u32 *rem)
{
    u32 hi = (u32)(v >> 32), lo = (u32)v;
    u32 qhi = hi / d;
    u32 r = hi % d;
    u32 qlo = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((lo >> i) & 1u);
        if (r >= d) { r -= d; qlo |= (1u << i); }
    }
    if (rem) *rem = r;
    return ((u64)qhi << 32) | (u64)qlo;
}

static void outu(u64 v)
{
    char b[24];
    int n = 0;
    if (!v) { outc('0'); return; }
    while (v && n < 20) {
        u32 r = 0;
        v = udiv_small(v, 10u, &r);
        b[n++] = (char)('0' + (int)r);
    }
    while (n) outc(b[--n]);
}

static void field(const char *label, u64 v, const char *unit)
{
    int n = 0;
    while (label[n]) n++;
    outs(label);
    while (n++ < 22) outc('.');
    outc(' ');
    outu(v);
    outs(unit);
    outs("\n");
}

/* ------------------------------------------------------------------ */
/* timebase                                                            */
/* ------------------------------------------------------------------ */
static u64 rdtsc(void)
{
#if BITS == 64
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
#else
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
#endif
}

static u64 tsc_per_sec;

/* ------------------------------------------------------------------ */
/* buffers                                                             */
/* ------------------------------------------------------------------ */
static unsigned char src[4096];
static unsigned char dst[4096];
static unsigned char sieve[65536];
static unsigned short sortbuf[1024];
static unsigned short sortwork[1024];
static unsigned int mat_a[32 * 32], mat_b[32 * 32], mat_c[32 * 32];
static volatile u64 sink;

/* ------------------------------------------------------------------ */
/* tests: each fills cnt with the work done in one second              */
/* ------------------------------------------------------------------ */
static u64 bench_alu(void)
{
    word_t x = 1, y = 3, z = 5;
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i;
        for (i = 0; i < 256; i++) {
            x += y; y ^= z; x ^= (y << 1); y += x;
            z += y; x ^= z; y = x + 1; z ^= y;
        }
        cnt += 256 * 8;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    sink = x + y + z;
    return cnt;
}

static u64 bench_mul(void)
{
    word_t a = 3, b = 5;
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i;
        for (i = 0; i < 256; i++) {
            a = a * 25173u + 13849u;
            b = b * 1103515245u + 12345u;
        }
        cnt += 256 * 2;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    sink = a + b;
    return cnt;
}

static u64 bench_div(void)
{
    word_t a = 1000000;
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i;
        for (i = 0; i < 256; i++) {
            a = a / 7u + 13u;
            if (a > 1000000u) a = 1000000u;
        }
        cnt += 256;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    sink = a;
    return cnt;
}

/* 4 KB per unit, word at a time (the loop the 16-bit version also uses) */
static u64 bench_copy(void)
{
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i;
        for (i = 0; i < 32; i++) {
            word_t *d = (word_t *)dst, *s = (word_t *)src;
            int n = 4096 / (int)sizeof(word_t);
            while (n--) *d++ = *s++;
        }
        cnt += 32 * 4;                 /* 32 x 4 KB */
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    return cnt;
}

static u64 bench_fill(void)
{
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i;
        for (i = 0; i < 32; i++) {
            word_t *d = (word_t *)dst;
            int n = 4096 / (int)sizeof(word_t);
            word_t v = 0x1234u;
            while (n--) *d++ = v;
        }
        cnt += 32 * 4;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    return cnt;
}

static u64 bench_fpu(void)
{
    double x = 1.5, y = 2.25, z = 0.75, w = 3.5;
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i;
        for (i = 0; i < 256; i++) {
            x = x + y;
            x = x * z;
            x = x - w;
            x = x / y;
            if (x > 1e6 || x < -1e6) x = 1.5;
        }
        cnt += 256 * 4;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    sink = (u64)(x < 0 ? -x : x);
    return cnt;
}

/* square root by Newton iteration -- the SDK has no libm */
static double dsqrt(double v)
{
    double g = v * 0.5 + 0.5;
    int i;
    for (i = 0; i < 20; i++) g = 0.5 * (g + v / g);
    return g;
}

static u64 bench_sqrt(void)
{
    double v = 2.0;
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i;
        for (i = 0; i < 64; i++) {
            v = dsqrt(v + 1.0);
            if (v > 1000000.0) v = 2.0;
        }
        cnt += 64;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    sink = (u64)v;
    return cnt;
}

static u64 bench_sieve(void)
{
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i, j, primes = 0;
        for (i = 0; i < 65536; i++) sieve[i] = 0;
        for (i = 2; i < 256; i++) {
            if (!sieve[i]) {
                for (j = i * i; j < 65536; j += i) sieve[j] = 1;
            }
        }
        for (i = 2; i < 65536; i++) if (!sieve[i]) primes++;
        sink = (u64)primes;
        cnt++;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    return cnt;
}

static u64 bench_sort(void)
{
    unsigned s = 12345u;
    int i;
    for (i = 0; i < 1024; i++) { s = s * 25173u + 13849u; sortbuf[i] = (unsigned short)(s >> 3); }
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        unsigned short *a = sortwork;
        int k;
        for (k = 0; k < 1024; k++) a[k] = sortbuf[k];
        for (k = 1; k < 1024; k++) {
            unsigned short key = a[k];
            int m = k - 1;
            while (m >= 0 && a[m] > key) { a[m + 1] = a[m]; m--; }
            a[m + 1] = key;
        }
        sink = a[0] + a[1023];
        cnt++;
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    return cnt;
}

static u64 bench_crc(void)
{
    u32 crc = 0xFFFFFFFFu;
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        int i, b;
        for (i = 0; i < 4096; i++) {
            crc ^= src[i];
            for (b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(int)(crc & 1u)));
        }
        sink = crc;
        cnt += 4;                      /* 4 KB per pass */
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    return cnt;
}

static u64 bench_mat(void)
{
    int i, j, k;
    for (i = 0; i < 1024; i++) { mat_a[i] = (unsigned)(i * 7u + 1u) & 0xFFu; mat_b[i] = (unsigned)(i * 3u + 2u) & 0xFFu; }
    u64 cnt = 0, t0 = rdtsc();
    for (;;) {
        for (i = 0; i < 32; i++) {
            for (j = 0; j < 32; j++) {
                unsigned acc = 0;
                for (k = 0; k < 32; k++) acc += mat_a[i * 32 + k] * mat_b[k * 32 + j];
                mat_c[i * 32 + j] = acc;
            }
        }
        sink = mat_c[0] + mat_c[1023];
        cnt += 32 * 32 * 32;           /* multiply-accumulates */
        if (rdtsc() - t0 >= tsc_per_sec) break;
    }
    return cnt;
}

/* ------------------------------------------------------------------ */

int uefidos_main(const uefidos_exports *api, const char *args)
{
    u8 yd = 0, mo = 1, dy = 1, hh = 0, mi = 0, ss = 0, cs = 0;
    u64 t0, t1;
    u32 ticks;
    (void)args;

    if (!api || !api->svc) return 1;
    S = api->svc;

    /* Result file name: B<yymmdd>.TXT, the same name AUTOBANC.COM builds. */
    S->get_date(&yd, &mo, &dy);
    S->get_time(&hh, &mi, &ss, &cs);
    {
        int yy = (1980 + (int)yd) % 100;
        fname[0] = 'B';
        fname[1] = (char)('0' + yy / 10);
        fname[2] = (char)('0' + yy % 10);
        fname[3] = (char)('0' + ((int)mo / 10) % 10);
        fname[4] = (char)('0' + (int)mo % 10);
        fname[5] = (char)('0' + ((int)dy / 10) % 10);
        fname[6] = (char)('0' + (int)dy % 10);
        fname[7] = '.';
        fname[8] = 'T';
        fname[9] = 'X';
        fname[10] = 'T';
        fname[11] = 0;
    }
    {
        u16 rc = S->open(fname, DOS_OPEN_RDWR, &fhandle);
        if (rc) rc = S->create(fname, 0, &fhandle);
        if (!rc) {
            u32 pos = 0;
            S->seek(fhandle, 0, DOS_SEEK_END, &pos);
        } else {
            fhandle = 0xFFFF;
        }
    }

    outs("\n");
    outs("========================================================\n");
    outs("BENCH");
    outs(BW);
    outs(".EXE -- native ");
    outs(BW);
    outs("-bit UEFI-DOS application\n");
    field("UEFI-DOS API version", (u64)api->version, "");
    outs("DOS version ..........  ");   /* (minor<<8)|major */
    outu(S->get_version() & 0xFF);
    outc('.');
    outu((S->get_version() >> 8) & 0xFF);
    outs("\n");
    outs("Time ...............  ");
    outu(1980 + (int)yd); outc('-');
    if (mo < 10) outc('0');
    outu(mo); outc('-');
    if (dy < 10) outc('0');
    outu(dy); outc(' ');
    if (hh < 10) outc('0');
    outu(hh); outc(':');
    if (mi < 10) outc('0');
    outu(mi); outc(':');
    if (ss < 10) outc('0');
    outu(ss);
    outs("\n");
    con_flush();
    file_flush();

    /* calibrate TSC against the 55 ms BIOS tick: 30 ticks is 1.65 s, which
     * puts the calibration error under 2% */
    outs("Calibrating the clock .... ");
    con_flush();
    t0 = rdtsc();
    ticks = S->get_ticks();
    while ((u32)(S->get_ticks() - ticks) < 30) { }
    t1 = rdtsc();
    {
        u32 dt = (u32)(S->get_ticks() - ticks);
        u64 per_tick = udiv_small(t1 - t0, dt, (u32 *)0);
        tsc_per_sec = udiv_small(per_tick * 182u, 10u, (u32 *)0);
    }
    outs("done\n");
    field("TSC per second", tsc_per_sec, "");
    outs("--------------------------------------------------------\n");
    outs("One second per test, TSC-timed.\n");

    field("ALU chain (8 ops)", bench_alu(), " /s");
    field("MUL (2 muls/unit)", bench_mul(), " /s");
    field("DIV", bench_div(), " /s");
    field("copy 4 KB (word)", bench_copy(), " KB/s");
    field("fill 4 KB (word)", bench_fill(), " KB/s");
    field("FPU double chain", bench_fpu(), " /s");
    field("DSQRT (Newton)", bench_sqrt(), " /s");
    field("Sieve to 65536", bench_sieve(), " /s");
    field("Insertion sort 1024", bench_sort(), " /s");
    field("CRC32 over 4 KB", bench_crc(), " KB/s");
    field("Matmul 32x32 (MACs)", bench_mat(), " /s");

    if (fhandle != 0xFFFF) {
        u16 rc;
        file_flush();
        rc = S->close(fhandle);
        (void)rc;
        fhandle = 0xFFFF;
        outs("--------------------------------------------------------\n");
        outs("Appended to ");
        outs(fname);
        outs("\n");
    } else {
        outs("(no result file: the volume is read-only)\n");
    }
    con_flush();

    return 0;
}
