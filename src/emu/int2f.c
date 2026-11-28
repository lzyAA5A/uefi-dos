/*
 * UEFI-DOS -- INT 2Fh native multiplex: the 16-bit -> 32/64-bit upcall.
 *
 * A program running in the 8086 compatibility machine (e.g. a .COM file)
 * can ask the native UEFI-DOS side for services that only make sense in
 * 64/32-bit code (UEFI time, the native API banner, capability probing).
 *
 *   AX = 5544h ('UD', UEFI-DOS multiplex)
 *   DX = 0001h   get native API info
 *                -> AX = API version (0100h = 1.00), BX = pointer size of
 *                   the native side (4 or 8), CX = 0
 *   DX = 0002h   native side prints its API banner
 *                -> AX = banner length
 *   DX = 0003h   read the firmware RTC through the native platform layer
 *                -> ES:BX[0]=hour, [1]=minute, [2]=second, AX = 0
 *   other        -> AX = FFFFh (not supported)
 */
#include "uefidos.h"
#include "emu/emu.h"
#include "api/dosapi.h"
#include "plat.h"

#define INT2F_UEFIDOS 0x5544u

void int2f_handler(Emu *e)
{
    /* DPMI installation check (INT 2Fh AX=1687h): the UEFI-DOS emulator is
     * the DPMI host, so 32-bit clients can bootstrap here. */
    if (e->ax == 0x1687) { dpmi_install_check(e); return; }

    /* XMS driver installation check and entry point (the standard way every
     * program since HIMEM.SYS finds extended memory, and what Windows 2.x and
     * every DOS extender ask for before they do anything else). */
    if (e->ax == 0x4300) {
        e->ax = (u16)((e->ax & 0xFF00) | 0x80);   /* AL=80h: XMS installed */
        return;
    }
    if (e->ax == 0x4310) {
        /* a far-callable stub in the service area: "INT E1h; RETF".  The INT
         * handler does the work and IRETs back here, and the RETF returns to
         * the caller -- exactly the ABI an XMS driver has. */
        u32 at = ((u32)0xF000u << 4) + 0xF000u;
        e->mem[at + 0] = 0xCD;                   /* int E1h */
        e->mem[at + 1] = 0xE1;
        e->mem[at + 2] = 0xCB;                   /* retf */
        e->es = 0xF000;
        e->bx = 0xF000;
        return;
    }

    /* Not ours: leave every register untouched, exactly like a real DOS that
     * does not implement the multiplex function (the caller decides). */
    if (e->ax != INT2F_UEFIDOS) return;
    switch (e->dx) {
    case 0x0001:
        e->ax = (u16)(DOSAPI_VERSION & 0xFFFFu);
        e->bx = (u16)sizeof(void *);
        e->cx = 0;
        return;
    case 0x0002: {
        /* the banner reports the live version, so bumping DOSAPI_VERSION can
         * never leave a stale string behind */
        plat_puts("UEFI-DOS native API v");
        plat_putc((char)('0' + ((DOSAPI_VERSION >> 8) & 0xF)));
        plat_putc('.');
        plat_putc((char)('0' + ((DOSAPI_VERSION >> 4) & 0xF)));
        plat_putc((char)('0' + (DOSAPI_VERSION & 0xF)));
        plat_putc('\n');
        e->ax = 27;
        return;
    }
    case 0x0003: {
        int hh = 0, mm = 0, ss = 0;
        plat_get_time(&hh, &mm, &ss);
        emu_wb(e, e->es, e->bx, (u8)hh);
        emu_wb(e, e->es, (u16)(e->bx + 1), (u8)mm);
        emu_wb(e, e->es, (u16)(e->bx + 2), (u8)ss);
        e->ax = 0;
        return;
    }
    default:
        /* unknown UEFI-DOS subfunction: report "not supported" in AX but keep
         * the other registers as the caller left them */
        e->ax = 0xFFFFu;
        return;
    }
}


/* ---- XMS driver (INT E1h, reached through the F000:F000 stub) ---------- *
 *
 * The standard extended-memory API.  Everything here is backed by the sparse
 * pool in emu.c: an allocation is a byte range in a pool whose pages only cost
 * firmware memory when a program writes to them, so "8 GB" is 8 GB of address
 * space and whatever the program actually touches.
 *
 * Errors go to BL: A0 all extended memory locked, A1 out of handles, A2 invalid
 * handle, A3/A4 source handle/offset invalid, A5/A6 destination handle/offset
 * invalid, A7 insufficient memory, A8 block too large, 8B page frame overrun.
 */
#ifdef UEFIDOS_HOST
#include <stdio.h>
#include <stdlib.h>
static int xms_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("UEFIDOS_TRACE_XMS") ? 1 : 0;
    return on;
}
#define XMS_NOTE(...) do { if (xms_trace()) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define XMS_NOTE(...) ((void)0)
#endif

#define XMS_ERR(e, code) do { (e)->ax = 0; \
    (e)->bx = (u16)(((e)->bx & 0xFF00u) | (code)); } while (0)

static u32 xms_emb_dword(Emu *e, u16 seg, u16 off)
{
    return emu_rd(e, seg, off);
}

void int_e1_handler(Emu *e)
{
    u8 fn = (u8)(e->ax >> 8);
    u16 h;

    switch (fn) {
    case 0x00:                                    /* get version */
        e->ax = 0x0300;                           /* XMS 3.00: 32-bit sizes */
        e->bx = 0x0001;
        e->dx = 0x0001;
        return;
    case 0x01:                                    /* request HMA */
        e->ax = emu_xms_hma_claim() ? 1 : 0;
        return;
    case 0x02:                                    /* release HMA */
        e->ax = emu_xms_hma_release() ? 1 : 0;
        return;
    case 0x03: case 0x05:                         /* global / local A20 on */
        e->a20_on = 1;
        e->ax = 1;
        return;
    case 0x04: case 0x06:                         /* global / local A20 off */
        e->a20_on = 0;
        e->ax = 1;
        return;
    case 0x07:                                    /* query A20 */
        e->ax = e->a20_on ? 1 : 0;
        return;
    case 0x08: {                                  /* query free (16-bit KB) */
        u32 free_kb = emu_xms_free_kb();
        e->ax = (u16)(free_kb > 0xFFFFu ? 0xFFFFu : free_kb);   /* largest */
        e->dx = e->ax;                             /* total (same: one arena) */
        e->bl = 0;
        return;
    }
    case 0x88: {                                  /* query any free (32-bit) */
        u32 free_kb = emu_xms_free_kb();
        e->eax = free_kb;
        e->edx = free_kb;
        e->bl = 0;                                /* no HMA-free flag games */
        return;
    }
    case 0x09:                                    /* allocate (DX = KB) */
        if (!emu_xms_alloc((u32)e->dx, &h)) { XMS_ERR(e, 0xA7); return; }
        e->ax = 1;
        e->dx = h;
        return;
    case 0x89: {                                  /* allocate any (EDX = KB) */
        u32 kb = e->edx;
        if (!emu_xms_alloc(kb, &h)) { XMS_ERR(e, 0xA7); return; }
        e->ax = 1;
        e->dx = h;
        return;
    }
    case 0x0A:                                    /* free block (DX = handle) */
        if (!emu_xms_free(e->dx)) { XMS_ERR(e, 0xA2); return; }
        e->ax = 1;
        return;
    case 0x0B: {                                  /* move EMB (DS:SI) */
        u16 seg = e->ds, off = e->si;
        u32 len = xms_emb_dword(e, seg, off);
        u16 src_h = emu_rw(e, seg, (u16)(off + 4));
        u32 src_o = xms_emb_dword(e, seg, (u16)(off + 6));
        u16 dst_h = emu_rw(e, seg, (u16)(off + 10));
        u32 dst_o = xms_emb_dword(e, seg, (u16)(off + 12));
        u64 src_b = 0, dst_b = 0;
        u32 src_s = 0, dst_s = 0;
        int src_ext = src_h != 0, dst_ext = dst_h != 0;
        /* a move may not cross a 64 KB boundary on either side */
        if (!src_ext) {
            if ((src_o & 0xFFFFu) + len > 0x10000u) { XMS_ERR(e, 0x8B); return; }
        } else if (!emu_xms_block(src_h, &src_b, &src_s)) {
            XMS_ERR(e, 0xA3); return;
        } else if (src_o >= src_s || len > src_s - src_o) {
            XMS_ERR(e, 0xA4); return;
        }
        if (!dst_ext) {
            if ((dst_o & 0xFFFFu) + len > 0x10000u) { XMS_ERR(e, 0x8B); return; }
        } else if (!emu_xms_block(dst_h, &dst_b, &dst_s)) {
            XMS_ERR(e, 0xA5); return;
        } else if (dst_o >= dst_s || len > dst_s - dst_o) {
            XMS_ERR(e, 0xA6); return;
        }
        /* handle 0 is conventional memory: the 32-bit offset is seg:off, so the
         * high word is the segment and the low word the offset */
        XMS_NOTE("[xms] move len=%u src=%s%u(+%u) dst=%s%u(+%u)\n",
                 (unsigned)len,
                 src_ext ? "emb" : "mem", (unsigned)src_h, (unsigned)src_o,
                 dst_ext ? "emb" : "mem", (unsigned)dst_h, (unsigned)dst_o);
        emu_xms_copy(e,
                     dst_ext ? dst_b + dst_o : 0u,
                     (u16)(dst_o >> 16), (u32)(dst_o & 0xFFFFu), dst_ext,
                     src_ext ? src_b + src_o : 0u,
                     (u16)(src_o >> 16), (u32)(src_o & 0xFFFFu), src_ext,
                     len);
        XMS_NOTE("[xms] move done, first bytes: %02X %02X %02X %02X / %02X %02X\n",
                 emu_xms_rb(dst_ext ? dst_b + dst_o : src_b + src_o),
                 emu_xms_rb((dst_ext ? dst_b + dst_o : src_b + src_o) + 1),
                 emu_xms_rb((dst_ext ? dst_b + dst_o : src_b + src_o) + 2),
                 emu_xms_rb((dst_ext ? dst_b + dst_o : src_b + src_o) + 3),
                 emu_xms_rb(src_b + src_o),
                 emu_xms_rb(src_b + src_o + 1));
        e->ax = 1;
        return;
    }
    case 0x0C: case 0x0D: {                       /* lock / unlock */
        u64 base = 0;
        if (!emu_xms_block(e->dx, &base, NULL)) { XMS_ERR(e, 0xA2); return; }
        e->ax = 1;
        e->bx = (u16)(base >> 16);                /* "physical" high word */
        e->dx = (u16)(base & 0xFFFFu);
        return;
    }
    case 0x0E: {                                  /* handle info */
        u32 size = 0;
        if (!emu_xms_block(e->dx, NULL, &size)) { XMS_ERR(e, 0xA2); return; }
        e->bh = 0;                                /* lock count */
        e->bl = emu_xms_free_handles();
        e->dx = (u16)(size / 1024u > 0xFFFFu ? 0xFFFFu : size / 1024u);
        e->ax = 1;
        return;
    }
    case 0x10:                                    /* request UMB: none */
        e->ax = 0;
        return;
    default:
        XMS_ERR(e, 0x80);                         /* function not implemented */
        return;
    }
}
