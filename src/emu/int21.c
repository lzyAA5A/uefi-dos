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
    static int trace_all = -1;
    if (g_trace_enabled < 0) {
        g_trace_enabled = getenv("UEFIDOS_TRACE_INTS") ? 1 : 0;
        if (g_trace_enabled) g_trace = fopen("build/int_trace.txt", "w");
    }
    if (!g_trace_enabled || !g_trace) return;
    g_trace_n++;
    if (trace_all < 0) trace_all = getenv("UEFIDOS_TRACE_ALL") ? 1 : 0;
    if (trace_all || g_trace_n <= 300 || (g_trace_n % 1000) == 0) {
        fprintf(g_trace, "%7lu INT%02X AH=%02X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X ES=%04X",
                g_trace_n, intno, (unsigned)(e->ax >> 8), e->ax, e->bx, e->cx, e->dx,
                e->si, e->di, e->es);
        /* Path-taking calls: printing the ASCIIZ string turns "DOS error 2"
         * into "it wanted this file, from this directory". */
        if (intno == 0x21) {
            u8 ah = (u8)(e->ax >> 8);
            int has_path = (ah == 0x39 || ah == 0x3A || ah == 0x3B || ah == 0x3C ||
                            ah == 0x3D || ah == 0x41 || ah == 0x43 || ah == 0x4B ||
                            ah == 0x4E || ah == 0x56 || ah == 0x5B || ah == 0x60);
            if (has_path) {
                char buf[80];
                u16 n = 0;
                while (n < sizeof(buf) - 1) {
                    u8 ch = emu_rb(e, e->ds, (u16)(e->dx + n));
                    if (!ch) break;
                    buf[n++] = (char)(ch >= 32 && ch < 127 ? ch : '?');
                }
                buf[n] = 0;
                fprintf(g_trace, "  \"%s\"", buf);
            }
        }
        fprintf(g_trace, "\n");
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

/* Error bookkeeping.  Every failed call must also leave a record behind, because
 * DOS programs routinely follow a CF=1 return with INT 21h AH=59h (extended
 * error) to get the failure class and the suggested action. */
static void set_err(Emu *e, u8 code)
{
    e->flags |= F_CF;
    e->ax = code;
    g_dos.last_error = code;
    g_dos.last_error_valid = 1;
#ifdef UEFIDOS_HOST
    if (g_trace_enabled == 1 && g_trace) { fprintf(g_trace, "        -> DOS error %u\n", code); fflush(g_trace); }
#endif
}
static void set_ok(Emu *e)
{
    e->flags = (u16)(e->flags & ~F_CF);
#ifdef UEFIDOS_HOST
    /* a failed call right after the trace line is what a compatibility run
     * actually needs to see: "AH=3D ... SIM.OVL" + "-> DOS error 2" */
    if (g_trace_enabled == 1 && g_trace) { fprintf(g_trace, "        -> ok\n"); fflush(g_trace); }
#else
    (void)e;
#endif
}

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

/* ------------------------------------------------------------------ */
/* DOS 4.0 data structures                                             */
/* ------------------------------------------------------------------ */
/* F000:C000.. is the reserved ROM window: no guest code ever runs there, so
 * it is a safe home for the structures DOS programs ask us for a pointer to. */
#define DOS_LOL_OFF  0xC000u   /* INT 21h AH=52h: list of lists */
#define DOS_DPB_OFF  0xC100u   /* INT 21h AH=32h: one DPB per mounted drive */
#define DOS_DPB_LEN  32
#define DOS_SFT_OFF  0xC800u   /* SFT chain (list of lists +4 points here) */
#define DOS_SFT_STRIDE 0x3Bu   /* DOS 3.1+/4.0 SFT entry: 0x35 + name */
#define DOS_SFT_MAX  24
#define DOS_SFT_END  0x0000FFFFu  /* offset FFFFh / segment 0000h */
#define DOS_SDA_OFF  0xDC00u   /* swappable data area, INT 21h AH=5D06h */
#define DOS_SDA_LEN  0x100
#define DOS_SDA_LIST 0xDD00u   /* DOS 4.0 style SDA list, AH=5D0Bh */

static int boot_drive(void)
{
    for (int d = 0; d < DOS_MAX_DRIVES; d++)
        if (g_dos.drives[d].mounted) return d;
    return 2;
}

static u16 dpb16(u32 v)
{
    return (u16)(v > 0xFFFFu ? 0xFFFFu : v);
}

/* ------------------------------------------------------------------ */
/* DOS memory arena (INT 21h AH=48h/49h/4Ah/58h)                       */
/* ------------------------------------------------------------------ */
/* Memory control blocks live in the emulated RAM itself, so tools that walk
 * the chain (AH=52h hands them the first one) see a normal DOS arena:
 *   +0 'M' (more blocks) or 'Z' (last), +1 owner PSP (0 = free),
 *   +3 size in paragraphs, data starts at mcb+1. */
#define MCB_MORE 'M'
#define MCB_LAST 'Z'

static u16 mcb_next_seg(Emu *e, u16 seg)
{
    if (emu_rb(e, seg, 0) == (u8)MCB_LAST) return 0;
    return (u16)(seg + 1 + emu_rw(e, seg, 3));
}

/* Walk the arena; returns the MCB segment of the first block that satisfies
 * `need` paragraphs (0 = none).  max_free gets the largest free block. */
static u16 arena_find(Emu *e, u16 need, u16 *max_free)
{
    u16 seg = g_dos.mem_base;
    u16 found = 0;
    if (max_free) *max_free = 0;
    if (!seg) return 0;
    for (int guard = 0; guard < 256 && seg; guard++) {
        u8 tag = emu_rb(e, seg, 0);
        u16 owner = emu_rw(e, seg, 1);
        u16 size = emu_rw(e, seg, 3);
        if (owner == 0) {
            if (!found && size >= need) found = seg;
            if (max_free && size > *max_free) *max_free = size;
        }
        if (tag == (u8)MCB_LAST) break;
        u16 nx = (u16)(seg + 1 + size);
        if (nx <= seg) break;
        seg = nx;
    }
    return found;
}

static int arena_alloc(Emu *e, u16 need, u16 *out_ds, u16 *out_max)
{
    u16 max_free = 0;
    if (need == 0) need = 1;
    u16 blk = arena_find(e, need, &max_free);
    if (out_max) *out_max = max_free;
    if (!blk) return 0;
    u16 size = emu_rw(e, blk, 3);
    u8 tag = emu_rb(e, blk, 0);
    if (size > need) {                        /* split: keep a free tail */
        u16 rest = (u16)(blk + 1 + need);
        emu_wb(e, blk, 0, (u8)MCB_MORE);
        emu_wb(e, rest, 0, tag);
        emu_ww(e, rest, 1, 0);
        emu_ww(e, rest, 3, (u16)(size - need - 1));
        emu_ww(e, blk, 3, need);
    }
    emu_ww(e, blk, 1, e->psp_seg);
    *out_ds = (u16)(blk + 1);
    return 1;
}

static int arena_free(Emu *e, u16 ds)
{
    u16 seg = g_dos.mem_base;
    if (!seg) return 0;
    for (int guard = 0; guard < 256 && seg; guard++) {
        u16 size = emu_rw(e, seg, 3);
        if ((u16)(seg + 1) == ds) {
            emu_ww(e, seg, 1, 0);
            /* merge with following blocks while they are free */
            while (emu_rb(e, seg, 0) != (u8)MCB_LAST) {
                u16 nx = (u16)(seg + 1 + size);
                if (nx <= seg) break;
                if (emu_rw(e, nx, 1) != 0) break;
                u8 ntag = emu_rb(e, nx, 0);
                u16 nsize = emu_rw(e, nx, 3);
                emu_wb(e, seg, 0, ntag);
                size = (u16)(size + 1 + nsize);
                emu_ww(e, seg, 3, size);
            }
            return 1;
        }
        seg = mcb_next_seg(e, seg);
    }
    return 0;
}

/* AH=4Ah -- resize a memory block.  ES:0000 is the block's data segment; a
 * program resizes its own block with ES = PSP (the MCB sits at PSP-1), which
 * is how every DOS startup shrinks the block DOS gave it and then allocates
 * the tail.  Growing absorbs following free blocks; shrinking splits them off
 * as a new free block. */
static int arena_resize(Emu *e, u16 ds, u16 want, u16 *out_max)
{
    u16 seg = g_dos.mem_base;
    for (int guard = 0; guard < 256 && seg; guard++) {
        u16 size = emu_rw(e, seg, 3);
        if ((u16)(seg + 1) == ds) {
            u16 owner = emu_rw(e, seg, 1);
            if (owner != e->psp_seg && owner != 0) { if (out_max) *out_max = 0; return 0; }
            /* how large can this block become?  absorb free neighbours */
            u16 total = size, probe = seg;
            while (emu_rb(e, probe, 0) != (u8)MCB_LAST) {
                u16 nx = (u16)(probe + 1 + emu_rw(e, probe, 3));
                if (nx <= probe) break;
                if (emu_rw(e, nx, 1) != 0) break;
                total = (u16)(total + 1 + emu_rw(e, nx, 3));
                probe = nx;
            }
            if (want > total) { if (out_max) *out_max = total; return 0; }
            u8 last_tag = emu_rb(e, probe, 0);
            if (want == total) {              /* take the whole run */
                emu_wb(e, seg, 0, last_tag);
                emu_ww(e, seg, 3, want);
            } else {                          /* split: free tail block */
                u16 rest = (u16)(seg + 1 + want);
                emu_wb(e, seg, 0, (u8)MCB_MORE);
                emu_ww(e, seg, 3, want);
                emu_wb(e, rest, 0, last_tag);
                emu_ww(e, rest, 1, 0);
                emu_ww(e, rest, 3, (u16)(total - want - 1));
            }
            if (out_max) *out_max = want;
            return 1;
        }
        seg = mcb_next_seg(e, seg);
    }
    if (out_max) *out_max = 0;
    return 0;
}

static u16 dpb_write(Emu *e, int slot, int drive, u16 next)
{
    fat_vol *v = &g_dos.drives[drive].vol;
    u16 off = (u16)(DOS_DPB_OFF + slot * DOS_DPB_LEN);
    u16 spc = v->spc ? (u16)v->spc : 1u;
    u8 shift = 0;
    while (shift < 8 && (u16)(1u << shift) < spc) shift++;
    u32 data_sec = v->data_lba > v->part_lba ? v->data_lba - v->part_lba : 0;
    u32 root_sec = v->root_lba > v->part_lba ? v->root_lba - v->part_lba : 0;
    for (int i = 0; i < DOS_DPB_LEN; i++) emu_wb(e, MAGIC_SEG, (u16)(off + i), 0);
    emu_wb(e, MAGIC_SEG, (u16)(off + 0x00), (u8)drive);         /* drive */
    emu_wb(e, MAGIC_SEG, (u16)(off + 0x01), (u8)drive);         /* unit */
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x02), (u16)(v->bytes_per_sector ? v->bytes_per_sector : 512));
    emu_wb(e, MAGIC_SEG, (u16)(off + 0x04), (u8)(spc - 1));
    emu_wb(e, MAGIC_SEG, (u16)(off + 0x05), shift);
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x06), dpb16(v->reserved));
    emu_wb(e, MAGIC_SEG, (u16)(off + 0x08), (u8)v->nfats);
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x09), dpb16(v->root_entries));
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x0B), dpb16(data_sec));   /* first data sector */
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x0D), dpb16(v->cluster_count + 1));
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x0F), dpb16(v->fat_sectors));
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x11), dpb16(root_sec));
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x13), 0xFFFF);            /* no driver */
    emu_wb(e, MAGIC_SEG, (u16)(off + 0x15), (u8)(v->cluster_count < 4085u ? 0xF0 : 0xF8));
    emu_wb(e, MAGIC_SEG, (u16)(off + 0x16), 0x00);              /* accessed */
    emu_ww(e, MAGIC_SEG, (u16)(off + 0x17), next);              /* next DPB */
    return off;
}

/* Rebuild the DPB chain and return the offset of the first entry (0 if no
 * volume is mounted).  Walking downwards keeps the chain ascending. */
static u16 dpb_chain(Emu *e)
{
    u16 head = 0;
    for (int d = DOS_MAX_DRIVES - 1; d >= 0; d--) {
        if (!g_dos.drives[d].mounted) continue;
        head = dpb_write(e, d, d, head);
    }
    return head;
}

static void ext_error(u16 code, u8 *cls, u8 *act, u8 *locus);

/* The DOS-internal blocks below (SFT, swappable data area, job file table)
 * live at fixed real-mode addresses.  INT 21h can also be called from a
 * protected-mode DPMI client, where emu_wb(e, MAGIC_SEG, ...) would interpret
 * 0F000h as a selector and latch a protection fault, so these writes go by
 * linear address instead. */
static void mag_wb(Emu *e, u16 off, u8 v)
{
    u32 lin = ((u32)MAGIC_SEG << 4) + off;
    if (lin < e->ram_size) e->mem[lin] = v;
}

static u8 mag_rb(Emu *e, u16 off)
{
    u32 lin = ((u32)MAGIC_SEG << 4) + off;
    return lin < e->ram_size ? e->mem[lin] : 0;
}

static void mag_ww(Emu *e, u16 off, u16 v)
{
    mag_wb(e, off, (u8)v);
    mag_wb(e, (u16)(off + 1), (u8)(v >> 8));
}

static void mag_wd(Emu *e, u16 off, u32 v)
{
    mag_ww(e, off, (u16)v);
    mag_ww(e, (u16)(off + 2), (u16)(v >> 16));
}

static void psp_wb(Emu *e, u16 seg, u16 off, u8 v)
{
    u32 lin = ((u32)seg << 4) + off;
    if (lin < e->ram_size) e->mem[lin] = v;
}

/* ---- System File Table ---------------------------------------------- *
 * DOS 3.1+/4.x keep one SFT entry per open handle and chain them with a far
 * pointer in the first four bytes of each entry; the list of lists points at
 * the first block, and the last block's link is FFFFh:0000h.  Multitaskers walk
 * that chain (Windows 1.x/2.x and DESQview do) to size their handle tables and
 * to keep private state in the unused terminator entry, which is why the
 * terminator has to stay writable RAM.  Handles map one to one onto entries, so
 * the table is simply rebuilt whenever AH=52h asks for it.
 *
 * Field offsets follow the documented DOS 3.1-4.0 entry: +00 next entry,
 * +04 handle count, +06 open mode, +08 attribute, +09 device info word,
 * +0B starting cluster, +0D time, +0F date, +11 current position, +2E 8.3 name.
 */
static void sft_entry_write(Emu *e, int h, u16 off, u32 next)
{
    dos_handle *H = &g_dos.handles[h];
    int used = (h < 5) || H->used;
    u16 info = 0;

    mag_wd(e, off, next);
    mag_ww(e, (u16)(off + 0x04), (u16)(used ? 1 : 0));
    mag_ww(e, (u16)(off + 0x06), used ? (u16)H->open_mode : 0);
    mag_wb(e, (u16)(off + 0x08), used ? H->attr : 0);
    if (H->console == 1) info = 0x0080;             /* CON */
    else if (H->console == 2) info = 0x0082;        /* PRN / AUX */
    else if (used) info = (u16)(H->drive + 1);      /* disk file: drive number */
    mag_ww(e, (u16)(off + 0x09), info);
    mag_ww(e, (u16)(off + 0x0B), used && !H->console ? (u16)H->cluster : 0);
    mag_ww(e, (u16)(off + 0x0D), 0);                /* time stamp */
    mag_ww(e, (u16)(off + 0x0F), 0);                /* date stamp */
    mag_wd(e, (u16)(off + 0x11), used ? H->pos : 0);
    for (int i = 0; i < 11; i++)
        mag_wb(e, (u16)(off + 0x2E + i), used && !H->console ? H->name11[i] : 0);
}

/* Refresh the job file table at PSP:18h.  DOS stores, for each of the 20
 * handle slots, the SFT index of the open file (0FFh when the slot is free);
 * our SFT entries are numbered exactly like the handles, so the entry is its
 * own index.  Programs that read the PSP directly instead of asking DOS --
 * Windows 1.x does exactly that while it builds its own handle table -- use
 * it to decide which handles are live, so it has to stay current after every
 * open, create and close. */
void dos_jft_sync(Emu *e)
{
    u16 psp = g_dos.psp_seg;
    if (!psp) return;
    for (int i = 0; i < 20; i++) {
        int used = (i < 5) || (i < g_dos.files_limit && g_dos.handles[i].used);
        psp_wb(e, psp, (u16)(0x18 + i), used ? (u8)i : 0xFF);
    }
}

static u16 sft_build(Emu *e)
{
    int n = g_dos.files_limit;
    if (n < 6) n = 6;
    if (n > DOS_SFT_MAX) n = DOS_SFT_MAX;
    mag_wd(e, DOS_SFT_OFF, DOS_SFT_END);                  /* end of the chain */
    mag_ww(e, (u16)(DOS_SFT_OFF + 4), (u16)n);
    for (int i = 0; i < n; i++) {
        u16 off = (u16)(DOS_SFT_OFF + 6 + i * DOS_SFT_STRIDE);
        u32 next = (i + 1 < n)
                 ? (((u32)MAGIC_SEG << 16) | (u16)(off + DOS_SFT_STRIDE))
                 : DOS_SFT_END;
        sft_entry_write(e, i, off, next);
    }
    return DOS_SFT_OFF;
}

/* ---- Swappable data area (INT 21h AH=5D06h / 5D0Bh) ----------------- *
 * Offsets are the documented DOS 3.10-3.30 (and 4.x) layout: +00 critical
 * error flag, +01 InDOS, +02 drive of last critical error, +03 error locus,
 * +04 extended error, +06 suggested action, +07 error class, +0C current DTA,
 * +10 current PSP, +16 current drive, +17 extended break flag.  Windows 1.01
 * grabs this area and swaps the DTA/PSP/drive fields per application.
 */
static void sda_build(Emu *e)
{
    for (int i = 0; i < DOS_SDA_LEN; i++)
        mag_wb(e, (u16)(DOS_SDA_OFF + i), 0);
    mag_wb(e, (u16)(DOS_SDA_OFF + 0x00), 0x00);
    mag_wb(e, (u16)(DOS_SDA_OFF + 0x01), 0x00);
    mag_wb(e, (u16)(DOS_SDA_OFF + 0x02), 0xFF);
    if (g_dos.last_error_valid && g_dos.last_error) {
        u8 cls = 0, act = 0, locus = 0;
        ext_error(g_dos.last_error, &cls, &act, &locus);
        mag_wb(e, (u16)(DOS_SDA_OFF + 0x03), locus);
        mag_ww(e, (u16)(DOS_SDA_OFF + 0x04), g_dos.last_error);
        mag_wb(e, (u16)(DOS_SDA_OFF + 0x06), act);
        mag_wb(e, (u16)(DOS_SDA_OFF + 0x07), cls);
    }
    mag_ww(e, (u16)(DOS_SDA_OFF + 0x0C), g_dos.dta_off);
    mag_ww(e, (u16)(DOS_SDA_OFF + 0x0E), g_dos.dta_seg);
    mag_ww(e, (u16)(DOS_SDA_OFF + 0x10), g_dos.psp_seg);
    mag_ww(e, (u16)(DOS_SDA_OFF + 0x12), 0xFFFE);
    mag_ww(e, (u16)(DOS_SDA_OFF + 0x14), 0x0000);
    mag_wb(e, (u16)(DOS_SDA_OFF + 0x16), (u8)dos_current_drive());
    mag_wb(e, (u16)(DOS_SDA_OFF + 0x17), 0x00);
}

static void ext_error(u16 code, u8 *cls, u8 *act, u8 *locus)
{
    switch (code) {
    case 0x02: case 0x03: case 0x0F:                     /* not found      */
        *cls = 8; *act = 1; *locus = 1; break;
    case 0x04: case 0x08:                                /* out of handles/memory */
        *cls = 1; *act = 1; *locus = (code == 0x08) ? 5 : 1; break;
    case 0x05: case 0x21: case 0x32:                     /* access/sharing */
        *cls = 3; *act = 4; *locus = 1; break;
    case 0x13: case 0x1E: case 0x1F: case 0x28:          /* media/protect  */
        *cls = 5; *act = 4; *locus = 2; break;
    case 0x27:                                           /* disk full      */
        *cls = 11; *act = 4; *locus = 2; break;
    case 0x50:                                           /* already exists */
        *cls = 12; *act = 4; *locus = 1; break;
    case 0x0B:                                           /* bad format     */
        *cls = 9; *act = 4; *locus = 2; break;
    default:
        *cls = 13; *act = 1; *locus = 1; break;
    }
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
    dos_jft_sync(e);
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
        /* Keep the swappable data area in step: Windows 1.x saves and restores
         * the DTA through SDA+0Ch when it switches between applications. */
        mag_ww(e, (u16)(DOS_SDA_OFF + 0x0C), g_dos.dta_off);
        mag_ww(e, (u16)(DOS_SDA_OFF + 0x0E), g_dos.dta_seg);
        set_ok(e);
        return;
    case 0x25: {
        /* AH=25h: set the interrupt vector.  The interrupt number travels in
         * AL (not AH) -- getting this wrong quietly satisfies programs that
         * only save vectors and breaks every one that installs its own
         * handler, which is what Windows 1.x does with INT 22h. */
        u8 n = (u8)(e->ax & 0xFF);
        u16 off = e->dx, seg = e->ds;
        emu_ww(e, 0, (u16)(n * 4), off);
        emu_ww(e, 0, (u16)(n * 4 + 2), seg);
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
        /* DOS returns AL = major version, AH = minor version, so MS-DOS 4.0
         * reports AX = 0x0004 and MS-DOS 5.0 reports 0x000A.  The level we
         * claim is UEFIDOS_DOSVER_* (see src/uefidos_config.h): 4.00 by
         * default, with a documented way back to the 2.10 quirk that the
         * Compaq DOS 2.11 SYS.COM version gate wants. */
        {
            int mj = UEFIDOS_DOSVER_MAJOR;
            int mn = UEFIDOS_DOSVER_COMPAT_MINOR;
            int om, on;
            /* a SETVER entry for the running program wins over the default */
            if (dos_setver_lookup(emu_current_program(), &om, &on)) {
                mj = om;
                mn = on;
            }
            e->ax = (u16)(((mn & 0xFF) << 8) | (mj & 0xFF));
#ifdef UEFIDOS_HOST
            if (getenv("UEFIDOS_TRACE_DOSVER"))
                fprintf(stderr, "[ver] %s -> %d.%02d (AX=%04X)\n",
                        emu_current_program(), mj, mn, (unsigned)e->ax);
#endif
        }
        set_ok(e);
        return;
    case 0x32: {
        /* Get the Drive Parameter Block for DL (0 = default drive). */
        int d = (e->dx & 0xFF) ? (int)(e->dx & 0xFF) - 1 : dos_current_drive();
        dpb_chain(e);
        if (d < 0 || d >= DOS_MAX_DRIVES || !g_dos.drives[d].mounted) {
            set_err(e, 0x0F);
            return;
        }
        e->ds = MAGIC_SEG;
        e->bx = (u16)(DOS_DPB_OFF + d * DOS_DPB_LEN);
        set_ok(e);
        return;
    }
    case 0x33:
        switch (e->ax & 0xFF) {
        case 0x00: case 0x01:
            /* Ctrl-Break check: report (and accept) "off". */
            e->dx = (u16)(e->dx & 0xFF00);
            break;
        case 0x02: case 0x03:
            /* DOS 3.3+ extended-break flags: report "off" as well. */
            e->dx = 0;
            break;
        case 0x05:
            /* DOS 4.0: boot drive, 0 = A:. */
            e->dx = (u16)((e->dx & 0xFF00) | (u8)boot_drive());
            break;
        case 0x06:
            break;                                  /* set boot drive: ignored */
        default:
            break;
        }
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
        /* AH=36h: free space on a drive.  DL=0 means "the current drive".
         * AX=sectors/cluster, BX=free clusters, CX=bytes/sector, DX=total
         * clusters, AX=FFFFh for an invalid drive.  The free count has to come
         * from the volume itself: installers (Windows 1.01 SETUP among them)
         * check it before every write, and a zero here truncates the install. */
        int d = (e->dx & 0xFF) ? (int)(e->dx & 0xFF) - 1 : dos_current_drive();
        if (d < 0 || d >= DOS_MAX_DRIVES || !g_dos.drives[d].mounted) {
            e->ax = 0xFFFF;
            set_ok(e);
            return;
        }
        fat_vol *v = &g_dos.drives[d].vol;
        e->ax = (u16)v->spc;
        e->bx = (u16)(v->free_count > 0xFFFFu ? 0xFFFFu : v->free_count);
        e->cx = (u16)v->bytes_per_sector;
        e->dx = (u16)(v->cluster_count > 0xFFFFu ? 0xFFFFu : v->cluster_count);
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
        dos_jft_sync(e);
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
        dos_jft_sync(e);
        set_ok(e);
        e->ax = (u16)h;
        return;
    }
    case 0x3E: {
        int rc = dos_close(e->bx);
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        dos_jft_sync(e);
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
        u32 start_pos = (h < DOS_HANDLE_MAX) ? g_dos.handles[h].pos : 0;
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
#ifdef UEFIDOS_HOST
        /* UEFIDOS_TRACE_READ=<handle> dumps every read of that handle with the
           file position and the first bytes, so a seek/read mismatch is
           visible instead of being inferred from a program's error. */
        {
            static int rd_trace = -1;
            static int rd_handle = -99;
            if (rd_trace < 0) {
                const char *v = getenv("UEFIDOS_TRACE_READ");
                rd_trace = v ? 1 : 0;
                if (v) rd_handle = (int)strtol(v, 0, 0);
            }
            if (rd_trace && (rd_handle < 0 || rd_handle == h)) {
                static const char hexd[] = "0123456789ABCDEF";
                char line[96];
                int n = 0;
                line[n++] = '['; line[n++] = 'r'; line[n++] = 'e'; line[n++] = 'a'; line[n++] = 'd'; line[n++] = ']';
                line[n++] = ' ';
                line[n++] = 'h'; line[n++] = '='; line[n++] = hexd[(h >> 4) & 15]; line[n++] = hexd[h & 15];
                line[n++] = ' '; line[n++] = 'p'; line[n++] = 'o'; line[n++] = 's'; line[n++] = '=';
                for (int sh = 28; sh >= 0; sh -= 4) line[n++] = hexd[(start_pos >> sh) & 15];
                line[n++] = ' '; line[n++] = 'w'; line[n++] = '='; line[n++] = hexd[(want >> 4) & 15]; line[n++] = hexd[want & 15];
                line[n++] = ' '; line[n++] = 'd'; line[n++] = '='; line[n++] = ' ';
                for (u32 i = 0; i < total && i < 8 && i < sizeof(tmp); i++) {
                    line[n++] = hexd[(tmp[i] >> 4) & 15];
                    line[n++] = hexd[tmp[i] & 15];
                    line[n++] = ' ';
                }
                line[n++] = '\n';
                line[n] = 0;
                plat_puts(line);
            }
        }
#endif
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
        /* AH=47h: current directory WITHOUT the drive letter, with a leading
         * backslash, and an empty string in the root.  Programs concatenate
         * "X:" in front of it, so handing back "C:\SIMEARTH" made SimEarth ask
         * for "C:\C:\SIMEARTH\GR.OVL" and then give up on its overlays. */
        int d = dos_current_drive();
        const char *cwd = (d >= 0 && d < DOS_MAX_DRIVES) ? g_dos.drives[d].cwd : "";
        int n = 0;
        if (cwd[0]) {
            emu_wb(e, e->ds, (u16)(e->si + n), (u8)'\\');
            n++;
            while (cwd[n - 1] && n < DOS_MAX_PATH - 1) {
                emu_wb(e, e->ds, (u16)(e->si + n), (u8)cwd[n - 1]);
                n++;
            }
        }
        emu_wb(e, e->ds, (u16)(e->si + n), 0);
        e->ax = (u16)(cwd[0] ? 0x0100 : 0x0000);   /* bit 8: not the root */
        set_ok(e);
        return;
    }
    case 0x48: {                                 /* allocate memory */
        u16 ds = 0, max_free = 0;
        if (!arena_alloc(e, e->bx, &ds, &max_free)) {
            e->bx = max_free;
            set_err(e, 8);                       /* insufficient memory */
            return;
        }
        e->ax = ds;
        set_ok(e);
        return;
    }
    case 0x49:                                   /* free memory block */
        if (!arena_free(e, e->es)) { set_err(e, 9); return; }
        set_ok(e);
        return;
    case 0x4A: {                                 /* resize memory block */
        u16 max_size = 0;
        if (!arena_resize(e, e->es, e->bx, &max_size)) {
            e->bx = max_size;
            set_err(e, 8);
            return;
        }
        set_ok(e);
        return;
    }
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
    case 0x34:
        /* AH=34h: address of the InDOS flag, a byte that is non-zero while a
         * DOS call is active.  In every DOS 2+ that byte is SDA+1, and
         * multitaskers/TSRs poll it before re-entering DOS.  Windows 1.x keeps
         * the pointer and tests the flag while its scheduler runs. */
        e->es = MAGIC_SEG;
        e->bx = (u16)(DOS_SDA_OFF + 0x01);
        set_ok(e);
        return;
    case 0x50:
        /* AH=50h: set the current PSP (DOS 2+).  Shells and environments set
         * it around EXEC, and Windows 1.x sets it to its own PSP and then
         * verifies the result through the swappable data area (SDA+10h), so
         * the emulator and the DOS kernel copies have to move together. */
        e->psp_seg = e->bx;
        g_dos.psp_seg = e->bx;
        set_ok(e);
        return;
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
        e->ax = 0;                               /* allocation strategy: first fit, low */
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
    case 0x52: {
        /* AH=52h: pointer to the DOS "list of lists" (ES:BX).  We model the
         * header fields that portable tools read: +0 the MCB chain, +2 the DPB
         * chain, +4 a far pointer to the SFT chain (see sft_build above). */
        u16 lol = DOS_LOL_OFF;
        u16 dpbs = dpb_chain(e);
        u16 sft = sft_build(e);
        for (int i = 0; i < 0x40; i++) mag_wb(e, (u16)(lol + i), 0);
        mag_ww(e, (u16)(lol + 0x00), g_dos.mem_base);     /* first MCB */
        mag_ww(e, (u16)(lol + 0x02), dpbs);               /* first DPB */
        mag_wd(e, (u16)(lol + 0x04), ((u32)MAGIC_SEG << 16) | sft); /* first SFT */
        e->es = MAGIC_SEG;
        e->bx = lol;
        set_ok(e);
        return;
    }
    case 0x5D: {
        /* AH=5Dh: DOS-internal interface.  AL=06h hands out the swappable data
         * area that multitaskers save and restore around a task switch (Windows
         * 1.x and 2.x swap the DTA/PSP per application this way); AL=0Bh is the
         * DOS 4.0 form returning a list describing the same area. */
        u8 sub = (u8)(e->ax & 0xFF);
        if (sub == 0x06 || sub == 0x0B) {
            sda_build(e);
            e->ds = MAGIC_SEG;
            e->cx = (u16)DOS_SDA_LEN;      /* bytes to swap while inside DOS */
            e->dx = 0x20;                  /* bytes that must always be swapped */
            if (sub == 0x06) {
                e->si = (u16)DOS_SDA_OFF;
            } else {
                mag_ww(e, (u16)DOS_SDA_LIST, 1);                  /* one area */
                mag_wd(e, (u16)(DOS_SDA_LIST + 2),
                       ((u32)MAGIC_SEG << 16) | (u16)DOS_SDA_OFF);
                mag_ww(e, (u16)(DOS_SDA_LIST + 6),
                       (u16)(DOS_SDA_LEN | 0x8000));              /* swap always */
                e->si = (u16)DOS_SDA_LIST;
            }
            set_ok(e);
            return;
        }
        if (sub == 0x07) {                 /* read the InDOS flag */
            e->dx = mag_rb(e, (u16)(DOS_SDA_OFF + 0x01));
            set_ok(e);
            return;
        }
        if (sub == 0x08) {                 /* set the InDOS flag */
            mag_wb(e, (u16)(DOS_SDA_OFF + 0x01), (u8)(e->dx & 0xFF));
            set_ok(e);
            return;
        }
        set_ok(e);
        return;
    }
    case 0x59: {
        /* AH=59h: extended error.  AH=59h itself always clears CF; the code
         * comes from the last failed call, which set_err() records. */
        if (!g_dos.last_error_valid || g_dos.last_error == 0) {
            e->ax = 0;
            e->bx = 0;
            e->cx = 0;
        } else {
            u8 cls = 0, act = 0, locus = 0;
            ext_error(g_dos.last_error, &cls, &act, &locus);
            e->ax = g_dos.last_error;
            e->bx = (u16)(((u16)cls << 8) | act);
            e->cx = (u16)((u16)locus << 8);
        }
        set_ok(e);
        return;
    }
    case 0x5A: {
        /* AH=5Ah: create a unique temporary file.  We append TMPxxxxx.TMP to
         * the directory named in DS:DX and retry on a name collision, which is
         * what DOS 4.0 does with the 11-character template it is given. */
        char tmpl[DOS_MAX_PATH];
        char path[DOS_MAX_PATH + 16];
        static const char hexd[] = "0123456789ABCDEF";
        read_dos_str(e, e->ds, e->dx, tmpl, sizeof(tmpl));
        int h = -1, rc = FAT_EIO;
        u32 seed = plat_ticks_ms();
        for (int attempt = 0; attempt < 64; attempt++) {
            char tail[16];
            u32 s = seed + (u32)attempt * 0x9E3779B9u + (u32)attempt;
            int n = 0;
            tail[n++] = 'T'; tail[n++] = 'M'; tail[n++] = 'P';
            for (int k = 0; k < 5; k++) tail[n++] = hexd[(s >> (k * 4)) & 0xF];
            tail[n++] = '.'; tail[n++] = 'T'; tail[n++] = 'M'; tail[n++] = 'P';
            tail[n] = 0;
            if (strlen(tmpl) + (u32)n + 2 >= sizeof(path)) { rc = FAT_ENOPATH; break; }
            strcpy(path, tmpl);
            int len = (int)strlen(path);
            if (len == 0) {
                strcpy(path, tail);
            } else if (path[len - 1] == '\\' || path[len - 1] == '/') {
                strcat(path, tail);
            } else {
                /* Replace the template's name part with our own name. */
                int cut = len;
                while (cut > 0 && path[cut - 1] != '\\' && path[cut - 1] != '/' &&
                       path[cut - 1] != ':') cut--;
                path[cut] = 0;
                strcat(path, tail);
            }
            rc = dos_creat(path, &h);
            if (rc == FAT_OK) break;
            if (rc != FAT_EEXIST) break;
        }
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        e->ax = (u16)h;
        return;
    }
    case 0x60: {
        /* AH=60h: canonicalise DS:SI into a full "C:\DIR\FILE.EXT" at ES:DI. */
        char in[DOS_MAX_PATH];
        char out[DOS_MAX_PATH + 8];
        char abs[DOS_MAX_PATH];
        int drive = 0;
        read_dos_str(e, e->ds, e->si, in, sizeof(in));
        int rc = dos_parse_path(in, &drive, abs, sizeof(abs));
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        int n = 0;
        out[n++] = (char)('A' + drive);
        out[n++] = ':';
        out[n++] = '\\';
        for (int i = 0; abs[i] && n < (int)sizeof(out) - 1; i++)
            out[n++] = (abs[i] == '/') ? '\\' : abs[i];
        out[n] = 0;
        for (int i = 0; i < n; i++) emu_wb(e, e->es, (u16)(e->di + i), (u8)out[i]);
        emu_wb(e, e->es, (u16)(e->di + n), 0);
        set_ok(e);
        return;
    }
    case 0x63:
        /* AH=63h: DBCS lead byte table.  AL=0 (no DBCS support), which is what
         * DOS reports when the loaded country uses a single-byte code page. */
        e->ax = (u16)(e->ax & 0xFF00);
        set_ok(e);
        return;
    case 0x68: {
        /* AH=68h: commit a file's buffered data to the device.  Our writes are
         * synchronous, so this only has to validate the handle. */
        int h = e->bx;
        if (h < 0 || h >= g_dos.files_limit) { set_err(e, 0x06); return; }
        if (h >= 5 && !g_dos.handles[h].used) { set_err(e, 0x06); return; }
        set_ok(e);
        return;
    }
    case 0x6C: {
        /* AH=6Ch: extended open/create.  DS:SI name, BX mode, CX attributes,
         * DX action bits: bit0 open existing, bit1 create if missing,
         * bit2 replace existing.  Returns AX = handle, CX = 1 opened /
         * 2 created / 3 replaced. */
        char path[DOS_MAX_PATH];
        u8 attr8 = 0;
        int exists, h = -1, rc, took = 0;
        u16 mode = e->bx;
        u8 action = (u8)(e->dx & 0xFF);
        read_dos_str(e, e->ds, e->si, path, sizeof(path));
        int open_mode = (int)(mode & 0x03);
        int share = (int)((mode >> 4) & 0x07);
        if (open_mode == 3) open_mode = 2;
        if (share > DOS_SHARE_DENY_NONE) share = DOS_SHARE_COMPAT;
        exists = (dos_attr_get(path, &attr8) == FAT_OK);
        u8 newattr = (u8)(e->cx & 0x27);
        if (!newattr) newattr = 0x20;
        if (exists && (action & 0x04)) {
            rc = dos_creat_ex(path, newattr, share, open_mode, &h);
            took = 3;
        } else if (exists && (action & 0x01)) {
            rc = dos_open_ex(path, open_mode, share, &h);
            took = 1;
        } else if (exists) {
            set_err(e, 0x50);              /* asked to fail if it exists */
            return;
        } else if (action & 0x02) {
            rc = dos_creat_ex(path, newattr, share, open_mode, &h);
            took = 2;
        } else {
            set_err(e, 0x02);              /* file not found */
            return;
        }
        if (rc != FAT_OK) { set_err(e, (u8)dos_error_code(rc)); return; }
        set_ok(e);
        e->ax = (u16)h;
        e->cx = (u16)took;
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

/* ---- VESA VBE (INT 10h AH=4Fh) ------------------------------------ */
static void fb_put(Emu *e, u16 seg, u16 off, const u8 *src, int n)
{
    for (int i = 0; i < n; i++) emu_wb(e, seg, (u16)(off + i), src[i]);
}

static void fb_w16(Emu *e, u16 seg, u16 off, u16 v)
{
    emu_wb(e, seg, off, (u8)(v & 0xFF));
    emu_wb(e, seg, (u16)(off + 1), (u8)(v >> 8));
}

static void vesa_info(Emu *e, u16 seg, u16 off)
{
    /* 256-byte VbeInfoBlock */
    for (int i = 0; i < 256; i++) emu_wb(e, seg, (u16)(off + i), 0);
    fb_put(e, seg, off, (const u8 *)"VESA", 4);      /* signature */
    fb_w16(e, seg, (u16)(off + 4), 0x0200);          /* VBE 2.0 */
    /* OEM string pointer: point at an empty string in the same buffer */
    fb_w16(e, seg, (u16)(off + 6), (u16)(off + 0xF0));
    fb_w16(e, seg, (u16)(off + 8), seg);
    /* capabilities: DAC is programmable to 8 bits per primary */
    fb_w16(e, seg, (u16)(off + 10), 0x0001);
    fb_w16(e, seg, (u16)(off + 12), 0x0000);
    /* video mode list: an 8bpp mode then 0xFFFF */
    static const u16 modes[] = { 0x100, 0x101, 0x103, 0xFFFF };
    for (int i = 0; i < 4; i++) fb_w16(e, seg, (u16)(off + 14 + i * 2), modes[i]);
    /* 0xF0: OEM vendor name */
    fb_put(e, seg, (u16)(off + 0xF0), (const u8 *)"UEFI-DOS\0", 9);
    e->ax = 0x004F;
}

static void vesa_mode_info(Emu *e, u16 mode, u16 seg, u16 off)
{
    int w = 0, h = 0;
    if (!emu_gfx_geometry(mode, &w, &h)) {
        e->ax = 0x014F;
        return;
    }
    for (int i = 0; i < 256; i++) emu_wb(e, seg, (u16)(off + i), 0);
    fb_w16(e, seg, (u16)(off + 0), 0x001B);  /* supported|graphics|colour|LFB */
    fb_w16(e, seg, (u16)(off + 2), (u16)(off + 0xE0));   /* window A attrs ptr */
    fb_w16(e, seg, (u16)(off + 4), seg);
    fb_w16(e, seg, (u16)(off + 6), (u16)(off + 0xE0));   /* window B */
    fb_w16(e, seg, (u16)(off + 8), seg);
    fb_w16(e, seg, (u16)(off + 10), 0);       /* granularity (not pageable) */
    fb_w16(e, seg, (u16)(off + 12), 64);      /* window size in KB */
    fb_w16(e, seg, (u16)(off + 14), 0xA000);  /* window A segment */
    fb_w16(e, seg, (u16)(off + 16), 0);       /* window B segment */
    fb_w16(e, seg, (u16)(off + 18), w);       /* bytes per scan line */
    fb_w16(e, seg, (u16)(off + 20), (u16)w);
    fb_w16(e, seg, (u16)(off + 22), (u16)h);
    emu_wb(e, seg, (u16)(off + 24), 8);       /* char width */
    emu_wb(e, seg, (u16)(off + 25), 16);      /* char height */
    emu_wb(e, seg, (u16)(off + 26), 1);       /* planes */
    emu_wb(e, seg, (u16)(off + 27), 8);       /* bits per pixel */
    emu_wb(e, seg, (u16)(off + 28), 1);       /* banks */
    emu_wb(e, seg, (u16)(off + 29), 4);       /* memory model: packed pixel */
    emu_wb(e, seg, (u16)(off + 30), 0);       /* bank size */
    emu_wb(e, seg, (u16)(off + 31), 1);       /* images */
    /* Linear framebuffer: we expose emulated VRAM at A0000 for every mode */
    emu_wb(e, seg, (u16)(off + 0x71), 0x00);  /* phys address low */
    emu_wb(e, seg, (u16)(off + 0x72), 0x00);
    emu_wb(e, seg, (u16)(off + 0x73), 0x0A);
    emu_wb(e, seg, (u16)(off + 0x74), 0x00);
    fb_w16(e, seg, (u16)(off + 0xE0), 0x0007);  /* window attrs: usable/rw */
    e->ax = 0x004F;
}

static void vesa_handler(Emu *e)
{
    u8 sub = (u8)(e->ax & 0xFF);
    u16 seg = e->es, off = e->di;
    switch (sub) {
    case 0x00: vesa_info(e, seg, off); break;
    case 0x01: vesa_mode_info(e, e->cx, seg, off); break;
    case 0x02:                                  /* set mode (BX) */
        emu_set_video_mode(e, (u16)(e->bx & 0x3FFF));
        e->ax = (u16)(e->gfx_w ? 0x004F : 0x014F);
        break;
    case 0x03:                                  /* get current mode */
        e->bx = e->video_mode;
        e->ax = 0x004F;
        break;
    case 0x05:                                  /* bank switch: no-op (LFB) */
    case 0x06:                                  /* logical scan line length */
    case 0x07:                                  /* display start */
        e->ax = 0x004F;
        break;
    case 0x08:                                  /* set/get DAC palette format */
        if ((e->bx & 0xFF) == 1) e->bx = 1;     /* 8-bit per primary */
        e->ax = 0x004F;
        break;
    case 0x0A:                                  /* protected-mode interface */
        e->ax = 0x014F;
        break;
    default:
        e->ax = 0x014F;
        break;
    }
}

void int10_handler(Emu *e)
{
    trace_call(e, 0x10);
    u8 ah = (u8)(e->ax >> 8);
    switch (ah) {
    case 0x00: {
        u16 want = (u16)(e->ax & 0xFF);
        int gw = 0, gh = 0;
        /* mode 13h / VESA modes switch the display to a pixel framebuffer, and
         * coming back from one has to restore the text mirror, so both
         * directions go through emu_set_video_mode() */
        if (emu_gfx_geometry(want, &gw, &gh) || e->gfx_w) {
            emu_set_video_mode(e, want);
            break;
        }
        e->video_mode = want;
        e->curx = 0; e->cury = 0;
        cur_sync(e);
        if (want == 3 || want == 7 || want == 0) clear_screen(e);
        break;
    }
    case 0x10: {                     /* palette (DAC) programming */
        u8 sub = (u8)(e->ax & 0xFF);
        if (sub == 0x00 || sub == 0x10) {
            emu_dac_write(e, (u8)(e->bx & 0xFF), (u8)(e->dx >> 8),
                          (u8)(e->cx >> 8), (u8)(e->cx & 0xFF));
        } else if (sub == 0x12) {    /* set block of DAC registers: ES:DX */
            int n = e->cx;
            for (int i = 0; i < n; i++) {
                u8 r = emu_rb(e, e->es, (u16)(e->dx + i * 3));
                u8 g = emu_rb(e, e->es, (u16)(e->dx + i * 3 + 1));
                u8 b = emu_rb(e, e->es, (u16)(e->dx + i * 3 + 2));
                emu_dac_write(e, (u8)((e->bx + i) & 0xFF), r, g, b);
            }
        } else if (sub == 0x15) {    /* read one DAC register -> DH=R CL=B CH=G? */
            u8 rgb[3];
            emu_dac_read(e, (u8)(e->bx & 0xFF), rgb);
            e->dx = (u16)((rgb[0] << 8) | rgb[2]);
            e->cx = (u16)(rgb[1] << 8);
        }
        break;
    }
    case 0x4F:                       /* VESA VBE */
        vesa_handler(e);
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
    case 0x0B: { /* BH=0: background/border colour, BH=1: CGA palette select */
        if ((u8)(e->bx >> 8) == 0) {
            e->cga_bg = (u8)(e->bx & 0x0F);
        } else {
            e->cga_pal = (u8)(e->bx & 0x01);
            e->cga_int = (u8)((e->bx & 0x80) ? 1 : 0);
        }
        emu_cga_palette(e);
        break;
    }
    case 0x0E: { /* teletype output: writes, advances, scrolls at the bottom */
        tele_put(e, (u8)(e->ax & 0xFF), (u8)(e->bx & 0xFF));
        break;
    }
    case 0x0F: { /* current video mode: AL = mode, AH = columns */
        int gw = 0, gh = 0;
        int cols = 80;
        if (emu_gfx_geometry(e->video_mode, &gw, &gh) && gw < 640) cols = 40;
        else if (!emu_gfx_geometry(e->video_mode, &gw, &gh)) cols = 80;
        e->ax = (u16)(((cols & 0xFF) << 8) | (u8)e->video_mode);
        e->bx = (u16)(e->bx & 0xFF00);
        break;
    }
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
