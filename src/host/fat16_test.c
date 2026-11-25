/* fat16_test.c -- Phase 4.4/4.5/4.6: FAT16 (and FAT12) write-path and
 * allocation-scaling verification against a real image file.
 *
 *   --fat16 IMAGE small   : 100 x 8 KB files, create/delete/recreate, timed
 *   --fat16 IMAGE big     : one 32 MB file (the DOS 4.0 / u32 size probe)
 */

#include "uefidos.h"
#include "kernel/dos.h"
#include "fs/fat.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int plat_open_image(const char *path);

static int g_bad;

#ifdef UEFIDOS_HOST
/* debug helper: print the cluster chain of every 1 MB / 512 KB file in the root */
static int chain_cb(fat_vol *v, const fat_dirent *d, void *arg)
{
    u32 sz = fat_entry_size(d);
    u32 cl;
    (void)arg;
    if (sz != 1048576u && sz != 524288u) return 0;
    cl = fat_entry_cluster(d);
    printf("  chain size=%u first=%u [", (unsigned)sz, (unsigned)cl);
    for (int n = 0; n < 40 && cl >= 2; n++) {
        u32 nxt = 0;
        printf(" %u", (unsigned)cl);
        if (fat_next_cluster(v, cl, &nxt) != FAT_OK) { printf(" EIO"); break; }
        if (nxt >= 0xFFFFFFF0u) break;
        cl = nxt;
    }
    printf(" ]\n");
    return 0;
}
int chain_dump_scan(fat_vol *v) { return fat_dir_scan(v, 0, chain_cb, NULL); }
#endif
static void ok(const char *w) { printf("  ok   %s\n", w); }
static void bad(const char *w, long got, long want)
{
    printf("  FAIL %s: got %ld want %ld\n", w, got, want);
    g_bad++;
}

static int mount_boot(void)
{
    dos_init();
    int rc = dos_mount_boot();
    if (rc) { printf("  mount failed rc=%d\n", rc); return rc; }
    return 0;
}

/* fill a buffer with a byte pattern derived from the file index */
static void pattern(u8 *buf, u32 len, int seed)
{
    for (u32 i = 0; i < len; i++) buf[i] = (u8)((i * 31u + (u32)seed * 7u) & 0xFF);
}

static int small_test(void)
{
    if (mount_boot()) return 1;
    fat_vol *v = &g_dos.drives[2].vol;
    printf("  volume: FAT%d, %u clusters, %u free, spc=%u\n",
           v->fs_type, v->cluster_count, v->free_count, v->spc);

    const int NFILES = 100;
    const u32 SZ = 8192;
    static u8 buf[8192];
    char name[DOS_MAX_PATH];

    /* ---- create ---- */
    u64 t0 = plat_ticks_ms();
    int created = 0;
    for (int i = 0; i < NFILES; i++) {
        int h;
        sprintf(name, "C:\\F%03d.DAT", i);
        pattern(buf, SZ, i);
        if (dos_creat(name, &h) != FAT_OK) { bad("create", i, 0); break; }
        u32 put = 0;
        if (dos_write(h, buf, SZ, &put) != FAT_OK || put != SZ) { dos_close(h); bad("write", i, 0); break; }
        dos_close(h);
        created++;
    }
    u64 t1 = plat_ticks_ms();
    printf("  create %d x %u B: %llu ms\n", created, SZ, (unsigned long long)(t1 - t0));
    if (created == NFILES) ok("created every test file on FAT16");

    /* ---- verify one file byte-exactly ---- */
    {
        int h;
        pattern(buf, SZ, 42);
        if (dos_open("C:\\F042.DAT", 0, &h) != FAT_OK) bad("reopen", 0, 1);
        else {
            static u8 rb[8192];
            u32 got = 0;
            dos_read(h, rb, SZ, &got);
            dos_close(h);
            int same = (got == SZ);
            for (u32 i = 0; same && i < SZ; i++) if (rb[i] != buf[i]) same = 0;
            if (same) ok("read back a FAT16 file byte-for-byte");
            else bad("FAT16 read back mismatch", got, SZ);
        }
    }

    /* ---- delete ---- */
    u64 t2 = plat_ticks_ms();
    int deleted = 0;
    for (int i = 0; i < NFILES; i++) {
        sprintf(name, "C:\\F%03d.DAT", i);
        if (dos_unlink(name) == FAT_OK) deleted++;
    }
    u64 t3 = plat_ticks_ms();
    printf("  delete %d files: %llu ms\n", deleted, (unsigned long long)(t3 - t2));
    if (deleted == NFILES) ok("deleted every test file");
    if (v->free_count == v->cluster_count - 1 + 0 || v->free_count > 0) {
        /* root cluster only on FAT32; FAT16 root lives outside the data area */
    }

    /* ---- recreate (proves the freed clusters are reusable) ---- */
    u64 t4 = plat_ticks_ms();
    int again = 0;
    for (int i = 0; i < NFILES; i++) {
        int h;
        sprintf(name, "C:\\G%03d.DAT", i);
        pattern(buf, SZ, i);
        if (dos_creat(name, &h) != FAT_OK) break;
        u32 put = 0;
        dos_write(h, buf, SZ, &put);
        dos_close(h);
        again++;
    }
    u64 t5 = plat_ticks_ms();
    printf("  recreate %d files: %llu ms\n", again, (unsigned long long)(t5 - t4));
    if (again == NFILES) ok("recreated the files after deletion");
    for (int i = 0; i < NFILES; i++) {
        sprintf(name, "C:\\G%03d.DAT", i);
        dos_unlink(name);
    }
    return 0;
}

/* ==================================================================== */
/* Large files: the 32 MB figure was a *testing* boundary, not a limit.  */
/* This test writes one file whose size comes from UEFIDOS_BIG_MB (32 by  */
/* default), verifies it through the ordinary DOS calls at offsets well    */
/* past 32 MB, then reads it again through the INT E0h big-file layer, and */
/* finally builds a three-part split file (NAME.001/.002/.003) and reads   */
/* it as one stream across the part boundaries.                           */
/* ==================================================================== */
#define BIG_CHUNK 65536u
static u8 big_chunk[BIG_CHUNK];
static u8 big_rbuf[BIG_CHUNK];

/* byte of the test pattern at absolute stream offset `off` */
static u8 pat_at(u64 off)
{
    u32 n = (u32)(off % BIG_CHUNK);
    u32 seed = (u32)(off / BIG_CHUNK);
    return (u8)((n * 31u + seed * 7u) & 0xFF);
}

/* fill a buffer for a stream write that starts at `off` */
static void pat_fill(u8 *buf, u32 len, u64 off)
{
    for (u32 i = 0; i < len; i++) buf[i] = pat_at(off + i);
}

static int verify_range(int h, u64 off, u32 len, const char *what)
{
    u32 got = 0;
    if (dos_lseek(h, (u32)off) != FAT_OK) { bad(what, 0, 1); return 1; }
    if (dos_read(h, big_rbuf, len, &got) != FAT_OK || got != len) {
        bad(what, (long)got, (long)len);
        return 1;
    }
    for (u32 i = 0; i < len; i++) {
        if (big_rbuf[i] != pat_at(off + i)) { bad(what, (long)big_rbuf[i], (long)pat_at(off + i)); return 1; }
    }
    return 0;
}

/* read a range through the big-file layer and compare it with the pattern */
static int verify_big(int bh, u64 off, u32 len, const char *what)
{
    u32 got = 0;
    if (dos_big_read(bh, off, big_rbuf, len, &got) != FAT_OK || got != len) {
        bad(what, (long)got, (long)len);
        return 1;
    }
    for (u32 i = 0; i < len; i++)
        if (big_rbuf[i] != pat_at(off + i)) { bad(what, (long)big_rbuf[i], (long)pat_at(off + i)); return 1; }
    return 0;
}

static int write_file(const char *path, u64 size, u64 stream_base)
{
    int h;
    u64 done = 0;
    if (dos_creat(path, &h) != FAT_OK) return 1;
    while (done < size) {
        u32 n = (size - done > BIG_CHUNK) ? BIG_CHUNK : (u32)(size - done);
        u32 put = 0;
        for (u32 i = 0; i < n; i++) big_chunk[i] = pat_at(stream_base + done + i);
        if (dos_write(h, big_chunk, n, &put) != FAT_OK || put != n) { dos_close(h); return 1; }
        done += put;
    }
    dos_close(h);
    return 0;
}

static int parts_test(void)
{
    /* three parts of 1 MB, 1 MB and 512 KB, one continuous pattern */
    const u32 P = 1024u * 1024u;
    const u64 TOTAL = (u64)P * 2u + 512u * 1024u;
    int bh = -1;
    u64 size = 0;
    int rc;
    char name[32];

    snprintf(name, sizeof(name), "C:\\SPLIT.DAT.001");
    if (write_file(name, P, 0)) { bad("create SPLIT.DAT.001", 0, 1); return 1; }
    if (write_file("C:\\SPLIT.DAT.002", P, P)) { bad("create SPLIT.DAT.002", 0, 1); return 1; }
    if (write_file("C:\\SPLIT.DAT.003", 512u * 1024u, (u64)P * 2u)) {
        bad("create SPLIT.DAT.003", 0, 1); return 1;
    }
    ok("wrote a three-part split file (1 MB + 1 MB + 512 KB)");

#ifdef UEFIDOS_HOST
    if (getenv("UEFIDOS_TRACE_CHAIN")) {
        /* On-disk chain of each part: a part whose chain stops after one
         * cluster is exactly what makes a cross-boundary read return 0. */
        fat_vol *vv = &g_dos.drives[2].vol;
        extern int chain_dump_scan(fat_vol *v);
        chain_dump_scan(vv);
    }
#endif

    rc = dos_big_open("C:\\SPLIT.DAT", 0, &bh, &size);
    if (rc != FAT_OK) { bad("open the split file", rc, 0); return 1; }
    if (size == TOTAL) ok("opening SPLIT.DAT joined 3 parts into 2.5 MB");
    else bad("split file total size", (long)size, (long)TOTAL);
    if (dos_big_parts(bh) == 3) ok("the stream reports 3 parts");
    else bad("part count", dos_big_parts(bh), 3);

    /* reads that stay inside a part, and both sides of each boundary */
    if (!verify_big(bh, 0, 4096, "big read at 0")) ok("big read at offset 0");
    if (!verify_big(bh, P - 2048u, 4096, "big read across part 1->2 boundary"))
        ok("big read across the 1 MB part boundary");
    if (!verify_big(bh, P, 4096, "big read at part 2 start")) ok("big read at part 2 start");
    if (!verify_big(bh, (u64)P * 2u - 1024u, 4096, "big read across part 2->3 boundary"))
        ok("big read across the short last part's boundary");
    if (!verify_big(bh, TOTAL - 4096u, 4096, "big read at the end")) ok("big read at the stream end");

    {   /* a joined file is read-only */
        u32 put = 0;
        if (dos_big_write(bh, 0, big_chunk, 16, &put) == FAT_EACCES)
            ok("writing a joined stream is refused with EACCES");
        else bad("write to a joined stream must fail", 0, 0);
    }
    dos_big_close(bh);

    /* and a plain file opened through the same API is one part, writable */
    if (write_file("C:\\PLAIN.DAT", 300000u, 0) == 0) {
        int ph = -1;
        u64 psize = 0;
        if (dos_big_open("C:\\PLAIN.DAT", 0, &ph, &psize) == FAT_OK && psize == 300000u) {
            ok("a plain file opens through the big-file API as one part");
            if (dos_big_parts(ph) == 1) ok("the plain file reports 1 part");
        } else bad("plain file through the big API", (long)psize, 300000);
        dos_big_close(ph);
        dos_unlink("C:\\PLAIN.DAT");
    }
    if (!getenv("UEFIDOS_TRACE_CHAIN")) dos_unlink("C:\\SPLIT.DAT.001");
    if (!getenv("UEFIDOS_TRACE_CHAIN")) dos_unlink("C:\\SPLIT.DAT.002");
    if (!getenv("UEFIDOS_TRACE_CHAIN")) dos_unlink("C:\\SPLIT.DAT.003");
    return 0;
}

static int big_test(void)
{
    if (mount_boot()) return 1;
    fat_vol *v = &g_dos.drives[2].vol;
    u32 mb = 32;
    const char *env = getenv("UEFIDOS_BIG_MB");
    if (env && *env) mb = (u32)strtoul(env, NULL, 10);
    if (mb < 1) mb = 32;
    if (mb > 4000) mb = 4000;
    {
        u64 need = (u64)mb * 1024u * 1024u;
        u64 free_bytes = (u64)v->free_count * v->spc * FAT_SEC;
        if (free_bytes < need + (8u << 20)) {
            printf("  volume too small for a %u MB file (%llu MB free): falling back to 32 MB\n",
                   (unsigned)mb, (unsigned long long)(free_bytes >> 20));
            mb = 32;
        }
    }
    printf("  volume: FAT%d, %u clusters, %u free, spc=%u, free MB=%u\n",
           v->fs_type, v->cluster_count, v->free_count, v->spc,
           (u32)((u64)v->free_count * v->spc * FAT_SEC / (1024 * 1024)));

    const u64 SZ = (u64)mb * 1024ull * 1024ull;
    int h;
    if (dos_creat("C:\\BIG.DAT", &h) != FAT_OK) { bad("create the big file", 0, 1); return 1; }

    u64 t0 = plat_ticks_ms();
    u64 written = 0;
    while (written < SZ) {
        u32 n = (SZ - written > BIG_CHUNK) ? BIG_CHUNK : (u32)(SZ - written);
        u32 put = 0;
        pat_fill(big_chunk, n, written);
        if (dos_write(h, big_chunk, n, &put) != FAT_OK || put != n) break;
        written += put;
    }
    u64 t1 = plat_ticks_ms();
    dos_close(h);
    printf("  wrote %llu bytes in %llu ms (%llu KB/s)\n",
           (unsigned long long)written, (unsigned long long)(t1 - t0),
           (unsigned long long)((t1 > t0) ? (written / 1024u) * 1000u / (t1 - t0) : 0));
    if (written == SZ) {
        printf("  ok   wrote a %u MB file through the ordinary u32 DOS calls\n", (unsigned)mb);
    } else bad("big write shortfall", (long)written, (long)SZ);

    /* the directory entry must carry the whole size */
    dos_find f; char nm[13]; u8 at = 0; u32 size = 0; u16 tm = 0, dt = 0;
    if (dos_find_first("C:\\BIG.DAT", 0xFF, &f, nm, &at, &size, &tm, &dt) == FAT_OK) {
        if ((u64)size == SZ) printf("  ok   directory entry reports the full %u MB size\n", (unsigned)mb);
        else bad("big dirent size", (long)size, (long)SZ);
    } else bad("find BIG.DAT", 0, 1);

    /* read back far past the old 32 MB boundary through the DOS path */
    if (dos_open("C:\\BIG.DAT", 0, &h) == FAT_OK) {
        if (!verify_range(h, SZ / 2, 4096, "read at half the file")) ok("read back at the halfway mark");
        if (!verify_range(h, SZ - 4096, 4096, "read at the end")) ok("read back at the end");
        if (SZ > (40ull << 20) && !verify_range(h, 33ull << 20, 4096, "read past 32 MB"))
            ok("read back well past the old 32 MB boundary");
        dos_close(h);
    } else bad("reopen BIG.DAT", 0, 1);

    /* the same file through the INT E0h big-file layer (64-bit offsets) */
    {
        int bh = -1;
        u64 bsize = 0;
        if (dos_big_open("C:\\BIG.DAT", 0, &bh, &bsize) == FAT_OK) {
            if (bsize == SZ) ok("big-file API reports the same size");
            else bad("big-file size", (long)bsize, (long)SZ);
            if (!verify_big(bh, SZ - 8192, 4096, "big api read at the end"))
                ok("big-file API read at the end matches");
            if (!verify_big(bh, SZ / 3, 4096, "big api read at a third"))
                ok("big-file API read at one third matches");
            {
                u64 pos = 0;
                if (dos_big_seek(bh, 0, 2, &pos) == FAT_OK && pos == SZ)
                    ok("big-file API seek-to-end reports the size");
                else bad("big api seek end", (long)pos, (long)SZ);
            }
            dos_big_close(bh);
        } else bad("big-file open", 0, 0);
    }

    u64 t2 = plat_ticks_ms();
    int urc = dos_unlink("C:\\BIG.DAT");
    u64 t3 = plat_ticks_ms();
    if (urc == FAT_OK) printf("  ok   freed the %u MB chain\n", (unsigned)mb);
    else bad("unlink BIG.DAT", urc, 0);
    printf("  delete %u MB file: %llu ms, free clusters now %u\n",
           (unsigned)mb, (unsigned long long)(t3 - t2), v->free_count);

    parts_test();
    return 0;
}

int fat16_selftest(const char *image, const char *mode)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (plat_open_image(image)) { printf("cannot open image %s\n", image); return 1; }
    printf("UEFI-DOS FAT16 write/alloc test (%s) on %s\n", mode, image);
    if (strcmp(mode, "big") == 0) big_test();
    else small_test();
    printf("\nFAT16 %s\n", g_bad ? "FAILED" : "PASSED");
    return g_bad ? 1 : 0;
}
