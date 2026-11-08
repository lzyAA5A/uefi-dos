/*
 * UEFI-DOS -- DPMI (DOS Protected Mode Interface) host, INT 31h.
 *
 * The emulator is the DPMI host: 32-bit protected-mode clients get descriptor
 * management, DOS memory, real-mode interrupt reflection (AX=0300h) and
 * real-mode far calls (AX=0301h).  The LDT the host hands out lives at a
 * private linear address inside emulated RAM, so a client's GDT is never
 * touched and `read_desc()` can resolve LDT selectors without an LDTR.
 */
#include "uefidos.h"
#include "emu/emu.h"
#include "kernel/dos.h"
#include "plat.h"

/* ------------------------------------------------------------------ */
/* host LDT: DUMMY_LDT_LIN + index*8, 8192 descriptors (64 KiB)        */
/* ------------------------------------------------------------------ */
#define DPMI_LDT_LIN     0x00F00000u
#define DPMI_LDT_ENTRIES 8192
#define DPMI_LDT_MAX_SEL (DPMI_LDT_ENTRIES - 1)

static u8 g_ldt_used[DPMI_LDT_ENTRIES / 8];

static int ldt_idx_of(u16 sel) { return (int)(sel >> 3); }

static int ldt_is_free(int idx)
{
    if (idx <= 0 || idx >= DPMI_LDT_ENTRIES) return 0;
    return (g_ldt_used[idx >> 3] & (u8)(1u << (idx & 7))) == 0;
}
static void ldt_mark(int idx, int used)
{
    if (idx <= 0 || idx >= DPMI_LDT_ENTRIES) return;
    if (used) g_ldt_used[idx >> 3] |= (u8)(1u << (idx & 7));
    else g_ldt_used[idx >> 3] &= (u8)~(1u << (idx & 7));
}

static u8 *ldt_entry(Emu *e, int idx)
{
    return e->mem + (((DPMI_LDT_LIN + (u32)idx * 8u) & (EMU_RAM_SIZE - 1u)));
}

static void desc_write(Emu *e, int idx, u32 base, u32 limit, u16 attr)
{
    u8 *d = ldt_entry(e, idx);
    d[0] = (u8)(limit & 0xFF);
    d[1] = (u8)((limit >> 8) & 0xFF);
    d[2] = (u8)(base & 0xFF);
    d[3] = (u8)((base >> 8) & 0xFF);
    d[4] = (u8)((base >> 16) & 0xFF);
    d[5] = (u8)(attr & 0xFF);
    d[6] = (u8)(((attr >> 8) & 0xF0) | ((limit >> 16) & 0x0F));
    d[7] = (u8)((base >> 24) & 0xFF);
}
static void desc_read(Emu *e, int idx, u32 *base, u32 *limit, u16 *attr)
{
    u8 *d = ldt_entry(e, idx);
    u32 l = (u32)d[0] | ((u32)d[1] << 8) | ((u32)(d[6] & 0x0F) << 16);
    if (d[6] & 0x80) l = (l << 12) | 0xFFFu;
    *base = (u32)d[2] | ((u32)d[3] << 8) | ((u32)d[4] << 16) | ((u32)d[7] << 24);
    *limit = l;
    *attr = (u16)((u16)d[5] | ((u16)d[6] << 8));
}

/* Allocate a free descriptor; returns the selector (0x0004-based, TI=1). */
static u16 ldt_alloc(Emu *e, u32 base, u32 limit, u16 attr)
{
    int i;
    for (i = 1; i < DPMI_LDT_ENTRIES; i++) {
        if (ldt_is_free(i)) {
            ldt_mark(i, 1);
            desc_write(e, i, base, limit, attr);
            return (u16)(0x0004 | (i << 3));
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DOS memory pool for AX=0501h (real-mode addressable, below 1 MiB)   */
/* ------------------------------------------------------------------ */
#define DPMI_POOL_FIRST_PARA 0x1000u     /* 64 KiB */
#define DPMI_POOL_LAST_PARA  0x9F00u     /* 636 KiB */
#define DPMI_POOL_BLOCKS     128

typedef struct {
    u16 para;      /* start paragraph */
    u16 paras;     /* size in paragraphs */
    u8 used;
    u16 sel;
} dpmi_blk;

static dpmi_blk g_pool[DPMI_POOL_BLOCKS];
static int g_pool_init = 0;

static void pool_init(void)
{
    if (g_pool_init) return;
    g_pool_init = 1;
    g_pool[0].para = DPMI_POOL_FIRST_PARA;
    g_pool[0].paras = (u16)(DPMI_POOL_LAST_PARA - DPMI_POOL_FIRST_PARA);
    g_pool[0].used = 0;
    g_pool[0].sel = 0;
}

static int pool_alloc(Emu *e, u16 paras, u16 *seg_out, u16 *sel_out)
{
    int i;
    pool_init();
    for (i = 0; i < DPMI_POOL_BLOCKS; i++) {
        if (g_pool[i].paras == 0 || g_pool[i].used) continue;
        if (g_pool[i].paras < paras) continue;
        /* split: [allocated][rest] */
        if (g_pool[i].paras > paras) {
            int j;
            for (j = DPMI_POOL_BLOCKS - 1; j > i; j--) g_pool[j] = g_pool[j - 1];
            g_pool[i + 1].para = (u16)(g_pool[i].para + paras);
            g_pool[i + 1].paras = (u16)(g_pool[i].paras - paras);
            g_pool[i + 1].used = 0;
            g_pool[i + 1].sel = 0;
            g_pool[i].paras = paras;
        }
        g_pool[i].used = 1;
        {
            u16 sel = ldt_alloc(e, (u32)g_pool[i].para << 4,
                                ((u32)paras << 4) - 1u, 0x0092);
            if (!sel) return 0;
            g_pool[i].sel = sel;
            *seg_out = g_pool[i].para;
            *sel_out = sel;
            /* a freshly allocated DOS block is zeroed, like XMS/DOS memory */
            memset(e->mem + (((u32)g_pool[i].para << 4) & (EMU_RAM_SIZE - 1u)), 0,
                   (u32)paras << 4);
            return 1;
        }
    }
    return 0;
}

static int pool_free(Emu *e, u16 sel)
{
    int i;
    (void)e;
    for (i = 0; i < DPMI_POOL_BLOCKS; i++) {
        if (g_pool[i].paras && g_pool[i].used && g_pool[i].sel == sel) {
            g_pool[i].used = 0;
            g_pool[i].sel = 0;
            ldt_mark(ldt_idx_of(sel), 0);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* real-mode register structure (DPMI 0300h/0301h), at ES:EDI          */
/* ------------------------------------------------------------------ */
#define RM_STRUCT_SIZE 0x32

static u32 rm_u32(Emu *e, u16 seg, u32 off) { return emu_rd32(e, seg, off); }
static void rm_w32(Emu *e, u16 seg, u32 off, u32 v) { emu_wd32(e, seg, off, v); }

static void load_regs_from_struct(Emu *e, u16 seg, u32 off)
{
    e->edi = rm_u32(e, seg, off + 0x00);
    e->esi = rm_u32(e, seg, off + 0x04);
    e->ebp = rm_u32(e, seg, off + 0x08);
    e->ebx = rm_u32(e, seg, off + 0x10);
    e->edx = rm_u32(e, seg, off + 0x14);
    e->ecx = rm_u32(e, seg, off + 0x18);
    e->eax = rm_u32(e, seg, off + 0x1C);
    e->flags = (u16)(rm_u32(e, seg, off + 0x20) & 0xFFFFu);
    e->es = (u16)(rm_u32(e, seg, off + 0x22) & 0xFFFFu);
    e->ds = (u16)(rm_u32(e, seg, off + 0x24) & 0xFFFFu);
    e->fs = (u16)(rm_u32(e, seg, off + 0x26) & 0xFFFFu);
    e->gs = (u16)(rm_u32(e, seg, off + 0x28) & 0xFFFFu);
    e->ip = (u16)(rm_u32(e, seg, off + 0x2A) & 0xFFFFu);
    e->cs = (u16)(rm_u32(e, seg, off + 0x2C) & 0xFFFFu);
    e->sp = (u16)(rm_u32(e, seg, off + 0x2E) & 0xFFFFu);
    e->ss = (u16)(rm_u32(e, seg, off + 0x30) & 0xFFFFu);
}

static void store_regs_to_struct(Emu *e, u16 seg, u32 off)
{
    rm_w32(e, seg, off + 0x00, e->edi);
    rm_w32(e, seg, off + 0x04, e->esi);
    rm_w32(e, seg, off + 0x08, e->ebp);
    rm_w32(e, seg, off + 0x10, e->ebx);
    rm_w32(e, seg, off + 0x14, e->edx);
    rm_w32(e, seg, off + 0x18, e->ecx);
    rm_w32(e, seg, off + 0x1C, e->eax);
    rm_w32(e, seg, off + 0x20, (u32)e->flags);
    rm_w32(e, seg, off + 0x22, (u32)e->es);
    rm_w32(e, seg, off + 0x24, (u32)e->ds);
    rm_w32(e, seg, off + 0x26, (u32)e->fs);
    rm_w32(e, seg, off + 0x28, (u32)e->gs);
    rm_w32(e, seg, off + 0x2A, (u32)e->ip);
    rm_w32(e, seg, off + 0x2C, (u32)e->cs);
    rm_w32(e, seg, off + 0x2E, (u32)e->sp);
    rm_w32(e, seg, off + 0x30, (u32)e->ss);
}

/* Which vectors does UEFI-DOS service itself? */
static int is_host_vector(int v)
{
    return v == 0x10 || v == 0x13 || v == 0x16 || v == 0x1A || v == 0x20 ||
           v == 0x21 || v == 0x27 || v == 0x2F || v == 0x31;
}

static void call_host_vector(Emu *e, int v)
{
    if (v == 0x21) int21_handler(e);
    else if (v == 0x10) int10_handler(e);
    else if (v == 0x16) int16_handler(e);
    else if (v == 0x13) int13_handler(e);
    else if (v == 0x1A) int1a_handler(e);
    else if (v == 0x2F) int2f_handler(e);
    else if (v == 0x31) int31_handler(e);
}

/* ------------------------------------------------------------------ */
/* AX=0300h: reflect a real-mode interrupt                             */
/* ------------------------------------------------------------------ */
static void dpmi_reflect_int(Emu *e, int vec, u16 sseg, u32 soff)
{
    Emu saved;
    u16 vec_off, vec_seg;
    int hooked = is_host_vector(vec);

    /* remember everything the client had, so we can come back to PM */
    saved = *e;
    load_regs_from_struct(e, sseg, soff);
    e->cr0 &= ~CR0_PE;              /* run the reflection in real mode */
    e->op32 = 0;
    e->addr32 = 0;
    emu_seg_cache_sync(e);

    vec_off = emu_rw(e, 0, (u16)(vec * 4));
    vec_seg = emu_rw(e, 0, (u16)(vec * 4 + 2));

    if (hooked && (vec_seg == MAGIC_SEG || vec_seg == TRAMP_SEG || vec_seg == 0)) {
        /* UEFI-DOS owns this vector: run the service directly.  Real-mode
         * services keep the caller's flags (they return with IRET), so we do
         * not push an interrupt frame here. */
        call_host_vector(e, vec);
        if (vec == 0x20 || vec == 0x27) {
            /* terminate: hand control back to the shell */
            e->halted = saved.halted;
        }
    } else {
        /* The real-mode handler has to be run to completion: push a frame and
         * let the instruction loop execute it, then detect the IRET back to
         * our sentinel address. */
        u16 entry_off = vec_off, entry_seg = vec_seg;
        u16 ret_ip = e->ip, ret_cs = e->cs, ret_sp = e->sp, ret_ss = e->ss;
        int guard = 0;

        /* sentinel: return address pointing one byte before the entry point is
         * unreliable, so we simply run until SP/SS come back to the values we
         * saved here and CS:IP equal the values just before the frame push. */
        emu_push16(e, e->flags);
        emu_push16(e, ret_cs);
        emu_push16(e, ret_ip);
        e->cs = entry_seg;
        e->ip = entry_off;
        while (!e->halted && guard++ < 4000000) {
            if (e->ss == ret_ss && e->sp == (u16)(ret_sp - 6) && e->cs == ret_cs &&
                e->ip == ret_ip) break;
            if (step_public(e)) break;
        }
    }

    /* copy the 16-bit results back into the structure the client passed */
    store_regs_to_struct(e, sseg, soff);

    /* restore the protected-mode machine state; memory changes stay */
    {
        u8 *mem = e->mem;
        u32 ram = e->ram_size;
        u16 psp = e->psp_seg;
        int halted = e->halted;
        int exit_code = e->exit_code;
        u16 last_exit = e->last_exit_code;
        u16 vmode = e->video_mode;
        int curx = e->curx, cury = e->cury;
        u32 ldtr = e->ldtr, tr = e->tr;
        u32 gdtb = e->gdt_base, idtb = e->idt_base;
        u16 gdtl = e->gdt_limit, idtl = e->idt_limit;
        *e = saved;
        e->mem = mem;
        e->ram_size = ram;
        e->psp_seg = psp;
        e->halted = halted;
        e->exit_code = exit_code;
        e->last_exit_code = last_exit;
        e->video_mode = vmode;
        e->curx = curx;
        e->cury = cury;
        e->ldtr = (u16)ldtr;
        e->tr = (u16)tr;
        e->gdt_base = gdtb;
        e->idt_base = idtb;
        e->gdt_limit = gdtl;
        e->idt_limit = idtl;
        e->cr0 = saved.cr0;
        emu_seg_cache_sync(e);
    }
}

/* ------------------------------------------------------------------ */
/* AX=0301h: call a real-mode far procedure                            */
/* ------------------------------------------------------------------ */
static void dpmi_call_rm(Emu *e, u16 sseg, u32 soff)
{
    Emu saved = *e;
    u16 vec_off, vec_seg;
    u16 ret_ip, ret_cs;
    int guard = 0;

    load_regs_from_struct(e, sseg, soff);
    vec_off = e->ip;
    vec_seg = e->cs;
    e->cr0 &= ~CR0_PE;
    e->op32 = 0;
    e->addr32 = 0;
    emu_seg_cache_sync(e);

    ret_ip = 0xFFF0;    /* sentinel return address inside our IRET stub area */
    ret_cs = TRAMP_SEG;
    emu_push16(e, ret_cs);
    emu_push16(e, ret_ip);
    e->cs = vec_seg;
    e->ip = vec_off;
    while (!e->halted && guard++ < 4000000) {
        if (e->cs == ret_cs && e->ip == ret_ip) break;
        if (step_public(e)) break;
    }
    store_regs_to_struct(e, sseg, soff);

    {
        u8 *mem = e->mem;
        u32 ram = e->ram_size;
        *e = saved;
        e->mem = mem;
        e->ram_size = ram;
        emu_seg_cache_sync(e);
    }
}

/* ------------------------------------------------------------------ */
/* DPMI host bootstrap: INT 2Fh AX=1687h hands out the entry point,    */
/* the entry point itself is "INT 60h" at DPMI_ENTRY_SEG:OFF.          */
/* ------------------------------------------------------------------ */

/* host GDT selectors handed to the client */
#define SEL_CODE16 0x0008u
#define SEL_DATA16 0x0010u
#define SEL_STACK16 0x0018u
#define SEL_ES16 0x0020u
#define SEL_LDT 0x0028u
#define SEL_CODE32 0x0030u
#define SEL_DATA32 0x0038u

static void gdt_set(Emu *e, int idx, u32 base, u32 limit, u16 attr)
{
    u8 *d = e->mem + ((DPMI_GDT_LIN + (u32)idx * 8u) & (EMU_RAM_SIZE - 1u));
    d[0] = (u8)(limit & 0xFF);
    d[1] = (u8)((limit >> 8) & 0xFF);
    d[2] = (u8)(base & 0xFF);
    d[3] = (u8)((base >> 8) & 0xFF);
    d[4] = (u8)((base >> 16) & 0xFF);
    d[5] = (u8)(attr & 0xFF);
    d[6] = (u8)(((attr >> 8) & 0xF0) | ((limit >> 16) & 0x0F));
    d[7] = (u8)((base >> 24) & 0xFF);
}

/* Enter protected mode for the client that just far-called our entry point.
 * That call pushed CS:IP on the client's real-mode stack, so the return
 * address sits there; the selectors we hand out carry base = realseg<<4, so
 * every offset inside the client keeps its meaning. */
static void dpmi_enter_pm(Emu *e, int want32)
{
    u16 ret_ip, ret_cs;
    u16 sp, ss_sel, ds_sel, es_sel;
    u32 i;

    if (e->cr0 & CR0_PE) {          /* already in protected mode */
        e->ax = 0;
        return;
    }
    ret_ip = emu_pop16(e);
    ret_cs = emu_pop16(e);
    sp = e->sp;
    ss_sel = e->ss;
    ds_sel = e->ds;
    es_sel = e->es;

    /* null + client code/data/stack/extra + LDT + flat 32-bit code/data */
    gdt_set(e, 0, 0, 0, 0);
    gdt_set(e, 1, (u32)ret_cs << 4, 0xFFFFu, (u16)(want32 ? 0x409A : 0x009A));
    gdt_set(e, 2, (u32)ds_sel << 4, 0xFFFFu, 0x0092);
    gdt_set(e, 3, (u32)ss_sel << 4, 0xFFFFu, 0x0092);
    gdt_set(e, 4, (u32)es_sel << 4, 0xFFFFu, 0x0092);
    gdt_set(e, 5, DPMI_LDT_LIN, 0xFFFFu, 0x0082);            /* LDT */
    gdt_set(e, 6, 0, 0xFFFFFu, 0xC09A);                      /* 32-bit flat code */
    gdt_set(e, 7, 0, 0xFFFFFu, 0xC092);                      /* 32-bit flat data */

    e->gdt_base = DPMI_GDT_LIN;
    e->gdt_limit = 8 * 8 - 1;
    e->ldtr = SEL_LDT;
    e->tr = 0;

    for (i = 0; i < 256u * 8u; i++) e->mem[(DPMI_IDT_LIN + i) & (EMU_RAM_SIZE - 1u)] = 0;
    e->idt_base = DPMI_IDT_LIN;
    e->idt_limit = 256 * 8 - 1;
    pm_patch_idt(e);

    e->cr0 |= CR0_PE;
    e->op32 = 0;
    e->addr32 = 0;
    emu_set_seg(e, SEG_CS, SEL_CODE16);
    emu_set_seg(e, SEG_DS, SEL_DATA16);
    emu_set_seg(e, SEG_SS, SEL_STACK16);
    emu_set_seg(e, SEG_ES, SEL_ES16);
    emu_set_seg(e, SEG_FS, 0);
    emu_set_seg(e, SEG_GS, 0);
    e->eip = ret_ip;
    e->esp = sp;
    e->ax = 0;
    e->flags &= (u16)~F_CF;
}

void int60_handler(Emu *e)
{
    if (e->ax == 0x0001) dpmi_enter_pm(e, (e->bx & 1) != 0);
    else e->ax = 0;
}

/* INT 2Fh AX=1687h: report a DPMI 0.9 host with a 32-bit capable client API. */
int dpmi_install_check(Emu *e)
{
    e->ax = 0x0000;
    e->bx = 0x0003;                             /* 32-bit + 16-bit DPMI calls */
    e->cx = (u16)((e->cx & 0xFF00) | 0x0003);   /* 386 */
    e->dx = 0x000A;                             /* version 0.9 */
    e->es = DPMI_ENTRY_SEG;
    e->di = DPMI_ENTRY_OFF;
    e->si = 0;                                  /* no host private data */
    e->flags &= (u16)~F_CF;
    return 1;
}

/* ------------------------------------------------------------------ */
/* INT 31h entry                                                       */
/* ------------------------------------------------------------------ */
void int31_handler(Emu *e)
{
    switch (e->ax) {
    case 0x0000: { /* allocate LDT descriptor */
        u16 sel = ldt_alloc(e, 0, 0, 0x0092);
        if (!sel) { e->flags |= F_CF; e->ax = 0x8011; }
        else { e->flags &= (u16)~F_CF; e->ax = sel; e->cx = 0; }
        break;
    }
    case 0x0001: { /* free LDT descriptor */
        int idx = ldt_idx_of(e->bx);
        if (!ldt_is_free(idx)) { ldt_mark(idx, 0); e->flags &= (u16)~F_CF; }
        else { e->flags |= F_CF; e->ax = 0x8022; }
        break;
    }
    case 0x0002: { /* real-mode segment -> selector */
        u32 base = (u32)e->bx << 4;
        u16 sel = ldt_alloc(e, base, 0xFFFFu, 0x0092);
        if (!sel) { e->flags |= F_CF; e->ax = 0x8011; }
        else { e->flags &= (u16)~F_CF; e->ax = sel; e->cx = 0; }
        break;
    }
    case 0x0003: /* get selector increment */
        e->flags &= (u16)~F_CF;
        e->ax = 0x0008;
        e->cx = 0;
        break;
    case 0x0006: { /* get segment base address */
        u32 base = 0, limit = 0;
        u16 attr = 0;
        if (e->bx & 4) desc_read(e, ldt_idx_of(e->bx), &base, &limit, &attr);
        else base = 0;
        e->flags &= (u16)~F_CF;
        e->cx = (u16)(base >> 16);
        e->dx = (u16)(base & 0xFFFFu);
        break;
    }
    case 0x0007: { /* set segment base address */
        u32 base = ((u32)e->cx << 16) | (u32)e->dx;
        u32 old_base = 0, limit = 0;
        u16 attr = 0;
        if (e->bx & 4) {
            desc_read(e, ldt_idx_of(e->bx), &old_base, &limit, &attr);
            desc_write(e, ldt_idx_of(e->bx), base, limit, attr);
        }
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x0008: { /* set segment limit */
        u32 base = 0, limit = 0;
        u16 attr = 0;
        u32 newlim = ((u32)e->cx << 16) | (u32)e->dx;
        if (e->bx & 4) {
            desc_read(e, ldt_idx_of(e->bx), &base, &limit, &attr);
            /* byte granularity: clear the G bit */
            desc_write(e, ldt_idx_of(e->bx), base, newlim, (u16)(attr & 0x7FFF));
        }
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x0009: { /* set access rights */
        u32 base = 0, limit = 0;
        u16 attr = 0;
        if (e->bx & 4) {
            desc_read(e, ldt_idx_of(e->bx), &base, &limit, &attr);
            desc_write(e, ldt_idx_of(e->bx), base, limit, e->cx);
        }
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x000A: { /* create alias descriptor (approximated by a copy) */
        u32 base = 0, limit = 0;
        u16 attr = 0;
        desc_read(e, ldt_idx_of(e->bx), &base, &limit, &attr);
        {
            u16 sel = ldt_alloc(e, base, limit, attr);
            if (!sel) { e->flags |= F_CF; e->ax = 0x8011; }
            else { e->flags &= (u16)~F_CF; e->ax = sel; e->cx = 0; }
        }
        break;
    }
    case 0x000B: { /* get descriptor (raw) */
        u32 base = 0, limit = 0;
        u16 attr = 0;
        desc_read(e, ldt_idx_of(e->bx), &base, &limit, &attr);
        emu_ww32(e, e->es, e->edi + 0, (u16)(limit & 0xFFFF));
        emu_ww32(e, e->es, e->edi + 2, (u16)(base & 0xFFFF));
        emu_wb32(e, e->es, e->edi + 4, (u8)((base >> 16) & 0xFF));
        emu_wb32(e, e->es, e->edi + 5, (u8)(attr & 0xFF));
        emu_wb32(e, e->es, e->edi + 6, (u8)(((attr >> 8) & 0xF0) | ((limit >> 16) & 0x0F)));
        emu_wb32(e, e->es, e->edi + 7, (u8)((base >> 24) & 0xFF));
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x000C: { /* set descriptor (raw) */
        u32 limit = (u32)emu_rw32(e, e->es, e->edi + 0);
        u32 base = (u32)emu_rw32(e, e->es, e->edi + 2);
        u16 attr;
        base |= (u32)emu_rb32(e, e->es, e->edi + 4) << 16;
        attr = (u16)((u16)emu_rb32(e, e->es, e->edi + 5) |
                     ((u16)(emu_rb32(e, e->es, e->edi + 6) & 0xF0) << 8));
        limit |= (u32)(emu_rb32(e, e->es, e->edi + 6) & 0x0F) << 16;
        if (emu_rb32(e, e->es, e->edi + 6) & 0x80) limit = (limit << 12) | 0xFFFu;
        base |= (u32)emu_rb32(e, e->es, e->edi + 7) << 24;
        desc_write(e, ldt_idx_of(e->bx), base, limit, attr);
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x000D: { /* allocate specific LDT descriptor */
        int idx = ldt_idx_of(e->bx);
        if (ldt_is_free(idx)) {
            ldt_mark(idx, 1);
            desc_write(e, idx, 0, 0, 0x0092);
            e->flags &= (u16)~F_CF;
        } else { e->flags |= F_CF; e->ax = 0x8021; }
        break;
    }
    case 0x0200: case 0x0302: { /* get real-mode interrupt vector */
        int v = e->bl;
        e->cx = emu_rw(e, 0, (u16)(v * 4 + 2));
        e->dx = emu_rw(e, 0, (u16)(v * 4));
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x0201: case 0x0303: { /* set real-mode interrupt vector */
        int v = e->bl;
        emu_ww(e, 0, (u16)(v * 4), e->dx);
        emu_ww(e, 0, (u16)(v * 4 + 2), e->cx);
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x0202: /* get PM exception handler vector */
        e->cx = 0;
        e->dx = 0;
        e->flags &= (u16)~F_CF;
        break;
    case 0x0203: /* set PM exception handler vector */
        e->flags &= (u16)~F_CF;
        break;
    case 0x0204: /* get coprocessor status */
        e->ax = 0;
        e->flags &= (u16)~F_CF;
        break;
    case 0x0205: { /* set PM interrupt vector: rebuild our IDT gate */
        int v = e->bl;
        if (e->idt_base && (u32)v * 8u + 8u <= (u32)e->idt_limit) {
            u32 addr = e->idt_base + (u32)v * 8u;
            int is32 = (e->seg[SEG_CS].valid && (e->seg[SEG_CS].attr & 0x4000)) ? 1 : 0;
            e->mem[(addr + 0) & (EMU_RAM_SIZE - 1u)] = (u8)(e->dx & 0xFF);
            e->mem[(addr + 1) & (EMU_RAM_SIZE - 1u)] = (u8)(e->dx >> 8);
            e->mem[(addr + 2) & (EMU_RAM_SIZE - 1u)] = (u8)(e->cx & 0xFF);
            e->mem[(addr + 3) & (EMU_RAM_SIZE - 1u)] = (u8)(e->cx >> 8);
            e->mem[(addr + 4) & (EMU_RAM_SIZE - 1u)] = 0;
            e->mem[(addr + 5) & (EMU_RAM_SIZE - 1u)] = (u8)(is32 ? 0x8E : 0x86);
            e->mem[(addr + 6) & (EMU_RAM_SIZE - 1u)] = 0;
            e->mem[(addr + 7) & (EMU_RAM_SIZE - 1u)] = 0;
        }
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x0300: { /* simulate a real-mode interrupt */
        int v = e->bl;
        e->flags &= (u16)~F_CF;
        dpmi_reflect_int(e, v, e->es, e->edi);
        break;
    }
    case 0x0301: { /* call a real-mode far procedure */
        e->flags &= (u16)~F_CF;
        dpmi_call_rm(e, e->es, e->edi);
        break;
    }
    case 0x0400: /* get DPMI version: 0.9, 32-bit host */
        e->ax = 0x000A;                 /* AH=0 major, AL=0Ah => 0.9 */
        e->bx = 0x5A5A;                 /* "running under DPMI" signature */
        e->cl = 3;                      /* 386 */
        e->dx = 0x0001;                 /* 32-bit clients supported, 1 CPU */
        e->flags &= (u16)~F_CF;
        break;
    case 0x0500: { /* free memory information */
        u32 total_paras = (u32)(DPMI_POOL_LAST_PARA - DPMI_POOL_FIRST_PARA);
        u32 used = 0;
        int i;
        pool_init();
        for (i = 0; i < DPMI_POOL_BLOCKS; i++) if (g_pool[i].paras && g_pool[i].used) used += g_pool[i].paras;
        e->bx = (u16)(total_paras - used);   /* largest free DOS block, paragraphs */
        e->ax = 0;
        e->cx = 0;                           /* no linear memory limits */
        e->dx = 0xFFFFu;
        e->si = 0;
        e->di = 0xFFFFu;
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x0501: { /* allocate DOS memory */
        u16 seg = 0, sel = 0;
        if (e->bx == 0) { e->flags |= F_CF; e->ax = 0x8013; break; }
        if (pool_alloc(e, e->bx, &seg, &sel)) {
            e->flags &= (u16)~F_CF;
            e->ax = seg;
            e->dx = sel;
        } else {
            e->flags |= F_CF;
            e->ax = 0x8013;
            e->bx = 0;
        }
        break;
    }
    case 0x0502: { /* free DOS memory */
        if (pool_free(e, e->dx)) e->flags &= (u16)~F_CF;
        else { e->flags |= F_CF; e->ax = 0x8023; }
        break;
    }
    case 0x0503: { /* resize DOS memory (approximated: refuse growth) */
        e->flags &= (u16)~F_CF;
        break;
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: case 0x0604:
    case 0x0702: case 0x0703: case 0x0800: case 0x0801:
    case 0x0900: case 0x0A00:
        /* paging / vendor API stubs: nothing to do */
        e->flags &= (u16)~F_CF;
        e->ax = 0;
        break;
    default:
        e->flags |= F_CF;
        e->ax = 0x8001;     /* unsupported function */
        break;
    }
}
