/*
 * UEFI-DOS -- instruction-level test framework ("8086 test vectors").
 *
 * The 8086 interpreter in emu/emu.c is one very large switch; a regression in
 * it shows up as "some game renders garbage" days later.  This file gives every
 * opcode a place to be tested in isolation, the way QEMU's own tests/ does it: a
 * test case is a byte stream plus a register/memory expectation, executed
 * through the real decoder on a scratch segment.
 *
 * A case is data, not code:
 *
 *     CASE("add ax,bx", c_add_rr,
 *          {.ax = 0x1234, .bx = 0x0111},   // registers before
 *          {.ax = 0x1345},                 // registers expected after
 *          IT_C_AX, 1,                     // what to verify, instructions to run
 *          0, 0, 0, 0, 0);                 // optional memory: bytes, mexp, seg, off, len
 *
 * Run:
 *     hosttest.exe --itest            # every case
 *     hosttest.exe --itest add        # only cases whose name contains "add"
 *
 * Add a case for every opcode you touch: this is the cheapest possible way to
 * stop the interpreter from regressing.
 */
#include "plat.h"
#include "emu/emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct it_regs {
    u16 ax, bx, cx, dx, si, di, bp, sp, ds, es, ss, cs, ip, flags;
} it_regs;

/* what a case verifies */
#define IT_C_AX 0x00001
#define IT_C_BX 0x00002
#define IT_C_CX 0x00004
#define IT_C_DX 0x00008
#define IT_C_SI 0x00010
#define IT_C_DI 0x00020
#define IT_C_BP 0x00040
#define IT_C_SP 0x00080
#define IT_C_DS 0x00100
#define IT_C_ES 0x00200
#define IT_C_SS 0x00400
#define IT_C_IP 0x00800
#define IT_C_FLAGS 0x01000
#define IT_C_CS 0x04000
#define IT_C_MEM 0x02000

typedef struct it_case {
    const char *name;
    const u8   *code;
    u32         code_len;
    it_regs     in;
    it_regs     out;
    u32         check_mask;
    int         steps;         /* instructions to execute (0 = 1) */
    const u8   *mem;           /* bytes to preload before running */
    const u8   *mexp;          /* bytes expected afterwards (IT_C_MEM) */
    u16         mem_seg, mem_off;
    u32         mem_len;       /* how many bytes of mem/mexp to use */
} it_case;

#define CASE(nm, cd, in_, out_, mask, nsteps, mem_, mexp_, seg_, off_, len_) \
    { nm, cd, (u32)sizeof(cd), in_, out_, mask, nsteps, mem_, mexp_, seg_, off_, len_ }

/* scratch segment: nothing else touches 0x9000 while the tests run */
#define IT_SEG       0x9000u
#define IT_CODE_OFF  0x0100u
#define IT_DATA_OFF  0x0400u

/* ---- code streams ---- */
static const u8 c_add_rr[]    = { 0x01, 0xD8 };              /* add ax,bx */
static const u8 c_sub_rr[]    = { 0x29, 0xD8 };              /* sub ax,bx */
static const u8 c_add_dir[]   = { 0x00, 0xD8 };              /* add al,bl */
static const u8 c_mov_rm[]    = { 0x8B, 0x07 };              /* mov ax,[bx] */
static const u8 c_mov_mr[]    = { 0x89, 0x07 };              /* mov [bx],ax */
static const u8 c_shl_ax1[]   = { 0xD1, 0xE0 };              /* shl ax,1 */
static const u8 c_shr_ax1[]   = { 0xD1, 0xE8 };              /* shr ax,1 */
static const u8 c_sar_ax1[]   = { 0xD1, 0xF8 };              /* sar ax,1 */
static const u8 c_rol_al[]    = { 0xD0, 0xC0 };              /* rol al,1 */
static const u8 c_ror_ax1[]   = { 0xD1, 0xC8 };              /* ror ax,1 */
static const u8 c_rcl_ax1[]   = { 0xD1, 0xD0 };              /* rcl ax,1 */
static const u8 c_sar_ax_cl[] = { 0xD3, 0xF8 };              /* sar ax,cl */
static const u8 c_mul[]       = { 0xF7, 0xE3 };              /* mul bx */
static const u8 c_imul[]      = { 0xF7, 0xEB };              /* imul bx */
static const u8 c_div[]       = { 0xF7, 0xF3 };              /* div bx */
static const u8 c_idiv[]      = { 0xF7, 0xFB };              /* idiv bx */
static const u8 c_inc[]       = { 0x40 };                    /* inc ax */
static const u8 c_dec[]       = { 0x48 };                    /* dec ax */
static const u8 c_push_pop[]  = { 0x50, 0x5B };              /* push ax; pop bx */
static const u8 c_xchg[]      = { 0x93 };                    /* xchg ax,bx */
static const u8 c_lea[]       = { 0x8D, 0x47, 0x10 };        /* lea ax,[bx+10h] */
static const u8 c_rep_stosw[] = { 0xF3, 0xAB };              /* rep stosw */
static const u8 c_rep_movsw[] = { 0xF3, 0xA5 };              /* rep movsw */
static const u8 c_jz[]        = { 0x74, 0x02, 0x90, 0x90 };  /* jz $+4 */
static const u8 c_jnz[]       = { 0x75, 0x02, 0x90, 0x90 };  /* jnz $+4 */
static const u8 c_jmp[]       = { 0xEB, 0x02, 0x90, 0x90 };  /* jmp $+4 */
static const u8 c_cmp[]       = { 0x39, 0xD8 };              /* cmp ax,bx */
static const u8 c_test[]      = { 0x85, 0xD8 };              /* test ax,bx */
static const u8 c_and_or[]    = { 0x21, 0xD8, 0x09, 0xD8 };  /* and ax,bx; or ax,bx */
static const u8 c_xor[]       = { 0x31, 0xD8 };              /* xor ax,bx */
static const u8 c_neg[]       = { 0xF7, 0xDB };              /* neg bx */
static const u8 c_sahf[]      = { 0x9E };                    /* sahf */
static const u8 c_lahf[]      = { 0x9F };                    /* lahf */
static const u8 c_not[]       = { 0xF7, 0xD3 };              /* not bx */
static const u8 c_cbw[]       = { 0x98 };                    /* cbw */
static const u8 c_cwd[]       = { 0x99 };                    /* cwd */
static const u8 c_aam[]       = { 0xD4, 0x0A };              /* aam */
static const u8 c_salc_cf[]   = { 0xF9, 0xD6 };              /* stc; salc */
static const u8 c_salc_nc[]   = { 0xF8, 0xD6 };              /* clc; salc */
static const u8 c_lodsb[]     = { 0xAC };                    /* lodsb */
static const u8 c_xlat[]      = { 0xD7 };                    /* xlat */
static const u8 c_les[]       = { 0xC4, 0x07 };              /* les ax,[bx] */
static const u8 c_seg_ovr[]   = { 0x26, 0x8B, 0x07 };        /* mov ax,es:[bx] */
static const u8 c_imul3[]     = { 0x6B, 0xC3, 0x05 };        /* imul ax,bx,5 */
static const u8 c_bound[]     = { 0x62, 0x07 };              /* bound ax,[bx] */
static const u8 c_arpl[]      = { 0x63, 0xD8 };              /* arpl ax,bx */
static const u8 c_insb[]      = { 0x6C };                    /* insb */
static const u8 c_outsb[]     = { 0x6E };                    /* outsb */
static const u8 c_add_imm[]   = { 0x05, 0x10, 0x27 };        /* add ax,2710h */
static const u8 c_mov_imm[]   = { 0xB8, 0x34, 0x12 };        /* mov ax,1234h */
static const u8 c_jmp_far[]   = { 0xEA, 0x00, 0x02, 0x00, 0x90 }; /* jmp 9000:0200 */
static const u8 c_repnz_scasb[] = { 0xF3, 0xAE };             /* repz scasb */
static const u8 c_rep_movsb_df1[] = { 0xF3, 0xA4 };           /* rep movsb (DF=1) */
static const u8 c_lodsb_df1[]  = { 0xAC };                    /* lodsb (DF=1) */
static const u8 c_mov_bp_neg[]  = { 0x8B, 0x46, 0xF8 };        /* mov ax,[bp-08h] */
static const u8 c_mov_bp_neg_w[] = { 0x89, 0x46, 0xFC };       /* mov [bp-04h],ax */
static const u8 c_mov_bx_si_neg[] = { 0x8B, 0x40, 0xFE };      /* mov ax,[bx+si-02h] */
static const u8 c_mov_bp_pos[]  = { 0x8B, 0x46, 0x04 };        /* mov ax,[bp+04h] */
static const u8 c_rep_stosb_df1[] = { 0xF3, 0xAA };           /* rep stosb (DF=1) */

/* 386 additions: POPF keeping IOPL/NT (DOS/16M's CPU probe) and the
   0F BA BT/BTS/BTR/BTC immediate forms (the DOOM 0.99 stub died on them). */
static const u8 c_popf_iopl[]  = { 0xB8, 0x00, 0xF0, 0x50, 0x9D, 0x9C, 0x58 };  /* mov ax,0F000h; push ax; popf; pushf; pop ax */
static const u8 c_bt_imm[]     = { 0x0F, 0xBA, 0xE0, 0x0C };  /* bt ax,12 */
static const u8 c_bts_imm[]    = { 0x0F, 0xBA, 0xE8, 0x03 };  /* bts ax,3 */
static const u8 c_btr_imm[]    = { 0x0F, 0xBA, 0xF0, 0x01 };  /* btr ax,1 */
static const u8 c_btc_imm[]    = { 0x0F, 0xBA, 0xF8, 0x00 };  /* btc ax,0 */

/* ---- 186 / 286 sweep ----
 * Every stream below exists because the decoder used to get that instruction
 * wrong (PUSH SP pushed the pre-decrement SP, so `cmp ax,sp` said "8086"),
 * silently ignored it (BOUND), or answered a stub (SMSW wrote CR0, VERR
 * overwrote its operand with the task register). */
static const u8 c_push_sp[]     = { 0x54, 0x58 };                    /* push sp; pop ax */
static const u8 c_pusha[]       = { 0x60 };                          /* pusha */
static const u8 c_enter0[]      = { 0xC8, 0x08, 0x00, 0x00 };        /* enter 8,0 */
static const u8 c_enter2[]      = { 0xC8, 0x04, 0x00, 0x02 };        /* enter 4,2 */
static const u8 c_shl_cl[]      = { 0xD3, 0xE0 };                    /* shl ax,cl */
static const u8 c_shl_imm4[]    = { 0xC1, 0xE0, 0x04 };              /* shl ax,4 (186 imm8) */
static const u8 c_lmsw_smsw[]   = { 0xB9, 0x0C, 0x00,              /* mov cx,0Ch */
                                    0x0F, 0x01, 0xF1,              /* lmsw cx   */
                                    0x0F, 0x01, 0xE0,              /* smsw ax   */
                                    0x33, 0xDB,                    /* xor bx,bx */
                                    0x0F, 0x01, 0xF3 };            /* lmsw bx (restore) */
static const u8 c_verr[]        = { 0xB8, 0x88, 0x00,              /* mov ax,88h */
                                    0x0F, 0x00, 0xD0,              /* ltr ax    */
                                    0x33, 0xC0,                    /* xor ax,ax */
                                    0x0F, 0x00, 0xE0 };            /* verr ax   */
static const u8 c_verw[]        = { 0xB8, 0x88, 0x00,              /* mov ax,88h */
                                    0x0F, 0x00, 0xD0,              /* ltr ax    */
                                    0x33, 0xC0,                    /* xor ax,ax */
                                    0x0F, 0x00, 0xE8 };            /* verw ax   */
static const u8 c_lar_null[]    = { 0xB8, 0x88, 0x00,              /* mov ax,88h */
                                    0x0F, 0x00, 0xD0,              /* ltr ax    */
                                    0x33, 0xC0,                    /* xor ax,ax */
                                    0x0F, 0x02, 0xD0 };            /* lar dx,ax */
static const u8 c_lsl_null[]    = { 0x33, 0xC0,                    /* xor ax,ax */
                                    0x0F, 0x03, 0xD0 };            /* lsl dx,ax */
static const u8 c_bsf_hit[]     = { 0x0F, 0xBC, 0xC3 };              /* bsf ax,bx */
static const u8 c_bsf_zero[]    = { 0x0F, 0xBC, 0xC3 };              /* bsf ax,bx (bx=0) */
static const u8 c_bsr_hit[]     = { 0x0F, 0xBD, 0xC3 };              /* bsr ax,bx */
static const u8 c_cmpxchg_eq[]  = { 0x0F, 0xB1, 0xD3 };              /* cmpxchg bx,dx */
static const u8 c_cmpxchg_ne[]  = { 0x0F, 0xB1, 0xD3 };              /* cmpxchg bx,dx */
static const u8 c_xadd[]        = { 0x0F, 0xC1, 0xD3 };              /* xadd bx,dx */
static const u8 c_bswap[]       = { 0x66, 0xB9, 0x78, 0x56, 0x34, 0x12,
                                    0x0F, 0xC9,                    /* bswap ecx */
                                    0x89, 0xC8 };                  /* mov ax,cx */
static const u8 c_lgs[]         = { 0x0F, 0xB5, 0x07 };              /* lgs ax,[bx] */
static const u8 c_insw[]        = { 0x6D };                          /* insw */
/* ---- x87 (8087) streams.  Every one starts with FNINIT so the cases are
 * independent of each other: the emulator instance is shared.  DD 1F is
 * "fstp qword [bx]", DF 1F "fistp word [bx]", DF 07 "fild word [bx]". ---- */
static const u8 c_fpu_fadd[]    = { 0xDB, 0xE3,                    /* fninit */
                                    0xD9, 0xE8,                    /* fld1 */
                                    0xD8, 0xC0,                    /* fadd st0,st0 */
                                    0xDD, 0x1F };                  /* fstp qword [bx] */
static const u8 c_fpu_sqrt[]    = { 0xDB, 0xE3, 0xD9, 0xE8, 0xD8, 0xC0,
                                    0xD9, 0xFA,                    /* fsqrt */
                                    0xDD, 0x1F };
static const u8 c_fpu_fild[]    = { 0xDB, 0xE3,
                                    0xDF, 0x07,                    /* fild word [bx] */
                                    0xD8, 0xC0,                    /* fadd st0,st0 */
                                    0xDF, 0x1F };                  /* fistp word [bx] */
static const u8 c_fpu_fscale[]  = { 0xDB, 0xE3,
                                    0xDF, 0x07,                    /* fild word [bx] = 3 */
                                    0xD9, 0xE8,                    /* fld1 */
                                    0xD9, 0xFD,                    /* fscale -> 8 */
                                    0xDF, 0x1F };                  /* fistp word [bx] */
static const u8 c_fpu_fcom[]    = { 0xDB, 0xE3,
                                    0xD9, 0xE8,                    /* fld1 */
                                    0xD9, 0xEE,                    /* fldz  (st0=0, st1=1) */
                                    0xD8, 0xD1,                    /* fcom st1 -> C0=1 */
                                    0xDF, 0xE0 };                  /* fnstsw ax */
static const u8 c_fpu_fchs[]    = { 0xDB, 0xE3, 0xD9, 0xE8,
                                    0xD9, 0xE0,                    /* fchs */
                                    0xDD, 0x1F };
static const u8 c_outsw[]       = { 0x6F };                          /* outsw */

/* ---- memory fixtures (8 bytes each) ---- */
static const u8 m_ff_tail[]      = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_ff_b0[]        = { 0xFF, 0xFF, 0xFF, 0xB0, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_ff_b0_fwd[]    = { 0xFF, 0xFF, 0xB0, 0xFF, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_df1_mov_src[]  = { 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_df1_mov_dst[]  = { 0x00, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11, 0x00 };

static const u8 m_words[]        = { 0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_mov_rm_expect[] = { 0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_mov_mr_expect[] = { 0xBB, 0xAA, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_stos_expect[]   = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x00, 0x00 };
static const u8 m_movsw_expect[]  = { 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12 };
static const u8 m_lods_src[]      = { 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_bound_lim[]     = { 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 m_zeros16[]       = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
/* PUSHA's stack image, lowest address first.  The pushes go AX CX DX BX SP BP
 * SI DI, so the LAST one pushed (DI) sits lowest: DI SI BP SP BX DX CX AX. */
static const u8 m_pusha_stack[]   = { 0x66, 0x66, 0x55, 0x55, 0x77, 0x77, 0x00, 0xFF,
                                      0x22, 0x22, 0x44, 0x44, 0x33, 0x33, 0x11, 0x11 };
/* x87 fixtures: 2.0 and -1.0 as IEEE doubles, sqrt(2) as an IEEE double,
 * and the 16-bit integers the FILD/FIST cases use. */
static const u8 m_fpu_two[]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40 };
static const u8 m_fpu_neg1[]    = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xBF };
static const u8 m_fpu_sqrt2[]   = { 0xCD, 0x3B, 0x7F, 0x66, 0x9E, 0xA0, 0xF6, 0x3F };
static const u8 m_fpu_three[]   = { 0x03, 0x00 };
static const u8 m_fpu_eight[]   = { 0x08, 0x00 };
static const u8 m_fpu_six[]     = { 0x06, 0x00 };

static const it_case g_cases[] = {
    /* --- ALU with the register as destination.  Real bug: the result was
     * written back to the SOURCE operand (sub bx,ax left BX alone). --- */
    CASE("add ax,bx (reg dst)", c_add_rr, ((it_regs){.ax = 0x1234, .bx = 0x0111}), ((it_regs){.ax = 0x1345, .bx = 0x0111}), IT_C_AX | IT_C_BX, 1, 0, 0, 0, 0, 0),
    CASE("sub ax,bx (reg dst)", c_sub_rr, ((it_regs){.ax = 0x1234, .bx = 0x0111}), ((it_regs){.ax = 0x1123, .bx = 0x0111}), IT_C_AX | IT_C_BX, 1, 0, 0, 0, 0, 0),
    CASE("add al,bl (r/m dst)", c_add_dir, ((it_regs){.ax = 0x1204, .bx = 0x0101}), ((it_regs){.ax = 0x1205}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("add ax,imm16", c_add_imm, ((it_regs){.ax = 0x0001}), ((it_regs){.ax = 0x2711}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("mov ax,imm16", c_mov_imm, ((it_regs){.ax = 0}), ((it_regs){.ax = 0x1234}), IT_C_AX, 1, 0, 0, 0, 0, 0),

    /* --- group-2 shifts.  Real bug: selector order was grouped wrong, so
     * D1 E0 (shl ax,1) executed as ror ax,1 and every game glyph landed one
     * column off.  Architectural order: 0=ROL 1=ROR 2=RCL 3=RCR 4=SHL 5=SHR
     * 6=SAL 7=SAR. --- */
    CASE("shl ax,1", c_shl_ax1, ((it_regs){.ax = 0x4000}), ((it_regs){.ax = 0x8000}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("shr ax,1", c_shr_ax1, ((it_regs){.ax = 0x0002}), ((it_regs){.ax = 0x0001}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("sar ax,1", c_sar_ax1, ((it_regs){.ax = 0x8000}), ((it_regs){.ax = 0xC000}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("rol al,1", c_rol_al, ((it_regs){.ax = 0x0081}), ((it_regs){.ax = 0x0003}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("ror ax,1", c_ror_ax1, ((it_regs){.ax = 0x0001}), ((it_regs){.ax = 0x8000}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("rcl ax,1 (cf=0)", c_rcl_ax1, ((it_regs){.ax = 0x4000, .flags = 0x0202}), ((it_regs){.ax = 0x8000}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("sar ax,cl (4)", c_sar_ax_cl, ((it_regs){.ax = 0x8000, .cx = 4}), ((it_regs){.ax = 0xF800}), IT_C_AX, 1, 0, 0, 0, 0, 0),

    /* --- mul / div --- */
    CASE("mul bx (3*4)", c_mul, ((it_regs){.ax = 3, .bx = 4}), ((it_regs){.ax = 12, .dx = 0}), IT_C_AX | IT_C_DX, 1, 0, 0, 0, 0, 0),
    CASE("imul bx (-2*3)", c_imul, ((it_regs){.ax = 0xFFFE, .bx = 3}), ((it_regs){.ax = 0xFFFA, .dx = 0xFFFF}), IT_C_AX | IT_C_DX, 1, 0, 0, 0, 0, 0),
    CASE("div bx (0x18000/2)", c_div, ((it_regs){.ax = 0x8000, .dx = 1, .bx = 2}), ((it_regs){.ax = 0xC000, .dx = 0}), IT_C_AX | IT_C_DX, 1, 0, 0, 0, 0, 0),
    CASE("idiv bx (-10/2)", c_idiv, ((it_regs){.ax = 0xFFF6, .dx = 0xFFFF, .bx = 2}), ((it_regs){.ax = 0xFFFB, .dx = 0}), IT_C_AX | IT_C_DX, 1, 0, 0, 0, 0, 0),

    /* --- inc/dec preserve CF (DOS programs rely on loop counters) --- */
    CASE("inc ax", c_inc, ((it_regs){.ax = 0x7FFF}), ((it_regs){.ax = 0x8000}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("dec ax", c_dec, ((it_regs){.ax = 0x0000}), ((it_regs){.ax = 0xFFFF}), IT_C_AX, 1, 0, 0, 0, 0, 0),

    /* --- stack / exchange / address formation --- */
    CASE("push ax; pop bx", c_push_pop, ((it_regs){.ax = 0xBEEF, .sp = 0xFF00}), ((it_regs){.bx = 0xBEEF, .sp = 0xFF00}), IT_C_BX | IT_C_SP, 2, 0, 0, 0, 0, 0),
    CASE("xchg ax,bx", c_xchg, ((it_regs){.ax = 0x1111, .bx = 0x2222}), ((it_regs){.ax = 0x2222, .bx = 0x1111}), IT_C_AX | IT_C_BX, 1, 0, 0, 0, 0, 0),
    CASE("lea ax,[bx+10h]", c_lea, ((it_regs){.bx = 0x0100}), ((it_regs){.ax = 0x0110}), IT_C_AX, 1, 0, 0, 0, 0, 0),

    /* --- memory forms --- */
    CASE("mov ax,[bx]", c_mov_rm, ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.ax = 0x5678}), IT_C_AX, 1, m_words, m_mov_rm_expect, IT_SEG, IT_DATA_OFF, 4),
    CASE("mov [bx],ax", c_mov_mr, ((it_regs){.ax = 0xAABB, .bx = IT_DATA_OFF}), ((it_regs){}), IT_C_MEM, 1, m_words, m_mov_mr_expect, IT_SEG, IT_DATA_OFF, 4),
    CASE("mov ax,es:[bx]", c_seg_ovr, ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.ax = 0x5678}), IT_C_AX, 1, m_words, m_mov_rm_expect, IT_SEG, IT_DATA_OFF, 2),
    CASE("les ax,[bx]", c_les, ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.ax = 0x5678, .es = 0x1234}), IT_C_AX | IT_C_ES, 1, m_words, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("lodsb", c_lodsb, ((it_regs){.si = IT_DATA_OFF}), ((it_regs){.ax = 0x00CD, .si = IT_DATA_OFF + 1}), IT_C_AX | IT_C_SI, 1, m_lods_src, 0, IT_SEG, IT_DATA_OFF, 2),
    CASE("xlat", c_xlat, ((it_regs){.ax = 0x0002, .bx = IT_DATA_OFF}), ((it_regs){.ax = 0x0034}), IT_C_AX, 1, m_words, 0, IT_SEG, IT_DATA_OFF, 4),

    /* --- string ops (DF clear) --- */
    CASE("rep stosw (3 words, 2 store units)", c_rep_stosw, ((it_regs){.ax = 0xAAAA, .cx = 3, .di = IT_DATA_OFF}), ((it_regs){.cx = 0, .di = IT_DATA_OFF + 6}), IT_C_CX | IT_C_DI | IT_C_MEM, 1, m_words, m_stos_expect, IT_SEG, IT_DATA_OFF, 6),
    CASE("rep movsw (2 words)", c_rep_movsw, ((it_regs){.cx = 2, .si = IT_DATA_OFF, .di = IT_DATA_OFF + 4}), ((it_regs){.cx = 0, .si = IT_DATA_OFF + 4, .di = IT_DATA_OFF + 8}), IT_C_CX | IT_C_SI | IT_C_DI | IT_C_MEM, 1, m_words, m_movsw_expect, IT_SEG, IT_DATA_OFF, 8),

    /* --- string ops with DF=1 (backwards).  EXEPACK's loader stub walks its
     * compressed stream downwards, so every one of these must work in both
     * directions.  Real bug found with SimEarth's INSTALL.EXE: repz scasb
     * stopped on the FIRST compare even though the byte matched AL. --- */
    CASE("repz scasb DF=1 all match", c_repnz_scasb, ((it_regs){.ax = 0x00FF, .cx = 4, .di = IT_DATA_OFF + 3, .flags = 0x0400}), ((it_regs){.cx = 0, .di = IT_DATA_OFF - 1}), IT_C_CX | IT_C_DI, 1, m_ff_tail, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("repz scasb DF=1 stops at B0", c_repnz_scasb, ((it_regs){.ax = 0x00FF, .cx = 4, .di = IT_DATA_OFF + 3, .flags = 0x0400}), ((it_regs){.cx = 3, .di = IT_DATA_OFF + 2}), IT_C_CX | IT_C_DI, 1, m_ff_b0, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("repz scasb DF=0 stops at B0", c_repnz_scasb, ((it_regs){.ax = 0x00FF, .cx = 4, .di = IT_DATA_OFF, .flags = 0x0002}), ((it_regs){.cx = 1, .di = IT_DATA_OFF + 3}), IT_C_CX | IT_C_DI, 1, m_ff_b0_fwd, 0, IT_SEG, IT_DATA_OFF, 4),
    /* --- signed 8-bit displacement.  Real bug: the 16-bit address decoder
     * zero-extended disp8, so `mov [bp-4],ax` stored to [bp+0FCh] and every
     * Microsoft/Turbo C stack frame quietly corrupted its caller. --- */
    CASE("mov ax,[bp-8] (disp8 negative)", c_mov_bp_neg, ((it_regs){.bp = IT_DATA_OFF + 8}), ((it_regs){.ax = 0x5678}), IT_C_AX, 1, m_words, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("mov [bp-4],ax (disp8 negative)", c_mov_bp_neg_w, ((it_regs){.ax = 0xAABB, .bp = IT_DATA_OFF + 4}), ((it_regs){}), IT_C_MEM, 1, m_words, m_mov_mr_expect, IT_SEG, IT_DATA_OFF, 4),
    CASE("mov ax,[bx+si-2] (disp8 negative)", c_mov_bx_si_neg, ((it_regs){.bx = IT_DATA_OFF + 2, .si = 0}), ((it_regs){.ax = 0x5678}), IT_C_AX, 1, m_words, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("mov ax,[bp+4] (disp8 positive)", c_mov_bp_pos, ((it_regs){.bp = IT_DATA_OFF - 4}), ((it_regs){.ax = 0x5678}), IT_C_AX, 1, m_words, 0, IT_SEG, IT_DATA_OFF, 4),

    CASE("rep movsb DF=1 (4 bytes)", c_rep_movsb_df1, ((it_regs){.cx = 4, .si = IT_DATA_OFF + 3, .di = IT_DATA_OFF + 7, .flags = 0x0400}), ((it_regs){.cx = 0, .si = IT_DATA_OFF - 1, .di = IT_DATA_OFF + 3}), IT_C_CX | IT_C_SI | IT_C_DI, 1, m_df1_mov_src, m_df1_mov_dst, IT_SEG, IT_DATA_OFF, 8),
    CASE("lodsb DF=1", c_lodsb_df1, ((it_regs){.si = IT_DATA_OFF + 2, .flags = 0x0400}), ((it_regs){.ax = 0x0033, .si = IT_DATA_OFF + 1}), IT_C_AX | IT_C_SI, 1, m_df1_mov_src, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("rep stosb DF=1 (2 bytes)", c_rep_stosb_df1, ((it_regs){.ax = 0x00AA, .cx = 2, .di = IT_DATA_OFF + 3, .flags = 0x0400}), ((it_regs){.cx = 0, .di = IT_DATA_OFF + 1}), IT_C_CX | IT_C_DI, 1, m_ff_tail, 0, IT_SEG, IT_DATA_OFF, 4),

    /* --- control flow --- */
    CASE("jz taken", c_jz, ((it_regs){.flags = 0x0246}), ((it_regs){.ip = IT_CODE_OFF + 4}), IT_C_IP, 1, 0, 0, 0, 0, 0),
    CASE("jz not taken", c_jz, ((it_regs){.flags = 0x0202}), ((it_regs){.ip = IT_CODE_OFF + 2}), IT_C_IP, 1, 0, 0, 0, 0, 0),
    CASE("jnz taken", c_jnz, ((it_regs){.flags = 0x0202}), ((it_regs){.ip = IT_CODE_OFF + 4}), IT_C_IP, 1, 0, 0, 0, 0, 0),
    CASE("jmp short", c_jmp, ((it_regs){.flags = 0x0202}), ((it_regs){.ip = IT_CODE_OFF + 4}), IT_C_IP, 1, 0, 0, 0, 0, 0),
    CASE("jmp far (EA)", c_jmp_far, ((it_regs){}), ((it_regs){.cs = IT_SEG, .ip = 0x0200}), IT_C_IP, 1, 0, 0, 0, 0, 0),

    /* --- flags: cmp/test set the arithmetic flags exactly --- */
    CASE("cmp ax,bx equal", c_cmp, ((it_regs){.ax = 0x1234, .bx = 0x1234, .flags = 0x0002}), ((it_regs){.flags = 0x0246}), IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("cmp ax,bx below", c_cmp, ((it_regs){.ax = 0x0001, .bx = 0x0002, .flags = 0x0002}), ((it_regs){.flags = 0x0297}), IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("test ax,bx nonzero", c_test, ((it_regs){.ax = 1, .bx = 1, .flags = 0x0002}), ((it_regs){.flags = 0x0202}), IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("and ax,bx; or ax,bx", c_and_or, ((it_regs){.ax = 0x00FF, .bx = 0x0F0F}), ((it_regs){.ax = 0x0F0F}), IT_C_AX, 2, 0, 0, 0, 0, 0),
    CASE("xor ax,bx", c_xor, ((it_regs){.ax = 0xFFFF, .bx = 0x0F0F}), ((it_regs){.ax = 0xF0F0}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    /* LAHF/SAHF exchange FLAGS and AH; getting them the wrong way round
     * hides every flag test that a program does right after a LAHF
     * (Turbo Link 3.01's command-line scanner does exactly that). */
    CASE("sahf loads the low flags from ah", c_sahf, ((it_regs){.ax = 0xD700, .flags = 0x0002}), ((it_regs){.ax = 0xD700, .flags = 0x00D7}), IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("lahf copies the low flags into ah", c_lahf, ((it_regs){.ax = 0x1234, .flags = 0x00D7}), ((it_regs){.ax = 0xD734, .flags = 0x00D7}), IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),

    /* --- unary --- */
    CASE("neg bx", c_neg, ((it_regs){.bx = 1}), ((it_regs){.bx = 0xFFFF}), IT_C_BX, 1, 0, 0, 0, 0, 0),
    CASE("not bx", c_not, ((it_regs){.bx = 0x0F0F}), ((it_regs){.bx = 0xF0F0}), IT_C_BX, 1, 0, 0, 0, 0, 0),
    CASE("cbw (al=FFh)", c_cbw, ((it_regs){.ax = 0x00FF}), ((it_regs){.ax = 0xFFFF}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("cwd (ax=8000h)", c_cwd, ((it_regs){.ax = 0x8000}), ((it_regs){.ax = 0x8000, .dx = 0xFFFF}), IT_C_AX | IT_C_DX, 1, 0, 0, 0, 0, 0),
    CASE("aam (al=45)", c_aam, ((it_regs){.ax = 45}), ((it_regs){.ax = 0x0405}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("salc with CF=1 (stc;salc)", c_salc_cf, ((it_regs){.ax = 0x1234}), ((it_regs){.ax = 0x12FF}), IT_C_AX, 2, 0, 0, 0, 0, 0),
    CASE("salc with CF=0 (clc;salc)", c_salc_nc, ((it_regs){.ax = 0x1234}), ((it_regs){.ax = 0x1200}), IT_C_AX, 2, 0, 0, 0, 0, 0),

    /* --- 386 additions to the 16-bit decoder (MINIGAME died on 0x6F) --- */
    CASE("imul ax,bx,5", c_imul3, ((it_regs){.bx = 7}), ((it_regs){.ax = 35}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("bound ax,[bx]", c_bound, ((it_regs){.ax = 5, .bx = IT_DATA_OFF}), ((it_regs){.ax = 5}), IT_C_AX, 1, m_bound_lim, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("arpl ax,bx (rpl 3)", c_arpl, ((it_regs){.ax = 0, .bx = 3}), ((it_regs){.ax = 3}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("insb from port 61h", c_insb, ((it_regs){.di = IT_DATA_OFF, .cx = 1}), ((it_regs){.di = IT_DATA_OFF + 1}), IT_C_DI, 1, 0, 0, 0, 0, 0),
    CASE("outsb to port 61h", c_outsb, ((it_regs){.si = IT_DATA_OFF, .cx = 1}), ((it_regs){.si = IT_DATA_OFF + 1}), IT_C_SI, 1, m_words, 0, IT_SEG, IT_DATA_OFF, 1),

    /* --- 386 POPF keeps IOPL/NT (bits 12-14).  DOS/16M, the loader inside
     * DOOM 0.99, writes FLAGS=0F000h and reads it back to tell a 386 from a
     * 286; bit 15 is reserved and reads back 0 on a 386. --- */
    CASE("popf keeps IOPL/NT (386)", c_popf_iopl, ((it_regs){.flags = 0x0002}), ((it_regs){.ax = 0x7002}), IT_C_AX, 5, 0, 0, 0, 0, 0),
    /* --- 0F BA: BT/BTS/BTR/BTC with an immediate bit index (386). --- */
    CASE("bt ax,12 sets CF", c_bt_imm, ((it_regs){.ax = 0x1000, .flags = 0x0002}), ((it_regs){.ax = 0x1000, .flags = 0x0001}), IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("bts ax,3", c_bts_imm, ((it_regs){.ax = 0x0000, .flags = 0x0002}), ((it_regs){.ax = 0x0008, .flags = 0x0002}), IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("btr ax,1", c_btr_imm, ((it_regs){.ax = 0x0002, .flags = 0x0002}), ((it_regs){.ax = 0x0000, .flags = 0x0001}), IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("btc ax,0", c_btc_imm, ((it_regs){.ax = 0x0001, .flags = 0x0002}), ((it_regs){.ax = 0x0000, .flags = 0x0001}), IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),

    /* ================= 186 sweep ===================================== */
    /* PUSH SP is how every DOS-era program asks "is this an 8086 or a 186?",
     * and the answer has to be "186 and later" (the new, decremented SP). */
    CASE("push sp pushes the decremented value (186)", c_push_sp,
         ((it_regs){.sp = 0xFF00}), ((it_regs){.ax = 0xFEFE, .sp = 0xFF00}),
         IT_C_AX | IT_C_SP, 2, 0, 0, 0, 0, 0),
    CASE("pusha stores 8 words in Intel order", c_pusha,
         ((it_regs){.ax = 0x1111, .bx = 0x2222, .cx = 0x3333, .dx = 0x4444,
                    .si = 0x5555, .di = 0x6666, .bp = 0x7777, .sp = 0xFF00}),
         ((it_regs){.sp = 0xFEF0}), IT_C_SP | IT_C_MEM, 1,
         m_zeros16, m_pusha_stack, IT_SEG, 0xFEF0, 16),
    CASE("enter 8,0 leaves bp at the frame", c_enter0,
         ((it_regs){.bp = 0x0777, .sp = 0xFF00}), ((it_regs){.bp = 0xFEFE, .sp = 0xFEF6}),
         IT_C_BP | IT_C_SP, 1, 0, 0, 0, 0, 0),
    /* level 2: the display copy must be frame-based, so BP ends up at the
     * frame pointer (0FFFEh) and not at the top of the pushed display. */
    CASE("enter 4,2 builds the display chain", c_enter2,
         ((it_regs){.bp = 0x0777, .sp = 0xFF00}), ((it_regs){.bp = 0xFEFE, .sp = 0xFEF6}),
         IT_C_BP | IT_C_SP, 1, 0, 0, 0, 0, 0),
    CASE("shl ax,cl with cl=21h shifts by 1", c_shl_cl,
         ((it_regs){.ax = 0x0001, .cx = 0x0021}), ((it_regs){.ax = 0x0002}),
         IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("shl ax,cl with cl=0 keeps every flag", c_shl_cl,
         ((it_regs){.ax = 0x1234, .cx = 0x0000, .flags = 0x0246}),
         ((it_regs){.ax = 0x1234, .flags = 0x0246}),
         IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("shl ax,4 (C1 imm8)", c_shl_imm4,
         ((it_regs){.ax = 0x0001}), ((it_regs){.ax = 0x0010}),
         IT_C_AX, 1, 0, 0, 0, 0, 0),

    /* ================= 286 sweep ===================================== */
    /* LMSW loads CR0's low nibble and SMSW must *read* it back.  The old
     * decoder had the two swapped, so a 286 program that saved CR0 and
     * restored it really loaded the saved value as CR0 twice. */
    CASE("lmsW sets CR0 bits 0-3, smsw reads them", c_lmsw_smsw,
         ((it_regs){}), ((it_regs){.ax = 0x000C}), IT_C_AX, 5, 0, 0, 0, 0, 0),
    /* VERR/VERW on a null selector must report "no" (ZF=0) and must not touch
     * the operand.  The old decoder treated /4 and /5 as STR and wrote the
     * task register over the operand register. */
    CASE("verr null selector: ZF=0, ax untouched", c_verr,
         ((it_regs){.flags = 0x0246}), ((it_regs){.ax = 0x0000, .flags = 0x0004}),
         IT_C_AX | IT_C_FLAGS, 4, 0, 0, 0, 0, 0),
    CASE("verw null selector: ZF=0, ax untouched", c_verw,
         ((it_regs){.flags = 0x0246}), ((it_regs){.ax = 0x0000, .flags = 0x0004}),
         IT_C_AX | IT_C_FLAGS, 4, 0, 0, 0, 0, 0),
    CASE("lar null selector: ZF=0, dx untouched", c_lar_null,
         ((it_regs){.dx = 0x1234, .flags = 0x0246}),
         ((it_regs){.dx = 0x1234, .flags = 0x0004}),
         IT_C_DX | IT_C_FLAGS, 4, 0, 0, 0, 0, 0),
    CASE("lsl null selector: ZF=0, dx untouched", c_lsl_null,
         ((it_regs){.dx = 0x1234, .flags = 0x0246}),
         ((it_regs){.dx = 0x1234, .flags = 0x0004}),
         IT_C_DX | IT_C_FLAGS, 2, 0, 0, 0, 0, 0),

    /* ================= 386 / 486 sweep =============================== */
    CASE("bsf ax,bx finds bit 4", c_bsf_hit,
         ((it_regs){.bx = 0x0010, .flags = 0x0246}), ((it_regs){.ax = 0x0004, .flags = 0x0004}),
         IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("bsf ax,bx with bx=0 leaves ax and sets ZF", c_bsf_zero,
         ((it_regs){.ax = 0x1234, .bx = 0x0000, .flags = 0x0202}),
         ((it_regs){.ax = 0x1234, .flags = 0x0040}),
         IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("bsr ax,bx finds bit 15", c_bsr_hit,
         ((it_regs){.bx = 0x8000, .flags = 0x0246}), ((it_regs){.ax = 0x000F, .flags = 0x0004}),
         IT_C_AX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("cmpxchg bx,dx equal stores dx", c_cmpxchg_eq,
         ((it_regs){.ax = 0x0005, .bx = 0x0005, .dx = 0x0007, .flags = 0x0202}),
         ((it_regs){.bx = 0x0007, .flags = 0x0040}), IT_C_BX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("cmpxchg bx,dx differ loads ax", c_cmpxchg_ne,
         ((it_regs){.ax = 0x0005, .bx = 0x0003, .dx = 0x0007, .flags = 0x0246}),
         ((it_regs){.ax = 0x0003, .bx = 0x0003, .flags = 0x0004}),
         IT_C_AX | IT_C_BX | IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
    CASE("xadd bx,dx", c_xadd,
         ((it_regs){.bx = 0x0002, .dx = 0x0003}), ((it_regs){.bx = 0x0005, .dx = 0x0002}),
         IT_C_BX | IT_C_DX, 1, 0, 0, 0, 0, 0),
    CASE("bswap ecx (1234 5678h -> 7856 3412h)", c_bswap,
         ((it_regs){}), ((it_regs){.ax = 0x3412, .cx = 0x3412}), IT_C_AX | IT_C_CX, 3, 0, 0, 0, 0, 0),
    CASE("lgs ax,[bx]", c_lgs,
         ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.ax = 0x5678}), IT_C_AX, 1,
         m_words, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("insw from port 61h", c_insw,
         ((it_regs){.di = IT_DATA_OFF}), ((it_regs){.di = IT_DATA_OFF + 2}), IT_C_DI, 1,
         0, 0, 0, 0, 0),
    CASE("outsw to port 61h", c_outsw,
         ((it_regs){.si = IT_DATA_OFF}), ((it_regs){.si = IT_DATA_OFF + 2}), IT_C_SI, 1,
         m_words, 0, IT_SEG, IT_DATA_OFF, 2),
    /* --- x87 floating point (D8h..DFh) --- */
    CASE("fpu fld1; fadd st0,st0; fstp qword", c_fpu_fadd,
         ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.bx = IT_DATA_OFF}), IT_C_MEM,
         4, m_zeros16, m_fpu_two, IT_SEG, IT_DATA_OFF, 8),
    CASE("fpu fsqrt 2 -> 1.41421356 (double)", c_fpu_sqrt,
         ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.bx = IT_DATA_OFF}), IT_C_MEM,
         5, m_zeros16, m_fpu_sqrt2, IT_SEG, IT_DATA_OFF, 8),
    CASE("fpu fchs -1.0 (double)", c_fpu_fchs,
         ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.bx = IT_DATA_OFF}), IT_C_MEM,
         4, m_zeros16, m_fpu_neg1, IT_SEG, IT_DATA_OFF, 8),
    CASE("fpu fild word, fadd st0,st0, fistp word", c_fpu_fild,
         ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.bx = IT_DATA_OFF}), IT_C_MEM,
         4, m_fpu_three, m_fpu_six, IT_SEG, IT_DATA_OFF, 2),
    CASE("fpu fscale (1.0 * 2^3 = 8)", c_fpu_fscale,
         ((it_regs){.bx = IT_DATA_OFF}), ((it_regs){.bx = IT_DATA_OFF}), IT_C_MEM,
         5, m_fpu_three, m_fpu_eight, IT_SEG, IT_DATA_OFF, 2),
    CASE("fpu fcom st1 + fnstsw ax (0 < 1 -> C0, top=6)", c_fpu_fcom,
         ((it_regs){}), ((it_regs){.ax = 0x3100}), IT_C_AX,
         5, 0, 0, 0, 0, 0),};

#define IT_NCASES ((int)(sizeof(g_cases) / sizeof(g_cases[0])))

#define FLAG_MASK 0x08D5u   /* CF PF AF ZF SF OF */

static int reg_get(const it_regs *r, int bit)
{
    switch (bit) {
    case IT_C_AX: return r->ax;
    case IT_C_BX: return r->bx;
    case IT_C_CX: return r->cx;
    case IT_C_DX: return r->dx;
    case IT_C_SI: return r->si;
    case IT_C_DI: return r->di;
    case IT_C_BP: return r->bp;
    case IT_C_SP: return r->sp;
    case IT_C_DS: return r->ds;
    case IT_C_ES: return r->es;
    case IT_C_SS: return r->ss;
    case IT_C_CS: return r->cs;
    case IT_C_IP: return r->ip;
    case IT_C_FLAGS: return (int)r->flags;
    }
    return -1;
}

static const char *reg_name(int bit)
{
    switch (bit) {
    case IT_C_AX: return "AX";
    case IT_C_BX: return "BX";
    case IT_C_CX: return "CX";
    case IT_C_DX: return "DX";
    case IT_C_SI: return "SI";
    case IT_C_DI: return "DI";
    case IT_C_BP: return "BP";
    case IT_C_SP: return "SP";
    case IT_C_DS: return "DS";
    case IT_C_ES: return "ES";
    case IT_C_SS: return "SS";
    case IT_C_CS: return "CS";
    case IT_C_IP: return "IP";
    case IT_C_FLAGS: return "FLAGS";
    }
    return "?";
}

static const u32 g_reg_bits[] = { IT_C_AX, IT_C_BX, IT_C_CX, IT_C_DX, IT_C_SI, IT_C_DI,
                                  IT_C_BP, IT_C_SP, IT_C_DS, IT_C_ES, IT_C_SS, IT_C_CS,
                                  IT_C_IP, IT_C_FLAGS };

static void state_save(const Emu *e, it_regs *r)
{
    r->ax = (u16)e->ax; r->bx = (u16)e->bx; r->cx = (u16)e->cx; r->dx = (u16)e->dx;
    r->si = (u16)e->si; r->di = (u16)e->di; r->bp = (u16)e->bp; r->sp = (u16)e->sp;
    r->ds = e->ds; r->es = e->es; r->ss = e->ss; r->cs = e->cs;
    r->ip = (u16)e->eip; r->flags = (u16)e->eflags;
}

/* One case on an already-initialised Emu.  Returns 1 = pass. */
static int case_run(const it_case *c, Emu *e, char *msg, u32 msg_len)
{
    it_regs st = c->in;
    st.cs = IT_SEG;
    st.ip = IT_CODE_OFF;
    st.ss = IT_SEG;
    if (!st.sp) st.sp = 0xFF00;
    if (!st.ds) st.ds = IT_SEG;
    if (!st.es) st.es = IT_SEG;
    st.flags = (u16)((st.flags & 0x0FD5) | 0x0202);

    e->ax = st.ax; e->bx = st.bx; e->cx = st.cx; e->dx = st.dx;
    e->si = st.si; e->di = st.di; e->bp = st.bp; e->sp = st.sp;
    e->eflags = st.flags;
    e->eip = IT_CODE_OFF;
    emu_set_seg(e, 0, st.es);
    emu_set_seg(e, 1, st.cs);
    emu_set_seg(e, 2, st.ss);
    emu_set_seg(e, 3, st.ds);

    for (u32 i = 0; i < c->code_len && i < 64; i++)
        emu_wb(e, IT_SEG, (u16)(IT_CODE_OFF + i), c->code[i]);
    if (c->mem && c->mem_len) {
        for (u32 i = 0; i < c->mem_len; i++)
            emu_wb(e, c->mem_seg, (u16)(c->mem_off + i), c->mem[i]);
    }

    int steps = c->steps > 0 ? c->steps : 1;
    for (int i = 0; i < steps; i++) {
        if (step_public(e) != 0) {
            snprintf(msg, msg_len, "unsupported instruction (step %d)", i);
            return 0;
        }
    }

    it_regs got;
    state_save(e, &got);
    for (unsigned i = 0; i < sizeof(g_reg_bits) / sizeof(g_reg_bits[0]); i++) {
        int bit = (int)g_reg_bits[i];
        if (!(c->check_mask & (u32)bit)) continue;
        int want = reg_get(&c->out, bit);
        int have = reg_get(&got, bit);
        if (bit == IT_C_FLAGS) { want &= (int)FLAG_MASK; have &= (int)FLAG_MASK; }
        if (want != have) {
            snprintf(msg, msg_len, "%s: expected %04X, got %04X", reg_name(bit),
                     (unsigned)want, (unsigned)have);
            return 0;
        }
    }
    if ((c->check_mask & IT_C_MEM) && c->mexp) {
        for (u32 i = 0; i < c->mem_len; i++) {
            u8 want = c->mexp[i];
            u8 have = emu_rb(e, c->mem_seg, (u16)(c->mem_off + i));
            if (want != have) {
                snprintf(msg, msg_len, "mem[%u]: expected %02X, got %02X",
                         (unsigned)i, (unsigned)want, (unsigned)have);
                return 0;
            }
        }
    }
    return 1;
}

/* Returns 0 when every selected case passed. */
int itest_run(const char *filter)
{
    Emu *e = (Emu *)plat_alloc(sizeof(Emu));
    if (!e) { plat_puts("itest: out of memory\n"); return 1; }
    if (emu_init(e) != 0) { plat_free(e); plat_puts("itest: emu_init failed\n"); return 1; }

    int pass = 0, fail = 0;
    char msg[128];
    char num[16];
    for (int i = 0; i < IT_NCASES; i++) {
        const it_case *c = &g_cases[i];
        if (filter && *filter && !strstr(c->name, filter)) continue;
        msg[0] = 0;
        if (case_run(c, e, msg, sizeof(msg))) {
            pass++;
            plat_puts("  [PASS] ");
            plat_puts(c->name);
            plat_puts("\n");
        } else {
            fail++;
            plat_puts("  [FAIL] ");
            plat_puts(c->name);
            plat_puts(" -- ");
            plat_puts(msg);
            plat_puts("\n");
        }
    }
    emu_free(e);
    plat_free(e);

    snprintf(num, sizeof(num), "%d", pass);
    plat_puts("itest: ");
    plat_puts(num);
    plat_puts(" passed, ");
    snprintf(num, sizeof(num), "%d", fail);
    plat_puts(num);
    plat_puts(" failed\n");
    plat_puts(fail == 0 ? "ITEST PASSED\n" : "ITEST FAILED\n");
    return fail == 0 ? 0 : 1;
}

/* ====================================================================== */
/* Brute-force opcode audit ("ifuzz")                                     */
/*                                                                        */
/* The 8086 decoder is a long switch, and a missing opcode only shows up   */
/* as "program X died" months later.  This feeds every first-byte opcode   */
/* (and every 0F-prefixed second byte) into a fresh Emu with a handful of  */
/* addressing-mode fillers, steps it a few instructions and records which  */
/* ones the decoder rejects.  It is an audit, not an assertion: the report  */
/* lists the holes so they can be triaged.                                 */
/* ====================================================================== */
#define IFUZZ_SEG      0x9100
#define IFUZZ_CODE_OFF 0x0100
#define IFUZZ_STEP     6          /* instructions to run per stream */

static const u8 g_fillers[] = { 0x00, 0x01, 0x04, 0x05, 0x06, 0x44, 0x84, 0xC0, 0xE0, 0xF8 };
#define IFUZZ_NFILL ((int)(sizeof(g_fillers) / sizeof(g_fillers[0])))

/* Run one stream; returns the number of unsupported opcodes it hit. */
static int fuzz_stream(Emu *e, const u8 *code, int len)
{
    int before = emu_unsupported_ops;
    e->ax = 0x1234; e->bx = 0x0100; e->cx = 0x0004; e->dx = 0x0300;
    e->si = 0x0200; e->di = 0x0240; e->bp = 0x0300; e->sp = 0xF000;
    e->eflags = 0x0202;
    e->eip = IFUZZ_CODE_OFF;
    emu_set_seg(e, 0, IFUZZ_SEG);
    emu_set_seg(e, 1, IFUZZ_SEG);
    emu_set_seg(e, 2, IFUZZ_SEG);
    emu_set_seg(e, 3, IFUZZ_SEG);
    for (int i = 0; i < len && i < 32; i++)
        emu_wb(e, IFUZZ_SEG, (u16)(IFUZZ_CODE_OFF + i), code[i]);
    for (int s = 0; s < IFUZZ_STEP; s++) {
        if (step_public(e) != 0) break;
        if (e->halted) break;
    }
    return emu_unsupported_ops - before;
}

static void itest_puthex(u32 v)
{
    char hex[3];
    hex[0] = "0123456789ABCDEF"[(v >> 4) & 15];
    hex[1] = "0123456789ABCDEF"[v & 15];
    hex[2] = 0;
    plat_puts(hex);
}

/* Records one rejected opcode per (prefix, opcode) pair. */
static u8 g_bad_first[256];         /* 8086 first-byte opcode rejected */
static u8 g_bad_386[256];           /* 0F xx rejected */

int ifuzz_run(int verbose)
{
    Emu *e = (Emu *)plat_alloc(sizeof(Emu));
    if (!e) { plat_puts("ifuzz: out of memory\n"); return 1; }
    if (emu_init(e) != 0) { plat_free(e); plat_puts("ifuzz: emu_init failed\n"); return 1; }

    int saved_quiet = emu_unsupported_quiet;
    emu_unsupported_quiet = 1;      /* report them summarised, not per hit */

    u32 streams = 0, holes = 0;
    u8 code[8];

    /* --- 8086 first-byte opcodes, with prefixes and addressing fillers --- */
    for (int pre = 0; pre < 4; pre++) {
        static const u8 prefixes[4] = { 0x00, 0x26, 0x66, 0x67 };  /* none, seg, opsz, adsz */
        for (int op = 0; op < 256; op++) {
            for (int f = 0; f < IFUZZ_NFILL; f++) {
                int n = 0;
                if (prefixes[pre]) code[n++] = prefixes[pre];
                code[n++] = (u8)op;
                code[n++] = g_fillers[f];
                code[n++] = 0x90;   /* nop padding: immediates land on it */
                code[n++] = 0x90;
                code[n++] = 0x90;
                code[n++] = 0x90;
                streams++;
                if (fuzz_stream(e, code, n)) {
                    holes++;
                    if (!g_bad_first[op]) {
                        g_bad_first[op] = 1;
                        if (verbose) {
                            char line[64];
                            snprintf(line, sizeof(line), "  rejected 8086 op %02X (prefix %02X)\n",
                                     (unsigned)op, (unsigned)prefixes[pre]);
                            plat_puts(line);
                        }
                    }
                }
            }
        }
    }

    /* --- 0F-prefixed (386 / DPMI extension) second bytes --- */
    for (int op2 = 0; op2 < 256; op2++) {
        for (int f = 0; f < IFUZZ_NFILL; f++) {
            code[0] = 0x0F;
            code[1] = (u8)op2;
            code[2] = g_fillers[f];
            code[3] = 0x90;
            code[4] = 0x90;
            code[5] = 0x90;
            code[6] = 0x90;
            streams++;
            if (fuzz_stream(e, code, 7)) {
                holes++;
                if (!g_bad_386[op2]) {
                    g_bad_386[op2] = 1;
                    if (verbose) {
                        char line[32];
                        snprintf(line, sizeof(line), "  rejected 0F %02X\n", (unsigned)op2);
                        plat_puts(line);
                    }
                }
            }
        }
    }

    emu_unsupported_quiet = saved_quiet;
    emu_free(e);
    plat_free(e);

    /* --- summary --- */
    char num[16];
    int n8086 = 0, n386 = 0;
    plat_puts("ifuzz: ");
    snprintf(num, sizeof(num), "%u", (unsigned)streams);
    plat_puts(num);
    plat_puts(" streams, ");
    snprintf(num, sizeof(num), "%u", (unsigned)holes);
    plat_puts(num);
    plat_puts(" rejects\n");
    plat_puts("ifuzz: 8086 opcodes missing:");
    for (int i = 0; i < 256; i++) if (g_bad_first[i]) { n8086++; if (n8086 <= 32) { plat_putc(' '); itest_puthex((u32)i); } }
    if (!n8086) plat_puts(" none");
    else { plat_puts(" ("); snprintf(num, sizeof(num), "%d", n8086); plat_puts(num); plat_puts(" total)"); }
    plat_putc('\n');
    plat_puts("ifuzz: 0F opcodes missing:");
    for (int i = 0; i < 256; i++) if (g_bad_386[i]) { n386++; if (n386 <= 32) { plat_putc(' '); itest_puthex((u32)i); } }
    if (!n386) plat_puts(" none");
    else { plat_puts(" ("); snprintf(num, sizeof(num), "%d", n386); plat_puts(num); plat_puts(" total)"); }
    plat_putc('\n');
    plat_puts("IFUZZ DONE\n");
    return 0;
}
