/*
 * UEFI-DOS -- x86 CPU emulation interface (DOS compatibility layer).
 *
 * The emulator provides a V86-like illusion: real-mode 8086/80186 execution
 * (and, from v0.4, a 386 protected mode subset) over a flat emulated RAM
 * array.  INT 21h/10h/16h are routed to the native UEFI-DOS kernel
 * (kernel/dos.c + emu/int21.c); all other interrupts behave like real
 * hardware (program-installed handlers work).
 */
#ifndef UEFIDOS_EMU_H
#define UEFIDOS_EMU_H

#include "uefidos.h"

typedef i8  s8;
typedef i16 s16;
typedef i32 s32;
typedef i64 s64;

/* Segment register indices, in the order the cache array uses. */
#define SEG_ES 0
#define SEG_CS 1
#define SEG_SS 2
#define SEG_DS 3
#define SEG_FS 4
#define SEG_GS 5

/* Hidden part of a segment register (386 semantics). */
typedef struct {
    u16 sel;        /* selector currently loaded */
    u32 base;       /* linear base address */
    u32 limit;      /* limit in bytes (granularity already applied) */
    u16 attr;       /* access rights (descriptor bytes 5-6) */
    u8  valid;      /* cache entry usable */
} SegCache;

typedef struct Emu {
    u8 *mem;                 /* emulated RAM (EMU_RAM_SIZE bytes) */
    u32 ram_size;

    /* 32-bit general registers with 16-bit and 8-bit aliases: writing ax
     * keeps the upper half, writing eax replaces all 32 bits (hardware
     * behaviour we rely on when a 32-bit program drops back to 16 bits). */
    union { u32 eax; u16 ax; struct { u8 al, ah; }; };
    union { u32 ebx; u16 bx; struct { u8 bl, bh; }; };
    union { u32 ecx; u16 cx; struct { u8 cl, ch; }; };
    union { u32 edx; u16 dx; struct { u8 dl, dh; }; };
    union { u32 esi; u16 si; };
    union { u32 edi; u16 di; };
    union { u32 ebp; u16 bp; };
    union { u32 esp; u16 sp; };
    union { u32 eip; u16 ip; };
    union { u32 eflags; u16 flags; };

    u16 cs, ds, es, ss, fs, gs;
    SegCache seg[6];

    int op32;                /* operand-size 32 for the current instruction */
    int addr32;              /* address-size 32 for the current instruction */

    /* 386 control state (protected mode, phase 2/3) */
    u32 cr0, cr2, cr3, cr4;
    u32 gdt_base, idt_base;
    u16 gdt_limit, idt_limit;
    u16 ldtr, tr;
    int iopl;
    int pm_fault;            /* protection check failed: run loop reports it */
    u32 pm_fault_addr;
    int a20_on;              /* A20 gate: 0 = real-mode addresses wrap at 1 MB */

    u16 psp_seg;
    int halted;
    int exit_code;
    u16 last_exit_code;
    u16 video_mode;
    int curx, cury;
    /* graphics modes (INT 10h AH=00 mode 13h / VESA 4F02h): the framebuffer
     * lives at A0000 in emulated RAM, 8 bits per pixel */
    u16 gfx_w, gfx_h;
    u8  dac[256 * 3];       /* 6-bit VGA DAC, R,G,B */
    u16 pal_index;          /* 3C8h write index */
    u8  pal_phase;          /* 0..2 within a 3C9h triple (write) */
    u8  pal_read_phase;
    u16 pal_read_index;     /* 3C7h read index */
} Emu;

/* flags */
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800
#define F_NT 0x4000
#define F_VM 0x00020000u

/* CR0 bits */
#define CR0_PE 0x00000001u
#define CR0_MP 0x00000002u
#define CR0_EM 0x00000004u
#define CR0_TS 0x00000008u
#define CR0_WP 0x00010000u
#define CR0_PG 0x80000000u

#define EFLAGS_FIXED 0x00000002u

/* IVT wiring: serviced vectors point at a per-interrupt trampoline cell
 * (F000:E000 + n*4, holding "INT n; IRET") so that programs which fetch a
 * vector with INT 21h AH=35h and far-call it get real BIOS-like behaviour.
 * Unserviced vectors point at the F000:FFE0 IRET stub. */
#define MAGIC_SEG 0xF000u
#define MAGIC_OFF 0xFFF0u
#define STUB_OFF  0xFFE0u
#define TRAMP_SEG 0xF000u
#define TRAMP_BASE 0xE000u
#define TRAMP_SLOT(n) ((u16)(TRAMP_BASE + (n) * 4))

u16  emu_in_port(Emu *e, u16 port);
void emu_kbd_pump(Emu *e);
int  emu_kbd_peek(Emu *e);
int  emu_kbd_get_full(Emu *e, int *scan, int *ascii);
void emu_out_port(Emu *e, u16 port, u16 val);

int  emu_shell_exec(const char *name, const char *args);

int  emu_init(Emu *e);
void emu_free(Emu *e);
int  emu_load_program(Emu *e, u16 psp_seg, const u8 *img, u32 size, const char *argline);
int  emu_exec_program(Emu *e, const char *prog, const char *args);
int  emu_run(Emu *e);

u8   emu_rb(Emu *e, u16 seg, u16 off);
void emu_wb(Emu *e, u16 seg, u16 off, u8 v);
u16  emu_rw(Emu *e, u16 seg, u16 off);
void emu_ww(Emu *e, u16 seg, u16 off, u16 v);
u32  emu_rd(Emu *e, u16 seg, u16 off);
void emu_wd(Emu *e, u16 seg, u16 off, u32 v);

/* 32-bit-offset variants (386 segmented access) */
u8   emu_rb32(Emu *e, u16 seg, u32 off);
void emu_wb32(Emu *e, u16 seg, u32 off, u8 v);
u16  emu_rw32(Emu *e, u16 seg, u32 off);
void emu_ww32(Emu *e, u16 seg, u32 off, u16 v);
u32  emu_rd32(Emu *e, u16 seg, u32 off);
void emu_wd32(Emu *e, u16 seg, u32 off, u32 v);

/* machine-state helpers shared with emu/dpmi.c */
/* graphics / DAC services (INT 10h modes 13h, 100h, 101h, 103h) */
void emu_set_video_mode(Emu *e, u16 mode);
int  emu_gfx_geometry(u16 mode, int *w, int *h);
void emu_dac_reset(Emu *e);
void emu_dac_read(Emu *e, u8 index, u8 out[3]);
void emu_dac_write(Emu *e, u8 index, u8 r, u8 g, u8 b);
void emu_video_flush(Emu *e);          /* repaint now (text mirror or framebuffer) */
void emu_seg_cache_sync(Emu *e);
void emu_set_seg(Emu *e, int idx, u16 sel);
void emu_push16(Emu *e, u16 v);
u16  emu_pop16(Emu *e);

/* INT 21h/10h/16h handlers (emu/int21.c) */
void int21_handler(Emu *e);
void int10_handler(Emu *e);
void int16_handler(Emu *e);
void int13_handler(Emu *e);
void int1a_handler(Emu *e);
void int2f_handler(Emu *e);
int  fcb_int21(Emu *e, u8 ah);
void int31_handler(Emu *e);   /* DPMI (emu/dpmi.c) */
void int60_handler(Emu *e);   /* DPMI real-mode host entry */
int  dpmi_install_check(Emu *e);
void pm_patch_idt(Emu *e);    /* install service gates after LIDT */
int  step_public(Emu *e);     /* execute one instruction (DPMI reflection) */
int  itest_run(const char *filter); /* emu/itest.c: instruction-level tests */
int  ifuzz_run(int verbose);        /* emu/itest.c: brute-force opcode audit */
extern int emu_unsupported_ops;     /* number of unsupported opcodes executed */
extern int emu_unsupported_quiet;   /* 1 = do not print them (fuzz mode) */

/* Linear address of the DPMI host LDT handed to protected-mode clients. */
#define DPMI_LDT_LIN 0x00F00000u
/* DPMI host descriptor tables and the real-mode entry point. */
#define DPMI_GDT_LIN 0x00F08000u
#define DPMI_IDT_LIN 0x00F10000u
#define DPMI_ENTRY_SEG 0xF000u
#define DPMI_ENTRY_OFF 0xF000u

#endif
