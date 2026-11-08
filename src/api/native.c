/*
 * UEFI-DOS -- native 64-bit application loader.
 *
 * A "native app" is an ordinary PE image (the SDK in sdk/ builds one with the
 * same linker flags as the firmware binary: --subsystem,10 --enable-reloc-section)
 * whose entry point has this signature:
 *
 *     int uefidos_main(const uefidos_exports *api, const char *args);
 *
 * The loader maps the image into executable memory, applies the base
 * relocations, and calls the entry point directly on the real CPU: no 8086
 * interpreter, no 16-bit translation, no reboot into another mode.  The app
 * gets the same service table the firmware publishes through the UEFI
 * configuration table, so native code and INT 21h programs see one DOS.
 *
 * Fixed address assumptions are deliberately avoided: mingw emits a .reloc
 * section, so the image runs wherever the loader puts it.
 */
#include "uefidos.h"
#include "plat.h"
#include "kernel/dos.h"
#include "api/dosapi.h"
#include "api/native.h"

#include <string.h>

#define PE_MACHINE_AMD64 0x8664u
#define PE_MACHINE_I386  0x014Cu

#define DIR_RELOC 5

/* A PE image can only run in a process of its own bitness: 32-bit code cannot
 * execute inside the x64 firmware build (or vice versa).  So the loader accepts
 * exactly one machine type, and reports the other as a bitness error instead of
 * jumping into code the CPU would decode wrongly. */
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#define NATIVE_SELF_MACHINE PE_MACHINE_AMD64
#define NATIVE_SELF_BITS    64
#else
#define NATIVE_SELF_MACHINE PE_MACHINE_I386
#define NATIVE_SELF_BITS    32
#endif

static u16 rd16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }

typedef int (*native_entry)(const dosapi_exports *api, const char *args);

/* Load a PE image, apply relocations, run it.  Returns 0 on success and stores
 * the value the app returned from uefidos_main in *exit_code. */
int native_run_image(const u8 *img, u32 size, const char *args, int *exit_code)
{
    if (!img || size < 0x40 || img[0] != 'M' || img[1] != 'Z') return 1;
    u32 pe = rd32(img + 0x3C);
    if (pe + 0x18 + 2 > size) return 1;
    if (img[pe] != 'P' || img[pe + 1] != 'E' || img[pe + 2] || img[pe + 3]) return 1;

    const u8 *coff = img + pe + 4;
    u16 machine = rd16(coff);
    u16 nsect = rd16(coff + 2);
    u16 opt_size = rd16(coff + 16);
    if (machine != PE_MACHINE_AMD64 && machine != PE_MACHINE_I386) return 2;
    if (machine != NATIVE_SELF_MACHINE) return 4;   /* wrong bitness */

    const u8 *opt = coff + 20;
    if ((u32)(opt - img) + opt_size > size || opt_size < 64) return 1;
    u16 magic = rd16(opt);
    int pe32plus = (magic == 0x020B);
    if (!pe32plus && magic != 0x010B) return 2;

    u32 entry_rva = rd32(opt + 16);
    u64 image_base = pe32plus ? rd64(opt + 24) : (u64)rd32(opt + 28);
    u32 size_of_image = rd32(opt + 56);
    u32 size_of_headers = rd32(opt + 60);
    u32 ndirs = rd32(opt + (pe32plus ? 108 : 92));
    const u8 *dirs = opt + (pe32plus ? 112 : 96);
    u32 reloc_rva = 0, reloc_size = 0;
    if (ndirs > DIR_RELOC) {
        reloc_rva = rd32(dirs + DIR_RELOC * 8);
        reloc_size = rd32(dirs + DIR_RELOC * 8 + 4);
    }
    if (!size_of_image || size_of_image > (64u << 20)) return 2;

    u8 *base = (u8 *)plat_alloc_code(size_of_image);
    if (!base) return 3;
    memset(base, 0, size_of_image);

    u32 hdr = size_of_headers < size ? size_of_headers : size;
    if (hdr > size_of_image) hdr = size_of_image;
    memcpy(base, img, hdr);

    const u8 *sect = opt + opt_size;
    for (u16 i = 0; i < nsect; i++) {
        const u8 *s = sect + (u32)i * 40;
        if ((u32)(s - img) + 40 > size) { plat_free_code(base, size_of_image); return 1; }
        u32 vsize = rd32(s + 8);
        u32 rva = rd32(s + 12);
        u32 raw_size = rd32(s + 16);
        u32 raw_ptr = rd32(s + 20);
        if (!vsize && !raw_size) continue;
        if (rva >= size_of_image) continue;
        u32 n = raw_size ? raw_size : vsize;
        if (n > size_of_image - rva) n = size_of_image - rva;
        if (raw_ptr + n > size) n = (raw_ptr < size) ? size - raw_ptr : 0;
        if (n) memcpy(base + rva, img + raw_ptr, n);
    }

    /* base relocations: the image was linked for image_base but is loaded at
     * `base`.  64-bit images only ever carry DIR64 for data (RIP-relative code
     * needs none), 32-bit ones carry HIGHLOW. */
    if (reloc_rva && reloc_size && reloc_rva < size_of_image) {
        u64 delta = (u64)(unsigned long long)base - image_base;
        u32 off = 0;
        while (off + 8 <= reloc_size) {
            const u8 *blk = base + reloc_rva + off;
            u32 page = rd32(blk);
            u32 blk_size = rd32(blk + 4);
            if (blk_size < 8) break;
            u32 nent = (blk_size - 8) / 2;
            for (u32 e = 0; e < nent; e++) {
                u16 ent = rd16(blk + 8 + e * 2);
                u32 type = ent >> 12;
                u32 rva = page + (ent & 0xFFF);
                if (type == 0) continue;                    /* ABSOLUTE: padding */
                if (rva + 8 > size_of_image) continue;
                if (type == 10) {                           /* DIR64 */
                    u64 v = rd64(base + rva) + delta;
                    memcpy(base + rva, &v, 8);
                } else if (type == 3) {                     /* HIGHLOW */
                    u32 v = rd32(base + rva) + (u32)delta;
                    memcpy(base + rva, &v, 4);
                }
            }
            off += blk_size;
        }
    }

    if (entry_rva >= size_of_image) { plat_free_code(base, size_of_image); return 2; }
    native_entry fn = (native_entry)(void *)(base + entry_rva);
    int rc = fn(&g_dosapi_exports, args);
    if (exit_code) *exit_code = rc;
    /* the image stays mapped: a native app may leave state behind that later
     * apps in the same session still point at */
    return 0;
}

/* Load a DOS path from the current drive and run it natively. */
int native_run_file(const char *dos_path, const char *args, int *exit_code)
{
    int h = 0;
    int rc = dos_open(dos_path, 0, &h);
    if (rc != FAT_OK) {
        plat_puts(dos_path);
        plat_puts(": cannot open\n");
        return rc;
    }
    u32 size = (h >= 0 && h < DOS_HANDLE_MAX) ? g_dos.handles[h].size : 0;
    if (size < 0x40 || size > (64u << 20)) {
        dos_close(h);
        plat_puts(dos_path);
        plat_puts(": not a native image\n");
        return FAT_EINVAL;
    }
    u8 *img = (u8 *)plat_alloc(size);
    if (!img) { dos_close(h); return FAT_ENOSPC; }
    u32 got = 0;
    rc = dos_read(h, img, size, &got);
    dos_close(h);
    if (rc != FAT_OK || got != size) {
        plat_free(img);
        plat_puts(dos_path);
        plat_puts(": read failed\n");
        return FAT_EIO;
    }
    rc = native_run_image(img, size, args, exit_code);
    plat_free(img);
    if (rc == 3) { plat_puts("native loader: out of memory\n"); return FAT_ENOSPC; }
    if (rc == 4) {
        plat_puts("native loader: image bitness does not match this UEFI-DOS build (");
        plat_puts(NATIVE_SELF_BITS == 64 ? "64-bit kernel runs 64-bit images only"
                                         : "32-bit kernel runs 32-bit images only");
        plat_puts(")\n");
        return FAT_EINVAL;
    }
    if (rc == 2) { plat_puts("native loader: bad machine or image\n"); return FAT_EINVAL; }
    if (rc != 0) { plat_puts("native loader: not a PE image\n"); return FAT_EINVAL; }
    return FAT_OK;
}
