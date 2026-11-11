/*
 * UEFI-DOS -- host FAT selftest.
 *
 * Boots nothing; opens an image, mounts it with the FAT driver and exercises
 * read / write / mkdir / rename / delete paths, asserting every step.
 *
 * usage: hosttest.exe image.img [--selftest]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "uefidos.h"
#include "plat.h"
#include "fs/fat.h"
#include "kernel/dos.h"
#include "shell/shell.h"

int plat_open_image(const char *path);

static int g_fail;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); g_fail++; } \
} while (0)

static void name11(const char *s, u8 out[11])
{
    char base[9] = {0}, ext[4] = {0};
    const char *dot = strrchr(s, '.');
    if (dot) {
        int n = (int)(dot - s); if (n > 8) n = 8;
        memcpy(base, s, n);
        strncpy(ext, dot + 1, 3);
    } else {
        strncpy(base, s, 8);
    }
    memset(out, ' ', 11);
    for (int i = 0; i < 8 && base[i]; i++) out[i] = (u8)toupper(base[i]);
    for (int i = 0; i < 3 && ext[i]; i++) out[8 + i] = (u8)toupper(ext[i]);
}

static int list_cb(fat_vol *v, const fat_dirent *d, void *arg)
{
    char nm[12];
    UFD_UNUSED(v);
    memcpy(nm, d->raw, 11);
    nm[11] = 0;
    printf("  %s%s  %8lu\n", nm, (d->raw[11] & 0x10) ? "\\" : " ",
           (unsigned long)fat_entry_size(d));
    UFD_UNUSED(arg);
    return 0;
}

static u32 find_part_lba(blockdev *bd)
{
    u8 sec[512];
    bd->read(bd, 0, sec, 1);
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    u8 *e = sec + 446;
    for (int i = 0; i < 4; i++, e += 16) {
        u8 status = e[0];
        u8 type = e[4];
        if (status != 0x00 && status != 0x80) continue;
        if (type != 0x01 && type != 0x04 && type != 0x06 && type != 0x07 &&
            type != 0x0B && type != 0x0C && type != 0x0E && type != 0x0F && type != 0xEF)
            continue;
        u32 lba = (u32)e[8] | ((u32)e[9] << 8) | ((u32)e[10] << 16) | ((u32)e[11] << 24);
        u32 cnt = (u32)e[12] | ((u32)e[13] << 8) | ((u32)e[14] << 16) | ((u32)e[15] << 24);
        if (lba == 0 || cnt == 0) continue;
        u8 bs2[512];
        if (bd->read(bd, lba, bs2, 1)) continue;
        if (bs2[510] == 0x55 && bs2[511] == 0xAA) return lba;
    }
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "uefidos.h"
#include "plat.h"
#include "kernel/dos.h"
#include "shell/shell.h"

#ifdef UEFIDOS_DEBUG_INT
static LONG WINAPI dbg_crash(EXCEPTION_POINTERS *ep)
{
    HMODULE mod = GetModuleHandleA(NULL);
    fprintf(stderr, "CRASH code=%08lx addr=%p base=%p off=%llx\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress,
            (void *)mod,
            (unsigned long long)((char *)ep->ExceptionRecord->ExceptionAddress - (char *)mod));
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "  AV: %s at %p  rcx=%p rdx=%p r8=%p r9=%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (void *)ep->ExceptionRecord->ExceptionInformation[1],
                (void *)(uintptr_t)ep->ContextRecord->Rcx,
                (void *)(uintptr_t)ep->ContextRecord->Rdx,
                (void *)(uintptr_t)ep->ContextRecord->R8,
                (void *)(uintptr_t)ep->ContextRecord->R9);
    }
    {
        void *frames[24];
        USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        for (USHORT i = 0; i < n; i++) {
            HMODULE m = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &m);
            char nm[MAX_PATH] = "?";
            if (m) GetModuleFileNameA(m, nm, MAX_PATH);
            fprintf(stderr, "  #%u %p %s+%llx\n", i, frames[i], nm,
                    (unsigned long long)(m ? ((char *)frames[i] - (char *)m) : 0));
        }
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char **argv)
{
#ifdef UEFIDOS_DEBUG_INT
    setvbuf(stdout, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(dbg_crash);
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: %s image.img\n", argv[0]);
        return 2;
    }
    /* instruction-level 8086 test vectors: no disk image needed */
    if (strcmp(argv[1], "--itest") == 0) {
        extern int itest_run(const char *filter);
        return itest_run(argc >= 3 ? argv[2] : NULL);
    }
    /* brute-force opcode audit: every first byte, every 0F second byte.
       --ifuzz -v lists each rejected opcode as it is found. */
    if (strcmp(argv[1], "--ifuzz") == 0) {
        extern int ifuzz_run(int verbose);
        return ifuzz_run(argc >= 3);
    }
    if (argc >= 4 && strcmp(argv[1], "--fat16") == 0) {
        extern int fat16_selftest(const char *image, const char *mode);
        return fat16_selftest(argv[2], argv[3]);
    }    if (argc >= 3 && strcmp(argv[1], "--phase46") == 0) {
        extern int dual_selftest(const char *image);
        return dual_selftest(argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "--findtest") == 0) {
        if (plat_open_image(argv[2])) return 1;
        dos_init();
        int mrc = dos_mount_boot();
        if (mrc == 0) dos_mount_all();
        printf("mount rc=%d\n", mrc);
        if (mrc) return 1;
        dos_read_config_sys();
        dos_find f;
        char name[13];
        u8 attr;
        u32 size;
        u16 t, d;
        int rc = dos_find_first("*.*", 0x3F, &f, name, &attr, &size, &t, &d);
        while (rc == FAT_OK) {
            printf("entry '%s' attr=%02x size=%u\n", name, attr, size);
            rc = dos_find_next(&f, name, &attr, &size, &t, &d);
        }
        printf("find rc=%d\n", rc);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--fatlist") == 0) {
        if (plat_open_image(argv[2])) {
            fprintf(stderr, "cannot open image: %s\n", argv[2]);
            return 1;
        }
        blockdev bd;
        if (plat_boot_blockdev(&bd)) return 1;
        u32 part = find_part_lba(&bd);
        fat_vol v;
        int rc = fat_mount(&v, &bd, part, 1);
        printf("mount rc=%d type=FAT%u spc=%u clusters=%u root_entries=%u\n",
               rc, v.fs_type, v.spc, v.cluster_count, v.root_entries);
        if (rc) return 1;
        u32 root = v.fs_type == 32 ? v.root_cluster : 0;
        fat_dir_scan(&v, root, list_cb, NULL);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--shell") == 0) {
        if (plat_open_image(argv[2])) {
            fprintf(stderr, "cannot open image: %s\n", argv[2]);
            return 1;
        }
        if (argc >= 5 && strcmp(argv[3], "--dumpfile") == 0) {
            /* --shell IMAGE --dumpfile DOSPATH OFFSET COUNT */
            dos_init();
            if (dos_mount_boot()) return 1;
            if (argv[4][0] == 'A' && argv[4][1] == ':')
                dos_mount_image_drive(0, "DOS211.IMG");
            int h;
            if (dos_open(argv[4], 0, &h) != FAT_OK) {
                fprintf(stderr, "cannot open %s\n", argv[4]);
                return 1;
            }
            u32 off = (u32)strtoul(argv[5], NULL, 0);
            u32 cnt = (u32)strtoul(argv[6], NULL, 0);
            u8 *buf = (u8 *)malloc(cnt);
            u32 got = 0;
            if (dos_lseek(h, off) != FAT_OK || dos_read(h, buf, cnt, &got) != FAT_OK) {
                fprintf(stderr, "read failed\n");
                return 1;
            }
            dos_close(h);
            printf("read %u bytes at %u\n", got, off);
            for (u32 i = 0; i < got; i += 16) {
                printf("%04X: ", off + i);
                for (u32 j = 0; j < 16 && i + j < got; j++)
                    printf("%02X ", buf[i + j]);
                printf("\n");
            }
            free(buf);
            return 0;
        }
        dos_init();
        int mrc = dos_mount_boot();
        if (mrc == 0) dos_mount_all();   /* D:, E:, ... from UEFIDOS_EXTRA_DRIVES */
        printf("mount C: rc=%d\n", mrc);
        if (mrc != 0) return 1;
        dos_read_config_sys();
        shell_init();
        for (int i = 3; i < argc; i++) {
            printf(">> %s\n", argv[i]);
            shell_exec_line(argv[i]);
        }
        printf("SHELLDONE\n");
        return 0;
    }
    if (plat_open_image(argv[1])) {
        fprintf(stderr, "cannot open image: %s\n", argv[1]);
        return 1;
    }
    blockdev bd;
    if (plat_boot_blockdev(&bd)) return 1;
    u32 part = find_part_lba(&bd);
    printf("image %s, %llu sectors, partition LBA %u\n", argv[1],
           (unsigned long long)bd.nblocks, part);
    fat_vol v;
    int rc = fat_mount(&v, &bd, part, 0);
    printf("mount rc=%d type=FAT%u spc=%u clusters=%u fatsz=%u\n",
           rc, v.fs_type, v.spc, v.cluster_count, v.fat_sectors);
    if (rc) return 1;

    uint32_t root = v.fs_type == 32 ? v.root_cluster : 0;
    printf("root listing:\n");
    fat_dir_scan(&v, root, list_cb, NULL);

    /* read HELLO.TXT */
    u8 n11[11];
    name11("HELLO.TXT", n11);
    fat_dirent d;
    u32 cl = 0;
    rc = fat_find(&v, root, n11, &d, &cl, NULL, NULL);
    CHECK(rc == 0, "find HELLO.TXT");
    if (rc == 0) {
        char buf[64];
        u32 got = 0;
        rc = fat_read_chain_byte(&v, cl, fat_entry_size(&d), 0, buf, sizeof(buf) - 1, &got);
        buf[got] = 0;
        printf("  HELLO.TXT = '%s' (%u bytes)\n", buf, got);
        CHECK(rc == 0 && got > 0, "read HELLO.TXT");
    }

    /* create a file and write content */
    u8 n12[11];
    name11("T1.TXT", n12);
    u32 cl1 = 0, sz1 = 0;
    rc = fat_add_entry(&v, root, n12, 0x20, 0, 0);
    CHECK(rc == 0, "create T1.TXT");
    fat_dirent e;
    rc = fat_find(&v, root, n12, &e, &cl1, NULL, NULL);
    CHECK(rc == 0, "find T1.TXT after create");
    const char *content = "UEFI-DOS write test 0123456789";
    rc = fat_write_chain(&v, &cl1, &sz1, 0, content, (u32)strlen(content));
    CHECK(rc == 0 && sz1 == strlen(content), "write T1.TXT");
    /* update size field in the dir entry */
    {
        u32 lba = 0, idx = 0;
        rc = fat_find(&v, root, n12, &e, NULL, &lba, &idx);
        if (rc == 0) {
            fat_entry_set_size(&e, sz1);
            fat_entry_set_cluster(&e, cl1);
            rc = fat_write_entry_at(&v, lba, idx, &e);
        }
        CHECK(rc == 0, "update T1.TXT entry");
    }
    /* re-read */
    {
        char buf[64];
        u32 got = 0;
        fat_find(&v, root, n12, &e, &cl1, NULL, NULL);
        rc = fat_read_chain_byte(&v, cl1, fat_entry_size(&e), 0, buf, sizeof(buf) - 1, &got);
        buf[got] = 0;
        CHECK(rc == 0 && strcmp(buf, content) == 0, "re-read T1.TXT matches");
    }

    /* mkdir / rename / delete */
    u8 nd[11], nr[11];
    name11("SUBDIR", nd);
    rc = fat_mkdir(&v, root, nd, &cl);
    CHECK(rc == 0, "mkdir SUBDIR");
    name11("RENAMED", nr);
    rc = fat_rename_entry(&v, root, nd, nr);
    CHECK(rc == 0, "rename SUBDIR -> RENAMED");
    rc = fat_rmdir(&v, root, nr);
    CHECK(rc == 0, "rmdir RENAMED");
    rc = fat_delete_file(&v, root, n12);
    CHECK(rc == 0, "delete T1.TXT");

    printf(g_fail ? "SELFTEST FAILED (%d)\n" : "SELFTEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
