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
static const u8 c_not[]       = { 0xF7, 0xD3 };              /* not bx */
static const u8 c_cbw[]       = { 0x98 };                    /* cbw */
static const u8 c_cwd[]       = { 0x99 };                    /* cwd */
static const u8 c_aam[]       = { 0xD4, 0x0A };              /* aam */
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

    /* --- unary --- */
    CASE("neg bx", c_neg, ((it_regs){.bx = 1}), ((it_regs){.bx = 0xFFFF}), IT_C_BX, 1, 0, 0, 0, 0, 0),
    CASE("not bx", c_not, ((it_regs){.bx = 0x0F0F}), ((it_regs){.bx = 0xF0F0}), IT_C_BX, 1, 0, 0, 0, 0, 0),
    CASE("cbw (al=FFh)", c_cbw, ((it_regs){.ax = 0x00FF}), ((it_regs){.ax = 0xFFFF}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("cwd (ax=8000h)", c_cwd, ((it_regs){.ax = 0x8000}), ((it_regs){.ax = 0x8000, .dx = 0xFFFF}), IT_C_AX | IT_C_DX, 1, 0, 0, 0, 0, 0),
    CASE("aam (al=45)", c_aam, ((it_regs){.ax = 45}), ((it_regs){.ax = 0x0405}), IT_C_AX, 1, 0, 0, 0, 0, 0),

    /* --- 386 additions to the 16-bit decoder (MINIGAME died on 0x6F) --- */
    CASE("imul ax,bx,5", c_imul3, ((it_regs){.bx = 7}), ((it_regs){.ax = 35}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("bound ax,[bx]", c_bound, ((it_regs){.ax = 5, .bx = IT_DATA_OFF}), ((it_regs){.ax = 5}), IT_C_AX, 1, m_bound_lim, 0, IT_SEG, IT_DATA_OFF, 4),
    CASE("arpl ax,bx (rpl 3)", c_arpl, ((it_regs){.ax = 0, .bx = 3}), ((it_regs){.ax = 3}), IT_C_AX, 1, 0, 0, 0, 0, 0),
    CASE("insb from port 61h", c_insb, ((it_regs){.di = IT_DATA_OFF, .cx = 1}), ((it_regs){.di = IT_DATA_OFF + 1}), IT_C_DI, 1, 0, 0, 0, 0, 0),
    CASE("outsb to port 61h", c_outsb, ((it_regs){.si = IT_DATA_OFF, .cx = 1}), ((it_regs){.si = IT_DATA_OFF + 1}), IT_C_SI, 1, m_words, 0, IT_SEG, IT_DATA_OFF, 1),
};

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
