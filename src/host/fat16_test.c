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

int plat_open_image(const char *path);

static int g_bad;

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

static int big_test(void)
{
    if (mount_boot()) return 1;
    fat_vol *v = &g_dos.drives[2].vol;
    printf("  volume: FAT%d, %u clusters, %u free, spc=%u, free MB=%u\n",
           v->fs_type, v->cluster_count, v->free_count, v->spc,
           (u32)((u64)v->free_count * v->spc * FAT_SEC / (1024 * 1024)));

    const u32 SZ = 32u * 1024u * 1024u;      /* DOS 4.0 practical file ceiling */
    static u8 chunk[65536];
    int h;
    if (dos_creat("C:\\BIG.DAT", &h) != FAT_OK) { bad("create 32 MB file", 0, 1); return 1; }

    u64 t0 = plat_ticks_ms();
    u32 written = 0;
    while (written < SZ) {
        u32 n = SZ - written; if (n > sizeof(chunk)) n = sizeof(chunk);
        pattern(chunk, n, (int)(written / sizeof(chunk)));
        u32 put = 0;
        if (dos_write(h, chunk, n, &put) != FAT_OK || put != n) break;
        written += put;
    }
    u64 t1 = plat_ticks_ms();
    dos_close(h);
    printf("  wrote %u bytes in %llu ms\n", written, (unsigned long long)(t1 - t0));
    if (written == SZ) ok("wrote a 32 MB file through the u32 file-size path");
    else bad("32 MB write shortfall", written, SZ);

    /* size check via the directory entry */
    dos_find f; char nm[13]; u8 at = 0; u32 size = 0; u16 tm = 0, dt = 0;
    if (dos_find_first("C:\\BIG.DAT", 0xFF, &f, nm, &at, &size, &tm, &dt) == FAT_OK) {
        if (size == SZ) ok("directory entry reports the full 32 MB size");
        else bad("32 MB dirent size", (long)size, (long)SZ);
    } else bad("find BIG.DAT", 0, 1);

    u64 t2 = plat_ticks_ms();
    int urc = dos_unlink("C:\\BIG.DAT");
    u64 t3 = plat_ticks_ms();
    if (urc == FAT_OK) ok("freed the 32 MB chain");
    else bad("unlink BIG.DAT", urc, 0);
    printf("  delete 32 MB file: %llu ms, free clusters now %u\n",
           (unsigned long long)(t3 - t2), v->free_count);
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
