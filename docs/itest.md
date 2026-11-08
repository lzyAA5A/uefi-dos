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
