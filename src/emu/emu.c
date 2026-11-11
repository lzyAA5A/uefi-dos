/*
 * UEFI-DOS -- 8086 real-mode emulator.
 *
 * A compact interpreter for the 8086/80186 instruction set with the pieces
 * of 386 real mode that 16-bit DOS programs actually rely on.  The CPU sees
 * 1 MiB of RAM, a populated BIOS data area, an IVT where INT 21h/10h/16h are
 * wired to UEFI-DOS kernel services and everything else points at an IRET
 * stub (so program-installed interrupt handlers behave like real hardware).
 */
#include "uefidos.h"
#include "emu/emu.h"
#include "kernel/dos.h"
#include "plat.h"

#ifdef UEFIDOS_HOST
#include <stdlib.h>
#include <stdio.h>
#endif

#define MEM_MASK (EMU_RAM_SIZE - 1u)

static int hooked_int(int n);

/* ------------------------------------------------------------------ */
/* I/O ports                                                           */
/* ------------------------------------------------------------------ */
/* Text-mode DOS programs poll the video status register and poke CRTC /
 * CGA registers; a retrace bit that never changes hangs them forever, so
 * the status port alternates.  Everything else reads back as 0. */
static u16 g_crtc_idx = 0;
static u8  g_crtc_reg[32];
static unsigned long g_io_ticks = 0;

u16 emu_in_port(Emu *e, u16 port)
{
    g_io_ticks++;
    switch (port) {
    case 0x3DA:     /* VGA/EGA input status 1: alternate retrace bits */
        return (u16)((g_io_ticks & 1) ? 0x09 : 0x00);
    case 0x3D4: return g_crtc_idx;
    case 0x3D5: return g_crtc_reg[g_crtc_idx & 31];
    case 0x3C8: return e->pal_index;
    case 0x3C9: {
        u16 idx = (u16)(e->pal_read_index + e->pal_read_phase / 3);
        u8 v = e->dac[(idx & 0xFF) * 3 + (e->pal_read_phase % 3)];
        e->pal_read_phase = (u8)((e->pal_read_phase + 1) % 768);
        return v;
    }
    case 0x61: return 0x20;     /* timer gate / speaker off */
    default: return 0;
    }
}

void emu_out_port(Emu *e, u16 port, u16 val)
{
    switch (port) {
    case 0x3D4: g_crtc_idx = (u16)(val & 0x1F); break;
    case 0x3D5: g_crtc_reg[g_crtc_idx & 31] = (u8)val; break;
    case 0x3C7: e->pal_read_index = (u16)(val & 0xFF); e->pal_read_phase = 0; break;
    case 0x3C8: e->pal_index = (u16)(val & 0xFF); e->pal_phase = 0; break;
    case 0x3C9:
        e->dac[(e->pal_index & 0xFF) * 3 + (e->pal_phase % 3)] = (u8)(val & 0x3F);
        e->pal_phase = (u8)((e->pal_phase + 1) % 3);
        if (e->pal_phase == 0) e->pal_index = (u16)((e->pal_index + 1) & 0xFF);
        break;
    default: break;
    }
}

#ifdef UEFIDOS_HOST
static FILE *g_io_trace;
static int g_io_trace_on = -1;
static unsigned long g_io_n;
static void trace_io(u16 port, int is_in, u16 val)
{
    if (g_io_trace_on < 0) {
        g_io_trace_on = getenv("UEFIDOS_TRACE_PORTS") ? 1 : 0;
        if (g_io_trace_on) g_io_trace = fopen("build/port_trace.txt", "w");
    }
    if (!g_io_trace_on || !g_io_trace) return;
    g_io_n++;
    if (g_io_n <= 200 || (g_io_n % 500) == 0) {
        fprintf(g_io_trace, "%7lu %s %04X = %04X\n", g_io_n, is_in ? "IN " : "OUT", port, val);
        fflush(g_io_trace);
    }
}
#else
#define trace_io(port, is_in, val) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* BIOS keyboard buffer (0040:001A head, 0040:001C tail, 0040:001E data) */
/* ------------------------------------------------------------------ */
/* Programs compiled against real DOS libraries (Digital Mars, Turbo C)
 * test for a key by looking at these BIOS variables instead of calling
 * INT 16h, so the emulator has to fill in the buffer like the BIOS does. */
#define KBD_SEG     0x40
#define KBD_HEAD    0x1A
#define KBD_TAIL    0x1C
#define KBD_BUF     0x1E
#define KBD_BUF_LEN 30      /* 15 two-byte entries */

static int kbd_scan_of(int c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    switch (c) {
    case 0x1B: return 0x01;
    case '1': return 0x02; case '2': return 0x03; case '3': return 0x04;
    case '4': return 0x05; case '5': return 0x06; case '6': return 0x07;
    case '7': return 0x08; case '8': return 0x09; case '9': return 0x0A;
    case '0': return 0x0B; case '-': return 0x0C; case '=': return 0x0D;
    case 8:   return 0x0E; case 9:   return 0x0F;
    case 'Q': return 0x10; case 'W': return 0x11; case 'E': return 0x12;
    case 'R': return 0x13; case 'T': return 0x14; case 'Y': return 0x15;
    case 'U': return 0x16; case 'I': return 0x17; case 'O': return 0x18;
    case 'P': return 0x19; case '[': return 0x1A; case ']': return 0x1B;
    case 13:  return 0x1C;
    case 'A': return 0x1E; case 'S': return 0x1F; case 'D': return 0x20;
    case 'F': return 0x21; case 'G': return 0x22; case 'H': return 0x23;
    case 'J': return 0x24; case 'K': return 0x25; case 'L': return 0x26;
    case ';': return 0x27; case '\'': return 0x28; case '`': return 0x29;
    case '\\': return 0x2B;
    case 'Z': return 0x2C; case 'X': return 0x2D; case 'C': return 0x2E;
    case 'V': return 0x2F; case 'B': return 0x30; case 'N': return 0x31;
    case 'M': return 0x32; case ',': return 0x33; case '.': return 0x34;
    case '/': return 0x35; case ' ': return 0x39;
    default: return 0;
    }
}

static int kbd_count(Emu *e)
{
    int head = emu_rw(e, KBD_SEG, KBD_HEAD) - KBD_BUF;
    int tail = emu_rw(e, KBD_SEG, KBD_TAIL) - KBD_BUF;
    if (head < 0 || head > KBD_BUF_LEN - 2) head = 0;
    if (tail < 0 || tail > KBD_BUF_LEN - 2) tail = 0;
    return ((tail - head + KBD_BUF_LEN) % KBD_BUF_LEN) / 2;
}

static u32 g_kbd_seq = 0;   /* bumped on every key delivery */

static void kbd_push(Emu *e, int scan, int ascii)
{
    int tail = emu_rw(e, KBD_SEG, KBD_TAIL) - KBD_BUF;
    if (tail < 0 || tail > KBD_BUF_LEN - 2) tail = 0;
    emu_wb(e, KBD_SEG, (u16)(KBD_BUF + tail), (u8)scan);
    emu_wb(e, KBD_SEG, (u16)(KBD_BUF + tail + 1), (u8)ascii);
    tail = (tail + 2) % KBD_BUF_LEN;
    emu_ww(e, KBD_SEG, KBD_TAIL, (u16)(KBD_BUF + tail));
    g_kbd_seq++;
}

static int kbd_pop(Emu *e, int *scan, int *ascii)
{
    int head = emu_rw(e, KBD_SEG, KBD_HEAD) - KBD_BUF;
    int tail = emu_rw(e, KBD_SEG, KBD_TAIL) - KBD_BUF;
    if (head < 0 || head > KBD_BUF_LEN - 2) head = 0;
    if (tail < 0 || tail > KBD_BUF_LEN - 2) tail = 0;
    if (head == tail) return 0;
    *scan = emu_rb(e, KBD_SEG, (u16)(KBD_BUF + head));
    *ascii = emu_rb(e, KBD_SEG, (u16)(KBD_BUF + head + 1));
    head = (head + 2) % KBD_BUF_LEN;
    emu_ww(e, KBD_SEG, KBD_HEAD, (u16)(KBD_BUF + head));
    return 1;
}

void emu_kbd_pump(Emu *e)
{
    int k, scan, asc;
    if (kbd_count(e) >= 15) return;
    k = plat_kbd_poll();
    if (k < 0) return;
    if (k & 0x100) { scan = k & 0xFF; asc = 0; }
    else { asc = k & 0xFF; scan = kbd_scan_of(asc); }
    kbd_push(e, scan, asc);
}

int emu_kbd_peek(Emu *e)
{
    if (kbd_count(e) == 0) emu_kbd_pump(e);
    return kbd_count(e) > 0;
}

int emu_kbd_get_full(Emu *e, int *scan, int *ascii)
{
    long guard = 0;
#ifdef UEFIDOS_HOST
    u32 start = plat_ticks_ms();    /* automated tests must not block forever */
#endif
    /* repaint before blocking: a program that has just finished drawing and
     * then waits for a key would otherwise leave a half-painted frame on
     * screen (the flush used to happen only from do_int / the step loop) */
    emu_video_flush(e);
    while (!kbd_pop(e, scan, ascii)) {
        emu_kbd_pump(e);
#ifdef UEFIDOS_HOST
        if (plat_ticks_ms() - start > 3000) { *scan = 0x01; *ascii = 27; return 1; }
#else
        if (++guard > 120000) { *scan = 0x01; *ascii = 27; return 1; }   /* safety net */
#endif
        plat_sleep_ms(2);
    }
    (void)guard;
    return 1;
}

/* ------------------------------------------------------------------ */
/* memory helpers                                                      */
/* ------------------------------------------------------------------ */
/* emulated 80x25 colour text screen lives at 0xB8000; direct writes   */
/* mark it dirty so the host console can be refreshed.                 */
static int g_video_dirty = 0;

#ifdef UEFIDOS_HOST
static void trace_vw(Emu *e, u32 rel, u8 v);
#else
#define trace_vw(e, rel, v) ((void)0)
#endif

/* ---- descriptor / segment resolution (386 phase) ------------------ */
static void parse_desc(const u8 *d, u32 *base, u32 *limit, u16 *attr)
{
    u32 l = (u32)d[0] | ((u32)d[1] << 8) | ((u32)(d[6] & 0x0F) << 16);
    u32 b = (u32)d[2] | ((u32)d[3] << 8) | ((u32)d[4] << 16) | ((u32)d[5] << 24);
    if (d[6] & 0x80) l = (l << 12) | 0xFFFu;          /* G: 4 KiB granularity */
    *base = b;
    *limit = l;
    *attr = (u16)((u16)d[5] | ((u16)d[6] << 8));
}

static int read_desc(Emu *e, u16 sel, u32 *base, u32 *limit, u16 *attr);

static int read_desc(Emu *e, u16 sel, u32 *base, u32 *limit, u16 *attr)
{
    u32 tbl = (sel & 4) ? DPMI_LDT_LIN : e->gdt_base;
    u32 idx = (u32)(sel >> 3) * 8u;
    u32 addr = (tbl + idx) & MEM_MASK;
    u8 d[8];
    int i;
    if (sel == 0) return 0;
    for (i = 0; i < 8; i++) d[i] = e->mem[(addr + (u32)i) & MEM_MASK];
    parse_desc(d, base, limit, attr);
    return 1;
}

/* Resolve a selector to its linear base (and limit).  Real mode is the
 * classic sel<<4; protected mode walks the descriptor table, with the segment
 * cache used only as a memo keyed on the selector value so a stale entry can
 * never produce a wrong base. */
static u32 seg_resolve(Emu *e, u16 sel, u32 *limit)
{
    int i;
    u32 b, l = 0xFFFFu;
    u16 a = 0;
    if (!(e->cr0 & CR0_PE)) {
        if (limit) *limit = 0xFFFFu;
        return (u32)sel << 4;
    }
    if (sel == 0) {
        if (limit) *limit = e->ram_size - 1u;
        return 0;
    }
    for (i = 0; i < 6; i++) {
        if (e->seg[i].valid && e->seg[i].sel == sel) {
            if (limit) *limit = e->seg[i].limit;
            return e->seg[i].base;
        }
    }
    if (read_desc(e, sel, &b, &l, &a)) {
        if (limit) *limit = l;
        return b;
    }
    if (limit) *limit = 0xFFFFu;
    return (u32)sel << 4;
}

/* Linear address of seg:off for an access of n bytes; enforces the segment
 * limit in protected mode (a violation latches e->pm_fault). */
static u32 lin_of(Emu *e, u16 sel, u32 off, u32 n)
{
    u32 limit = 0xFFFFu;
    u32 base = seg_resolve(e, sel, &limit);
    if ((e->cr0 & CR0_PE) && ((u64)off + n - 1u) > (u64)limit) {
        e->pm_fault = 1;
        e->pm_fault_addr = base + off;
        return 0;
    }
    return (base + off) & MEM_MASK;
}

u8 emu_rb(Emu *e, u16 seg, u16 off)
{
    return e->mem[lin_of(e, seg, off, 1)];
}

/* 32-bit-offset variants used by 386 code paths */
u8 emu_rb32(Emu *e, u16 seg, u32 off)
{
    return e->mem[lin_of(e, seg, off, 1)];
}
void emu_wb32(Emu *e, u16 seg, u32 off, u8 v)
{
    u32 a = lin_of(e, seg, off, 1);
    if (e->pm_fault) return;
    e->mem[a] = v;
    if (e->gfx_w && a >= 0xA0000u && a < 0xA0000u + (u32)e->gfx_w * e->gfx_h) {
        g_video_dirty = 1;
    }
    if (a >= 0xB8000u && a < 0xB8000u + 80u * 25u * 2u) {
        if (!e->gfx_w) g_video_dirty = 1;
        trace_vw(e, a - 0xB8000u, v);
    }
}
u16 emu_rw32(Emu *e, u16 seg, u32 off)
{
    return (u16)(emu_rb32(e, seg, off) | ((u16)emu_rb32(e, seg, off + 1) << 8));
}
void emu_ww32(Emu *e, u16 seg, u32 off, u16 v)
{
    emu_wb32(e, seg, off, (u8)v);
    emu_wb32(e, seg, off + 1, (u8)(v >> 8));
}
u32 emu_rd32(Emu *e, u16 seg, u32 off)
{
    return (u32)emu_rw32(e, seg, off) | ((u32)emu_rw32(e, seg, off + 2) << 16);
}
void emu_wd32(Emu *e, u16 seg, u32 off, u32 v)
{
    emu_ww32(e, seg, off, (u16)v);
    emu_ww32(e, seg, off + 2, (u16)(v >> 16));
}
#ifdef UEFIDOS_HOST
static void trace_vw(Emu *e, u32 rel, u8 v)
{
    static FILE *f = NULL;
    static long left = -1;
    if (left < 0) {
        const char *s = getenv("UEFIDOS_TRACE_VRAM");
        left = s ? atol(s) : 0;
        if (left) f = fopen("build/vram_write.txt", "w");
    }
    if (!f || left <= 0) return;
    left--;
    fprintf(f, "off=%05X <- %02X '%c'  at %04X:%04X bytes %02X %02X %02X %02X  AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X ES=%04X\n",
            rel, v, (v >= 32 && v < 127) ? v : '.', e->cs, e->ip,
            emu_rb(e, e->cs, (u16)(e->ip - 1)), emu_rb(e, e->cs, e->ip),
            emu_rb(e, e->cs, (u16)(e->ip + 1)), emu_rb(e, e->cs, (u16)(e->ip + 2)),
            e->ax, e->bx, e->cx, e->dx, e->si, e->di, e->es);
    fflush(f);
}
#else
#define trace_vw(e, rel, v) ((void)0)
#endif

void emu_wb(Emu *e, u16 seg, u16 off, u8 v)
{
    u32 a = lin_of(e, seg, off, 1);
    if (e->pm_fault) return;
    e->mem[a] = v;
    if (e->gfx_w && a >= 0xA0000u && a < 0xA0000u + (u32)e->gfx_w * e->gfx_h) {
        g_video_dirty = 1;
    }
    if (a >= 0xB8000u && a < 0xB8000u + 80u * 25u * 2u) {
        if (!e->gfx_w) g_video_dirty = 1;
        trace_vw(e, a - 0xB8000u, v);
    }
}
u16 emu_rw(Emu *e, u16 seg, u16 off)
{
    return (u16)(emu_rb(e, seg, off) | ((u16)emu_rb(e, seg, (u16)(off + 1)) << 8));
}
void emu_ww(Emu *e, u16 seg, u16 off, u16 v)
{
    emu_wb(e, seg, off, (u8)v);
    emu_wb(e, seg, (u16)(off + 1), (u8)(v >> 8));
}
u32 emu_rd(Emu *e, u16 seg, u16 off)
{
    return (u32)emu_rw(e, seg, off) | ((u32)emu_rw(e, seg, (u16)(off + 2)) << 16);
}
void emu_wd(Emu *e, u16 seg, u16 off, u32 v)
{
    emu_ww(e, seg, off, (u16)v);
    emu_ww(e, seg, (u16)(off + 2), (u16)(v >> 16));
}

/* The guest screen lives in emulated VRAM, so a program that paints with
 * INT 10h already updates it.  Console text streams through INT 21h can dirty
 * it hundreds of times per second, and every flush rewrites 25 rows, so the
 * mirror is throttled; the force flush at the end of emu_run catches up. */
static u32 g_last_flush_ms = 0;

void emu_video_flush(Emu *e)
{
    if (e->gfx_w) {
        /* graphics: the whole A0000 window is the framebuffer */
        plat_video_gfx_show(e->mem + 0xA0000, (int)e->gfx_w, (int)e->gfx_h, e->dac);
        return;
    }
    plat_video_show(e->mem + 0xB8000);
}

static void video_flush(Emu *e)
{
    if (!g_video_dirty) return;
    u32 now = plat_ticks_ms();
    if (g_last_flush_ms && (now - g_last_flush_ms) < 30) return;
    g_last_flush_ms = now;
    g_video_dirty = 0;
    emu_video_flush(e);
}

static void video_flush_force(Emu *e)
{
    if (!g_video_dirty) return;
    g_last_flush_ms = plat_ticks_ms();
    g_video_dirty = 0;
    emu_video_flush(e);
}

/* ------------------------------------------------------------------ */
/* graphics modes                                                      */
/* ------------------------------------------------------------------ */

/* the DAC starts as a plain 16-colour CGA set followed by a grey ramp: mode 13h
 * programs are expected to load their own palette (INT 10h AX=1012h or the
 * 3C8h/3C9h ports), which every game and paint program does */
static const u8 g_vga_default[16][3] = {
    { 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 },
    { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
    { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 }
};

void emu_dac_reset(Emu *e)
{
    memset(e->dac, 0, sizeof(e->dac));
    for (int i = 0; i < 16; i++) {
        e->dac[i * 3 + 0] = g_vga_default[i][0];
        e->dac[i * 3 + 1] = g_vga_default[i][1];
        e->dac[i * 3 + 2] = g_vga_default[i][2];
    }
    for (int i = 16; i < 256; i++) {
        u8 v = (u8)((i - 16) * 63 / 239);
        e->dac[i * 3 + 0] = v;
        e->dac[i * 3 + 1] = v;
        e->dac[i * 3 + 2] = v;
    }
}

void emu_dac_read(Emu *e, u8 index, u8 out[3])
{
    out[0] = e->dac[index * 3 + 0];
    out[1] = e->dac[index * 3 + 1];
    out[2] = e->dac[index * 3 + 2];
}

void emu_dac_write(Emu *e, u8 index, u8 r, u8 g, u8 b)
{
    e->dac[index * 3 + 0] = (u8)(r & 0x3F);
    e->dac[index * 3 + 1] = (u8)(g & 0x3F);
    e->dac[index * 3 + 2] = (u8)(b & 0x3F);
}

/* geometry of the modes we emulate; 0 means "not available" */
int emu_gfx_geometry(u16 mode, int *w, int *h)
{
    switch (mode) {
    case 0x13:  *w = 320; *h = 200; return 1;   /* VGA mode 13h */
    case 0x100: *w = 640; *h = 400; return 1;   /* VESA 640x400x8 */
    case 0x101: *w = 640; *h = 480; return 1;   /* VESA 640x480x8 */
    case 0x103: *w = 800; *h = 600; return 1;   /* VESA 800x600x8 */
    default: return 0;
    }
}

/* program-visible mode set (INT 10h AH=00); mode 3/7/0 fall back to text */
void emu_set_video_mode(Emu *e, u16 mode)
{
    int w = 0, h = 0;
    e->video_mode = mode;
    e->curx = 0;
    e->cury = 0;
    if (emu_gfx_geometry(mode, &w, &h) && plat_video_gfx_begin(w, h) == 0) {
        e->gfx_w = (u16)w;
        e->gfx_h = (u16)h;
        if (e->video_mode != 0x13) emu_dac_reset(e);
        memset(e->mem + 0xA0000, 0, (u32)w * (u32)h);
        e->mem[0x450] = 0;
        e->mem[0x451] = 0;
        g_video_dirty = 1;
        video_flush_force(e);
        return;
    }
    /* text mode: reset the hardware cursor and blank the 80x25 buffer */
    e->gfx_w = 0;
    e->gfx_h = 0;
    plat_video_text();
    e->mem[0x450] = 0;                    /* 0040:0050 = cursor position */
    e->mem[0x451] = 0;
    for (int i = 0; i < 80 * 25; i++) {
        e->mem[0xB8000 + i * 2] = ' ';
        e->mem[0xB8000 + i * 2 + 1] = 0x07;
    }
    g_video_dirty = 1;
    video_flush_force(e);
}

/* ------------------------------------------------------------------ */
/* registers                                                           */
/* ------------------------------------------------------------------ */
static u8 r8(Emu *e, int i)
{
    switch (i) {
    case 0: return (u8)(e->ax & 0xFF);
    case 1: return (u8)(e->cx & 0xFF);
    case 2: return (u8)(e->dx & 0xFF);
    case 3: return (u8)(e->bx & 0xFF);
    case 4: return (u8)(e->ax >> 8);
    case 5: return (u8)(e->cx >> 8);
    case 6: return (u8)(e->dx >> 8);
    default:return (u8)(e->bx >> 8);
    }
}
static void w8(Emu *e, int i, u8 v)
{
    switch (i) {
    case 0: e->ax = (u16)((e->ax & 0xFF00) | v); break;
    case 1: e->cx = (u16)((e->cx & 0xFF00) | v); break;
    case 2: e->dx = (u16)((e->dx & 0xFF00) | v); break;
    case 3: e->bx = (u16)((e->bx & 0xFF00) | v); break;
    case 4: e->ax = (u16)((e->ax & 0x00FF) | ((u16)v << 8)); break;
    case 5: e->cx = (u16)((e->cx & 0x00FF) | ((u16)v << 8)); break;
    case 6: e->dx = (u16)((e->dx & 0x00FF) | ((u16)v << 8)); break;
    default:e->bx = (u16)((e->bx & 0x00FF) | ((u16)v << 8)); break;
    }
}
static u16 r16(Emu *e, int i)
{
    switch (i) {
    case 0: return e->ax;
    case 1: return e->cx;
    case 2: return e->dx;
    case 3: return e->bx;
    case 4: return e->sp;
    case 5: return e->bp;
    case 6: return e->si;
    default:return e->di;
    }
}
static void w16(Emu *e, int i, u16 v)
{
    switch (i) {
    case 0: e->ax = v; break;
    case 1: e->cx = v; break;
    case 2: e->dx = v; break;
    case 3: e->bx = v; break;
    case 4: e->sp = v; break;
    case 5: e->bp = v; break;
    case 6: e->si = v; break;
    default:e->di = v; break;
    }
}
static u32 r32(Emu *e, int i)
{
    switch (i) {
    case 0: return e->eax;
    case 1: return e->ecx;
    case 2: return e->edx;
    case 3: return e->ebx;
    case 4: return e->esp;
    case 5: return e->ebp;
    case 6: return e->esi;
    default:return e->edi;
    }
}
static void w32(Emu *e, int i, u32 v)
{
    switch (i) {
    case 0: e->eax = v; break;
    case 1: e->ecx = v; break;
    case 2: e->edx = v; break;
    case 3: e->ebx = v; break;
    case 4: e->esp = v; break;
    case 5: e->ebp = v; break;
    case 6: e->esi = v; break;
    default:e->edi = v; break;
    }
}
/* Operand-width generic accessors: width is 8, 16 or 32. */
static u32 rv(Emu *e, int i, int width)
{
    if (width == 8) return r8(e, i);
    if (width == 32) return r32(e, i);
    return r16(e, i);
}
static void wv(Emu *e, int i, u32 v, int width)
{
    if (width == 8) { w8(e, i, (u8)v); return; }
    if (width == 32) { w32(e, i, v); return; }
    w16(e, i, (u16)v);
}

static u16 seg_reg(Emu *e, int i)
{
    switch (i) {
    case 0: return e->es;
    case 1: return e->cs;
    case 2: return e->ss;
    case 3: return e->ds;
    case 4: return e->fs;
    default:return e->gs;
    }
}

/* Refresh one hidden segment cache entry from the descriptor tables (protected
 * mode) or from the sel<<4 rule (real mode). */
static void seg_cache_load(Emu *e, int idx, u16 sel)
{
    u32 base = (u32)sel << 4, limit = 0xFFFFu;
    u16 attr = 0x0092;                  /* real-mode: present, writable data */
    if (e->cr0 & CR0_PE) {
        u16 dattr = 0;
        if (sel == 0) {
            base = 0; limit = e->ram_size - 1u; attr = 0;
        } else if (!read_desc(e, sel, &base, &limit, &dattr)) {
            base = (u32)sel << 4; limit = 0xFFFFu; attr = 0x0092;
        } else {
            attr = dattr;
        }
    }
    e->seg[idx].sel = sel;
    e->seg[idx].base = base;
    e->seg[idx].limit = limit;
    e->seg[idx].attr = attr;
    e->seg[idx].valid = 1;
}

static void set_seg_reg(Emu *e, int i, u16 v)
{
    switch (i) {
    case 0: e->es = v; break;
    case 1: e->cs = v; break;
    case 2: e->ss = v; break;
    case 3: e->ds = v; break;
    case 4: e->fs = v; break;
    default:e->gs = v; break;
    }
    seg_cache_load(e, i, v);
}

/* reload every cache entry from the visible selectors (used after a mode
 * switch or a direct register poke by the kernel) */
static void seg_cache_sync(Emu *e)
{
    int i;
    for (i = 0; i < 6; i++) seg_cache_load(e, i, seg_reg(e, i));
}

/* D bit of the current code segment: selects 16- or 32-bit fetch/jump size. */
static int cs_is32(Emu *e)
{
    return (e->seg[SEG_CS].valid && (e->seg[SEG_CS].attr & 0x4000)) ? 1 : 0;
}

static u32 ip_mask(Emu *e) { return cs_is32(e) ? 0xFFFFFFFFu : 0xFFFFu; }

/* In a 32-bit code segment the default operand and address size is 32, and
 * the 0x66/0x67 prefixes switch back to 16.  In a 16-bit segment it is the
 * other way round, which is also the real-mode case. */
static int op_is32(Emu *e) { return e->op32 ^ cs_is32(e); }
static int ad_is32(Emu *e) { return e->addr32 ^ cs_is32(e); }
static u32 sp_mask(Emu *e)
{
    return (e->seg[SEG_SS].valid && (e->seg[SEG_SS].attr & 0x4000)) ? 0xFFFFFFFFu : 0xFFFFu;
}

static u8 fetch8(Emu *e) { u8 v = emu_rb32(e, e->cs, e->eip); e->eip = (e->eip + 1u) & ip_mask(e); return v; }
static u16 fetch16(Emu *e) { u16 v = (u16)(fetch8(e) | ((u16)fetch8(e) << 8)); return v; }
static u32 fetch32(Emu *e)
{
    u32 v = (u32)fetch16(e);
    v |= (u32)fetch16(e) << 16;
    return v;
}
static s16 fetchs16(Emu *e) { return (s16)fetch16(e); }
static s32 fetchs32(Emu *e) { return (s32)fetch32(e); }
/* immediate of the current operand size */
static u32 fetch_imm(Emu *e, int width)
{
    if (width == 8) return fetch8(e);
    if (width == 32) return fetch32(e);
    return fetch16(e);
}

/* stack */
static void push16(Emu *e, u16 v) { e->esp = (e->esp - 2u) & sp_mask(e); emu_ww32(e, e->ss, e->esp, v); }
static u16 pop16(Emu *e) { u16 v = emu_rw32(e, e->ss, e->esp); e->esp = (e->esp + 2u) & sp_mask(e); return v; }
static void push32(Emu *e, u32 v) { e->esp = (e->esp - 4u) & sp_mask(e); emu_wd32(e, e->ss, e->esp, v); }
static u32 pop32(Emu *e) { u32 v = emu_rd32(e, e->ss, e->esp); e->esp = (e->esp + 4u) & sp_mask(e); return v; }
/* operand-size aware push/pop */
static void pushv(Emu *e, u32 v, int width)
{
    if (width == 32) push32(e, v); else push16(e, (u16)v);
}
static u32 popv(Emu *e, int width)
{
    return (width == 32) ? pop32(e) : (u32)pop16(e);
}

/* flags */
static void setf(Emu *e, u16 f, int v) { if (v) e->flags |= f; else e->flags = (u16)(e->flags & ~f); }
static int getf(Emu *e, u16 f) { return (e->flags & f) != 0; }
static int parity8(u8 v)
{
    v ^= (u8)(v >> 4); v ^= (u8)(v >> 2); v ^= (u8)(v >> 1);
    return (v & 1) == 0;
}
static void set_szp8(Emu *e, u8 r)
{
    setf(e, F_ZF, r == 0);
    setf(e, F_SF, (r & 0x80) != 0);
    setf(e, F_PF, parity8(r));
}
static void set_szp16(Emu *e, u16 r)
{
    setf(e, F_ZF, r == 0);
    setf(e, F_SF, (r & 0x8000) != 0);
    setf(e, F_PF, parity8((u8)r));
}

/* arithmetic */
static u8 add8(Emu *e, u8 a, u8 b, int c)
{
    u32 res = (u32)a + b + (c ? getf(e, F_CF) : 0);
    u8 r = (u8)res;
    setf(e, F_CF, res > 0xFF);
    setf(e, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    setf(e, F_OF, ((a ^ r) & (b ^ r) & 0x80) != 0);
    set_szp8(e, r);
    return r;
}
static u16 add16(Emu *e, u16 a, u16 b, int c)
{
    u32 res = (u32)a + b + (c ? getf(e, F_CF) : 0);
    u16 r = (u16)res;
    setf(e, F_CF, res > 0xFFFF);
    setf(e, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    setf(e, F_OF, ((a ^ r) & (b ^ r) & 0x8000) != 0);
    set_szp16(e, r);
    return r;
}
static u8 sub8(Emu *e, u8 a, u8 b, int c)
{
    u32 sub = (u32)b + (c ? getf(e, F_CF) : 0);
    u8 r = (u8)(a - sub);
    setf(e, F_CF, (u32)a < sub);
    setf(e, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    setf(e, F_OF, ((a ^ b) & (a ^ r) & 0x80) != 0);
    set_szp8(e, r);
    return r;
}
static u16 sub16(Emu *e, u16 a, u16 b, int c)
{
    u32 sub = (u32)b + (c ? getf(e, F_CF) : 0);
    u16 r = (u16)((u32)a - sub);
    setf(e, F_CF, (u32)a < sub);
    setf(e, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    setf(e, F_OF, ((a ^ b) & (a ^ r) & 0x8000) != 0);
    set_szp16(e, r);
    return r;
}
static u8 inc8(Emu *e, u8 a)
{
    u8 r = (u8)(a + 1);
    setf(e, F_AF, (a & 0x0F) == 0x0F);
    setf(e, F_OF, a == 0x7F);
    set_szp8(e, r);
    return r;
}
static u8 dec8(Emu *e, u8 a)
{
    u8 r = (u8)(a - 1);
    setf(e, F_AF, (a & 0x0F) == 0);
    setf(e, F_OF, a == 0x80);
    set_szp8(e, r);
    return r;
}
static u16 inc16(Emu *e, u16 a)
{
    u16 r = (u16)(a + 1);
    setf(e, F_AF, (a & 0x0F) == 0x0F);
    setf(e, F_OF, a == 0x7FFF);
    set_szp16(e, r);
    return r;
}
static u16 dec16(Emu *e, u16 a)
{
    u16 r = (u16)(a - 1);
    setf(e, F_AF, (a & 0x0F) == 0);
    setf(e, F_OF, a == 0x8000);
    set_szp16(e, r);
    return r;
}
static u8 logic8(Emu *e, u8 a, u8 b, int op) /* 0 and 1 or 2 xor */
{
    u8 r = op == 0 ? (u8)(a & b) : op == 1 ? (u8)(a | b) : (u8)(a ^ b);
    setf(e, F_CF, 0); setf(e, F_OF, 0); setf(e, F_AF, 0);
    set_szp8(e, r);
    return r;
}
static u16 logic16(Emu *e, u16 a, u16 b, int op)
{
    u16 r = op == 0 ? (u16)(a & b) : op == 1 ? (u16)(a | b) : (u16)(a ^ b);
    setf(e, F_CF, 0); setf(e, F_OF, 0); setf(e, F_AF, 0);
    set_szp16(e, r);
    return r;
}

/* modrm decode */
typedef struct {
    int mod, reg, rm;
    int is_reg;
    u32 ea;          /* effective address (16- or 32-bit per addr32) */
    int seg_ov;      /* -1 default, else segment index 0..5 */
} MRM;

/* 32-bit SIB base/index table */
static const int sib_reg[8] = { 0, 1, 2, 3, -1, 5, 6, 7 };  /* -1 = ebp/none */

static void decode_mrm(Emu *e, MRM *m, int seg_ov)
{
    u8 b = fetch8(e);
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    m->seg_ov = seg_ov;
    m->is_reg = (m->mod == 3);
    if (m->is_reg) return;

    if (ad_is32(e)) {
        /* 32-bit addressing: base+index*scale+disp, with the SIB byte */
        u32 off = 0;
        int use_ss = 0;
        int rm = m->rm;
        if (rm == 4) {
            u8 sib = fetch8(e);
            int scale = sib >> 6;
            int index = (sib >> 3) & 7;
            int base = sib & 7;
            int base_reg = sib_reg[base];
            if (index != 4) off += r32(e, index) << scale;
            if (base_reg < 0 && m->mod == 0) {
                off += fetch32(e);              /* no base, disp32 */
            } else if (base_reg == 5) {         /* ebp base */
                off += e->ebp;
                use_ss = 1;
            } else if (base_reg >= 0) {
                off += r32(e, base_reg);
            }
        } else if (rm == 5 && m->mod == 0) {
            off = fetch32(e);                   /* disp32 */
        } else {
            off = r32(e, rm);
            if (rm == 5) use_ss = 1;            /* ebp based */
        }
        if (m->mod == 1) off += (u32)(s32)(s8)fetch8(e);
        else if (m->mod == 2) off += fetch32(e);
        m->ea = off;
        if (m->seg_ov < 0) m->seg_ov = use_ss ? SEG_SS : SEG_DS;
        return;
    }

    u16 off = 0;
    int use_ss = 0;
    switch (m->rm) {
    case 0: off = (u16)(e->bx + e->si); break;
    case 1: off = (u16)(e->bx + e->di); break;
    case 2: off = (u16)(e->bp + e->si); use_ss = 1; break;
    case 3: off = (u16)(e->bp + e->di); use_ss = 1; break;
    case 4: off = (u16)e->si; break;
    case 5: off = (u16)e->di; break;
    case 6: if (m->mod == 0) off = fetch16(e); else { off = (u16)e->bp; use_ss = 1; } break;
    default: off = (u16)e->bx; break;
    }
    if (m->mod == 1) off = (u16)(off + (u8)fetch8(e));
    else if (m->mod == 2) off = (u16)(off + fetch16(e));
    m->ea = off;
    if (m->seg_ov < 0) m->seg_ov = use_ss ? SEG_SS : SEG_DS;
}
static u16 mrm_seg(Emu *e, MRM *m) { return seg_reg(e, m->seg_ov); }
static u8 mrm_r8(Emu *e, MRM *m)
{
    if (m->is_reg) return r8(e, m->rm);
    return emu_rb32(e, mrm_seg(e, m), m->ea);
}
static void mrm_w8(Emu *e, MRM *m, u8 v)
{
    if (m->is_reg) { w8(e, m->rm, v); return; }
    emu_wb32(e, mrm_seg(e, m), m->ea, v);
}
static u16 mrm_r16(Emu *e, MRM *m)
{
    if (m->is_reg) return r16(e, m->rm);
    return emu_rw32(e, mrm_seg(e, m), m->ea);
}
static void mrm_w16(Emu *e, MRM *m, u16 v)
{
    if (m->is_reg) { w16(e, m->rm, v); return; }
    emu_ww32(e, mrm_seg(e, m), m->ea, v);
}
static u32 mrm_r32(Emu *e, MRM *m)
{
    if (m->is_reg) return r32(e, m->rm);
    return emu_rd32(e, mrm_seg(e, m), m->ea);
}
static void mrm_w32(Emu *e, MRM *m, u32 v)
{
    if (m->is_reg) { w32(e, m->rm, v); return; }
    emu_wd32(e, mrm_seg(e, m), m->ea, v);
}
/* operand-width generic r/m access */
static u32 mrm_rv(Emu *e, MRM *m, int width)
{
    if (width == 8) return mrm_r8(e, m);
    if (width == 32) return mrm_r32(e, m);
    return mrm_r16(e, m);
}
static void mrm_wv(Emu *e, MRM *m, u32 v, int width)
{
    if (width == 8) { mrm_w8(e, m, (u8)v); return; }
    if (width == 32) { mrm_w32(e, m, v); return; }
    mrm_w16(e, m, (u16)v);
}

/* group1/2 addressing helpers use reg field as operation selector */
static u8 alu8(Emu *e, int op, u8 a, u8 b)
{
    switch (op) {
    case 0: return add8(e, a, b, 0);
    case 1: return logic8(e, a, b, 1);
    case 2: return add8(e, a, b, 1);
    case 3: return sub8(e, a, b, 1);
    case 4: return logic8(e, a, b, 0);
    case 5: return sub8(e, a, b, 0);
    case 6: return logic8(e, a, b, 2);
    default: sub8(e, a, b, 0); return a; /* CMP */
    }
}
static u16 alu16(Emu *e, int op, u16 a, u16 b)
{
    switch (op) {
    case 0: return add16(e, a, b, 0);
    case 1: return logic16(e, a, b, 1);
    case 2: return add16(e, a, b, 1);
    case 3: return sub16(e, a, b, 1);
    case 4: return logic16(e, a, b, 0);
    case 5: return sub16(e, a, b, 0);
    case 6: return logic16(e, a, b, 2);
    default: sub16(e, a, b, 0); return a;
    }
}
static void set_szp32(Emu *e, u32 r)
{
    setf(e, F_ZF, r == 0);
    setf(e, F_SF, (r & 0x80000000u) != 0);
    setf(e, F_PF, parity8((u8)r));
}
static u32 add32(Emu *e, u32 a, u32 b, int c)
{
    u64 s = (u64)a + (u64)b + (u64)c;
    u32 r = (u32)s;
    setf(e, F_CF, s > 0xFFFFFFFFull);
    setf(e, F_AF, ((a ^ b ^ r) & 0x10u) != 0);
    setf(e, F_OF, ((a ^ r) & (b ^ r) & 0x80000000u) != 0);
    set_szp32(e, r);
    return r;
}
static u32 sub32(Emu *e, u32 a, u32 b, int c)
{
    u32 r = a - b - (u32)c;
    setf(e, F_CF, (u64)a < (u64)b + (u64)c);
    setf(e, F_AF, ((a ^ b ^ r) & 0x10u) != 0);
    setf(e, F_OF, ((a ^ b) & (a ^ r) & 0x80000000u) != 0);
    set_szp32(e, r);
    return r;
}
static u32 inc32(Emu *e, u32 a)
{
    u32 r = a + 1u;
    setf(e, F_AF, (a & 0x0Fu) == 0x0Fu);
    setf(e, F_OF, a == 0x7FFFFFFFu);
    set_szp32(e, r);
    return r;
}
static u32 dec32(Emu *e, u32 a)
{
    u32 r = a - 1u;
    setf(e, F_AF, (a & 0x0Fu) == 0);
    setf(e, F_OF, a == 0x80000000u);
    set_szp32(e, r);
    return r;
}
static u32 logic32(Emu *e, u32 a, u32 b, int op)
{
    u32 r = op == 0 ? (a & b) : op == 1 ? (a | b) : (a ^ b);
    setf(e, F_CF, 0); setf(e, F_OF, 0); setf(e, F_AF, 0);
    set_szp32(e, r);
    return r;
}
static u32 alu32(Emu *e, int op, u32 a, u32 b)
{
    switch (op) {
    case 0: return add32(e, a, b, 0);
    case 1: return logic32(e, a, b, 1);
    case 2: return add32(e, a, b, 1);
    case 3: return sub32(e, a, b, 1);
    case 4: return logic32(e, a, b, 0);
    case 5: return sub32(e, a, b, 0);
    case 6: return logic32(e, a, b, 2);
    default: sub32(e, a, b, 0); return a;   /* CMP */
    }
}
/* operand-width generic ALU (width 8/16/32) */
static u32 aluv(Emu *e, int op, u32 a, u32 b, int width)
{
    if (width == 8) return alu8(e, op, (u8)a, (u8)b);
    if (width == 32) return alu32(e, op, a, b);
    return alu16(e, op, (u16)a, (u16)b);
}
static u32 incv(Emu *e, u32 a, int width)
{
    if (width == 8) return inc8(e, (u8)a);
    if (width == 32) return inc32(e, a);
    return inc16(e, (u16)a);
}
static u32 decv(Emu *e, u32 a, int width)
{
    if (width == 8) return dec8(e, (u8)a);
    if (width == 32) return dec32(e, a);
    return dec16(e, (u16)a);
}

/* shifts: group-2 op encoding is /0=ROL /1=ROR /2=RCL /3=RCR /4=SHL /5=SHR
 * /6=SAL (undocumented alias of SHL) /7=SAR */
static u32 do_shift(Emu *e, int op, u32 v, unsigned cnt, int width)
{
    u32 mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    u32 sign = (width >= 32) ? 0x80000000u : (1u << (width - 1));
    u32 r;
    if (width == 8) v &= 0xFFu;
    else if (width == 16) v &= 0xFFFFu;
    r = v;
    for (unsigned i = 0; i < cnt; i++) {
        switch (op) {
        case 0: { /* rol */
            u32 top = (r & sign) ? 1 : 0;
            r = ((r << 1) | top) & mask;
            setf(e, F_CF, top != 0);
            if (i == 0) setf(e, F_OF, (top ? 1 : 0) ^ ((r & sign) ? 1 : 0));
            break;
        }
        case 1: { /* ror */
            u32 low = r & 1;
            r = ((r >> 1) | (low ? sign : 0)) & mask;
            setf(e, F_CF, low != 0);
            if (i == 0) setf(e, F_OF, (low ? 1 : 0) ^ ((r & (sign >> 1)) ? 1 : 0));
            break;
        }
        case 2: { /* rcl */
            u32 c = getf(e, F_CF) ? 1 : 0;
            u32 top = (r & sign) ? 1 : 0;
            r = ((r << 1) & mask) | c;
            setf(e, F_CF, top != 0);
            if (i == 0) setf(e, F_OF, (top ? 1 : 0) ^ ((r & sign) ? 1 : 0));
            break;
        }
        case 3: { /* rcr */
            u32 c = getf(e, F_CF) ? 1 : 0;
            u32 low = r & 1;
            r = (r >> 1) | (c ? sign : 0);
            setf(e, F_CF, low != 0);
            if (i == 0) setf(e, F_OF, (low ? 1 : 0) ^ ((r & (sign >> 1)) ? 1 : 0));
            break;
        }
        case 4: case 6: /* shl / sal */
            setf(e, F_CF, (r & sign) != 0);
            r = (r << 1) & mask;
            if (i == 0) setf(e, F_OF, ((r & sign) ? 1 : 0) ^ getf(e, F_CF));
            break;
        case 5: /* shr */
            setf(e, F_CF, (r & 1) != 0);
            r >>= 1;
            if (i == 0) setf(e, F_OF, (v & sign) != 0);
            break;
        default: /* sar */
            setf(e, F_CF, (r & 1) != 0);
            r = (r & sign) ? ((r >> 1) | sign) : (r >> 1);
            setf(e, F_OF, 0);
            break;
        }
    }
    return r;
}
static int trap_div(Emu *e)
{
    plat_puts("Divide overflow in 16-bit program\n");
    e->halted = 1;
    e->exit_code = 0xFF;
    return 0;
}

/* one instruction */
/* ==================================================================== */
/* operand / address size helpers                                       */
/* ==================================================================== */
static int op_width(Emu *e, int word)
{
    /* word bit of the opcode selects 16/32-bit operands; 0x66 flips it */
    return word ? ((op_is32(e) ? 32 : 16)) : 8;
}

static void jmp_rel(Emu *e, s32 rel)
{
    e->eip = (e->eip + (u32)rel) & ip_mask(e);
}

static void set_cs_ip(Emu *e, u16 sel, u32 ip)
{
    set_seg_reg(e, SEG_CS, sel);
    e->eip = ip & ip_mask(e);
}

/* condition codes for Jcc/SETcc/CMOVcc: 0..15 = O,NO,B,AE,E,NE,BE,A,S,NS,P,
 * NP,L,GE,LE,G */
static int cond_true(Emu *e, int cc)
{
    switch (cc & 0x0F) {
    case 0x0: return getf(e, F_OF);
    case 0x1: return !getf(e, F_OF);
    case 0x2: return getf(e, F_CF);
    case 0x3: return !getf(e, F_CF);
    case 0x4: return getf(e, F_ZF);
    case 0x5: return !getf(e, F_ZF);
    case 0x6: return getf(e, F_CF) || getf(e, F_ZF);
    case 0x7: return !getf(e, F_CF) && !getf(e, F_ZF);
    case 0x8: return getf(e, F_SF);
    case 0x9: return !getf(e, F_SF);
    case 0xA: return getf(e, F_PF);
    case 0xB: return !getf(e, F_PF);
    case 0xC: return getf(e, F_SF) != getf(e, F_OF);
    case 0xD: return getf(e, F_SF) == getf(e, F_OF);
    case 0xE: return getf(e, F_ZF) || (getf(e, F_SF) != getf(e, F_OF));
    default:  return !getf(e, F_ZF) && (getf(e, F_SF) == getf(e, F_OF));
    }
}

/* effective address of the register form of a string instruction */
static u32 str_cnt(Emu *e) { return ad_is32(e) ? e->ecx : (u32)e->cx; }
static void str_cnt_dec(Emu *e) { if (ad_is32(e)) e->ecx--; else e->cx--; }
static u32 str_si(Emu *e) { return ad_is32(e) ? e->esi : (u32)(u16)e->esi; }
static u32 str_di(Emu *e) { return ad_is32(e) ? e->edi : (u32)(u16)e->edi; }
static void str_adv_si(Emu *e, u32 n, int back)
{
    if (ad_is32(e)) e->esi = back ? e->esi - n : e->esi + n;
    else e->si = (u16)(back ? e->si - n : e->si + n);
}
static void str_adv_di(Emu *e, u32 n, int back)
{
    if (ad_is32(e)) e->edi = back ? e->edi - n : e->edi + n;
    else e->di = (u16)(back ? e->di - n : e->di + n);
}

static u32 rd_mem(Emu *e, u16 sel, u32 off, int width)
{
    if (width == 8) return emu_rb32(e, sel, off);
    if (width == 32) return emu_rd32(e, sel, off);
    return emu_rw32(e, sel, off);
}
static void wr_mem(Emu *e, u16 sel, u32 off, u32 v, int width)
{
    if (width == 8) { emu_wb32(e, sel, off, (u8)v); return; }
    if (width == 32) { emu_wd32(e, sel, off, v); return; }
    emu_ww32(e, sel, off, (u16)v);
}

/* ------------------------------------------------------------------ */
/* 387 escape (D8..DF): decode the modrm so the instruction length is  */
/* right, then do nothing, exactly like an 8086 without a coprocessor. */
/* ------------------------------------------------------------------ */
static int step_fpu(Emu *e, u8 op, int seg_ov)
{
    MRM m;
    decode_mrm(e, &m, seg_ov);
    (void)op;
    return 0;
}

/* ------------------------------------------------------------------ */
/* two-byte (0F xx) opcodes: 386 extensions + protected-mode control   */
/* ------------------------------------------------------------------ */
static int step_386(Emu *e, int seg_ov)
{
    u8 op2 = fetch8(e);
    int width = (op_is32(e) ? 32 : 16);

    switch (op2) {
    case 0x00: { /* LLDT / SLDT / LTR / STR */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        switch (m.reg) {
        case 0: e->ldtr = (u16)mrm_r16(e, &m); break;              /* LLDT */
        case 1: mrm_w16(e, &m, e->ldtr); break;                    /* SLDT */
        case 2: e->tr = (u16)mrm_r16(e, &m); break;                /* LTR */
        default: mrm_w16(e, &m, e->tr); break;                     /* STR */
        }
        return 0;
    }
    case 0x01: { /* SGDT / SIDT / LGDT / LIDT / SMSW / LMSW / INVLPG */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        switch (m.reg) {
        case 0: /* SGDT */
            wr_mem(e, mrm_seg(e, &m), m.ea, e->gdt_limit, 16);
            wr_mem(e, mrm_seg(e, &m), m.ea + 2, e->gdt_base, 32);
            break;
        case 1: /* SIDT */
            wr_mem(e, mrm_seg(e, &m), m.ea, e->idt_limit, 16);
            wr_mem(e, mrm_seg(e, &m), m.ea + 2, e->idt_base, 32);
            break;
        case 2: { /* LGDT */
            u16 lim = (u16)rd_mem(e, mrm_seg(e, &m), m.ea, 16);
            u32 base = rd_mem(e, mrm_seg(e, &m), m.ea + 2, 32);
            if (op_is32(e)) {
                lim = (u16)rd_mem(e, mrm_seg(e, &m), m.ea, 16);
                base = rd_mem(e, mrm_seg(e, &m), m.ea + 2, 32);
            }
            e->gdt_limit = lim;
            e->gdt_base = base;
            break;
        }
        case 3: { /* LIDT */
            u16 lim = (u16)rd_mem(e, mrm_seg(e, &m), m.ea, 16);
            u32 base = rd_mem(e, mrm_seg(e, &m), m.ea + 2, 32);
            e->idt_limit = lim;
            e->idt_base = base;
            pm_patch_idt(e);
            break;
        }
        case 4: e->cr0 = (e->cr0 & 0xFFFFFFF0u) | (u32)(mrm_r16(e, &m) & 0x000F); break; /* SMSW */
        case 5: break;                                             /* LMSW */
        default: break;                                            /* INVLPG */
        }
        return 0;
    }
    case 0x06: e->cr0 &= ~CR0_TS; return 0;                        /* CLTS */
    case 0x08: case 0x09: return 0;                                /* INVD / WBINVD */
    case 0x20: { /* MOV r32, CRn */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (m.is_reg) {
            u32 v = 0;
            switch (m.reg) {
            case 0: v = e->cr0; break;
            case 2: v = e->cr2; break;
            case 3: v = e->cr3; break;
            default: v = e->cr4; break;
            }
            w32(e, m.rm, v);
        }
        return 0;
    }
    case 0x22: { /* MOV CRn, r32 */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (m.is_reg) {
            u32 v = r32(e, m.rm);
            switch (m.reg) {
            case 0:
                e->cr0 = v;
                if ((v & CR0_PG) && !(v & CR0_PE)) {
                    e->pm_fault = 1; e->pm_fault_addr = 0;
                }
                seg_cache_sync(e);
                break;
            case 2: e->cr2 = v; break;
            case 3: e->cr3 = v; break;
            default: e->cr4 = v; break;
            }
        }
        return 0;
    }
    case 0x23: { /* MOV DRn, r32 */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (m.is_reg) w32(e, m.rm, 0);
        return 0;
    }
    case 0x31: { /* RDTSC */
        u64 t = plat_ticks_ms();
        e->eax = (u32)(t & 0xFFFFFFFFu);
        e->edx = (u32)(t >> 32);
        return 0;
    }
    case 0x0B: /* UD2 */
        plat_puts("UEFI-DOS: UD2 (undefined instruction)\n");
        e->halted = 1; e->exit_code = 0xFF;
        return 0;
    case 0xA2: { /* CPUID: report a plain 386/486-class CPU, no MMX/SSE */
        u32 leaf = e->eax;
        if (leaf == 0) {
            e->eax = 1;
            e->ebx = 0x534F4446u;   /* 'FDOS' */
            e->edx = 0x4F445546u;   /* 'UFDO' */
            e->ecx = 0x5344492Du;   /* '-IDS' */
        } else if (leaf == 1) {
            e->eax = 0x00000430u;   /* family 4, model 3, stepping 0 */
            e->ebx = 0;
            e->ecx = 0;
            e->edx = 0x00000008u;   /* TSC available */
        } else {
            e->eax = e->ebx = e->ecx = e->edx = 0;
        }
        return 0;
    }
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: { /* Jcc rel16/rel32 */
        s32 rel = (width == 32) ? fetchs32(e) : (s32)fetchs16(e);
        if (cond_true(e, (int)(op2 & 0x0F))) jmp_rel(e, rel);
        return 0;
    }
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: { /* SETcc */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        mrm_w8(e, &m, cond_true(e, (int)(op2 & 0x0F)) ? 1 : 0);
        return 0;
    }
    case 0xA0: pushv(e, e->fs, width); return 0;
    case 0xA1: set_seg_reg(e, SEG_FS, (u16)popv(e, width)); return 0;
    case 0xA8: pushv(e, e->gs, width); return 0;
    case 0xA9: set_seg_reg(e, SEG_GS, (u16)popv(e, width)); return 0;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: { /* BT / BTS / BTR / BTC */
        MRM m;
        int bit;
        u32 v, mask;
        decode_mrm(e, &m, seg_ov);
        bit = (op2 & 0x40) ? (int)r8(e, m.reg) : (int)fetch_imm(e, width);
        v = mrm_rv(e, &m, width);
        if (width == 32) { bit &= 31; mask = 1u << bit; }
        else if (width == 16) { bit &= 15; mask = 1u << bit; }
        else { bit &= 7; mask = 1u << bit; }
        setf(e, F_CF, (v & mask) != 0);
        if (op2 == 0xAB) mrm_wv(e, &m, v | mask, width);
        else if (op2 == 0xB3) mrm_wv(e, &m, v & ~mask, width);
        else if (op2 == 0xBB) mrm_wv(e, &m, v ^ mask, width);
        return 0;
    }
    case 0xA4: case 0xA5: { /* SHLD */
        MRM m;
        unsigned cnt;
        u32 dst, src, r;
        int bits = (op2 == 0xA5) ? width : 8;
        decode_mrm(e, &m, seg_ov);
        cnt = (op2 == 0xA5) ? (unsigned)fetch8(e) : (unsigned)fetch8(e);
        src = rv(e, m.reg, bits);
        dst = mrm_rv(e, &m, bits);
        if (cnt) {
            u32 mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            r = ((dst << cnt) | (src >> (bits - cnt))) & mask;
            setf(e, F_CF, ((dst >> (bits - cnt)) & 1u) != 0);
            if (bits == 32) set_szp32(e, r);
            else if (bits == 16) set_szp16(e, (u16)r);
            else set_szp8(e, (u8)r);
            mrm_wv(e, &m, r, bits);
        }
        return 0;
    }
    case 0xAC: case 0xAD: { /* SHRD */
        MRM m;
        unsigned cnt;
        u32 dst, src, r;
        int bits = (op2 == 0xAD) ? width : 8;
        decode_mrm(e, &m, seg_ov);
        cnt = (unsigned)fetch8(e);
        src = rv(e, m.reg, bits);
        dst = mrm_rv(e, &m, bits);
        if (cnt) {
            u32 mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            r = ((dst >> cnt) | (src << (bits - cnt))) & mask;
            setf(e, F_CF, ((dst >> (cnt - 1)) & 1u) != 0);
            if (bits == 32) set_szp32(e, r);
            else if (bits == 16) set_szp16(e, (u16)r);
            else set_szp8(e, (u8)r);
            mrm_wv(e, &m, r, bits);
        }
        return 0;
    }
    case 0xAF: { /* IMUL r, r/m (two operand form) */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (width == 32) {
            s64 p = (s64)(s32)r32(e, m.reg) * (s64)(s32)mrm_r32(e, &m);
            w32(e, m.reg, (u32)p);
            setf(e, F_CF, p != (s64)(s32)p);
            setf(e, F_OF, getf(e, F_CF));
        } else {
            s32 p = (s32)(s16)r16(e, m.reg) * (s32)(s16)mrm_r16(e, &m);
            w16(e, m.reg, (u16)p);
            setf(e, F_CF, p != (s32)(s16)p);
            setf(e, F_OF, getf(e, F_CF));
        }
        return 0;
    }
    case 0xB6: case 0xB7: { /* MOVZX */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (op2 == 0xB6) wv(e, m.reg, (u32)mrm_r8(e, &m), width);
        else wv(e, m.reg, (u32)mrm_r16(e, &m), width);
        return 0;
    }
    case 0xBE: case 0xBF: { /* MOVSX */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (op2 == 0xBE) wv(e, m.reg, (u32)(s32)(s8)mrm_r8(e, &m), width);
        else wv(e, m.reg, (u32)(s32)(s16)mrm_r16(e, &m), width);
        return 0;
    }
    case 0xAE: { /* group 15: fences / clflush - no-op */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        return 0;
    }
    case 0x02: case 0x03: { /* LAR / LSL */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        setf(e, F_ZF, 0);
        wv(e, m.reg, 0, width);
        return 0;
    }
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: { /* CMOVcc */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (cond_true(e, (int)(op2 & 0x0F))) wv(e, m.reg, mrm_rv(e, &m, width), width);
        return 0;
    }
    case 0x05: case 0x07: case 0x30: case 0x32: case 0x33: case 0x34: case 0x35:
        return 0;   /* SYSCALL/SYSRET/WRMSR/RDMSR/SYSENTER/SYSEXIT: ignored */
    default:
        plat_puts("UEFI-DOS: unsupported 386 opcode 0F ");
        {
            char hex[3];
            hex[0] = "0123456789ABCDEF"[op2 >> 4];
            hex[1] = "0123456789ABCDEF"[op2 & 15];
            hex[2] = 0;
            plat_puts(hex);
            plat_putc('\n');
        }
        e->halted = 1;
        e->exit_code = 0xFF;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* protected-mode interrupt delivery (386)                             */
/* ------------------------------------------------------------------ */
/* The UEFI-DOS services are reachable from protected mode through IDT gates
 * that point at our per-vector trampoline (TRAMP_SEG:TRAMP_SLOT(n)); those
 * gates are written into whatever IDT the program loads. */
static void dos_service_call(Emu *e, int intn)
{
    video_flush(e);
    if (intn == 0x21) int21_handler(e);
    else if (intn == 0x10) int10_handler(e);
    else if (intn == 0x16) int16_handler(e);
    else if (intn == 0x13) int13_handler(e);
    else if (intn == 0x1A) int1a_handler(e);
    else if (intn == 0x2F) int2f_handler(e);
    else if (intn == 0x31) int31_handler(e);
    else if (intn == 0x60) int60_handler(e);
    else if (intn == 0x20 || intn == 0x27) { e->halted = 1; e->exit_code = 0; }
    else { e->halted = 1; e->exit_code = 0; }
}

static u8 idt_gate_type32(u8 type_attr)
{
    return (u8)((type_attr & 0x08) ? 1 : 0);   /* 0xE/0xF = 32-bit gate */
}

/* Install our service gates into the program's IDT (called after LIDT). */
void pm_patch_idt(Emu *e)
{
    static const int serviced[] = { 0x10, 0x13, 0x16, 0x1A, 0x20, 0x21, 0x27, 0x2F, 0x31 };
    unsigned i;
    if (!e->idt_base || e->idt_limit < 8) return;
    for (i = 0; i < sizeof(serviced) / sizeof(serviced[0]); i++) {
        int v = serviced[i];
        u32 addr = e->idt_base + (u32)v * 8u;
        u16 off = TRAMP_SLOT(v);
        int is32 = (e->seg[SEG_CS].valid && (e->seg[SEG_CS].attr & 0x4000)) ? 1 : 0;
        if (addr + 8u > e->idt_base + (u32)e->idt_limit) continue;
        e->mem[(addr + 0) & MEM_MASK] = (u8)(off & 0xFF);
        e->mem[(addr + 1) & MEM_MASK] = (u8)(off >> 8);
        e->mem[(addr + 2) & MEM_MASK] = (u8)(TRAMP_SEG & 0xFF);
        e->mem[(addr + 3) & MEM_MASK] = (u8)(TRAMP_SEG >> 8);
        e->mem[(addr + 4) & MEM_MASK] = 0;
        e->mem[(addr + 5) & MEM_MASK] = (u8)(is32 ? 0x8E : 0x86);
        e->mem[(addr + 6) & MEM_MASK] = 0;
        e->mem[(addr + 7) & MEM_MASK] = 0;
    }
}

static void do_int_prot(Emu *e, int vec)
{
    u32 gaddr = e->idt_base + (u32)vec * 8u;
    u8 g[8];
    int k, is32;
    u16 off_low, sel, type_attr;
    u32 off_high, off;

    for (k = 0; k < 8; k++) g[k] = e->mem[(gaddr + (u32)k) & MEM_MASK];
    off_low = (u16)(g[0] | ((u16)g[1] << 8));
    sel = (u16)(g[2] | ((u16)g[3] << 8));
    type_attr = g[5];
    off_high = (u32)(g[6] | ((u32)g[7] << 8));
    off = (u32)off_low | (off_high << 16);

    if (sel == TRAMP_SEG && off == TRAMP_SLOT(vec)) {
        dos_service_call(e, vec);
        return;
    }
    if (!(type_attr & 0x80)) {
        {
            static const char hx[] = "0123456789ABCDEF";
            plat_puts("UEFI-DOS: PM int ");
            plat_putc(hx[(vec >> 4) & 15]);
            plat_putc(hx[vec & 15]);
            plat_puts(" idt_base=");
            plat_putc(hx[(e->idt_base >> 28) & 15]);
            plat_putc(hx[(e->idt_base >> 24) & 15]);
            plat_putc(hx[(e->idt_base >> 20) & 15]);
            plat_putc(hx[(e->idt_base >> 16) & 15]);
            plat_puts(" gate_sel=");
            plat_putc(hx[(sel >> 12) & 15]);
            plat_putc(hx[(sel >> 8) & 15]);
            plat_putc(hx[(sel >> 4) & 15]);
            plat_putc(hx[sel & 15]);
            plat_puts(" type=");
            plat_putc(hx[(type_attr >> 4) & 15]);
            plat_putc(hx[type_attr & 15]);
            plat_puts("\n");
        }
        plat_puts("UEFI-DOS: protected-mode interrupt ");
        plat_putc((char)(vec < 10 ? '0' + vec : '?'));
        plat_puts(" with no IDT gate\n");
        e->halted = 1;
        e->exit_code = 0xFF;
        return;
    }
    is32 = idt_gate_type32(type_attr);
    pushv(e, e->eflags, is32 ? 32 : 16);
    pushv(e, e->cs, is32 ? 32 : 16);
    pushv(e, e->eip, is32 ? 32 : 16);
    if ((type_attr & 0x0F) == 0x0E || (type_attr & 0x0F) == 0x06) setf(e, F_IF, 0);
    set_cs_ip(e, sel, off);
}

/* ------------------------------------------------------------------ */
/* one instruction                                                     */
/* ------------------------------------------------------------------ */
static int step_8086(Emu *e)
{
    int seg_ov = -1;
    int rep = 0, rep_ne = 0;
    int prefix_done = 0;
    int i;
    u8 op = 0;
    u8 intn = 0;

    e->op32 = 0;
    e->addr32 = 0;
    while (!prefix_done) {
        op = fetch8(e);
        switch (op) {
        case 0x26: seg_ov = SEG_ES; break;
        case 0x2E: seg_ov = SEG_CS; break;
        case 0x36: seg_ov = SEG_SS; break;
        case 0x3E: seg_ov = SEG_DS; break;
        case 0x64: seg_ov = SEG_FS; break;
        case 0x65: seg_ov = SEG_GS; break;
        case 0xF2: rep = 1; rep_ne = 1; break;
        case 0xF3: rep = 1; break;
        case 0xF0: break;                       /* LOCK */
        case 0x66: e->op32 = !e->op32; break;    /* operand-size */
        case 0x67: e->addr32 = !e->addr32; break;/* address-size */
        default: prefix_done = 1; break;
        }
    }

    switch (op) {
    /* --- ALU: 00..3B, register/memory forms --- */
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3A: case 0x3B: {
        int opgrp = op >> 3;
        int width = op_width(e, op & 1);
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (op & 2) {
            u32 a = rv(e, m.reg, width);
            wv(e, m.reg, aluv(e, opgrp, a, mrm_rv(e, &m, width), width), width);
        } else {
            u32 b = rv(e, m.reg, width);
            mrm_wv(e, &m, aluv(e, opgrp, mrm_rv(e, &m, width), b, width), width);
        }
        break;
    }
    /* --- ALU: accumulator, immediate --- */
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: {
        int opgrp = (op - 4) >> 3;
        u32 b = fetch_imm(e, 8);
        wv(e, 0, aluv(e, opgrp, rv(e, 0, 8), b, 8), 8);
        break;
    }
    case 0x3C: { u32 b = fetch_imm(e, 8); aluv(e, 7, rv(e, 0, 8), b, 8); break; }
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int opgrp = (op - 5) >> 3;
        int width = (op_is32(e) ? 32 : 16);
        u32 b = fetch_imm(e, width);
        u32 r = aluv(e, opgrp, rv(e, 0, width), b, width);
        if (op != 0x3D) wv(e, 0, r, width);
        break;
    }
    /* --- Jcc rel8 --- */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        s8 rel = (s8)fetch8(e);
        if (cond_true(e, op & 0x0F)) jmp_rel(e, rel);
        break;
    }
    case 0x8F: { MRM m; decode_mrm(e, &m, seg_ov); if (m.reg == 0) mrm_wv(e, &m, popv(e, (op_is32(e) ? 32 : 16)), (op_is32(e) ? 32 : 16)); break; }
    case 0x9A: { /* far CALL */
        int width = (op_is32(e) ? 32 : 16);
        u32 off = fetch_imm(e, width);
        u16 sel = fetch16(e);
        pushv(e, e->cs, width);
        pushv(e, e->eip, width);
        set_cs_ip(e, sel, off);
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int width = op_width(e, op != 0x80 && op != 0x82);
        MRM m;
        u32 imm;
        decode_mrm(e, &m, seg_ov);
        if (op == 0x83) imm = (u32)(s32)(s8)fetch8(e);
        else imm = fetch_imm(e, width);
        mrm_wv(e, &m, aluv(e, m.reg, mrm_rv(e, &m, width), imm, width), width);
        break;
    }
    case 0xA8: { u32 b = fetch_imm(e, 8); aluv(e, 4, rv(e, 0, 8), b, 8); break; }
    case 0xA9: { int width = (op_is32(e) ? 32 : 16); u32 b = fetch_imm(e, width); aluv(e, 4, rv(e, 0, width), b, width); break; }

    /* --- INC/DEC reg --- */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: {
        int width = (op_is32(e) ? 32 : 16);
        int r = op & 7;
        wv(e, r, incv(e, rv(e, r, width), width), width);
        break;
    }
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        int width = (op_is32(e) ? 32 : 16);
        int r = op & 7;
        wv(e, r, decv(e, rv(e, r, width), width), width);
        break;
    }

    /* --- PUSH/POP reg --- */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: {
        int width = (op_is32(e) ? 32 : 16);
        pushv(e, rv(e, op & 7, width), width);
        break;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int width = (op_is32(e) ? 32 : 16);
        wv(e, op & 7, popv(e, width), width);
        break;
    }
    case 0x06: pushv(e, e->es, (op_is32(e) ? 32 : 16)); break;
    case 0x0E: pushv(e, e->cs, (op_is32(e) ? 32 : 16)); break;
    case 0x16: pushv(e, e->ss, (op_is32(e) ? 32 : 16)); break;
    case 0x1E: pushv(e, e->ds, (op_is32(e) ? 32 : 16)); break;
    case 0x07: set_seg_reg(e, SEG_ES, (u16)popv(e, (op_is32(e) ? 32 : 16))); break;
    case 0x17: set_seg_reg(e, SEG_SS, (u16)popv(e, (op_is32(e) ? 32 : 16))); break;
    case 0x1F: set_seg_reg(e, SEG_DS, (u16)popv(e, (op_is32(e) ? 32 : 16))); break;
    case 0x60: { /* PUSHA / PUSHAD */
        if (op_is32(e)) {
            u32 oldesp = e->esp;
            push32(e, e->eax); push32(e, e->ecx); push32(e, e->edx); push32(e, e->ebx);
            push32(e, oldesp); push32(e, e->ebp); push32(e, e->esi); push32(e, e->edi);
        } else {
            u16 oldsp = e->sp;
            push16(e, e->ax); push16(e, e->cx); push16(e, e->dx); push16(e, e->bx);
            push16(e, oldsp); push16(e, e->bp); push16(e, e->si); push16(e, e->di);
        }
        break;
    }
    case 0x61: { /* POPA / POPAD */
        if (op_is32(e)) {
            e->edi = pop32(e); e->esi = pop32(e); e->ebp = pop32(e);
            pop32(e);
            e->ebx = pop32(e); e->edx = pop32(e); e->ecx = pop32(e); e->eax = pop32(e);
        } else {
            e->di = pop16(e); e->si = pop16(e); e->bp = pop16(e);
            pop16(e);
            e->bx = pop16(e); e->dx = pop16(e); e->cx = pop16(e); e->ax = pop16(e);
        }
        break;
    }
    case 0x68: { int width = (op_is32(e) ? 32 : 16); pushv(e, fetch_imm(e, width), width); break; }
    case 0x6A: { int width = (op_is32(e) ? 32 : 16); pushv(e, (u32)(s32)(s8)fetch8(e), width); break; }
    case 0x62: { /* BOUND r16, m16&16 */
        /* No #BR in this VM: decode the operands and continue.  Some DOS
         * binaries emit it from a bounds-checking compiler switch. */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        (void)m;
        break;
    }
    case 0x63: { /* ARPL r/m16, r16 (only meaningful in protected mode) */
        int width = op_is32(e) ? 32 : 16;
        MRM m;
        decode_mrm(e, &m, seg_ov);
        u32 dst = mrm_rv(e, &m, width), src = rv(e, m.reg, width);
        if ((dst & 3u) < (src & 3u)) {
            mrm_wv(e, &m, (dst & ~3u) | (src & 3u), width);
            setf(e, F_ZF, 1);
        } else {
            setf(e, F_ZF, 0);
        }
        break;
    }
    case 0x69: case 0x6B: { /* IMUL r16/32, r/m, imm16/imm8 */
        int width = op_is32(e) ? 32 : 16;
        MRM m;
        u32 imm;
        decode_mrm(e, &m, seg_ov);
        imm = (op == 0x69) ? fetch_imm(e, width) : (u32)(s32)(s8)fetch8(e);
        if (width == 32) {
            s64 p = (s64)(s32)mrm_rv(e, &m, width) * (s64)(s32)imm;
            wv(e, m.reg, (u32)p, width);
            setf(e, F_CF, p != (s64)(s32)p);
            setf(e, F_OF, getf(e, F_CF));
        } else {
            s32 p = (s32)(s16)mrm_rv(e, &m, width) * (s32)(s16)imm;
            wv(e, m.reg, (u32)p, width);
            setf(e, F_CF, p != (s32)(s16)p);
            setf(e, F_OF, getf(e, F_CF));
        }
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: { /* INSB/W and OUTSB/W */
        int ins = (op == 0x6C || op == 0x6D);
        int width = (op & 1) ? (op_is32(e) ? 32 : 16) : 8;
        u32 sz = (u32)width / 8;
        int back = getf(e, F_DF) ? 1 : 0;
        u32 cnt = rep ? str_cnt(e) : 1;
        u16 srsel = seg_ov >= 0 ? seg_reg(e, seg_ov) : e->ds;
        u16 port = (u16)e->dx;
        if (rep && cnt == 0) break;
        while (cnt--) {
            if (ins) {
                wr_mem(e, e->es, str_di(e), (u32)emu_in_port(e, port), width);
                str_adv_di(e, sz, back);
            } else {
                emu_out_port(e, port, (u16)rd_mem(e, srsel, str_si(e), width));
                str_adv_si(e, sz, back);
            }
            if (rep) str_cnt_dec(e);
        }
        break;
    }
    case 0x9C: pushv(e, e->eflags, (op_is32(e) ? 32 : 16)); break;
    case 0x9D: {
        if (op_is32(e)) e->eflags = (pop32(e) & 0x00FD5FFFu) | EFLAGS_FIXED;
        else e->flags = (u16)((pop16(e) & 0x0FD5) | 0x0002);
        break;
    }

    /* --- MOV --- */
    case 0x88: case 0x89: {
        int width = op_width(e, op & 1);
        MRM m;
        decode_mrm(e, &m, seg_ov);
        mrm_wv(e, &m, rv(e, m.reg, width), width);
        break;
    }
    case 0x8A: case 0x8B: {
        int width = op_width(e, op & 1);
        MRM m;
        decode_mrm(e, &m, seg_ov);
        wv(e, m.reg, mrm_rv(e, &m, width), width);
        break;
    }
    case 0x8C: { /* MOV r/m16, Sreg */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (m.reg <= 5) mrm_w16(e, &m, seg_reg(e, m.reg));
        break;
    }
    case 0x8E: { /* MOV Sreg, r/m16 */
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (m.reg <= 5) set_seg_reg(e, m.reg, mrm_r16(e, &m));
        break;
    }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        int width = op_width(e, op & 1);
        u16 s = seg_ov >= 0 ? seg_reg(e, seg_ov) : e->ds;
        u32 off = ad_is32(e) ? fetch32(e) : (u32)fetch16(e);
        if (op == 0xA0 || op == 0xA1) wv(e, 0, rd_mem(e, s, off, width), width);
        else wr_mem(e, s, off, rv(e, 0, width), width);
        break;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        w8(e, op & 7, fetch8(e));
        break;
    }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int width = (op_is32(e) ? 32 : 16);
        wv(e, op & 7, fetch_imm(e, width), width);
        break;
    }
    case 0xC6: case 0xC7: {
        int width = op_width(e, op & 1);
        MRM m;
        u32 v;
        decode_mrm(e, &m, seg_ov);
        v = fetch_imm(e, width);
        mrm_wv(e, &m, v, width);
        break;
    }
    case 0x8D: { /* LEA */
        MRM m;
        int width = (op_is32(e) ? 32 : 16);
        decode_mrm(e, &m, seg_ov);
        wv(e, m.reg, m.ea, width);
        break;
    }
    case 0xC4: case 0xC5: { /* LES / LDS */
        MRM m;
        int width = (op_is32(e) ? 32 : 16);
        u32 off2;
        u16 sel;
        decode_mrm(e, &m, seg_ov);
        off2 = rd_mem(e, mrm_seg(e, &m), m.ea, width);
        sel = (u16)rd_mem(e, mrm_seg(e, &m), m.ea + ((u32)width / 8), 16);
        if (op == 0xC4) set_seg_reg(e, SEG_ES, sel);
        else set_seg_reg(e, SEG_DS, sel);
        wv(e, m.reg, off2, width);
        break;
    }
    case 0x84: case 0x85: {
        int width = op_width(e, op & 1);
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (width == 8) logic8(e, (u8)mrm_r8(e, &m), r8(e, m.reg), 0);
        else if (width == 32) logic32(e, mrm_r32(e, &m), r32(e, m.reg), 0);
        else logic16(e, mrm_r16(e, &m), r16(e, m.reg), 0);
        break;
    }
    case 0x86: case 0x87: {
        int width = op_width(e, op & 1);
        MRM m;
        u32 a, b;
        decode_mrm(e, &m, seg_ov);
        a = mrm_rv(e, &m, width);
        b = rv(e, m.reg, width);
        mrm_wv(e, &m, b, width);
        wv(e, m.reg, a, width);
        break;
    }
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97: {
        int width = (op_is32(e) ? 32 : 16);
        int r = op & 7;
        u32 t = rv(e, 0, width);
        wv(e, 0, rv(e, r, width), width);
        wv(e, r, t, width);
        break;
    }
    /* --- string instructions --- */
    case 0xA4: case 0xA5: { /* MOVS */
        int width = (op & 1) ? ((op_is32(e) ? 32 : 16)) : 8;
        u32 sz = (u32)width / 8;
        int back = getf(e, F_DF) ? 1 : 0;
        u32 cnt = rep ? str_cnt(e) : 1;
        u16 srsel = seg_ov >= 0 ? seg_reg(e, seg_ov) : e->ds;
        if (rep && cnt == 0) break;
        while (cnt--) {
            wr_mem(e, e->es, str_di(e), rd_mem(e, srsel, str_si(e), width), width);
            str_adv_si(e, sz, back);
            str_adv_di(e, sz, back);
            if (rep) str_cnt_dec(e);
        }
        break;
    }
    case 0xAA: case 0xAB: { /* STOS */
        int width = (op & 1) ? ((op_is32(e) ? 32 : 16)) : 8;
        u32 sz = (u32)width / 8;
        int back = getf(e, F_DF) ? 1 : 0;
        u32 cnt = rep ? str_cnt(e) : 1;
        if (rep && cnt == 0) break;
        while (cnt--) {
            wr_mem(e, e->es, str_di(e), rv(e, 0, width), width);
            str_adv_di(e, sz, back);
            if (rep) str_cnt_dec(e);
        }
        break;
    }
    case 0xAC: case 0xAD: { /* LODS */
        int width = (op & 1) ? ((op_is32(e) ? 32 : 16)) : 8;
        u32 sz = (u32)width / 8;
        int back = getf(e, F_DF) ? 1 : 0;
        u32 cnt = rep ? str_cnt(e) : 1;
        u16 srsel = seg_ov >= 0 ? seg_reg(e, seg_ov) : e->ds;
        if (rep && cnt == 0) break;
        while (cnt--) {
            wv(e, 0, rd_mem(e, srsel, str_si(e), width), width);
            str_adv_si(e, sz, back);
            if (rep) str_cnt_dec(e);
        }
        break;
    }
    case 0xA6: case 0xA7: { /* CMPS */
        int width = (op & 1) ? ((op_is32(e) ? 32 : 16)) : 8;
        u32 sz = (u32)width / 8;
        int back = getf(e, F_DF) ? 1 : 0;
        u32 cnt = rep ? str_cnt(e) : 1;
        u16 srsel = seg_ov >= 0 ? seg_reg(e, seg_ov) : e->ds;
        if (rep && cnt == 0) break;
        while (cnt--) {
            aluv(e, 7, rd_mem(e, srsel, str_si(e), width),
                 rd_mem(e, e->es, str_di(e), width), width);
            str_adv_si(e, sz, back);
            str_adv_di(e, sz, back);
            if (rep) {
                str_cnt_dec(e);
                if (rep_ne ? !getf(e, F_ZF) : getf(e, F_ZF)) break;
            }
        }
        break;
    }
    case 0xAE: case 0xAF: { /* SCAS */
        int width = (op & 1) ? ((op_is32(e) ? 32 : 16)) : 8;
        u32 sz = (u32)width / 8;
        int back = getf(e, F_DF) ? 1 : 0;
        u32 cnt = rep ? str_cnt(e) : 1;
        if (rep && cnt == 0) break;
        while (cnt--) {
            aluv(e, 7, rv(e, 0, width), rd_mem(e, e->es, str_di(e), width), width);
            str_adv_di(e, sz, back);
            if (rep) {
                str_cnt_dec(e);
                if (rep_ne ? !getf(e, F_ZF) : getf(e, F_ZF)) break;
            }
        }
        break;
    }
    case 0x9E: { u8 ah = (u8)((e->flags & 0xFF) | 0x02); w8(e, 4, ah); break; }   /* SAHF */
    case 0x9F: { e->flags = (u16)((e->flags & 0xFF00) | r8(e, 4) | 0x02); break; } /* LAHF */
    case 0xF5: setf(e, F_CF, !getf(e, F_CF)); break;
    case 0xF8: setf(e, F_CF, 0); break;
    case 0xF9: setf(e, F_CF, 1); break;
    case 0xFA: setf(e, F_IF, 0); break;
    case 0xFB: setf(e, F_IF, 1); break;
    case 0xFC: setf(e, F_DF, 0); break;
    case 0xFD: setf(e, F_DF, 1); break;
    case 0x98: /* CBW / CWDE */
        if (op_is32(e)) e->eax = (u32)(s32)(s16)e->ax;
        else e->ax = (u16)(s8)(u8)e->ax;
        break;
    case 0x99: /* CWD / CDQ */
        if (op_is32(e)) e->edx = (e->eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        else e->dx = (u16)((e->ax & 0x8000) ? 0xFFFF : 0);
        break;
    case 0xD7: { /* XLAT */
        u16 s = seg_ov >= 0 ? seg_reg(e, seg_ov) : e->ds;
        u32 base = ad_is32(e) ? e->ebx : (u32)(u16)e->ebx;
        w8(e, 0, emu_rb32(e, s, base + (u32)r8(e, 0)));
        break;
    }
    case 0x27: { /* DAA */
        u8 al = r8(e, 0);
        u16 ax = e->ax;
        if ((al & 0x0F) > 9 || getf(e, F_AF)) { ax = (u16)(ax + 0x06); setf(e, F_AF, 1); }
        if (al > 0x99 || getf(e, F_CF)) { ax = (u16)(ax + 0x60); setf(e, F_CF, 1); }
        e->ax = ax;
        set_szp8(e, (u8)ax);
        break;
    }
    case 0x2F: { /* DAS */
        u8 al = r8(e, 0);
        u16 ax = e->ax;
        if ((al & 0x0F) > 9 || getf(e, F_AF)) { ax = (u16)(ax - 0x06); setf(e, F_AF, 1); }
        if (al > 0x99 || getf(e, F_CF)) { ax = (u16)(ax - 0x60); setf(e, F_CF, 1); }
        e->ax = ax;
        set_szp8(e, (u8)ax);
        break;
    }
    case 0x37: { /* AAA */
        if ((r8(e, 0) & 0x0F) > 9 || getf(e, F_AF)) {
            e->ax = (u16)(e->ax + 0x106);
            setf(e, F_AF, 1); setf(e, F_CF, 1);
        } else { setf(e, F_AF, 0); setf(e, F_CF, 0); }
        w8(e, 0, (u8)(r8(e, 0) & 0x0F));
        break;
    }
    case 0x3F: { /* AAS */
        if ((r8(e, 0) & 0x0F) > 9 || getf(e, F_AF)) {
            e->ax = (u16)(e->ax - 0x106);
            setf(e, F_AF, 1); setf(e, F_CF, 1);
        } else { setf(e, F_AF, 0); setf(e, F_CF, 0); }
        w8(e, 0, (u8)(r8(e, 0) & 0x0F));
        break;
    }
    case 0xD4: { /* AAM */
        u8 base = fetch8(e);
        u8 al = r8(e, 0);
        if (base == 0) { trap_div(e); break; }
        e->ax = (u16)(((u16)(al / base) << 8) | (al % base));
        set_szp8(e, r8(e, 0));
        break;
    }
    case 0xD5: { /* AAD */
        u8 base = fetch8(e);
        e->ax = (u16)((u16)((u16)r8(e, 0) + (u16)((u16)(e->ax >> 8) * base)) & 0xFF);
        set_szp8(e, r8(e, 0));
        break;
    }
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xC0: case 0xC1: {
        int width = op_width(e, op & 1);
        MRM m;
        unsigned cnt;
        u32 v, r;
        decode_mrm(e, &m, seg_ov);
        if (op == 0xC0 || op == 0xC1) cnt = fetch8(e);
        else if (op == 0xD0 || op == 0xD1) cnt = 1;
        else cnt = (unsigned)(ad_is32(e) ? r8(e, 1) : r8(e, 1));
        v = mrm_rv(e, &m, width);
        r = do_shift(e, m.reg, v, cnt, width);
        mrm_wv(e, &m, r, width);
        if (width == 8) set_szp8(e, (u8)r);
        else if (width == 32) set_szp32(e, r);
        else set_szp16(e, (u16)r);
        break;
    }
    case 0xF6: case 0xF7: {
        int width = op_width(e, op & 1);
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (width == 8) {
            u8 v = (u8)mrm_r8(e, &m);
            switch (m.reg) {
            case 0: case 1: { u8 imm = fetch8(e); logic8(e, v, imm, 0); break; }
            case 2: mrm_w8(e, &m, (u8)~v); break;
            case 3: { u8 r = (u8)(0 - v); setf(e, F_CF, v != 0); set_szp8(e, r); mrm_w8(e, &m, r); break; }
            case 4: { u16 p = (u16)((u16)r8(e, 0) * (u16)v); e->ax = p; setf(e, F_CF, (p >> 8) != 0); setf(e, F_OF, (p >> 8) != 0); break; }
            case 5: { s16 p = (s16)((s16)(s8)r8(e, 0) * (s16)(s8)v); e->ax = (u16)(u8)p; { int trunc = ((p & 0xFF00) == 0 || (p & 0xFF00) == 0xFF00); setf(e, F_CF, !trunc); setf(e, F_OF, !trunc); } break; }
            case 6: { if (!v) { trap_div(e); break; } { u16 n = e->ax; u16 q = (u16)(n / v), rr = (u16)(n % v); if (q > 0xFF) { trap_div(e); break; } e->ax = (u16)((rr << 8) | q); } break; }
            default: { s8 d = (s8)v; if (!d) { trap_div(e); break; } { s32 n = (s16)e->ax; s32 q = n / d, rr = n % d; if (q > 127 || q < -128) { trap_div(e); break; } e->ax = (u16)(((u8)rr << 8) | (u8)q); } break; }
            }
        } else if (width == 16) {
            u16 v = mrm_r16(e, &m);
            switch (m.reg) {
            case 0: case 1: { u16 imm = fetch16(e); logic16(e, v, imm, 0); break; }
            case 2: mrm_w16(e, &m, (u16)~v); break;
            case 3: { u16 r = (u16)(0 - v); setf(e, F_CF, v != 0); set_szp16(e, r); mrm_w16(e, &m, r); break; }
            case 4: { u32 p = (u32)e->ax * v; e->ax = (u16)p; e->dx = (u16)(p >> 16); setf(e, F_CF, e->dx != 0); setf(e, F_OF, getf(e, F_CF)); break; }
            case 5: { s32 p = (s32)(s16)e->ax * (s32)(s16)v; e->ax = (u16)p; e->dx = (u16)((u32)p >> 16); { s32 hi = (s32)(p >> 16); int trunc = (hi == ((p & 0x8000) ? -1 : 0)); setf(e, F_CF, !trunc); setf(e, F_OF, !trunc); } break; }
            case 6: { if (!v) { trap_div(e); break; } { u32 n = ((u32)e->dx << 16) | e->ax; u32 q = n / v, rr = n % v; if (q > 0xFFFF) { trap_div(e); break; } e->ax = (u16)q; e->dx = (u16)rr; } break; }
            default: { s16 d = (s16)v; if (!d) { trap_div(e); break; } { s32 n = (s32)(((u32)e->dx << 16) | e->ax); s32 q = n / d, rr = n % d; if (q > 32767 || q < -32768) { trap_div(e); break; } e->ax = (u16)q; e->dx = (u16)rr; } break; }
            }
        } else {
            u32 v = mrm_r32(e, &m);
            switch (m.reg) {
            case 0: case 1: { u32 imm = fetch32(e); logic32(e, v, imm, 0); break; }
            case 2: mrm_w32(e, &m, ~v); break;
            case 3: { u32 r = 0u - v; setf(e, F_CF, v != 0); set_szp32(e, r); mrm_w32(e, &m, r); break; }
            case 4: { u64 p = (u64)e->eax * (u64)v; e->eax = (u32)p; e->edx = (u32)(p >> 32); setf(e, F_CF, e->edx != 0); setf(e, F_OF, getf(e, F_CF)); break; }
            case 5: { s64 p = (s64)(s32)e->eax * (s64)(s32)v; e->eax = (u32)p; e->edx = (u32)((u64)p >> 32); { s64 hi = p >> 32; int trunc = (hi == ((p & 0x80000000ll) ? -1 : 0)); setf(e, F_CF, !trunc); setf(e, F_OF, !trunc); } break; }
            case 6: { if (!v) { trap_div(e); break; } { u64 n = ((u64)e->edx << 32) | e->eax; u64 q = n / v, rr = n % v; if (q > 0xFFFFFFFFull) { trap_div(e); break; } e->eax = (u32)q; e->edx = (u32)rr; } break; }
            default: { s32 d = (s32)v; if (!d) { trap_div(e); break; } { s64 n = (s64)(s32)e->eax; s64 q = n / d, rr = n % d; if (q > 2147483647ll || q < -2147483648ll) { trap_div(e); break; } e->eax = (u32)q; e->edx = (u32)rr; } break; }
            }
        }
        break;
    }
    case 0xFE: {
        MRM m;
        decode_mrm(e, &m, seg_ov);
        if (m.reg == 0) mrm_w8(e, &m, inc8(e, mrm_r8(e, &m)));
        else mrm_w8(e, &m, dec8(e, mrm_r8(e, &m)));
        break;
    }
    case 0xFF: {
        MRM m;
        int width = (op_is32(e) ? 32 : 16);
        decode_mrm(e, &m, seg_ov);
        switch (m.reg) {
        case 0: mrm_wv(e, &m, incv(e, mrm_rv(e, &m, width), width), width); break;
        case 1: mrm_wv(e, &m, decv(e, mrm_rv(e, &m, width), width), width); break;
        case 2: { /* near call */
            u32 tgt = mrm_rv(e, &m, width);
            pushv(e, e->eip, width);
            e->eip = tgt & ip_mask(e);
            break;
        }
        case 3: { /* far call */
            u32 off = mrm_rv(e, &m, width);
            u16 sel = (u16)rd_mem(e, mrm_seg(e, &m), m.ea + ((u32)width / 8), 16);
            pushv(e, e->cs, width);
            pushv(e, e->eip, width);
            set_cs_ip(e, sel, off);
            break;
        }
        case 4: e->eip = mrm_rv(e, &m, width) & ip_mask(e); break;
        case 5: { /* far jump */
            u32 off = mrm_rv(e, &m, width);
            u16 sel = (u16)rd_mem(e, mrm_seg(e, &m), m.ea + ((u32)width / 8), 16);
            set_cs_ip(e, sel, off);
            break;
        }
        default: pushv(e, mrm_rv(e, &m, width), width); break;
        }
        break;
    }
    case 0xE0: case 0xE1: case 0xE2: { /* LOOPNE / LOOPE / LOOP */
        s8 rel = (s8)fetch8(e);
        if (ad_is32(e)) e->ecx--; else e->cx = (u16)(e->cx - 1);
        {
            u32 c = ad_is32(e) ? e->ecx : (u32)e->cx;
            int take = (c != 0);
            if (take && op == 0xE0) take = !getf(e, F_ZF);
            if (take && op == 0xE1) take = getf(e, F_ZF);
            if (take) jmp_rel(e, rel);
        }
        break;
    }
    case 0xE3: { /* JCXZ / JECXZ */
        s8 rel = (s8)fetch8(e);
        if ((ad_is32(e) ? e->ecx : (u32)e->cx) == 0) jmp_rel(e, rel);
        break;
    }
    case 0xEB: { s8 rel = (s8)fetch8(e); jmp_rel(e, rel); break; }
    case 0xE9: { int width = (op_is32(e) ? 32 : 16); s32 rel = (width == 32) ? fetchs32(e) : (s32)fetchs16(e); jmp_rel(e, rel); break; }
    case 0xEA: { /* far JMP */
        int width = (op_is32(e) ? 32 : 16);
        u32 off = fetch_imm(e, width);
        u16 sel = fetch16(e);
        set_cs_ip(e, sel, off);
        break;
    }
    case 0xE8: { int width = (op_is32(e) ? 32 : 16); s32 rel = (width == 32) ? fetchs32(e) : (s32)fetchs16(e); pushv(e, e->eip, width); jmp_rel(e, rel); break; }
    case 0xC3: e->eip = popv(e, (op_is32(e) ? 32 : 16)) & ip_mask(e); break;
    case 0xC2: { int width = (op_is32(e) ? 32 : 16); u16 n = fetch16(e); e->eip = popv(e, width) & ip_mask(e); e->esp = (e->esp + n) & sp_mask(e); break; }
    case 0xCB: { int width = (op_is32(e) ? 32 : 16); u32 off = popv(e, width); u16 sel = (u16)popv(e, width); set_cs_ip(e, sel, off); break; }
    case 0xCA: { int width = (op_is32(e) ? 32 : 16); u16 n = fetch16(e); u32 off = popv(e, width); u16 sel = (u16)popv(e, width); set_cs_ip(e, sel, off); e->esp = (e->esp + n) & sp_mask(e); break; }
    case 0xCF: { /* IRET */
        int width = (op_is32(e) ? 32 : 16);
        u32 off = popv(e, width);
        u16 sel = (u16)popv(e, width);
        u32 fl = popv(e, width);
        if (width == 32) e->eflags = (fl & 0x00FD5FFFu) | EFLAGS_FIXED;
        else e->flags = (u16)((fl & 0x0FD5) | 0x0002);
        set_cs_ip(e, sel, off);
        break;
    }
    case 0xC8: { /* ENTER */
        u16 size = fetch16(e);
        u8 level = fetch8(e);
        int width = (op_is32(e) ? 32 : 16);
        pushv(e, e->ebp, width);
        u32 frame = ad_is32(e) ? e->ebp : (u32)(u16)e->ebp;
        for (i = 1; i < (int)(level & 0x1F); i++) {
            if (ad_is32(e)) e->ebp -= 4; else e->bp = (u16)(e->bp - 2);
            pushv(e, frame, width);
        }
        pushv(e, frame, width);
        if (ad_is32(e)) e->ebp = e->esp; else e->bp = e->sp;
        e->esp = (e->esp - size) & sp_mask(e);
        break;
    }
    case 0xC9: { /* LEAVE */
        if (ad_is32(e)) { e->esp = e->ebp; e->ebp = pop32(e); }
        else { e->sp = e->bp; e->bp = pop16(e); }
        break;
    }
    case 0xCD: intn = fetch8(e); goto do_int;
    case 0xCC: intn = 3; goto do_int;
    case 0xCE: if (getf(e, F_OF)) { intn = 4; goto do_int; } break;
    case 0x0F: step_386(e, seg_ov); break;
    case 0xF4: e->halted = 1; e->exit_code = 0; break;   /* HLT */
    case 0x9B: break;                                    /* WAIT */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        step_fpu(e, op, seg_ov);
        break;
    case 0xE4: case 0xE5: case 0xEC: case 0xED: {
        u16 port = (op == 0xE4 || op == 0xE5) ? (u16)fetch8(e) : (u16)e->dx;
        u32 val = (u32)emu_in_port(e, port);
        trace_io(port, 1, (u16)val);
        if (op == 0xE5 || op == 0xED) wv(e, 0, val, (op_is32(e) ? 32 : 16));
        else w8(e, 0, (u8)val);
        break;
    }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: {
        u16 port = (op == 0xE6 || op == 0xE7) ? (u16)fetch8(e) : (u16)e->dx;
        u32 val = (op == 0xE7 || op == 0xEF) ? rv(e, 0, (op_is32(e) ? 32 : 16)) : rv(e, 0, 8);
        emu_out_port(e, port, (u16)val);
        trace_io(port, 0, (u16)val);
        break;
    }
    default: {
        plat_puts("UEFI-DOS: unsupported 8086 opcode 0x");
        char hex[3];
        hex[0] = "0123456789ABCDEF"[op >> 4];
        hex[1] = "0123456789ABCDEF"[op & 15];
        hex[2] = 0;
        plat_puts(hex);
        plat_putc('\n');
        e->halted = 1;
        e->exit_code = 0xFF;
        return 0;
    }
    }
    return 0;

do_int: {
    if (e->cr0 & CR0_PE) { do_int_prot(e, intn); return 0; }
    {
        u16 vec_off = emu_rw(e, 0, (u16)(intn * 4));
        u16 vec_seg = emu_rw(e, 0, (u16)(intn * 4 + 2));
        if ((vec_seg == MAGIC_SEG && vec_off == MAGIC_OFF) ||
            (hooked_int(intn) && vec_seg == TRAMP_SEG && vec_off == TRAMP_SLOT(intn))) {
            video_flush(e);
            if (intn == 0x21) int21_handler(e);
            else if (intn == 0x10) int10_handler(e);
            else if (intn == 0x16) int16_handler(e);
            else if (intn == 0x13) int13_handler(e);
            else if (intn == 0x1A) int1a_handler(e);
            else if (intn == 0x2F) int2f_handler(e);
            else if (intn == 0x31) int31_handler(e);
            else if (intn == 0x60) int60_handler(e);
            else { e->halted = 1; e->exit_code = 0; }
            return 0;
        }
        /* program / stub handler: hardware-style interrupt */
        pushv(e, e->eflags, 16);
        push16(e, e->cs);
        push16(e, e->eip);
        set_cs_ip(e, vec_seg, vec_off);
        return 0;
    }
    }
}

/* FPU / 386 dispatch entry point */
static int step(Emu *e)
{
    return step_8086(e);
}

/* single instruction, for the DPMI real-mode reflection loops */
int step_public(Emu *e)
{
    return step_8086(e);
}

void emu_seg_cache_sync(Emu *e) { seg_cache_sync(e); }
void emu_set_seg(Emu *e, int idx, u16 sel) { set_seg_reg(e, idx, sel); }
void emu_push16(Emu *e, u16 v) { push16(e, v); }
u16 emu_pop16(Emu *e) { return pop16(e); }

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */
static int hooked_int(int n)
{
    return n == 0x10 || n == 0x13 || n == 0x16 || n == 0x1A || n == 0x20 ||
           n == 0x21 || n == 0x27 || n == 0x2F || n == 0x31 || n == 0x60;
}

static void install_ivt(Emu *e)
{
    for (int n = 0; n < 256; n++) {
        u16 off, seg;
        if (hooked_int(n)) {
            /* Callable trampoline: "INT n; IRET".  Real BIOS handlers end with
             * IRET, and DOS programs call a vector either as "pushf; call far"
             * or with the "push flags; iret" trick, so IRET fits both. */
            u16 slot = TRAMP_SLOT(n);
            emu_wb(e, TRAMP_SEG, slot, 0xCD);
            emu_wb(e, TRAMP_SEG, (u16)(slot + 1), (u8)n);
            emu_wb(e, TRAMP_SEG, (u16)(slot + 2), 0xCF);
            emu_wb(e, TRAMP_SEG, (u16)(slot + 3), 0x90);
            off = slot;
            seg = TRAMP_SEG;
        } else {
            off = STUB_OFF;
            seg = MAGIC_SEG;
        }
        emu_ww(e, 0, (u16)(n * 4), off);
        emu_ww(e, 0, (u16)(n * 4 + 2), seg);
    }
    /* DPMI real-mode entry point (INT 2Fh AX=1687h hands this out): a
     * far-call here runs "INT 60h", which is the host entry dispatcher. */
    emu_wb(e, DPMI_ENTRY_SEG, DPMI_ENTRY_OFF, 0xCD);
    emu_wb(e, DPMI_ENTRY_SEG, (u16)(DPMI_ENTRY_OFF + 1), 0x60);
    emu_wb(e, DPMI_ENTRY_SEG, (u16)(DPMI_ENTRY_OFF + 2), 0xCB);
    /* IRET stub at F000:FFE0, magic marker at F000:FFF0 */
    emu_wb(e, MAGIC_SEG, STUB_OFF, 0xCF);
    emu_wb(e, MAGIC_SEG, (u16)(STUB_OFF + 1), 0x90);
    emu_wb(e, MAGIC_SEG, MAGIC_OFF, 0xCF);
    /* BIOS data area */
    emu_ww(e, 0x40, 0x13, 640);      /* KB of conventional memory */
    emu_wb(e, 0x40, 0x49, 3);        /* video mode */
    emu_ww(e, 0x40, 0x4A, 80);       /* columns */
    emu_ww(e, 0x40, 0x84, 24);       /* rows-1 */
    emu_ww(e, 0x40, 0x1A, 0);        /* keyboard buffer head */
    emu_ww(e, 0x40, 0x1C, 0);        /* keyboard buffer tail */
}

static void build_psp(Emu *e, u16 psp_seg, const char *argline)
{
    u16 s = psp_seg;
    emu_ww(e, s, 0x00, 0x20CD);     /* INT 20h */
    emu_ww(e, s, 0x02, 0xA000);     /* top of memory */
    emu_wb(e, s, 0x05, 0x9A);       /* far call to DOS */
    emu_ww(e, s, 0x06, 0xF01E);     /* ??? */
    /* termination / ctrl-c / critical-error vectors from IVT */
    for (int i = 0; i < 3; i++) {
        u8 n = (u8)(0x22 + i);
        u16 off = emu_rw(e, 0, (u16)(n * 4));
        u16 seg = emu_rw(e, 0, (u16)(n * 4 + 2));
        emu_ww(e, s, (u16)(0x0A + i * 4), off);
        emu_ww(e, s, (u16)(0x0C + i * 4), seg);
    }
    /* environment */
    u16 env_seg = 0x0B00;
    u32 env_off = (u32)env_seg * 16;
    const char *env = "PATH=C:\\";
    u32 k = 0;
    for (const char *p = env; *p; p++) e->mem[(env_off + k++) & MEM_MASK] = (u8)*p;
    e->mem[(env_off + k++) & MEM_MASK] = 0;
    e->mem[(env_off + k++) & MEM_MASK] = 0;
    emu_ww(e, s, 0x2C, env_seg);
    /* command tail */
    u32 len = 0;
    if (argline && *argline) {
        e->mem[((u32)s * 16 + 0x81) & MEM_MASK] = ' ';
        len = 1;
        for (const char *p = argline; *p && len < 126; p++) {
            e->mem[((u32)s * 16 + 0x81 + len) & MEM_MASK] = (u8)*p;
            len++;
        }
    }
    e->mem[((u32)s * 16 + 0x81 + len) & MEM_MASK] = '\r';
    len++;
    e->mem[((u32)s * 16 + 0x80) & MEM_MASK] = (u8)len;
}

int emu_init(Emu *e)
{
    memset(e, 0, sizeof(*e));
    e->mem = (u8 *)plat_alloc(EMU_RAM_SIZE);
    if (!e->mem) return 1;
    memset(e->mem, 0, EMU_RAM_SIZE);
    e->ram_size = EMU_RAM_SIZE;
    emu_dac_reset(e);
    e->eflags = 0x0202;
    e->video_mode = 3;
    e->cr0 = 0;
    seg_cache_sync(e);          /* real-mode caches: base = sel<<4, limit 64K */
    install_ivt(e);
    return 0;
}

void emu_free(Emu *e)
{
    if (e->mem) plat_free(e->mem);
    e->mem = NULL;
}

static u16 rd16p(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static int load_com_at(Emu *e, u16 psp_seg, const u8 *img, u32 size, const char *args)
{
    if (size > 0xFF00u) return 1;
    build_psp(e, psp_seg, args);
    u32 base = (u32)psp_seg * 16;
    for (u32 i = 0; i < size; i++) e->mem[(base + 0x100 + i) & MEM_MASK] = img[i];
    e->cs = e->ds = e->es = e->ss = psp_seg;
    e->ip = 0x100;
    e->sp = 0xFFFE;
    e->bp = 0; e->si = 0; e->di = 0;
    e->ax = 0; e->bx = 0; e->cx = 0; e->dx = 0;
    e->psp_seg = psp_seg;
    g_dos.dta_seg = psp_seg;        /* DOS default DTA is PSP:0080h */
    g_dos.dta_off = 0x80;
    return 0;
}

static int load_exe_at(Emu *e, u16 psp_seg, const u8 *img, u32 size, const char *args)
{
    if (size < 0x1C) return 1;
    u16 e_crlc = rd16p(img + 0x06);
    u16 e_cparhdr = rd16p(img + 0x08);
    u16 e_ss = rd16p(img + 0x0E);
    u16 e_sp = rd16p(img + 0x10);
    u16 e_ip = rd16p(img + 0x14);
    u16 e_cs = rd16p(img + 0x16);
    u16 e_lfarlc = rd16p(img + 0x18);
    u32 hdr = (u32)e_cparhdr * 16u;
    if (hdr == 0 || hdr >= size) return 1;
    u32 modsize = size - hdr;
    if (modsize > 0xFF00u) return 1;
    build_psp(e, psp_seg, args);
    u16 load_seg = (u16)(psp_seg + 0x10);
    u32 base = (u32)load_seg * 16;
    for (u32 i = 0; i < modsize; i++) e->mem[(base + i) & MEM_MASK] = img[hdr + i];
    if ((u32)e_lfarlc + (u32)e_crlc * 4u <= hdr) {
        for (u32 i = 0; i < e_crlc; i++) {
            u32 off = (u32)e_lfarlc + i * 4u;
            u16 r_off = rd16p(img + off);
            u16 r_seg = rd16p(img + off + 2);
            u32 la = ((u32)load_seg + (u32)r_seg) * 16u + (u32)r_off;
            la &= MEM_MASK;
            u16 v = (u16)(e->mem[la] | (e->mem[(la + 1) & MEM_MASK] << 8));
            v = (u16)(v + load_seg);
            e->mem[la] = (u8)v;
            e->mem[(la + 1) & MEM_MASK] = (u8)(v >> 8);
        }
    }
    e->ss = (u16)(load_seg + e_ss);
    e->sp = e_sp;
    e->cs = (u16)(load_seg + e_cs);
    e->ip = e_ip;
    e->ds = e->es = psp_seg;
    e->ax = 0; e->bx = 0; e->cx = 0; e->dx = 0;
    e->bp = 0; e->si = 0; e->di = 0;
    e->psp_seg = psp_seg;
    g_dos.dta_seg = psp_seg;        /* DOS default DTA is PSP:0080h */
    g_dos.dta_off = 0x80;
    return 0;
}

static int is_mz(const u8 *img, u32 size)
{
    return size >= 2 && img[0] == 'M' && img[1] == 'Z';
}

static int load_program_at(Emu *e, u16 psp_seg, const u8 *img, u32 size, const char *args)
{
    if (is_mz(img, size)) return load_exe_at(e, psp_seg, img, size, args);
    return load_com_at(e, psp_seg, img, size, args);
}

/* Instruction budget for one emu_run() call.  Overridable on the host harness
 * (UEFIDOS_MAX_STEPS) so automated compatibility tests can bound spin loops. */
#ifdef UEFIDOS_HOST
/* ring buffer of the most recent steps, dumped when a program hits the
 * instruction limit so a spin loop can be located without a debugger */
typedef struct { u16 cs, ip, ax, bx, cx, dx, si, di, bp, sp, ds, es, ss; } StepRec;
#define RING_N 64
static StepRec g_ring[RING_N];
static unsigned g_ring_pos, g_ring_cnt;
static void ring_note(Emu *e)
{
    StepRec *r = &g_ring[g_ring_pos % RING_N];
    r->cs = e->cs; r->ip = e->ip;
    r->ax = e->ax; r->bx = e->bx; r->cx = e->cx; r->dx = e->dx;
    r->si = e->si; r->di = e->di; r->bp = e->bp; r->sp = e->sp;
    r->ds = e->ds; r->es = e->es; r->ss = e->ss;
    g_ring_pos++;
    if (g_ring_cnt < RING_N) g_ring_cnt++;
}
static void ring_dump(Emu *e, FILE *f)
{
    fprintf(f, "--- last %u steps ---\n", g_ring_cnt);
    for (unsigned i = g_ring_cnt; i > 0; i--) {
        StepRec *r = &g_ring[(g_ring_pos - i) % RING_N];
        fprintf(f, "%04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X  %02X %02X %02X %02X\n",
                r->cs, r->ip, r->ax, r->bx, r->cx, r->dx, r->si, r->di, r->bp, r->sp, r->ds, r->es, r->ss,
                emu_rb(e, r->cs, r->ip), emu_rb(e, r->cs, (u16)(r->ip+1)),
                emu_rb(e, r->cs, (u16)(r->ip+2)), emu_rb(e, r->cs, (u16)(r->ip+3)));
    }
}
#else
static void ring_note(Emu *e) { (void)e; }
static void ring_dump(Emu *e, void *fp) { (void)e; (void)fp; }
#endif

#ifdef UEFIDOS_HOST
static void trace_regs(Emu *e, u64 steps)
{
    FILE *f = fopen("build/limit_dump.txt", "a");
    if (!f) return;
    fprintf(f, "steps=%llu CS:IP=%04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X FL=%04X\n",
            (unsigned long long)steps, e->cs, e->ip, e->ax, e->bx, e->cx, e->dx,
            e->si, e->di, e->bp, e->sp, e->ds, e->es, e->ss, e->flags);
    fprintf(f, "bytes:");
    for (int i = 0; i < 32; i++) fprintf(f, " %02X", emu_rb(e, e->cs, (u16)(e->ip + i)));
    ring_dump(e, f);
    fprintf(f, "\n");
    fclose(f);
}
#else
static void trace_regs(Emu *e, u64 steps) { (void)e; (void)steps; }
#endif

#ifdef UEFIDOS_HOST
static void trace_step(Emu *e, u64 steps)
{
    static FILE *f = NULL;
    static long left = -1;
    static int win_on = -1;
    static u32 win_cs = 0, win_lo = 0, win_hi = 0xFFFF;
    if (left < 0) {
        const char *s = getenv("UEFIDOS_TRACE_STEPS");
        const char *w = getenv("UEFIDOS_TRACE_WINDOW");
        left = s ? atol(s) : 0;
        win_on = 0;
        if (w) {
            unsigned a = 0, b = 0, c = 0;
            if (sscanf(w, "%x:%x-%x", &c, &a, &b) == 3) { win_cs = c; win_lo = a; win_hi = b; win_on = 1; }
            else if (sscanf(w, "%x-%x", &a, &b) == 2) { win_lo = a; win_hi = b; win_on = 1; }
        }
        if (left || win_on) f = fopen("build/step_trace.txt", "w");
    }
    if (!f) return;
    if (win_on) {
        if (!((win_cs == 0 || e->cs == win_cs) && e->ip >= win_lo && e->ip <= win_hi)) return;
    } else {
        if (left <= 0) return;
        left--;
    }
    fprintf(f, "%6llu %04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X  %02X %02X %02X\n",
            (unsigned long long)steps, e->cs, e->ip, e->ax, e->bx, e->cx, e->dx, e->si, e->di, e->bp, e->sp, e->ds, e->es, e->ss,
            emu_rb(e, e->cs, e->ip), emu_rb(e, e->cs, (u16)(e->ip + 1)), emu_rb(e, e->cs, (u16)(e->ip + 2)));
    fflush(f);
}
#else
static void trace_step(Emu *e, u64 steps) { (void)e; (void)steps; }
#endif

static u64 step_budget(void)
{
#ifdef UEFIDOS_HOST
    static u64 lim = 0;
    if (!lim) {
        const char *s = getenv("UEFIDOS_MAX_STEPS");
        lim = s ? (u64)strtoull(s, 0, 10) : 200000000ull;
        if (!lim) lim = 200000000ull;
    }
    return lim;
#else
    return 200000000ull;
#endif
}

#ifdef UEFIDOS_HOST
/* UEFIDOS_DUMP_VRAM=1: write the emulated 80x25 text screen to
 * build/vram_dump.txt with a column ruler, for exact comparison with a
 * reference screenshot. */
static void dump_vram(Emu *e)
{
    static int on = -1;
    FILE *f;
    if (on < 0) on = getenv("UEFIDOS_DUMP_VRAM") ? 1 : 0;
    if (!on) return;
    f = fopen("build/vram_dump.txt", "w");
    if (!f) return;
    fprintf(f, "     ");
    for (int x = 0; x < 80; x++) fprintf(f, "%d", x % 10);
    fprintf(f, "\n");
    for (int y = 0; y < 25; y++) {
        fprintf(f, "%3d| ", y);
        for (int x = 0; x < 80; x++) {
            u8 ch = emu_rb(e, 0xB800, (u16)((y * 80 + x) * 2));
            if (ch < 32 || ch > 126) ch = ch ? '.' : ' ';
            fputc(ch, f);
        }
        fprintf(f, " |\n");
    }
    fclose(f);
}
#else
static void dump_vram(Emu *e) { (void)e; }
#endif

int emu_run(Emu *e)
{
    u64 steps = 0;
    u64 limit = step_budget();
    u64 total = 0;              /* never reset by keystrokes (host hard cap) */
    u64 hard = 0;
#ifdef UEFIDOS_HOST
    {
        const char *s = getenv("UEFIDOS_HARD_STEPS");
        hard = s ? (u64)strtoull(s, 0, 10) : 0;
    }
#endif
    u32 kbd_seen = g_kbd_seq;
    while (!e->halted && !g_plat_shutdown_requested) {
        trace_step(e, steps);
        ring_note(e);
        step(e);
        steps++;
        total++;
        if (g_video_dirty && (steps & 0x3FFFFull) == 0) video_flush(e);
        if ((steps & 0x3FFull) == 0) emu_kbd_pump(e);
        /* an interactive program that is receiving keys is alive, not stuck:
         * refill its instruction budget so it can never be killed while the
         * user is playing */
        if (g_kbd_seq != kbd_seen) { kbd_seen = g_kbd_seq; steps = 0; }
        if (steps > limit || (hard && total > hard)) {
            trace_regs(e, steps);
            plat_puts("\nUEFI-DOS: program exceeded instruction limit; terminating\n");
            e->halted = 1;
            e->exit_code = 0xFF;
            break;
        }
    }
    video_flush_force(e);
    dump_vram(e);
    g_dos.last_error = 0;
    e->last_exit_code = (u16)e->exit_code;
    return e->exit_code;
}

/* ------------------------------------------------------------------ */
/* program search / reading shared by shell and EXEC                   */
/* ------------------------------------------------------------------ */
static int exists_dos_file(const char *p)
{
    u8 attr;
    if (dos_attr_get(p, &attr) != FAT_OK) return 0;
    return (attr & 0x10) == 0;
}

static int build_candidate(const char *base, const char *ext, char *out, int max)
{
    int n = (int)strlen(base);
    if (n + (ext ? (int)strlen(ext) : 0) + 1 > max) return 0;
    strcpy(out, base);
    if (ext) strcat(out, ext);
    return 1;
}

static int locate_program(const char *name, char *out, int outmax)
{
    const char *exts[3] = { "", ".COM", ".EXE" };
    int has_dot = strchr(name, '.') != NULL;
    int nexts = has_dot ? 1 : 3;
    char cand[DOS_MAX_PATH];
    for (int stage = 0; stage < 2; stage++) {
        char dirs[2][DOS_MAX_PATH];
        int ndirs = 0;
        if (stage == 0) {
            dirs[0][0] = 0; /* current directory: bare name */
            ndirs = 1;
        } else {
            const char *p = g_dos.path_env[0] ? g_dos.path_env : "";
            const char *start = p;
            while (*start && ndirs < 2) {
                const char *e = start;
                while (*e && *e != ';') e++;
                int n = (int)(e - start);
                if (n > 0 && n < DOS_MAX_PATH) {
                    memcpy(dirs[ndirs], start, (size_t)n);
                    dirs[ndirs][n] = 0;
                    ndirs++;
                }
                if (!*e) break;
                start = e + 1;
            }
        }
        for (int d = 0; d < ndirs; d++) {
            for (int k = 0; k < nexts; k++) {
                if (!build_candidate(name, exts[k], cand, sizeof(cand))) continue;
                char full[DOS_MAX_PATH];
                if (dirs[d][0]) {
                    strcpy(full, dirs[d]);
                    strcat(full, "\\");
                    strcat(full, cand);
                } else {
                    strcpy(full, cand);
                }
                if (exists_dos_file(full)) {
                    strncpy(out, full, (size_t)(outmax - 1));
                    out[outmax - 1] = 0;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int read_program(const char *path, u8 **img_out, u32 *size_out)
{
    int h;
    int rc = dos_open(path, 0, &h);
    if (rc != FAT_OK) return rc;
    u32 size = g_dos.handles[h].size;
    if (size == 0 || size > 400000u) {
        dos_close(h);
        return FAT_EINVAL;
    }
    u8 *img = (u8 *)plat_alloc(size);
    if (!img) {
        dos_close(h);
        return FAT_ENOSPC;
    }
    u32 got = 0;
    rc = dos_read(h, img, size, &got);
    dos_close(h);
    if (rc != FAT_OK || got != size) {
        plat_free(img);
        return rc != FAT_OK ? rc : FAT_EIO;
    }
    *img_out = img;
    *size_out = size;
    return FAT_OK;
}

int emu_shell_exec(const char *name, const char *args)
{
    char found[DOS_MAX_PATH];
    if (locate_program(name, found, sizeof(found))) {
        plat_puts(name);
        plat_puts(": Bad command or file name\n");
        return FAT_ENOENT;
    }
    u8 *img = NULL;
    u32 size = 0;
    int rc = read_program(found, &img, &size);
    if (rc != FAT_OK) {
        plat_puts(found);
        plat_puts(": cannot read program\n");
        return rc;
    }
    Emu *e = (Emu *)plat_alloc(sizeof(Emu));
    if (!e) { plat_free(img); return FAT_ENOSPC; }
    if (emu_init(e)) { plat_free(img); plat_free(e); return FAT_ENOSPC; }
    u16 psp_seg = 0x0D00;
#ifdef UEFIDOS_HOST
    {
        const char *ps = getenv("UEFIDOS_PSP_SEG");
        if (ps) { unsigned long v = strtoul(ps, 0, 0); if (v) psp_seg = (u16)v; }
    }
#endif
    if (load_program_at(e, psp_seg, img, size, args)) {
        plat_free(img);
        emu_free(e);
        plat_free(e);
        plat_puts(found);
        plat_puts(": program too large or bad format\n");
        return FAT_EINVAL;
    }
    plat_free(img);
    emu_run(e);
    emu_free(e);
    plat_free(e);
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* EXEC (INT 21h AH=4Bh)                                               */
/* ------------------------------------------------------------------ */
static int g_exec_depth = 0;

int emu_exec_program(Emu *e, const char *prog, const char *args)
{
    char path[DOS_MAX_PATH];
    if (locate_program(prog, path, sizeof(path))) return -1;
    u8 *img = NULL;
    u32 size = 0;
    int rc = read_program(path, &img, &size);
    if (rc != FAT_OK) return (rc == FAT_ENOENT) ? -1 : -2;
    int depth = g_exec_depth;
    if (depth > 7) {
        plat_free(img);
        return -2;
    }
    u16 psp = (u16)(0x2000 + 0x1000 * depth);

    /* snapshot the parent context; the child runs in a new segment */
    Emu saved;
    memcpy(&saved, e, sizeof(Emu));
    u16 s_dta_seg = g_dos.dta_seg, s_dta_off = g_dos.dta_off;
    dos_handle s_handles[DOS_HANDLE_MAX];
    memcpy(s_handles, g_dos.handles, sizeof(s_handles));

    g_exec_depth++;
    int lr = load_program_at(e, psp, img, size, args);
    plat_free(img);
    if (lr) {
        g_exec_depth--;
        return -2;
    }
    g_dos.dta_seg = psp;
    g_dos.dta_off = 0x80;
    e->halted = 0;
    e->exit_code = 0;
    emu_run(e);
    int code = e->exit_code;

    memcpy(e, &saved, sizeof(Emu));
    g_dos.dta_seg = s_dta_seg;
    g_dos.dta_off = s_dta_off;
    memcpy(g_dos.handles, s_handles, sizeof(s_handles));
    g_exec_depth--;
    return code;
}
