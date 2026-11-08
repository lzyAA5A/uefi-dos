/*
 * UEFI-DOS -- INT 21h / INT 10h / INT 16h services for emulated programs.
 */
#include "uefidos.h"
#include "emu/emu.h"
#include "kernel/dos.h"
#include "plat.h"

static int g_pending_key = -1;
static int g_pending_scan = -1;
static int g_pending_asc = -1;
static int g_pending_cr = 0;
static dos_find g_find;
static int g_find_valid = 0;

/* Host-only interrupt trace: UEFIDOS_TRACE_INTS=1 writes build/int_trace.txt
 * so a compatibility sweep can see exactly which DOS services a program uses. */
#ifdef UEFIDOS_HOST
#include <stdio.h>
#include <stdlib.h>
static FILE *g_trace;
static unsigned long g_trace_n;
static int g_trace_enabled = -1;
static void trace_call(Emu *e, int intno)
{
    if (g_trace_enabled < 0) {
        g_trace_enabled = getenv("UEFIDOS_TRACE_INTS") ? 1 : 0;
        if (g_trace_enabled) g_trace = fopen("build/int_trace.txt", "w");
    }
    if (!g_trace_enabled || !g_trace) return;
    g_trace_n++;
    if (g_trace_n <= 300 || (g_trace_n % 1000) == 0) {
        fprintf(g_trace, "%7lu INT%02X AH=%02X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X ES=%04X\n",
                g_trace_n, intno, (unsigned)(e->ax >> 8), e->ax, e->bx, e->cx, e->dx,
                e->si, e->di, e->es);
        fflush(g_trace);
    }
}
#else
#define trace_call(e, intno) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static int read_dos_str(Emu *e, u16 seg, u16 off, char *dst, int max)
{
    int n = 0;
    while (n < max - 1) {
        u8 c = emu_rb(e, seg, (u16)(off + n));
        if (c == 0) break;
        dst[n++] = (char)c;
    }
    dst[n] = 0;
    return n;
}

static void con_put(Emu *e, char c);

static void set_err(Emu *e, u8 code) { e->flags |= F_CF; e->ax = code; }
static void set_ok(Emu *e) { e->flags = (u16)(e->flags & ~F_CF); }

/* Day of week for a Gregorian date: 0 = Sunday .. 6 = Saturday
   (Sakamoto's method, which is valid for any y >= 1). */
static int dos_weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12) return 0;
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return w < 0 ? w + 7 : w;
}

static int input_key(Emu *e)
{
    int scan = 0, asc = 0;
    if (g_pending_key >= 0) {
        int k = g_pending_key;
        g_pending_key = -1;
        g_pending_scan = g_pending_asc = -1;
        return k;
    }
    emu_kbd_get_full(e, &scan, &asc);
    return asc ? asc : (0x100 | (scan & 0xFF));
}

static int check_key(Emu *e)
{
    int scan = 0, asc = 0;
    if (g_pending_key >= 0) return 1;
    if (!emu_kbd_peek(e)) return 0;
    emu_kbd_get_full(e, &scan, &asc);
    g_pending_scan = scan;
    g_pending_asc = asc;
    g_pending_key = asc ? asc : (0x100 | (scan & 0xFF));
    return 1;
}

static void dta_set(Emu *e, const char *name, u8 attr, u32 size, u16 tm, u16 dt)
{
    u16 seg = g_dos.dta_seg, off = g_dos.dta_off;
    emu_wb(e, seg, (u16)(off + 0x15), attr);
    emu_ww(e, seg, (u16)(off + 0x16), tm);
    emu_ww(e, seg, (u16)(off + 0x18), dt);
    emu_wd(e, seg, (u16)(off + 0x1A), size);
    for (int i = 0; i < 13; i++) emu_wb(e, seg, (u16)(off + 0x1E + i), (u8)(name[i] ? name[i] : 0));
    emu_wb(e, seg, (u16)(off + 0x1E + 12), 0);
}

/* ------------------------------------------------------------------ */
/* INT 21h                                                             */
/* ------------------------------------------------------------------ */
void int21_handler(Emu *e)
{
    trace_call(e, 0x21);
    u8 ah = (u8)(e->ax >> 8);
    /* DOS 4.0 FCB call family (AH=0Fh..17h, 21h..23h, 27h, 28h) lives in fcb.c */
    if (fcb_int21(e, ah)) return;
    switch (ah) {
    case 0x00:
        e->halted = 1; e->exit_code = 0;
        return;
    case 0x26: { /* delete file from DTA */
        u8 attr;
        char name[13];
        for (int i = 0; i < 13; i++) name[i] = (char)emu_rb(e, g_dos.dta_seg, (u16)(g_dos.dta_off + 0x1E + i));
        (void)attr;
        int rc = dos_unlink(name);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x01: {
        int c = input_key(e);
        if (c == '\r') c = '\r';
        con_put(e, (char)c);
        set_ok(e);
        e->ax = (u16)((e->ax & 0xFF00) | (u8)c);
        return;
    }
    case 0x02:
        con_put(e, (char)(e->dx & 0xFF));
        set_ok(e);
        return;
    case 0x03: case 0x04:
        set_ok(e);
        e->ax = (u16)(e->ax & 0xFF00);
        return;
    case 0x05:
        con_put(e, (char)(e->dx & 0xFF));
        set_ok(e);
        return;
    case 0x06: {
        if ((e->dx & 0xFF) == 0xFF) {
            if (check_key(e)) {
                int k = g_pending_key; g_pending_key = -1;
                e->flags &= ~F_ZF;
                e->ax = (u16)((e->ax & 0xFF00) | (u8)k);
            } else {
                e->flags |= F_ZF;
                e->ax = (u16)(e->ax & 0xFF00);
            }
        } else {
            con_put(e, (char)(e->dx & 0xFF));
            set_ok(e);
        }
        return;
    }
    case 0x07: {
        int c = input_key(e);
        set_ok(e);
        e->ax = (u16)((e->ax & 0xFF00) | (u8)c);
        return;
    }
    case 0x08: {
        int c = input_key(e);
        set_ok(e);
        e->ax = (u16)((e->ax & 0xFF00) | (u8)c);
        return;
    }
    case 0x09: {
        u16 seg = e->ds, off = e->dx;
        for (;;) {
            u8 c = emu_rb(e, seg, off++);
            if (c == '$') break;
            con_put(e, (char)c);
        }
        set_ok(e);
        return;
    }
    case 0x0A: {
        u16 seg = e->ds, off = e->dx;
        u8 maxlen = emu_rb(e, seg, off);
        int n = 0;
        for (;;) {
            int c = input_key(e);
            if (c == '\r' || c == '\n') { con_put(e, '\n'); break; }
            if (c == 8) {
                if (n > 0) {
                    n--;
                    plat_putc(8); plat_putc(' '); plat_putc(8);
                }
                continue;
            }
            if (c >= 32 && c < 127 && n < (int)maxlen - 1) {
                emu_wb(e, seg, (u16)(off + 2 + n), (u8)c);
                con_put(e, (char)c);
                n++;
            }
        }
        emu_wb(e, seg, (u16)(off + 1), (u8)n);
        set_ok(e);
        return;
    }
    case 0x0B: {
        int avail = check_key(e);
        e->ax = (u16)((e->ax & 0xFF00) | (avail ? 0xFF : 0x00));
        set_ok(e);
        return;
    }
    case 0x0C: /* flush input */
        g_pending_key = -1;
        set_ok(e);
        e->ax = (u16)(e->ax & 0xFF00);
        return;
    case 0x0D: case 0x18:                       /* flush buffers / no-op */
        set_ok(e);
        e->ax = (u16)(e->ax & 0xFF00);
        return;
    case 0x0E:                                  /* select default drive */
        set_ok(e);
        dos_set_drive((int)(e->dx & 0xFF));
        e->ax = (u16)((e->ax & 0xFF00) | (u8)(g_dos.last_drive + 1));
        return;
    case 0x19:                                  /* current default drive */
        set_ok(e);
        e->ax = (u16)((e->ax & 0xFF00) | (u8)dos_current_drive());
        return;
    case 0x1A:
        g_dos.dta_seg = e->ds;
        g_dos.dta_off = e->dx;
        set_ok(e);
        return;
    case 0x25: {
        u16 off = e->dx, seg = e->ds;
        emu_ww(e, 0, (u16)(((e->ax >> 8) & 0xFF) * 4), off);
        emu_ww(e, 0, (u16)(((e->ax >> 8) & 0xFF) * 4 + 2), seg);
        set_ok(e);
        return;
    }
    case 0x2A: {
        int y, m, d;
        plat_get_date(&y, &m, &d);
        e->cx = (u16)y;
        e->dx = (u16)((m << 8) | d);
        set_ok(e);
        e->ax = (u16)((e->ax & 0xFF00) | (u8)dos_weekday(y, m, d));
        return;
    }
    case 0x2B: set_ok(e); return;
    case 0x2C: {
        int h, mi, s;
        plat_get_time(&h, &mi, &s);
        e->cx = (u16)((h << 8) | mi);
        e->dx = (u16)((s << 8));
        set_ok(e);
        return;
    }
    case 0x2D: set_ok(e); return;
    case 0x2E: set_ok(e); return;
    case 0x2F: e->es = g_dos.dta_seg; e->bx = g_dos.dta_off; set_ok(e); return;
    case 0x30:
        /* DOS returns AL = major version, AH = minor version:
         * 2.10 -> AX = 0x0A02 (see UEFIDOS_DOSVER_COMPAT_MINOR). */
        e->ax = (u16)(((UEFIDOS_DOSVER_COMPAT_MINOR & 0xFF) << 8) | (UEFIDOS_DOSVER_MAJOR & 0xFF));
        set_ok(e);
        return;
    case 0x33:
        if ((e->ax & 0xFF) == 0x00) { e->dx = (u16)((e->dx & 0xFF00) | 0); }
        set_ok(e);
        return;
    case 0x35: {
        u8 n = (u8)(e->ax & 0xFF);
        u16 off = emu_rw(e, 0, (u16)(n * 4));
        u16 seg = emu_rw(e, 0, (u16)(n * 4 + 2));
        /* Serviced vectors point at a callable trampoline (F000:E000+n*4,
         * "INT n; RETF"), so programs that save and far-call the old vector
         * get real BIOS behaviour instead of an IRET no-op. */
        if (seg == MAGIC_SEG && off == MAGIC_OFF) { seg = MAGIC_SEG; off = STUB_OFF; }
        e->bx = off;
        e->es = seg;
        set_ok(e);
        return;
    }
    case 0x36: {
        int d = (e->dx & 0xFF) - 1;
        if (d < 0) d = dos_current_drive();
        if (d < 0 || d >= DOS_MAX_DRIVES || !g_dos.drives[d].mounted) { e->ax = 0xFFFF; set_ok(e); return; }
        fat_vol *v = &g_dos.drives[d].vol;
        e->ax = v->spc;
        e->cx = 512;
        e->dx = (u16)(v->cluster_count > 0xFFFF ? 0xFFFF : v->cluster_count);
        /* count free clusters (bounded scan) */
        u32 freec = 0;
        for (u32 cl = 2; cl < v->cluster_count + 2 && cl < 30000; cl++) {
            u32 nxt;
            if (fat_next_cluster(v, cl, &nxt)) break;
            if (nxt == 0xFFFFFFFDu) freec++;
        }
        e->bx = (u16)(freec > 0xFFFF ? 0xFFFF : freec);
        set_ok(e);
        return;
    }
    case 0x39: {
        char path[DOS_MAX_PATH];
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        int rc = dos_mkdir(path);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x3A: {
        char path[DOS_MAX_PATH];
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        int rc = dos_rmdir(path);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x3B: {
        char path[DOS_MAX_PATH];
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        int rc = dos_chdir(path);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x3D: {
        char path[DOS_MAX_PATH];
        u8 mode = (u8)(e->ax & 0xFF);
        int open_mode = mode & 0x03;          /* 0 read, 1 write, 2 r/w */
        int share = (mode >> 4) & 0x07;       /* 0 compat .. 4 deny none */
        if (open_mode == 3) open_mode = 2;
        if (share > 4) share = 0;
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        int h;
        int rc = dos_open_ex(path, open_mode, share, &h);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        e->ax = (u16)h;
        return;
    }
    case 0x3C: {
        char path[DOS_MAX_PATH];
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        int h;
        int rc = dos_creat_ex(path, e->cx & 0xFF,
                              (e->ax & 0xFF) <= 4 ? (int)(e->ax & 0xFF) : 0, 2, &h);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        e->ax = (u16)h;
        return;
    }
    case 0x3E: {
        int rc = dos_close(e->bx);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x3F: {
        int h = e->bx;
        u32 want = e->cx;
        u16 seg = e->ds, off = e->dx;
        if (h < 5) {
            char tmp[512];
            u32 got = 0;
            int rc = dos_read(h, tmp, sizeof(tmp) - 1, &got);
            if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
            for (u32 i = 0; i < got; i++) emu_wb(e, seg, (u16)(off + i), (u8)tmp[i]);
            set_ok(e);
            e->ax = (u16)got;
            return;
        }
        u32 total = 0;
        char tmp[512];
        while (total < want) {
            u32 chunk = want - total;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            u32 got = 0;
            int rc = dos_read(h, tmp, chunk, &got);
            if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
            for (u32 i = 0; i < got; i++) emu_wb(e, seg, (u16)(off + total + i), (u8)tmp[i]);
            total += got;
            if (got < chunk) break;
        }
        set_ok(e);
        e->ax = (u16)total;
        return;
    }
    case 0x40: {
        int h = e->bx;
        u32 len = e->cx;
        u16 seg = e->ds, off = e->dx;
        if (h == 1 || h == 2) {
            for (u32 i = 0; i < len; i++) con_put(e, (char)emu_rb(e, seg, (u16)(off + i)));
            set_ok(e);
            e->ax = (u16)len;
            return;
        }
        u32 total = 0;
        char tmp[512];
        while (total < len) {
            u32 chunk = len - total;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            for (u32 i = 0; i < chunk; i++) tmp[i] = (char)emu_rb(e, seg, (u16)(off + total + i));
            u32 put = 0;
            int rc = dos_write(h, tmp, chunk, &put);
            if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
            total += put;
            if (put < chunk) break;
        }
        set_ok(e);
        e->ax = (u16)total;
        return;
    }
    case 0x41: {
        char path[DOS_MAX_PATH];
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        int rc = dos_unlink(path);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x42: {
        int h = e->bx;
        if (h < 0 || h >= DOS_HANDLE_MAX || !g_dos.handles[h].used) { set_err(e, 6); return; }
        u32 off = ((u32)e->cx << 16) | e->dx;
        u8 whence = (u8)(e->ax & 0xFF);
        u32 pos = off;
        if (whence == 1) pos = g_dos.handles[h].pos + off;
        else if (whence == 2) pos = g_dos.handles[h].size + off;
        int rc = dos_lseek(h, pos);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        e->ax = (u16)(g_dos.handles[h].pos & 0xFFFF);
        e->dx = (u16)(g_dos.handles[h].pos >> 16);
        return;
    }
    case 0x43: {
        char path[DOS_MAX_PATH];
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        if ((e->ax & 0xFF) == 0) {
            u8 attr;
            int rc = dos_attr_get(path, &attr);
            if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
            e->cx = attr;
            set_ok(e);
        } else {
            int rc = dos_attr_set(path, (u8)(e->cx & 0xFF));
            if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
            set_ok(e);
        }
        return;
    }
    case 0x44: {
        u8 sub = (u8)(e->ax & 0xFF);
        if (sub == 0 || sub == 1) {
            int h = e->bx;
            int is_con = h < 5;
            e->dx = is_con ? 0x0080 : 0x0000;
            set_ok(e);
        } else if (sub == 8) {
            /* media check: return "changed"=1? return AX=0, DX=0 */
            e->ax = 0;
            set_ok(e);
        } else {
            set_ok(e);
        }
        return;
    }
    case 0x45: {
        int src = e->bx;
        if (src < 0 || src >= DOS_HANDLE_MAX || !g_dos.handles[src].used) { set_err(e, 6); return; }
        for (int i = 5; i < DOS_HANDLE_MAX; i++) {
            if (!g_dos.handles[i].used) {
                g_dos.handles[i] = g_dos.handles[src];
                e->ax = (u16)i;
                set_ok(e);
                return;
            }
        }
        set_err(e, 4);
        return;
    }
    case 0x46: {
        int src = e->cx, dst = e->bx;
        if (src >= DOS_HANDLE_MAX || dst >= DOS_HANDLE_MAX || !g_dos.handles[src].used) { set_err(e, 6); return; }
        g_dos.handles[dst] = g_dos.handles[src];
        set_ok(e);
        return;
    }
    case 0x47: {
        char cwd[DOS_MAX_PATH];
        dos_get_cwd(cwd, sizeof(cwd));
        int n = 0;
        while (cwd[n]) n++;
        int subdirs = n > 2;
        for (int i = 0; i <= n; i++) emu_wb(e, e->ds, (u16)(e->si + i), (u8)cwd[i]);
        e->ax = (u16)(subdirs ? 0x0100 : 0x0000);
        set_ok(e);
        return;
    }
    case 0x48: case 0x49: case 0x4A:
        if (ah == 0x48) { set_err(e, 8); }       /* no extra memory */
        else { set_ok(e); }
        return;
    case 0x4B: {                                 /* EXEC: load & execute child */
        char prog[DOS_MAX_PATH];
        char args[130];
        args[0] = 0;
        read_dos_str(e, e->ds, e->dx, prog, sizeof(prog));
        u16 pseg = e->es;
        u16 poff = e->bx;
        u16 cmd_off = emu_rw(e, pseg, (u16)(poff + 2));
        u16 cmd_seg = emu_rw(e, pseg, (u16)(poff + 4));
        if (cmd_off) {
            u16 seg = cmd_seg ? cmd_seg : e->ds;
            u8 n = emu_rb(e, seg, cmd_off);
            if (n > 126) n = 126;
            for (u16 i = 0; i < n; i++)
                args[i] = (char)emu_rb(e, seg, (u16)(cmd_off + 1 + i));
            args[n] = 0;
        }
        int code = emu_exec_program(e, prog, args);
        if (code < 0) {
            set_err(e, code == -1 ? 2 : 0x0B);
            return;
        }
        set_ok(e);
        e->ax = (u16)code;
        return;
    }
    case 0x4C:
        e->halted = 1;
        e->exit_code = (u8)(e->ax & 0xFF);
        return;
    case 0x4D: e->ax = e->last_exit_code; set_ok(e); return;
    case 0x4E: {
        char pat[DOS_MAX_PATH];
        char name[13];
        u8 attr, want_attr = (u8)e->cx;
        u32 size;
        u16 tm, dt;
        read_dos_str(e, e->ds, e->dx, pat, sizeof(pat));
        int rc = dos_find_first(pat, want_attr, &g_find, name, &attr, &size, &tm, &dt);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        g_find_valid = 1;
        dta_set(e, name, attr, size, tm, dt);
        set_ok(e);
        return;
    }
    case 0x4F: {
        if (!g_find_valid) { set_err(e, 0x12); return; }
        char name[13];
        u8 attr;
        u32 size;
        u16 tm, dt;
        int rc = dos_find_next(&g_find, name, &attr, &size, &tm, &dt);
        if (rc != FAT_OK) { set_err(e, 0x12); return; }
        dta_set(e, name, attr, size, tm, dt);
        set_ok(e);
        return;
    }
    case 0x51: case 0x62: e->bx = e->psp_seg; set_ok(e); return;
    case 0x56: {
        char oldp[DOS_MAX_PATH], newp[DOS_MAX_PATH];
        read_dos_str(e, e->ds, e->dx, oldp, sizeof(oldp));
        read_dos_str(e, e->es, e->di, newp, sizeof(newp));
        int rc = dos_rename(oldp, newp);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x57:
        set_ok(e);
        e->cx = 0;
        e->dx = 0;
        return;
    case 0x58:
        e->ax = 0;
        set_ok(e);
        return;
    case 0x5B: {
        char path[DOS_MAX_PATH];
        u8 attr;
        read_dos_str(e, e->ds, e->dx, path, sizeof(path));
        if (dos_attr_get(path, &attr) == FAT_OK) { set_err(e, 0x50); return; }
        int h;
        int rc = dos_creat(path, &h);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        e->ax = (u16)h;
        return;
    }
    case 0x5C: {
        /* AL=0 lock, AL=1 unlock; BX handle, CX:DX offset, SI:DI length */
        int h = e->bx;
        u32 off = ((u32)e->cx << 16) | e->dx;
        u32 len = ((u32)e->si << 16) | e->di;
        int rc;
        if ((e->ax & 0xFF) == 0) rc = dos_lock_region(h, off, len);
        else                     rc = dos_unlock_region(h, off, len);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        return;
    }
    case 0x67: {
        /* AH=67h: set maximum open file handles for this process */
        int n = e->bx;
        if (n < 5) n = 5;
        if (n > DOS_HANDLE_MAX) n = DOS_HANDLE_MAX;
        g_dos.files_limit = n;
        set_ok(e);
        return;
    }
    default:
        set_ok(e);
        e->ax = (u16)(e->ax & 0xFF00);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* INT 10h minimal video services                                      */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* INT 10h text-mode video.                                            */
/*                                                                     */
/* These services write emulated VRAM at 0xB8000 -- the screen the host */
/* mirror shows.  They used to push characters onto the host console    */
/* stream instead, so every program that paints its own screen with     */
/* AH=02 (set cursor) + AH=09 (write char+attr) drew somewhere the      */
/* mirror then erased: the bundled editor came up as a black screen.    */
/* ------------------------------------------------------------------ */
#define VRAM_SEG 0xB800

static void cur_sync(Emu *e)
{
    /* BIOS data area cursor position for page 0: 0040:0050 */
    emu_ww(e, 0x40, 0x50, (u16)((e->cury << 8) | (e->curx)));
}

static void vram_put(Emu *e, int x, int y, u8 ch, u8 attr)
{
    if (x < 0 || x > 79 || y < 0 || y > 24) return;
    emu_wb(e, VRAM_SEG, (u16)((y * 80 + x) * 2), ch);
    emu_wb(e, VRAM_SEG, (u16)((y * 80 + x) * 2 + 1), attr);
}

static void vram_get(Emu *e, int x, int y, u8 *ch, u8 *attr)
{
    *ch = ' ';
    *attr = 0x07;
    if (x < 0 || x > 79 || y < 0 || y > 24) return;
    *ch = emu_rb(e, VRAM_SEG, (u16)((y * 80 + x) * 2));
    *attr = emu_rb(e, VRAM_SEG, (u16)((y * 80 + x) * 2 + 1));
}

static void vram_fill(Emu *e, int x0, int y0, int x1, int y1, u8 ch, u8 attr)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) vram_put(e, x, y, ch, attr);
}

static void clear_screen(Emu *e) { vram_fill(e, 0, 0, 79, 24, ' ', 0x07); }

/* AL lines of a window (CH,CL)=upper-left .. (DH,DL)=lower-right; AL=0 blanks
 * the whole window.  up=1 scrolls towards the top (AH=06), up=0 towards the
 * bottom (AH=07). */
static void video_scroll(Emu *e, int up, int lines, int x0, int y0, int x1, int y1, u8 attr)
{
    if (x0 < 0) x0 = 0;
    if (x1 > 79) x1 = 79;
    if (y0 < 0) y0 = 0;
    if (y1 > 24) y1 = 24;
    int h = y1 - y0 + 1;
    if (h <= 0 || x1 < x0) return;
    if (lines <= 0 || lines > h) lines = h;
    for (int y = y0; y <= y1; y++) {
        int sy = up ? y + lines : y - lines;
        if (sy >= y0 && sy <= y1) {
            for (int x = x0; x <= x1; x++) {
                u8 ch, at;
                vram_get(e, x, sy, &ch, &at);
                vram_put(e, x, y, ch, at);
            }
        } else {
            vram_fill(e, x0, y, x1, y, ' ', attr);
        }
    }
}

/* teletype output shared by AH=0Eh and AH=13h: write, advance, scroll */
static void tele_put(Emu *e, u8 c, u8 attr)
{
    int x = e->curx, y = e->cury;
    if (c == 0x0D) {
        x = 0;
    } else if (c == 0x0A) {
        y++;
    } else if (c == 0x08) {
        if (x > 0) x--;
    } else if (c == 0x07) {
        /* bell: no sound */
    } else {
        vram_put(e, x, y, c, attr);
        x++;
        if (x > 79) { x = 0; y++; }
    }
    if (y > 24) {
        video_scroll(e, 1, 1, 0, 0, 79, 24, 0x07);
        y = 24;
    }
    e->curx = x;
    e->cury = y;
    cur_sync(e);
}

/* INT 21h console output goes to the host console stream.  It deliberately does
 * NOT paint emulated VRAM: the mirror refreshes whole rows, so mirroring every
 * console character there flooded the screen and the serial log with
 * full-screen refreshes.  Programs that own the screen paint it via INT 10h. */
static void con_put(Emu *e, char c)
{
    (void)e;
    if (c == '\r') { g_pending_cr = 1; return; }
    if (c == '\n') { g_pending_cr = 0; plat_putc('\n'); return; }
    if (g_pending_cr) { plat_putc('\r'); g_pending_cr = 0; }
    plat_putc(c);
}

void int10_handler(Emu *e)
{
    trace_call(e, 0x10);
    u8 ah = (u8)(e->ax >> 8);
    switch (ah) {
    case 0x00:
        e->video_mode = (u16)(e->ax & 0xFF);
        e->curx = 0; e->cury = 0;
        cur_sync(e);
        if ((e->ax & 0xFF) == 3 || (e->ax & 0xFF) == 7 || (e->ax & 0xFF) == 0)
            clear_screen(e);
        break;
    case 0x01: /* set cursor shape: ignore */
        break;
    case 0x02:
        e->curx = (int)(e->dx & 0xFF);
        e->cury = (int)((e->dx >> 8) & 0xFF);
        if (e->curx > 79) e->curx = 79;
        if (e->cury > 24) e->cury = 24;
        cur_sync(e);
        break;
    case 0x03:
        e->dx = (u16)((e->cury << 8) | e->curx);
        e->cx = (u16)(0x0607);             /* cursor shape: a normal underline */
        break;
    case 0x05:
        break;
    case 0x06: case 0x07:
        /* scroll window CH,CL .. DH,DL by AL lines (AH=06 up, AH=07 down) */
        video_scroll(e, ah == 0x06, (int)(e->ax & 0xFF),
                     (int)(e->cx & 0xFF), (int)((e->cx >> 8) & 0xFF),
                     (int)(e->dx & 0xFF), (int)((e->dx >> 8) & 0xFF),
                     (u8)(e->bx & 0xFF));
        break;
    case 0x08: { /* read character and attribute at the cursor */
        u8 ch = ' ', at = 0x07;
        vram_get(e, e->curx, e->cury, &ch, &at);
        e->ax = (u16)((at << 8) | ch);
        break;
    }
    case 0x09: case 0x0A: {
        /* write character (AH=09 also writes BL into the attribute byte),
         * CX times from the cursor; the cursor itself does not move */
        int n = e->cx ? (int)e->cx : 1;
        u8 ch = (u8)(e->ax & 0xFF);
        u8 attr = (u8)(e->bx & 0xFF);
        int x = e->curx, y = e->cury;
        for (int i = 0; i < n; i++) {
            if (x > 79) { x = 0; y++; }
            if (y > 24) break;
            if (ah == 0x09) {
                vram_put(e, x, y, ch, attr);
            } else {
                u8 oldch, oldat;
                vram_get(e, x, y, &oldch, &oldat);
                vram_put(e, x, y, ch, oldat);
            }
            x++;
        }
        break;
    }
    case 0x0E: { /* teletype output: writes, advances, scrolls at the bottom */
        tele_put(e, (u8)(e->ax & 0xFF), (u8)(e->bx & 0xFF));
        break;
    }
    case 0x0F:
        e->ax = (u16)((80 << 8) | (u8)e->video_mode);
        e->bx = (u16)(e->bx & 0xFF00);
        break;
    case 0x12:
        e->ax = (u16)((e->ax & 0xFF00) | 0x12);
        e->bx = 0;
        break;
    case 0x13: {
        int n = e->cx;
        u16 seg = e->es, off = e->bp;
        for (int i = 0; i < n; i++) {
            u8 c = emu_rb(e, seg, (u16)(off + i));
            tele_put(e, c, (u8)(e->bx & 0xFF));
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* INT 16h keyboard                                                    */
/* ------------------------------------------------------------------ */
void int16_handler(Emu *e)
{
    trace_call(e, 0x16);
    u8 ah = (u8)(e->ax >> 8);
    switch (ah) {
    case 0x00: case 0x10: {
        int scan = 0, asc = 0;
        /* A key peeked by AH=01 is already off the BIOS buffer: consume that
         * one first, otherwise peek-then-read loses it and returns the *next*
         * keystroke (menus jumped two entries). */
        if (g_pending_key >= 0) {
            scan = g_pending_scan;
            asc = g_pending_asc;
            g_pending_key = -1;
            g_pending_scan = g_pending_asc = -1;
        } else {
            emu_kbd_get_full(e, &scan, &asc);
        }
        e->ax = (u16)(((scan & 0xFF) << 8) | (asc & 0xFF));
        break;
    }
    case 0x01: case 0x11: {
        if (check_key(e)) {
            int scan = g_pending_scan, asc = g_pending_asc;
            g_pending_key = -1;
            g_pending_scan = g_pending_asc = -1;
            e->ax = (u16)(((scan & 0xFF) << 8) | (asc & 0xFF));
            e->flags &= ~F_ZF;
        } else {
            e->ax = 0;
            e->flags |= F_ZF;
        }
        break;
    }
    case 0x02: case 0x12:
        e->ax = (u16)(e->ax & 0xFF00);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* INT 13h -- ROM BIOS disk services (CHS geometry 16 heads / 63 spt)  */
/* ------------------------------------------------------------------ */
#define INT13_HEADS 16
#define INT13_SPT   63

/* ------------------------------------------------------------------ */
/* INT 1Ah timer services                                              */
/* ------------------------------------------------------------------ */
/* Programs read the BIOS tick counter through INT 1Ah (and directly at
 * 0040:006C); games use it for timing and to seed their RNG, so returning
 * nothing made their counters show garbage. */
static u16 bcd(u16 v) { return (u16)(((v / 10) << 4) | (v % 10)); }

void int1a_handler(Emu *e)
{
    u8 ah = (u8)(e->ax >> 8);
    switch (ah) {
    case 0x00: {   /* read tick count -> CX:DX, AL=midnight flag */
        u32 ticks = (u32)(plat_ticks_ms() / 55u);
        emu_wd(e, 0x40, 0x6C, ticks);
        e->cx = (u16)(ticks >> 16);
        e->dx = (u16)ticks;
        e->ax = (u16)(e->ax & 0xFF00);
        break;
    }
    case 0x01:     /* set tick count from CX:DX */
        emu_wd(e, 0x40, 0x6C, ((u32)e->cx << 16) | e->dx);
        break;
    case 0x02: {   /* read real-time clock time in BCD -> CH:CL:DH */
        int h, mi, s;
        plat_get_time(&h, &mi, &s);
        e->cx = (u16)((bcd((u16)(h & 0xFF)) << 8) | bcd((u16)(mi & 0xFF)));
        e->dx = (u16)(bcd((u16)(s & 0xFF)) << 8);
        e->flags = (u16)(e->flags & ~F_CF);
        break;
    }
    case 0x03:     /* set RTC time: ignored (we do not write the host clock) */
        break;
    case 0x04: {   /* read RTC date in BCD -> CH=century CL=year DH=mon DL=day */
        int y, m, d;
        plat_get_date(&y, &m, &d);
        e->cx = (u16)((bcd((u16)(y / 100)) << 8) | bcd((u16)(y % 100)));
        e->dx = (u16)((bcd((u16)(m & 0xFF)) << 8) | bcd((u16)(d & 0xFF)));
        e->flags = (u16)(e->flags & ~F_CF);
        break;
    }
    default:
        break;
    }
}
void int13_handler(Emu *e)
{
    trace_call(e, 0x13);
    u8 ah = (u8)(e->ax >> 8);
    u8 dl = (u8)(e->dx & 0xFF);
    blockdev *bd = NULL;
    if (dl == 0x80 && g_dos.drives[2].mounted) bd = g_dos.drives[2].vol.dev;
    else if (dl == 0x00 && g_dos.drives[0].mounted) bd = &g_dos.drives[0].bd;

    switch (ah) {
    case 0x00:                                  /* reset disk */
        set_ok(e);
        e->ax = (u16)(e->ax & 0xFF00);
        return;
    case 0x08: {                                /* get drive parameters */
        set_ok(e);
        u32 cyls = 1;
        if (bd) {
            cyls = (u32)(bd->nblocks / ((u64)INT13_HEADS * INT13_SPT));
            if (cyls == 0) cyls = 1;
            if (cyls > 1024) cyls = 1024;
        }
        e->ax = 0;
        e->cx = (u16)(((cyls & 0xFF) << 8) | (((cyls >> 8) & 0x3) << 6) | INT13_SPT);
        e->dx = (u16)((dl << 8) | (INT13_HEADS - 1));
        return;
    }
    case 0x02: case 0x03: {                     /* read / write sectors */
        u16 cyl = (u16)(((e->cx >> 8) & 0xFF) | ((e->cx & 0xC0) << 2));
        u16 sec = (u16)(e->cx & 0x3F);
        u16 head = (u16)(e->dx >> 8);
        u8 count = (u8)(e->ax & 0xFF);
        if (!bd || sec == 0 || count == 0) {
            e->flags |= F_CF;
            e->ax = (u16)((e->ax & 0xFF00) | 0x01);
            return;
        }
        u64 lba = ((u64)cyl * INT13_HEADS + head) * INT13_SPT + (sec - 1);
        u8 buf[512];
        u32 es = (u32)e->es << 4;
        for (u16 i = 0; i < count; i++) {
            if (lba + i >= bd->nblocks) {
                e->flags |= F_CF;
                e->ax = (u16)((e->ax & 0xFF00) | 0x01);
                return;
            }
            if (ah == 0x02) {
                if (bd->read(bd, lba + i, buf, 1)) {
                    e->flags |= F_CF;
                    e->ax = (u16)((e->ax & 0xFF00) | 0x0A);
                    return;
                }
                for (int b = 0; b < 512; b++) {
                    u32 a = (es + (u32)e->bx + (u32)i * 512u + (u32)b) & 0xFFFFFu;
                    e->mem[a] = buf[b];
                }
            } else {
                if (!bd->write) {
                    e->flags |= F_CF;
                    e->ax = (u16)((e->ax & 0xFF00) | 0x0A);
                    return;
                }
                for (int b = 0; b < 512; b++) {
                    u32 a = (es + (u32)e->bx + (u32)i * 512u + (u32)b) & 0xFFFFFu;
                    buf[b] = e->mem[a];
                }
                if (bd->write(bd, lba + i, buf, 1)) {
                    e->flags |= F_CF;
                    e->ax = (u16)((e->ax & 0xFF00) | 0x0A);
                    return;
                }
            }
        }
        set_ok(e);
        e->ax = (u16)(e->ax & 0xFF00);
        return;
    }
    case 0x15:                                  /* get disk type */
        set_ok(e);
        e->ax = (u16)(bd ? 0x0300 : 0x0000);
        return;
    default:
        set_ok(e);
        e->ax = (u16)(e->ax & 0xFF00);
        return;
    }
}
