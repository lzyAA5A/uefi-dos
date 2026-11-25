# Instruction-level test framework

`src/emu/emu.c` is one very large `switch`. Adding an opcode there used to be
"build the image, boot QEMU, run a game, look at the screen" — slow, and it only
catches the opcodes that game happens to execute. `src/emu/itest.c` does for the
interpreter what QEMU's own `tests/` does: **every opcode is a byte stream plus a
register/memory expectation, executed through the real decoder.**

```sh
build\hosttest.exe --itest           # all cases
build\hosttest.exe --itest shift     # only cases whose name contains "shift"
```

```
  [PASS] add ax,bx (reg dst)
  [PASS] sub ax,bx (reg dst)
  [PASS] shl ax,1
  ...
itest: 49 passed, 0 failed
ITEST PASSED
```

`run_tests.ps1` runs it as check #16 of the suite, and `make test` runs it too.

## How a case works

A case is *data*, not code:

```c
CASE("shl ax,1", c_shl_ax1,
     ((it_regs){.ax = 0x4000}),          /* registers before           */
     ((it_regs){.ax = 0x8000}),          /* registers expected after   */
     IT_C_AX, 1,                          /* what to verify, #instructions */
     0, 0, 0, 0, 0),                      /* mem, mexp, seg, off, len   */
```

1. a fresh 16 MB `Emu` is initialised once for the whole run,
2. the case's registers/flags are loaded (`CS=SS=DS=ES=0x9000`, `SP=0xFF00`
   unless the case says otherwise, `FLAGS` forced to `0x0202 | case bits`),
3. the code bytes go to `9000:0100`, optional memory to `seg:off`,
4. `step_public()` runs exactly `steps` instructions (each one is a real decode
   + execute, so an unsupported opcode fails the case instead of hanging),
5. every register selected by `check_mask` is compared, `FLAGS` masked to
   `CF|PF|AF|ZF|SF|OF` (`0x08D5`), and `IT_C_MEM` compares memory bytes.

Two subtleties worth knowing if you add cases:

* the compound literals **must** be wrapped in parentheses
  (`((it_regs){...})`) — the commas inside a bare `{...}` would split the macro
  arguments;
* `lodsb` leaves `AH` alone, `div` writes the *quotient* to `AX`, and the
  scratch segment is `0x9000` — write expectations accordingly, not from
  intuition (the first run of this framework found four wrong expectations and
  zero wrong emulator behaviours, which is exactly the point).

## Adding a case

```c
static const u8 c_bt[] = { 0x0F, 0xA3, 0xD8 };   /* bt ax,bx */

CASE("bt ax,bx", c_bt, ((it_regs){.ax = 0x0004, .bx = 2}),
     ((it_regs){.flags = 0x0202}), IT_C_FLAGS, 1, 0, 0, 0, 0, 0),
```

Then `build\hosttest.exe --itest bt`. If the case fails you are looking at either
a decoder bug or your own expectation — the failure message names the register
and both values (`AX: expected 8000, got C000`).

## What it already caught / documents

| case group | why it exists |
| --- | --- |
| `add/sub ax,bx (reg dst)` | the direction bit was inverted: `sub bx,ax` left BX alone and put the difference in AX, corrupting every pointer calculation |
| `shl/shr/sar/rol/ror/rcl` | the group-2 selector order was grouped wrongly, so `D1 E0` (shl ax,1) executed as `ror ax,1`: every game glyph landed one column off |
| `rep stosw` / `rep movsw` | `rep` prefix handling plus the direction flag |
| `jmp far (EA)` | far jump updates CS *and* IP |
| `cmp`/`test` flag patterns | exact flag outcomes (`0x0246` for equal, `0x0297` for below) |
| `bound`, `arpl`, `imul r,r/m,imm` | 386 encodings that 16-bit programs (MINIGAME) do emit |
| `insb`/`outsb` | port I/O dispatch through `emu_in_port`/`emu_out_port` |

## Coverage and gaps

Covered: the ALU/MOV families with both operand directions, the shift/rotate
group, `mul/imul/div/idiv` (8- and 16-bit forms), `inc/dec` flag preservation,
stack, `xchg`, `lea`, direct/`[bx]`/segment-override memory forms, string ops
with `rep`, short/near/far control flow, conditional jumps, flag arithmetic,
`cbw/cwd/aam`, `xlat`, `les/lds`, `bound`, `arpl`, `imul` in all three forms,
`insb/outsb`.

Not covered (each needs a different harness): 32-bit operand/address encodings
(covered indirectly by `DPMITEST.COM`), interrupt reflection and the DPMI host
(`DPMITEST.COM`), FPU (`D8-DF` are decoded and ignored), and I/O devices beyond
the keyboard buffer and port 0x61.

The framework is deliberately tiny: ~430 lines, no dependencies beyond
`plat.h` + `emu.h`, and the case table is plain data so adding one is a 5-line
edit. When you touch `step_8086()`, add a case first.

## Brute-force opcode audit (`--ifuzz`)

The vectors above are curated: they encode the bugs that were actually found.
`--ifuzz` is the systematic counterpart. It feeds **every** first-byte opcode
(plus the `26`/`66`/`67` prefixes) and **every** `0F xx` second byte, each with
ten addressing-mode fillers, into a fresh emulator and steps it six
instructions. Any opcode whose decoder rejects it is reported.

```
build\hosttest.exe --ifuzz        # summary
build\hosttest.exe --ifuzz -v     # print each rejected opcode

ifuzz: 12800 streams, 1786 rejects
ifuzz: 8086 opcodes missing: 0F D6 F1 (3 total)
ifuzz: 0F opcodes missing: 04 0A 0C 0D ... (169 total)
IFUZZ DONE
```

The 8086 result is the useful gate: after the audit the only holes are `0F`
(the 386 escape prefix), `D6` (SALC) and `F1` (ICEBP) -- both undocumented on
the 8086 and unused by DOS software. `run_tests.ps1` asserts exactly that line,
so a decoder regression that loses an 8086 opcode fails the suite instead of
surfacing as "game X renders garbage".

The `0F` list is deliberately *not* asserted: it is dominated by MMX/SSE and
486-only system instructions that a DOS-era program cannot use, and it shrinks
as those get implemented. Use `-v` to see which ones a new program needs.

## 1.7.0 -- the 186 / 286 sweep

The interpreter was already carrying most of the 186 and 286 additions, but
several of them *decoded and then did the wrong thing*, which is worse than a
missing opcode: a missing opcode stops the program with a message, a wrong one
produces a plausible-looking bug report weeks later.  1.7.0 fixed these and
added the rest, with a regression case for every one of them (`--itest` went
from 68 to 90 checks, all passing):

| Instruction | What was wrong | What it does now |
| --- | --- | --- |
| `PUSH SP` (54h) | pushed the value SP had **before** the decrement, i.e. it answered "I am an 8086" to the canonical `push sp / pop ax / cmp ax,sp` probe | pushes the decremented SP (186 and later) |
| `SHL/SHR/SAR/ROL/ROR/RCL/RCR` with an imm8 or CL count | count was not taken modulo 32, and SZP flags were rewritten even when the count was zero | count `& 31`, count 0 leaves every flag alone |
| `ENTER` (C8h) | `BP` was loaded with the pushed caller's BP instead of the frame pointer, and the display copy pushed the wrong value: a nested `ENTER` (static links, Pascal/Modula-2 runtimes) built a broken frame chain | Intel order: `push BP`, `frame = SP`, `(level-1) x { BP -= opsize; push [SS:BP] }`, `push frame`, `BP = frame`, `SP -= alloc` |
| `BOUND` (62h) | decoded and ignored -- no trap at all | compares the signed index with the pair in memory and raises **INT 5** when it is outside |
| `SMSW` (0F 01 /4) | **wrote** the operand into CR0 (that is `LMSW`) | reads CR0's low 16 bits into the operand (a register destination zero-extends) |
| `LMSW` (0F 01 /6) | was a no-op (`/5` was decoded as it) | loads CR0 bits 0-3 and re-syncs the segment cache; it cannot clear PE, as on real silicon |
| `VERR` / `VERW` (0F 00 /4, /5) | fell into the `STR` arm, so they wrote the task register over the operand | checks the descriptor's type and DPL against CPL/RPL and sets ZF |
| `LAR` / `LSL` (0F 02, 0F 03) | always "ZF=0", dest zeroed | returns the access-rights word / the effective (granularity-adjusted) limit with ZF=1 |
| `LSS` / `LFS` / `LGS` (0F B2/B4/B5) | unsupported opcode: the machine halted | load the far pointer into the segment register and the GPR, like `LES`/`LDS` |
| `BSF` / `BSR` (0F BC/BD) | unsupported opcode: the machine halted | bit scan; ZF=1 (and the destination untouched) when the source is zero |
| `CMPXCHG` (0F B0/B1), `XADD` (0F C0/C1), `CMPXCHG8B` (0F C7 /1), `BSWAP` (0F C8-CF) | unsupported: halted | implemented with the flags the SDM specifies |
| `MOV r32, DRn` (0F 21), `MOV r32, TRn` (0F 25), `MOV TRn, r32` (0F 24/26) | unsupported: halted | accepted, debug/task registers read as zero |
| `0F 18-1F` (multi-byte NOP hints), `0F 0D` (PREFETCHW) | unsupported: halted | decoded and ignored |

`--ifuzz` after the sweep (the opcode audit, unchanged methodology):

```
ifuzz: 12800 streams, 1432 rejects      (was 1786 before 1.7.0)
ifuzz: 8086 opcodes missing: 0F (1 total)
ifuzz: 0F opcodes missing: 04 0A 0C 0E 0F 10 11 12 13 14 15 16 17 27 28 29 2A ... (138 total)
```

Every remaining `0F` hole is now either an SSE/MMX opcode or a reserved/undocumented
encoding -- exactly the set a 386/486-class CPU *should* reject.  The 8086 row is
still just the `0F` escape itself.

New `--itest` cases (one per row above, plus `PUSHA` stack order, `INSW`/`OUTSW`,
and `bswap ecx`):

```
push sp pushes the decremented value (186)
pusha stores 8 words in Intel order
enter 8,0 leaves bp at the frame
enter 4,2 builds the display chain
shl ax,cl with cl=21h shifts by 1
shl ax,cl with cl=0 keeps every flag
shl ax,4 (C1 imm8)
lmsW sets CR0 bits 0-3, smsw reads them
verr null selector: ZF=0, ax untouched
verw null selector: ZF=0, ax untouched
lar null selector: ZF=0, dx untouched
lsl null selector: ZF=0, dx untouched
bsf ax,bx finds bit 4
bsf ax,bx with bx=0 leaves ax and sets ZF
bsr ax,bx finds bit 15
cmpxchg bx,dx equal stores dx
cmpxchg bx,dx differ loads ax
xadd bx,dx
bswap ecx (1234 5678h -> 7856 3412h)
lgs ax,[bx]
insw from port 61h
outsw to port 61h
```
