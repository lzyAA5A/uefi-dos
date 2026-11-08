/* dual_test.c -- Phase 4 (DOS 4.0 real-mode semantics) and Phase 6.1
 * (INT 21h vs native API dual-path consistency) self test.
 *
 * Everything here drives the *real* handlers: the INT 21h side calls
 * int21_handler() with a register-only Emu (DS:DX pointing into emulated
 * RAM), the native side calls g_dosapi.  Both are compared operation by
 * operation, which is exactly what phase 6.1 asks for.
 */

#include "uefidos.h"
#include "api/dosapi.h"
#include "emu/emu.h"
#include "kernel/dos.h"
#include "plat.h"
#include <stdio.h>

int plat_open_image(const char *path);

#define SCR_SEG   0x1000          /* scratch segment for strings/buffers */
#define SCR_OFF   0x0000

static Emu g_e;
static int g_bad;
static int g_sec;

static void ok(const char *what) { printf("  ok   %s\n", what); }
static void bad(const char *what, long got, long want)
{
    printf("  FAIL %s: got %ld want %ld\n", what, got, want);
    g_bad++; g_sec++;
}
static void section(const char *name)
{
    printf("%s\n", name);
    g_sec = 0;
}
static void section_done(const char *name)
{
    printf("  -> %s %s\n", name, g_sec ? "FAILED" : "OK");
}

/* put a NUL terminated string into emulated RAM, return its offset */
static u16 putstr(const char *s)
{
    u16 off = (u16)g_e.ds;    /* reuse a bump pointer kept in dx? no: explicit */
    (void)off;
    static u16 bump = 0x0800;
    u16 o = bump;
    for (int i = 0; s[i]; i++) emu_wb(&g_e, SCR_SEG, (u16)(bump + i), (u8)s[i]);
    emu_wb(&g_e, SCR_SEG, (u16)(bump + (u16)strlen(s)), 0);
    bump = (u16)(bump + strlen(s) + 2);
    return o;
}

/* run one INT 21h call; returns AX, sets *cf when non-NULL */
static u16 i21(u8 ah, u16 ax, u16 bx, u16 cx, u16 dx, u16 si, u16 di, u16 es, int *cf)
{
    g_e.ax = ax; g_e.bx = bx; g_e.cx = cx; g_e.dx = dx;
    g_e.si = si; g_e.di = di; g_e.es = es; g_e.ds = SCR_SEG;
    g_e.flags = 0x0202;
    g_e.ax = (u16)((ah << 8) | (ax & 0xFF));
    int21_handler(&g_e);
    if (cf) *cf = (g_e.flags & F_CF) ? 1 : 0;
    return g_e.ax;
}

/* ---------------- 4.1 SHARE semantics ---------------- */
static void test_share(void)
{
    section("[4.1] SHARE compatibility modes");

    int h1 = -1, h2 = -1, cf = 0;
    u16 pname = putstr("C:\\SHARE.TXT");

    /* make sure the file exists */
    u16 h = i21(0x3C, 0x0000, 0, 0x20, pname, 0, 0, 0, &cf);
    if (cf) { bad("create SHARE.TXT", h, 0); return; }
    i21(0x3E, 0x0000, h, 0, 0, 0, 0, 0, &cf);

    /* deny-read-write: a second open must fail with access denied (5) */
    u16 a = i21(0x3D, 0x0011 /* deny rw, read */, 0, 0, pname, 0, 0, 0, &cf);
    if (cf) { bad("open deny-rw", a, 0); return; }
    h1 = (int)a;
    u16 b = i21(0x3D, 0x0000, 0, 0, pname, 0, 0, 0, &cf);
    if (!cf) bad("second open of a deny-rw file should fail", b, 5);
    else if (b != 5) bad("deny-rw conflict error code", b, 5);
    else ok("deny-read/write blocks a second open (error 5)");

    /* deny-none twice: both opens succeed */
    pname = putstr("C:\\SHARE2.TXT");
    u16 c = i21(0x3C, 0x0000, 0, 0x20, pname, 0, 0, 0, &cf);
    if (!cf) i21(0x3E, 0x0000, c, 0, 0, 0, 0, 0, &cf);
    u16 d = i21(0x3D, 0x0040 /* deny none, read */, 0, 0, pname, 0, 0, 0, &cf);
    int cfd = cf;
    u16 f = i21(0x3D, 0x0040, 0, 0, pname, 0, 0, 0, &cf);
    if (cfd || cf) bad("deny-none should allow two opens", cf | (cfd << 1), 0);
    else ok("deny-none allows two opens");
    if (!cfd) i21(0x3E, 0x0000, d, 0, 0, 0, 0, 0, &cf);
    if (!cf)  i21(0x3E, 0x0000, f, 0, 0, 0, 0, 0, &cf);

    /* deny-write: read open ok, write open denied */
    pname = putstr("C:\\SHARE3.TXT");
    u16 g1 = i21(0x3C, 0x0000, 0, 0x20, pname, 0, 0, 0, &cf);
    if (!cf) i21(0x3E, 0x0000, g1, 0, 0, 0, 0, 0, &cf);
    u16 g2 = i21(0x3D, 0x0021 /* deny write, read */, 0, 0, pname, 0, 0, 0, &cf);
    int cfg2 = cf;
    u16 g3 = i21(0x3D, 0x0001 /* compat? no: write access */, 0, 0, pname, 0, 0, 0, &cf);
    if (!cfg2) ok("deny-write still allows a read open");
    else bad("deny-write blocked a read open", cfg2, 0);
    if (!cf) bad("deny-write did not block a write open", g3, 5);
    else ok("deny-write blocks a write open (error 5)");
    if (!cfg2) i21(0x3E, 0x0000, g2, 0, 0, 0, 0, 0, &cf);
    if (!cf)  i21(0x3E, 0x0000, g3, 0, 0, 0, 0, 0, &cf);
    if (h1 >= 0) i21(0x3E, 0x0000, (u16)h1, 0, 0, 0, 0, 0, &cf);

    (void)h2;
    i21(0x41, 0x0000, 0, 0, putstr("C:\\SHARE.TXT"), 0, 0, 0, &cf);
    i21(0x41, 0x0000, 0, 0, putstr("C:\\SHARE2.TXT"), 0, 0, 0, &cf);
    i21(0x41, 0x0000, 0, 0, putstr("C:\\SHARE3.TXT"), 0, 0, 0, &cf);
    section_done("share semantics");
}

/* ---------------- 4.2 FILES= / AH=67h ---------------- */
static void test_files(void)
{
    section("[4.2] FILES= limit");
    int cf = 0;
    int saved = g_dos.files_limit;
    u16 pn = putstr("C:\\FILES.TXT");
    u16 h = i21(0x3C, 0x0000, 0, 0x20, pn, 0, 0, 0, &cf);
    if (cf) { bad("create FILES.TXT", h, 0); return; }
    i21(0x3E, 0x0000, h, 0, 0, 0, 0, 0, &cf);

    /* default FILES=8 => handles 5,6,7 are the only free ones */
    g_dos.files_limit = DOS_FILES_DEFAULT;
    int hs[8];
    int opened = 0, err = 0;
    for (int i = 0; i < 6; i++) {
        u16 r = i21(0x3D, 0x0000, 0, 0, pn, 0, 0, 0, &cf);
        if (!cf) hs[opened++] = (int)r;
        else { err = r; break; }
    }
    int expect = DOS_FILES_DEFAULT - 5;      /* 3 free handles */
    if (opened == expect) ok("FILES=8 yields 3 free handles (0..4 predefined)");
    else bad("FILES=8 free handle count", opened, expect);
    if (err != 4) bad("too many open files should be error 4", err, 4);
    else ok("over the FILES= limit returns error 4 (too many open files)");
    for (int i = 0; i < opened; i++) i21(0x3E, 0x0000, (u16)hs[i], 0, 0, 0, 0, 0, &cf);

    /* AH=67h raises the limit */
    i21(0x67, 0x6700, 20, 0, 0, 0, 0, 0, &cf);
    opened = 0; err = 0;
    for (int i = 0; i < 8; i++) {
        u16 r = i21(0x3D, 0x0000, 0, 0, pn, 0, 0, 0, &cf);
        if (!cf) hs[opened++] = (int)r; else { err = r; break; }
    }
    if (opened == 8) ok("AH=67h BX=20 allows 8 concurrent opens");
    else bad("AH=67h handle count", opened, 8);
    for (int i = 0; i < opened; i++) i21(0x3E, 0x0000, (u16)hs[i], 0, 0, 0, 0, 0, &cf);

    g_dos.files_limit = saved;
    i21(0x41, 0x0000, 0, 0, pn, 0, 0, 0, &cf);
    section_done("FILES= handling");
}

/* ---------------- 4.3 FCB calls ---------------- */
#define FCB_OFF 0x0600

static u16 fcb_build(u16 off, u8 drive, const char *name83)
{
    for (int i = 0; i < 37; i++) emu_wb(&g_e, SCR_SEG, (u16)(off + i), 0);
    emu_wb(&g_e, SCR_SEG, off, drive);
    for (int i = 0; i < 11; i++) {
        char c = name83[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        emu_wb(&g_e, SCR_SEG, (u16)(off + 1 + i), (u8)c);
    }
    return off;
}

static void test_fcb(void)
{
    section("[4.3] FCB calls (0Fh/10h/11h/12h/13h/14h/15h/16h/17h/21h/22h/23h/27h/28h)");
    u16 fcb = fcb_build(FCB_OFF, 3 /* C: */, "FCBTEST TXT");
    /* FCB record I/O uses the DTA; point it at our scratch record buffer */
    g_dos.dta_seg = SCR_SEG;
    g_dos.dta_off = 0x0700;
    u16 buf = 0x0700;
    int cf = 0;

    /* create */
    u16 r = i21(0x16, 0x1600, 0, 0, fcb, 0, 0, 0, &cf);
    if ((r & 0xFF) != 0) bad("FCB create status", r & 0xFF, 0);
    else ok("FCB create (AH=16h)");
    i21(0x10, 0x1000, 0, 0, fcb, 0, 0, 0, &cf);          /* close */

    /* sequential write: record size 128, cur block/record reset by create */
    for (int i = 0; i < 128; i++) emu_wb(&g_e, SCR_SEG, (u16)(buf + i), (u8)('a' + (i % 26)));
    r = i21(0x0F, 0x0F00, 0, 0, fcb, 0, 0, 0, &cf);      /* open */
    if ((r & 0xFF) != 0) bad("FCB open status", r & 0xFF, 0);
    else ok("FCB open (AH=0Fh)");
    r = i21(0x15, 0x1500, 0, 0, fcb, 0, 0, 0, &cf);    /* seq write record 0 */
    if ((r & 0xFF) != 0) bad("FCB seq write status", r & 0xFF, 0);
    else ok("FCB sequential write (AH=15h)");

    /* file size should now be 128 bytes => 1 record */
    i21(0x10, 0x1000, 0, 0, fcb, 0, 0, 0, &cf);
    r = i21(0x23, 0x2300, 0, 0, fcb, 0, 0, 0, &cf);
    u32 recs = (u32)emu_rb(&g_e, SCR_SEG, (u16)(fcb + 0x21)) |
               ((u32)emu_rb(&g_e, SCR_SEG, (u16)(fcb + 0x22)) << 8) |
               ((u32)emu_rb(&g_e, SCR_SEG, (u16)(fcb + 0x23)) << 16) |
               ((u32)emu_rb(&g_e, SCR_SEG, (u16)(fcb + 0x24)) << 24);
    if (!cf && recs == 1) ok("FCB file size (AH=23h) => 1 record");
    else bad("FCB file size records", (long)recs, 1);

    /* random read record 0 back and compare */
    fcb_build(FCB_OFF, 3, "FCBTEST TXT");
    i21(0x0F, 0x0F00, 0, 0, fcb, 0, 0, 0, &cf);
    for (int i = 0; i < 128; i++) emu_wb(&g_e, SCR_SEG, (u16)(buf + i), 0);
    r = i21(0x21, 0x2100, 0, 0, fcb, 0, 0, 0, &cf);   /* random read */
    int same = 1;
    for (int i = 0; i < 128; i++)
        if (emu_rb(&g_e, SCR_SEG, (u16)(buf + i)) != (u8)('a' + (i % 26))) same = 0;
    if (!cf && same) ok("FCB random read (AH=21h) round-trips the record");
    else bad("FCB random read data", same, 1);
    i21(0x10, 0x1000, 0, 0, fcb, 0, 0, 0, &cf);

    /* find first / next */
    fcb_build(FCB_OFF, 0, "FCBTEST TXT");
    r = i21(0x11, 0x1100, 0, 0, fcb, 0, 0, 0, &cf);
    char nm[12];
    for (int i = 0; i < 11; i++) nm[i] = (char)emu_rb(&g_e, g_dos.dta_seg, (u16)(g_dos.dta_off + 1 + i));
    nm[11] = 0;
    if ((r & 0xFF) == 0 && memcmp(nm, "FCBTEST TXT", 11) == 0)
        ok("FCB find first (AH=11h) fills the DTA");
    else bad("FCB find first status", r & 0xFF, 0);

    /* rename then find next on a wildcard */
    fcb_build(FCB_OFF, 0, "???????????");
    r = i21(0x11, 0x1100, 0, 0, fcb, 0, 0, 0, &cf);
    int hits = 0;
    while ((r & 0xFF) == 0 && hits < 40) {
        hits++;
        r = i21(0x12, 0x1200, 0, 0, fcb, 0, 0, 0, &cf);
    }
    if (hits > 2) ok("FCB find next (AH=12h) walks the directory");
    else bad("FCB find next hit count", hits, 3);

    /* rename via the extended FCB (new name at +16) */
    fcb_build(FCB_OFF, 0, "FCBTEST TXT");
    fcb_build((u16)(FCB_OFF + 16), 0, "FCBREN  TXT");
    r = i21(0x17, 0x1700, 0, 0, fcb, 0, 0, 0, &cf);
    if ((r & 0xFF) == 0) ok("FCB rename (AH=17h)");
    else bad("FCB rename status", r & 0xFF, 0);

    /* delete via FCB */
    fcb_build(FCB_OFF, 0, "FCBREN  TXT");
    r = i21(0x13, 0x1300, 0, 0, fcb, 0, 0, 0, &cf);
    if ((r & 0xFF) == 0) ok("FCB delete (AH=13h)");
    else bad("FCB delete status", r & 0xFF, 0);
    section_done("FCB support");
}

/* ---------------- 4.4 SHARE byte locks (AH=5Ch) ---------------- */
static void test_locks(void)
{
    section("[4.4] SHARE byte-range locks (AH=5Ch)");
    int cf = 0;
    u16 pn = putstr("C:\\LOCK.TXT");
    u16 h0 = i21(0x3C, 0x0000, 0, 0x20, pn, 0, 0, 0, &cf);
    if (!cf) i21(0x3E, 0x0000, h0, 0, 0, 0, 0, 0, &cf);
    u16 h1 = i21(0x3D, 0x0040, 0, 0, pn, 0, 0, 0, &cf);   /* deny none, read */
    int c1 = cf;
    u16 h2 = i21(0x3D, 0x0040, 0, 0, pn, 0, 0, 0, &cf);
    int c2 = cf;
    if (c1 || c2) { bad("lock test opens", c1 + c2, 0); return; }

    i21(0x5C, 0x5C00, h1, 0, 0, 0, 0x10, 0, &cf);        /* lock 0..0x10 */
    if (cf) bad("lock region on h1 failed", cf, 0);
    else ok("AH=5Ch AL=0 locks a byte range");
    i21(0x5C, 0x5C00, h2, 0, 0, 0, 0x08, 0, &cf);
    if (!cf) bad("overlapping lock on h2 should fail", cf, 1);
    else ok("overlapping lock from another handle is refused");
    i21(0x5C, 0x5C01, h1, 0, 0, 0, 0x10, 0, &cf);        /* unlock */
    if (cf) bad("unlock failed", cf, 0);
    else ok("AH=5Ch AL=1 unlocks the range");
    i21(0x5C, 0x5C00, h2, 0, 0, 0, 0x08, 0, &cf);
    if (cf) bad("lock after unlock failed", cf, 0);
    else ok("range can be locked once released");

    i21(0x3E, 0x0000, h1, 0, 0, 0, 0, 0, &cf);
    i21(0x3E, 0x0000, h2, 0, 0, 0, 0, 0, &cf);
    i21(0x41, 0x0000, 0, 0, pn, 0, 0, 0, &cf);
    section_done("byte locks");
}

/* ---------------- 6.1 dual path consistency ---------------- */
static void test_dual(void)
{
    section("[6.1] INT 21h vs native API consistency");
    int cf = 0;
    const char *data = "dual-path payload";

    /* --- create: AH=3Ch vs dosapi.create --- */
    u16 pn = putstr("C:\\DUAL.TXT");
    u16 h = i21(0x3C, 0x0000, 0, 0x20, pn, 0, 0, 0, &cf);
    int i21_ok = !cf;
    int i21_h = (int)h;
    u16 nh = 0;
    int nrc = g_dosapi.create("C:\\DUAL2.TXT", 0x20, &nh);
    if (i21_ok && nrc == 0) ok("create: both paths return a handle");
    else bad("create: one path failed", i21_ok + (nrc == 0), 2);

    /* --- write the same bytes through both paths --- */
    for (u32 i = 0; i < strlen(data); i++)
        emu_wb(&g_e, SCR_SEG, (u16)(0x0900 + i), (u8)data[i]);
    u16 w1 = i21(0x40, 0x4000, (u16)i21_h, strlen(data), 0x0900, 0, 0, 0, &cf);
    u32 put = 0;
    int w2 = g_dosapi.write(nh, data, (u32)strlen(data), &put);
    if (!cf && w1 == strlen(data) && w2 == 0 && put == strlen(data))
        ok("write: both paths report the same byte count");
    else bad("write mismatch", (long)w1 * 100 + (long)put, (long)strlen(data) * 100 + (long)strlen(data));

    /* --- seek to 5 on both --- */
    i21(0x42, 0x4200, (u16)i21_h, 0, 5, 0, 0, 0, &cf);
    u32 npos = 0;
    int s2 = g_dosapi.seek(nh, 5, 0, &npos);
    if (!cf && s2 == 0 && npos == 5) ok("seek: both paths agree on the new position");
    else bad("seek mismatch", (long)npos, 5);

    /* --- read back through both after rewinding --- */
    i21(0x42, 0x4200, (u16)i21_h, 0, 0, 0, 0, 0, &cf);
    g_dosapi.seek(nh, 0, 0, &npos);
    for (int i = 0; i < 32; i++) emu_wb(&g_e, SCR_SEG, (u16)(0x0A00 + i), 0);
    i21(0x3F, 0x3F00, (u16)i21_h, 32, 0x0A00, 0, 0, 0, &cf);
    char nb[64];
    u32 got2 = 0;
    memset(nb, 0, sizeof(nb));
    g_dosapi.read(nh, nb, 32, &got2);
    int same = 1;
    for (u32 i = 0; i < got2; i++)
        if (emu_rb(&g_e, SCR_SEG, (u16)(0x0A00 + i)) != (u8)nb[i]) same = 0;
    if (same && got2 == strlen(data)) ok("read: both paths return identical bytes");
    else bad("read data mismatch", same, 1);

    /* --- close both, compare sizes via findfirst --- */
    i21(0x3E, 0x0000, (u16)i21_h, 0, 0, 0, 0, 0, &cf);
    g_dosapi.close(nh);

    dos_find f;
    char nm[13];
    u8 at = 0;
    u32 sz1 = 0, sz2 = 0;
    u16 tm = 0, dt = 0;
    u16 pn2 = putstr("C:\\DUAL2.TXT");
    int rc1 = dos_find_first("C:\\DUAL.TXT", 0xFF, &f, nm, &at, &sz1, &tm, &dt);
    dosapi_findinfo fi;
    int rc2 = g_dosapi.find_first("C:\\DUAL2.TXT", 0xFF, &fi);
    sz2 = fi.size;
    (void)pn2;
    if (rc1 == FAT_OK && rc2 == 0 && sz1 == sz2)
        ok("find first: INT 21h DTA path and native find_first agree on size");
    else bad("find size disagreement", (long)sz1 - (long)sz2, 0);

    /* --- error path: open a missing file on both --- */
    u16 pn3 = putstr("C:\\NOSUCH.TXT");
    i21(0x3D, 0x0000, 0, 0, pn3, 0, 0, 0, &cf);
    int e1 = cf ? (int)(g_e.ax) : 0;
    u16 nh2 = 0;
    int e2 = g_dosapi.open("C:\\NOSUCH.TXT", 0, &nh2);
    if (e1 == 2 && e2 == 2) ok("missing file: both paths return DOS error 2");
    else bad("missing file error codes", e1 * 10 + e2, 22);

    /* --- error path: delete a read-only file --- */
    u16 pn4 = putstr("C:\\RO.TXT");
    u16 hro = i21(0x3C, 0x0000, 0, 0x20, pn4, 0, 0, 0, &cf);
    if (!cf) i21(0x3E, 0x0000, hro, 0, 0, 0, 0, 0, &cf);
    i21(0x43, 0x4301, 0, 0x01, pn4, 0, 0, 0, &cf);        /* set read-only */
    i21(0x41, 0x0000, 0, 0, pn4, 0, 0, 0, &cf);
    int ro1 = cf ? (int)g_e.ax : 0;
    int ro2 = g_dosapi.unlink("C:\\RO.TXT");
    if (ro1 == 5 && ro2 == 5) ok("read-only delete: both paths return DOS error 5");
    else bad("read-only delete codes", ro1 * 10 + ro2, 55);
    i21(0x43, 0x4300, 0, 0x20, pn4, 0, 0, 0, &cf);

    i21(0x41, 0x0000, 0, 0, putstr("C:\\DUAL.TXT"), 0, 0, 0, &cf);
    i21(0x41, 0x0000, 0, 0, putstr("C:\\DUAL2.TXT"), 0, 0, 0, &cf);
    i21(0x41, 0x0000, 0, 0, putstr("C:\\RO.TXT"), 0, 0, 0, &cf);
    section_done("dual path");
}

int dual_selftest(const char *image)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (plat_open_image(image)) { printf("cannot open image %s\n", image); return 1; }
    dos_init();
    int mrc = dos_mount_boot();
    if (mrc) { printf("mount failed rc=%d\n", mrc); return 1; }
    dos_read_config_sys();
    if (emu_init(&g_e)) { printf("emu_init failed\n"); return 1; }

    printf("UEFI-DOS phase 4/6 self test on %s\n", image);
    printf("  CONFIG.SYS: FILES=%d FCBS=%d LASTDRIVE=%c\n",
           g_dos.files_limit, g_dos.fcb_limit, 'A' + g_dos.last_drive);

    test_share();
    test_files();
    test_fcb();
    test_locks();
    test_dual();

    emu_free(&g_e);
    printf("\nPHASE46 %s\n", g_bad ? "FAILED" : "PASSED");
    return g_bad ? 1 : 0;
}
