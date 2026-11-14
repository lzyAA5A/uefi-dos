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
    /* INT 10h AH=00 also selects the CGA packed modes 4/5/6, whose VRAM is at
     * B8000 in the real even/odd-interleaved layout.  gfx_kind says how the
     * flush has to read it: 0 = linear 8bpp at A0000, 1 = CGA 2bpp (mode 4/5),
     * 2 = CGA 1bpp (mode 6). */
    int gfx_kind;
    /* CGA attribute state (INT 10h AH=0Bh and the 3D8h/3D9h ports): palette
     * 0/1, background index and the intensity bit.  Expanded into the DAC so
     * the platform layer never has to know about CGA colour rules. */
    u8 cga_pal, cga_bg, cga_int;
    /* INT 33h mouse state */
    int  mouse_x, mouse_y;
    int  mouse_minx, mouse_miny, mouse_maxx, mouse_maxy;
    int  mouse_buttons;          /* bit0 left, bit1 right, bit2 middle */
    int  mouse_visible;          /* show count > 0 */
    int  mouse_show_count;
    u16  mouse_press[3];         /* press counters per button */
    u16  mouse_release[3];       /* release counters per button */
    u16  mouse_handler_off, mouse_handler_seg;
    u16  mouse_mask;             /* event mask for the callback */
    int  mouse_pending_events;   /* events not yet delivered to the handler */
    int  mouse_cb_active;        /* inside the user callback right now */
    u16  mouse_cb_ip, mouse_cb_cs, mouse_cb_sp, mouse_cb_ss; /* return point */
    int  mouse_dx, mouse_dy;     /* motion accumulated since the last query */
    int  irq_depth;              /* hardware interrupt currently executing */
u32  irq_steps;              /* steps spent inside that handler (watchdog) */
    int  wrote_ss;               /* this instruction loaded SS: no IRQ yet (8086 rule) */
    u32  irq_last_tick_ms;       /* last INT 8h injection */
    u32  bios_ticks;             /* timer ticks counted into 0040:006C */
    int  kbd_irq_pending;        /* a key is waiting for IRQ1 delivery */
    u8   kbd_irq_scan;           /* scan code latched for port 60h */
    int  kbd_out_valid;          /* port 60h holds an unread scan code */
    int  bus_irq_pending;        /* bus mouse (InPort) raised IRQ5 */
    u16  irq_ip, irq_cs, irq_sp, irq_ss;  /* return point of the IRQ handler */
    u16  irq_frame_ss, irq_frame_sp;     /* where our frame sits in the guest stack */
    /* A DOS call that arrives through a chained vector (program hook -> saved
     * vector = our "INT n; IRET" trampoline) returns to the *original* caller
     * with the trampoline's IRET, which pops the flags pushed by that INT.  DOS
     * solves this by writing the result flags into the caller's saved frame, so
     * the carry flag survives the IRET; these fields carry that one-shot
     * override to the IRET implementation. */
    int  iret_fl_ok;
    u16  iret_fl_ss, iret_fl_sp, iret_fl_val;
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

/* The two hardware vectors get a *real* BIOS-style handler instead of the
 * do-nothing IRET stub, because that is what makes the machine's timer and
 * keyboard behave like an IBM PC/XT:
 *   INT 08h: push ax/ds, ds=0040, inc word [006C] (with [006E] carry),
 *            int 1Ch, EOI on port 20h, pop, iret -- so a program that hooks
 *            INT 1Ch (or chains the old INT 8) sees every timer tick.
 *   INT 09h: push ax, in al,60h, EOI on port 20h, pop, iret.
 * Both live in the unused part of the ROM window (the trampolines start at
 * F000:E000 and the IRET stub sits at F000:FFE0). */
#define BIOS_IRQ8_OFF 0x2300u
#define BIOS_IRQ9_OFF 0x2340u

/* The address of the last INT 21h that was issued by program code rather than
 * from inside one of our own trampoline cells.  A program that hooks INT 21h and
 * then chains to the saved vector re-enters the service through the trampoline,
 * so the instruction pointer at that moment is F000:E084 and useless for
 * "who called DOS" questions; this remembers the real call site instead. */
extern u16 g_i21_caller_cs;
extern u16 g_i21_caller_ip;

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
/* tail128: the caller's 128-byte command-tail buffer (PSP:0080h..00FFh) to be
 * copied verbatim into the child PSP, or NULL to synthesise one from args. */
int  emu_exec_program_ex(Emu *e, const char *prog, const char *args, const u8 *tail128);
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
void emu_cga_palette(Emu *e);          /* rebuild DAC entries 0..3 for CGA modes */
void emu_mouse_pump(Emu *e);           /* poll the host pointer, queue events */
int  emu_mouse_available(void);
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
void int15_handler(Emu *e);   /* INT 15h BIOS services (AH=88h/86h/84h) */
void int2f_handler(Emu *e);
void int33_handler(Emu *e);   /* INT 33h mouse driver (emu/int33.c) */
void dos_jft_sync(Emu *e);    /* publish PSP:18h job file table (emu/int21.c) */
int  fcb_int21(Emu *e, u8 ah);
void int31_handler(Emu *e);   /* DPMI (emu/dpmi.c) */
void int60_handler(Emu *e);   /* DPMI real-mode host entry */
int  dpmi_install_check(Emu *e);
void pm_patch_idt(Emu *e);    /* install service gates after LIDT */
int  step_public(Emu *e);     /* execute one instruction (DPMI reflection) */
const char *emu_current_program(void); /* DOS path of the running program */
int  itest_run(const char *filter); /* emu/itest.c: instruction-level tests */
int  ifuzz_run(int verbose);        /* emu/itest.c: brute-force opcode audit */
extern int emu_unsupported_ops;     /* number of unsupported opcodes executed */
extern int emu_unsupported_quiet;   /* 1 = do not print them (fuzz mode) */
/* EXE image placement.  0 (default, what DOS itself does) loads the image right
 * after the PSP, so the program's block grows upwards.  1 loads it flush
 * against the top of the caller's free block, leaving the space between PSP and
 * image free -- the geometry real Windows 1.x kernels end up running in, since
 * they compute their own arena from PSP+2 and then expect the image out of the
 * way.  Set with CONFIG.SYS "EXECHIGH=ON" (or the host knob
 * UEFIDOS_EXEC_HIGH_IMAGE). */
extern int g_exec_image_high;

/* Host counts per count reported by the emulated Microsoft bus mouse (the card
 * Windows 1.x drives).  0 = built-in default 1 (one reported count per host
 * count, i.e. 2 CGA pixels); CONFIG.SYS "MOUSESPEED=<n>" sets it, larger is
 * slower. */
extern int g_mouse_speed;

/* Linear address of the DPMI host LDT handed to protected-mode clients. */
#define DPMI_LDT_LIN 0x00F00000u
/* DPMI host descriptor tables and the real-mode entry point. */
#define DPMI_GDT_LIN 0x00F08000u
#define DPMI_IDT_LIN 0x00F10000u
/* INT 33h callback: the driver far-calls the client handler, which returns with
 * RETF.  The injected frame returns to a stub in the BIOS area. */
#define MOUSE_STUB_SEG 0xF000u
#define MOUSE_STUB_OFF 0xFFC0u
#define DPMI_ENTRY_SEG 0xF000u
#define DPMI_ENTRY_OFF 0xF000u

#endif

/* Dump the last emulated steps to the serial console when the guest prints its
 * own fatal message (Windows 1.x FatalExit). */
void emu_fatal_trace(Emu *e);

/* Record a DOS call and its caller for the fatal-exit ring. */
void emu_i21_note(u16 cs, u16 ip, u8 ah);
