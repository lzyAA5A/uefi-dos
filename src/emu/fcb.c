/* fcb.c -- DOS 4.0 FCB (File Control Block) calls for UEFI-DOS.
 *
 * Supported: AH=0Fh open, 10h close, 11h find first, 12h find next,
 *            13h delete, 14h seq read, 15h seq write, 16h create,
 *            17h rename, 21h random read, 22h random write,
 *            23h file size, 27h random block read, 28h random block write.
 *
 * Design note: real DOS keeps the resolved drive/dir/cluster in the FCB's
 * reserved area and shares open FCBs through SHARE.  UEFI-DOS is a single
 * process system, so the FCB layer here is *stateless*: every call resolves
 * the 8.3 name through the kernel (INT 21h path) and closes again.  That
 * keeps copied FCBs working (DOS 4.0 programs do copy them) at the cost of
 * one directory lookup per record I/O call.
 *
 * FCB layout used below (standard 37-byte FCB):
 *   +00 drive (0 = default, 1 = A)      +0C current block (word)
 *   +01 name[8]                         +0E record size (word, 0 = 128)
 *   +09 ext[3]                          +10 file size (dword)
 *   +20 current record (byte)           +14 date / +16 time
 *                                       +21 random record (dword)
 */

#include "uefidos.h"
#include "emu.h"
#include "kernel/dos.h"
#include "plat.h"

#define FCB_REC_DEFAULT 128
#define FCB_CHUNK       512

static u32 fcb_u32(Emu *e, u16 seg, u16 off)
{
    return (u32)emu_rb(e, seg, off) |
           ((u32)emu_rb(e, seg, (u16)(off + 1)) << 8) |
           ((u32)emu_rb(e, seg, (u16)(off + 2)) << 16) |
           ((u32)emu_rb(e, seg, (u16)(off + 3)) << 24);
}

static u16 fcb_u16(Emu *e, u16 seg, u16 off)
{
    return (u16)(emu_rb(e, seg, off) | (emu_rb(e, seg, (u16)(off + 1)) << 8));
}

static void fcb_w16(Emu *e, u16 seg, u16 off, u16 v)
{
    emu_wb(e, seg, off, (u8)(v & 0xFF));
    emu_wb(e, seg, (u16)(off + 1), (u8)(v >> 8));
}

static void fcb_w32(Emu *e, u16 seg, u16 off, u32 v)
{
    fcb_w16(e, seg, off, (u16)(v & 0xFFFF));
    fcb_w16(e, seg, (u16)(off + 2), (u16)(v >> 16));
}

/* status byte for FCB calls: 0 = ok, 1 = end of file (nothing read),
 * 3 = end of file (partial record), 0xFF = failure. */
static void fcb_set_al(Emu *e, u8 st) { e->ax = (u16)((e->ax & 0xFF00) | st); }

/* Read the drive + 8.3 name out of an FCB and render it as a DOS path
 * (drive prefix only when the FCB names one).  Returns 0 when the name is
 * empty/invalid. */
static int fcb_name(Emu *e, u16 seg, u16 off, char *path, int max, u8 nm11_out[11])
{
    u8 dr = emu_rb(e, seg, off);
    u8 nm[11];
    for (int i = 0; i < 8; i++) nm[i] = emu_rb(e, seg, (u16)(off + 1 + i));
    for (int i = 0; i < 3; i++) nm[8 + i] = emu_rb(e, seg, (u16)(off + 9 + i));
    if (nm11_out) memcpy(nm11_out, nm, 11);

    char base[9], ext[4];
    int bn = 0, en = 0;
    for (int i = 0; i < 8; i++) if (nm[i] && nm[i] != ' ') base[bn++] = (char)nm[i];
    base[bn] = 0;
    for (int i = 0; i < 3; i++) if (nm[8 + i] && nm[8 + i] != ' ') ext[en++] = (char)nm[8 + i];
    ext[en] = 0;
    if (bn == 0) return 0;

    int n = 0;
    if (dr >= 1 && dr <= 26) { path[n++] = (char)('A' + dr - 1); path[n++] = ':'; }
    for (int i = 0; i < bn && n < max - 1; i++) path[n++] = base[i];
    if (en) {
        if (n < max - 1) path[n++] = '.';
        for (int i = 0; i < en && n < max - 1; i++) path[n++] = ext[i];
    }
    path[n] = 0;
    return 1;
}

/* Look up size/date/time for a path (used to fill in the FCB after open). */
static int fcb_info(const char *path, u32 *size, u16 *date, u16 *time)
{
    dos_find f;
    char nm[13];
    u8 at = 0;
    u32 sz = 0;
    u16 tm = 0, dt = 0;
    int rc = dos_find_first(path, 0xFF, &f, nm, &at, &sz, &tm, &dt);
    if (rc != FAT_OK) return rc;
    if (size) *size = sz;
    if (date) *date = dt;
    if (time) *time = tm;
    return FAT_OK;
}

static u16 fcb_rec_size(Emu *e, u16 fs, u16 fo)
{
    u16 rs = fcb_u16(e, fs, (u16)(fo + 0x0E));
    return rs ? rs : FCB_REC_DEFAULT;
}

/* Fill size/date/time/record-size defaults after a successful open. */
static void fcb_fill(Emu *e, u16 fs, u16 fo, const char *path)
{
    u32 size = 0;
    u16 dt = 0, tm = 0;
    if (fcb_info(path, &size, &dt, &tm) == FAT_OK) {
        fcb_w32(e, fs, (u16)(fo + 0x10), size);
        fcb_w16(e, fs, (u16)(fo + 0x14), dt);
        fcb_w16(e, fs, (u16)(fo + 0x16), tm);
    }
    if (fcb_u16(e, fs, (u16)(fo + 0x0E)) == 0)
        fcb_w16(e, fs, (u16)(fo + 0x0E), FCB_REC_DEFAULT);
}

/* generic record transfer; pos = byte offset, len = bytes, returns DOS status */
static u8 fcb_transfer(Emu *e, const char *path, int is_write, u32 pos, u32 len,
                       u16 bseg, u16 boff, u32 *moved)
{
    int h;
    int rc;
    u32 done = 0;
    u8 buf[FCB_CHUNK];

    *moved = 0;
    if (is_write) {
        rc = dos_open_ex(path, 2, DOS_SHARE_COMPAT, &h);
        if (rc == FAT_ENOENT) rc = dos_creat(path, &h);
        if (rc != FAT_OK) return 0xFF;
    } else {
        rc = dos_open_ex(path, 0, DOS_SHARE_COMPAT, &h);
        if (rc != FAT_OK) return 0xFF;
    }
    dos_lseek(h, pos);

    while (done < len) {
        u32 chunk = len - done;
        if (chunk > FCB_CHUNK) chunk = FCB_CHUNK;
        u32 got = 0;
        if (is_write) {
            for (u32 i = 0; i < chunk; i++)
                buf[i] = emu_rb(e, bseg, (u16)(boff + done + i));
            rc = dos_write(h, buf, chunk, &got);
        } else {
            rc = dos_read(h, buf, chunk, &got);
            for (u32 i = 0; i < got; i++)
                emu_wb(e, bseg, (u16)(boff + done + i), buf[i]);
        }
        if (rc != FAT_OK) { dos_close(h); return 0xFF; }
        done += got;
        if (got < chunk) break;     /* short read => EOF */
    }
    dos_close(h);
    *moved = done;
    return 0x00;
}

/* ---------------------------------------------------------------- */
/* individual FCB functions                                          */
/* ---------------------------------------------------------------- */
static void fcb_open(Emu *e, u16 fs, u16 fo, int create)
{
    char path[DOS_MAX_PATH];
    if (!fcb_name(e, fs, fo, path, sizeof(path), NULL)) { fcb_set_al(e, 0xFF); return; }
    int h;
    int rc;
    if (create) {
        /* FCB byte +0x0B is the last character of the extension, not an
         * attribute: DOS always creates FCB files as normal archive files. */
        rc = dos_creat_ex(path, 0x20, DOS_SHARE_COMPAT, 2, &h);
    } else {
        rc = dos_open_ex(path, 2, DOS_SHARE_COMPAT, &h);
    }
    if (rc != FAT_OK) {
        fcb_set_al(e, 0xFF); return; }
    dos_close(h);
    fcb_w16(e, fs, (u16)(fo + 0x0C), 0);   /* current block */
    emu_wb(e, fs, (u16)(fo + 0x20), 0);    /* current record */
    fcb_w32(e, fs, (u16)(fo + 0x21), 0);   /* random record */
    fcb_fill(e, fs, fo, path);
    fcb_set_al(e, 0x00);
}

/* find first/next: fill the DTA with an FCB-style entry */
static void fcb_find(Emu *e, u16 fs, u16 fo, int next)
{
    static dos_find f;
    static int valid = 0;
    static int drive = 0;
    static const char *unused = NULL;
    (void)unused;

    char nm[13];
    u8 at = 0;
    u32 sz = 0;
    u16 tm = 0, dt = 0;
    int rc;

    if (!next) {
        char pat[DOS_MAX_PATH];
        if (!fcb_name(e, fs, fo, pat, sizeof(pat), NULL)) { fcb_set_al(e, 0xFF); return; }
        /* an all-'?' name means "*.*" for find-first */
        /* match normal/hidden/system/directory/archive but never a volume
         * label (DOS FCB find does not report those). */
        rc = dos_find_first(pat, 0x1F, &f, nm, &at, &sz, &tm, &dt);
        drive = f.drive;
        valid = 1;
    } else {
        if (!valid) { fcb_set_al(e, 0xFF); return; }
        rc = dos_find_next(&f, nm, &at, &sz, &tm, &dt);
    }
    if (rc != FAT_OK) { fcb_set_al(e, 0xFF); return; }

    u16 dseg = g_dos.dta_seg, doff = g_dos.dta_off;
    u8 n11[11];
    dos_fat_name(nm, n11, NULL);
    emu_wb(e, dseg, doff, (u8)((drive >= 0 && drive <= 25) ? drive + 1 : 0));
    for (int i = 0; i < 11; i++) emu_wb(e, dseg, (u16)(doff + 1 + i), n11[i]);
    emu_wb(e, dseg, (u16)(doff + 12), at);
    fcb_w16(e, dseg, (u16)(doff + 23), tm);
    fcb_w16(e, dseg, (u16)(doff + 25), dt);
    fcb_w32(e, dseg, (u16)(doff + 29), sz);
    fcb_set_al(e, 0x00);
}

/* sequential / random record read or write */
static void fcb_io(Emu *e, u16 fs, u16 fo, int is_write, int random, u32 count)
{
    char path[DOS_MAX_PATH];
    if (!fcb_name(e, fs, fo, path, sizeof(path), NULL)) { fcb_set_al(e, 0xFF); return; }

    u16 rs = fcb_rec_size(e, fs, fo);
    u32 pos;
    if (random) pos = fcb_u32(e, fs, (u16)(fo + 0x21)) * (u32)rs;
    else pos = (u32)fcb_u16(e, fs, (u16)(fo + 0x0C)) * (u32)rs +
               (u32)emu_rb(e, fs, (u16)(fo + 0x20));

    u32 want = rs * count;
    u32 moved = 0;
    /* FCB record I/O transfers through the current DTA (set by INT 21h AH=1Ah) */
    u8 st = fcb_transfer(e, path, is_write, pos, want,
                         g_dos.dta_seg, g_dos.dta_off, &moved);

    if (st != 0x00) { fcb_set_al(e, 0xFF); return; }

    u32 recs = rs ? (moved + rs - 1) / rs : 0;
    if (count > 1) {
        /* block mode: CX is updated with the number of records transferred */
        e->cx = (u16)recs;
    }
    if (random) {
        fcb_w32(e, fs, (u16)(fo + 0x21), fcb_u32(e, fs, (u16)(fo + 0x21)) + recs);
    } else {
        u16 blk = fcb_u16(e, fs, (u16)(fo + 0x0C));
        u16 rec = (u16)emu_rb(e, fs, (u16)(fo + 0x20));
        rec = (u16)(rec + recs);
        while (rec >= 128) { rec = (u16)(rec - 128); blk++; }
        fcb_w16(e, fs, (u16)(fo + 0x0C), blk);
        emu_wb(e, fs, (u16)(fo + 0x20), (u8)rec);
    }
    if (is_write) fcb_fill(e, fs, fo, path);

    if (!is_write && moved == 0) fcb_set_al(e, 0x01);
    else if (!is_write && moved < want) fcb_set_al(e, 0x03);
    else fcb_set_al(e, 0x00);
}

static void fcb_delete(Emu *e, u16 fs, u16 fo)
{
    char pat[DOS_MAX_PATH];
    if (!fcb_name(e, fs, fo, pat, sizeof(pat), NULL)) { fcb_set_al(e, 0xFF); return; }

    dos_find f;
    char nm[13];
    u8 at = 0;
    u32 sz = 0;
    u16 tm = 0, dt = 0;
    int rc = dos_find_first(pat, 0xFF, &f, nm, &at, &sz, &tm, &dt);
    if (rc != FAT_OK) { fcb_set_al(e, 0xFF); return; }

    /* delete the current-dir entries that matched; build exact paths */
    int found = 0, deleted = 0;
    for (;;) {
        found++;
        if (!(at & 0x10)) {
            char src[DOS_MAX_PATH];
            int n = 0;
            /* keep the "X:" prefix of the pattern when there is one */
            if (pat[0] && pat[1] == ':') { src[n++] = pat[0]; src[n++] = ':'; }
            for (int i = 0; nm[i] && n < (int)sizeof(src) - 1; i++) src[n++] = nm[i];
            src[n] = 0;
            if (dos_unlink(src) == FAT_OK) deleted++;
        }
        rc = dos_find_next(&f, nm, &at, &sz, &tm, &dt);
        if (rc != FAT_OK) break;
    }
    fcb_set_al(e, deleted ? 0x00 : 0xFF);
    (void)found;
}

static void fcb_rename(Emu *e, u16 fs, u16 fo)
{
    char oldp[DOS_MAX_PATH], newp[DOS_MAX_PATH];
    if (!fcb_name(e, fs, fo, oldp, sizeof(oldp), NULL)) { fcb_set_al(e, 0xFF); return; }
    /* the new name lives at offset 16 in the extended FCB */
    if (!fcb_name(e, fs, (u16)(fo + 16), newp, sizeof(newp), NULL)) { fcb_set_al(e, 0xFF); return; }
    int rc = dos_rename(oldp, newp);
    fcb_set_al(e, (rc == FAT_OK) ? 0x00 : 0xFF);
}

static void fcb_size(Emu *e, u16 fs, u16 fo)
{
    char path[DOS_MAX_PATH];
    if (!fcb_name(e, fs, fo, path, sizeof(path), NULL)) { fcb_set_al(e, 0xFF); return; }
    u32 size = 0;
    u16 dt = 0, tm = 0;
    if (fcb_info(path, &size, &dt, &tm) != FAT_OK) { fcb_set_al(e, 0xFF); return; }
    u16 rs = fcb_rec_size(e, fs, fo);
    fcb_w32(e, fs, (u16)(fo + 0x10), size);
    fcb_w32(e, fs, (u16)(fo + 0x21), rs ? (size + rs - 1) / rs : 0);
    fcb_set_al(e, 0x00);
}

/* entry point: returns 1 when AH was an FCB call and was handled */
int fcb_int21(Emu *e, u8 ah)
{
    /* DOS passes the FCB address in DS:DX (not ES:DI - that is the
     * "extended FCB" convention and only true for a couple of calls). */
    u16 fs = e->ds, fo = e->dx;
    switch (ah) {
    case 0x0F: fcb_open(e, fs, fo, 0); return 1;   /* open existing */
    case 0x10: fcb_set_al(e, 0x00); return 1;      /* close (stateless) */
    case 0x11: fcb_find(e, fs, fo, 0); return 1;   /* find first */
    case 0x12: fcb_find(e, fs, fo, 1); return 1;   /* find next */
    case 0x13: fcb_delete(e, fs, fo); return 1;    /* delete */
    case 0x14: fcb_io(e, fs, fo, 0, 0, 1); return 1;/* seq read */
    case 0x15: fcb_io(e, fs, fo, 1, 0, 1); return 1;/* seq write */
    case 0x16: fcb_open(e, fs, fo, 1); return 1;   /* create */
    case 0x17: fcb_rename(e, fs, fo); return 1;    /* rename */
    case 0x21: fcb_io(e, fs, fo, 0, 1, 1); return 1;/* random read */
    case 0x22: fcb_io(e, fs, fo, 1, 1, 1); return 1;/* random write */
    case 0x23: fcb_size(e, fs, fo); return 1;      /* file size */
    case 0x27: fcb_io(e, fs, fo, 0, 1, e->cx); return 1; /* random block read */
    case 0x28: fcb_io(e, fs, fo, 1, 1, e->cx); return 1; /* random block write */
    default: return 0;
    }
}
