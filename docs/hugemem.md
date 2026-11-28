# Extended memory: an 8 GB pool a 16-bit machine can reach (1.11.0)

A 16-bit PC cannot address 8 GB.  That is not a bug to fix — real-mode
`segment:offset` tops out at 1 MB (plus the 64 KB-16 high memory area once the
A20 gate is open) and even a 286 in protected mode stops at 16 MB.  What a
program *can* have is 8 GB of extended memory that it reaches through a window
and through block moves.  That is what the XMS specification exists for, and
that is what UEFI-DOS now provides.

## What was measured first

The old answer was "extended memory needs an XMS driver we do not provide", and
`src/emu/emu.c` returned `FFh` for every real-mode read at or above `100000h`.
DOS itself never had a 32 MB file limit (see `docs/largefiles.md`), but a *guest
program* had no way to touch a byte above 1 MB.

## The pool

`src/emu/emu.c`, the section that starts at
`/* ---- extended memory: an 8 GB pool, committed one page at a time ---- */`:

| item | value |
| --- | --- |
| address space | `UEFIDOS_XMS_MB` = **8192 MB** (`src/uefidos_config.h`) |
| page size | 64 KB (`XMS_PAGE_SHIFT` = 16) |
| pages | 131072 (`XMS_MAX_PAGES`) |
| cost when untouched | one `u8 *` table, 1 MB |
| cost per touched page | 64 KB, allocated from the firmware on first **write** |

`g_xms_tab[]` is the page table; a NULL entry is a page nobody has written to,
and reads of it return 0 without allocating.  `emu_xms_write()` allocates the
page it needs, one page at a time, so a program that allocates 8 GB and writes
40 MB pays for 40 MB.  Offsets are `u64` throughout — 8 GB is 33 bits, and the
first version of this code used `u32` and silently wrapped at 4 GB (the symptom
was a "top of the pool" report at 4095 MB).

Everything else in this document is a *view* of that one pool:

```
                 +----------------------------------+
   8 GB pool --- | page 0 | page 1 | ... | page N   |   sparse, 64 KB pages
                 +----------------------------------+
                      |          |             |
      XMS blocks  ----+          |             +---- window API (INT E0h)
     (INT 2Fh/43h)                |
                                   +---- MEM8 (the shell, runs outside the CPU)
```

## The three ways in

### 1. XMS driver — `INT 2Fh`

This is the standard route, the one HIMEM.SYS provides on a real machine, so
programs written before 1990 already speak it:

* `INT 2Fh AX=4300h` → `AL=80h` (installed),
* `INT 2Fh AX=4310h` → `ES:BX` = a far-callable entry point.

The entry point is three bytes at `F000:F000`: `CD E1 CB` — `int E1h; retf`.
The `INT E1h` handler does the work and `IRET`s back to that `retf`, which
returns to the caller: exactly the ABI an XMS driver has.

Implemented functions (`int_e1_handler` in `src/emu/int2f.c`):

| AH | function | notes |
| --- | --- | --- |
| 00h | get version | returns `AX=0300h` (XMS 3.00: 32-bit sizes) |
| 01h / 02h | request / release HMA | enables `FFFF:0010` … `10FFEF` (see below) |
| 03h–07h | global/local A20 on/off/query | sets `e->a20_on` |
| 08h | query free | `AX` = largest block KB, `DX` = total free KB (16-bit view) |
| 09h | allocate (DX = KB) | `DX` = handle on success |
| 0Ah | free (DX = handle) | |
| 0Bh | move EMB (DS:SI) | 32-bit offsets, `handle 0` = conventional memory |
| 0Ch / 0Dh | lock / unlock | |
| 0Eh | handle info | lock count, free handles, block size |
| 88h / 89h | 32-bit query / allocate | answers live in `EAX`/`EDX` |
| 10h | request UMB | none (no upper-memory blocks) |

Errors go to `BL`: `A0` all locked, `A1` out of handles, `A2` bad handle,
`A3`/`A4` bad source handle/offset, `A5`/`A6` bad destination, `A7` out of
memory, `8B` page-frame overrun.

An XMS block is at most 4 GB-64 KB (its size field is 32 bits), so the 8 GB pool
spans **two** blocks.  That is the specification, not a limitation of this
implementation.

### 2. Window API — `INT E0h AH=FDh`

For programs that would rather pass a 32-bit offset than fill in an EMB
structure.  Same 40-byte request block as the 64-bit file API
(`docs/largefiles.md`), plus `AL`:

| AL | meaning | in | out |
| --- | --- | --- | --- |
| 10h | HUGEMEM OPEN | `CX:BX` = KB to allocate | `BX` = handle, block base in the block |
| 11h | HUGEMEM MAP | handle, `+0` = 32-bit offset inside the block, `SI` = guest segment | that segment now *is* 64 KB of the pool |
| 12h | HUGEMEM COPY | handle, `+0` offset, `+8` count, `+16/+20` buffer off/seg, `+22` bit0 = direction | bytes moved |
| 13h | HUGEMEM CLOSE | handle | window flushed back into the pool, block freed |
| 14h | HUGEMEM STATS | — | `+28` total KB, `+32` committed KB, `+8` free KB |

`MAP` copies the 64 KB window into ordinary emulated RAM, so the guest addresses
it with normal 16-bit offsets and the emulator's memory hot path stays a single
mask.  The old window's contents are written back into the pool before a new
mapping is installed; `CLOSE` flushes the last one.

### 3. `MEM8` — the shell, which needs no emulated program

```
C:\> MEM8
mem8: pool .............8192 MB of address space
mem8: free .............8192 MB
mem8: committed ........0 MB (pages really backed by RAM)
mem8: XMS driver ....... present (INT 2Fh AX=4310h hands out a far entry point)
mem8: block 1 ..........4095 MB at offset 0 MB
mem8: block 2 ..........4095 MB at offset 4095 MB (past the 4 GB line)
mem8: top of the pool .. offset 8191 MB -- writing and reading it back
mem8: the top of the pool holds its data (that is past 4 GB)
mem8: writing .......... 64 MB across block 1, 4 KB at a time
mem8: verifying ........ 64 MB, every byte matched
mem8: elapsed ..........47 ms
mem8: throughput .......1361 MB/s (write plus verify)
mem8: committed after ..64 MB
mem8: claimed/committed 8192 MB claimed, only what the program touched costs RAM
```

`MEM8 [MB]` writes a position-dependent pattern into `MB` megabytes (default
64), reads it back and compares, and also writes and re-reads the **last 64 KB of
the pool** — the "past 4 GB" line is the real proof that the addressing is 33
bits wide.  The committed figure afterwards is the megabytes asked for, not the
megabytes claimed: that is the sparse store doing its job.

### `XMSTEST.COM` — the same thing from inside DOS

`tools/xmstest.asm` → `build/XMSTEST.COM` (9560 bytes, 8086-only).  It is a
normal DOS program, so everything it proves is proved *through the emulated CPU*:

```
xmstest: XMS driver present, entry point from INT 2Fh AX=4310h.
xmstest: XMS version ....... 0300
xmstest: largest block ...... 65535 KB, total free 65535
xmstest: allocated 1 MB .... handle 0001
xmstest: 4 KB moved into the block.
xmstest: moved back, all 4096 bytes identical.
xmstest: 32-bit offset inside the block works (512 KB in).
xmstest: block freed.
```

The `65535 KB` is not a cap on the pool: a 16-bit program asks with XMS function
08h, whose answer is a 16-bit field and therefore saturates.  The 32-bit figure
lives in `EAX`/`EDX` of function 88h, which a real-mode program can only read
with an operand-size prefix.  The pool's real size is what `MEM8` prints.

## The high memory area

`XMS 01h` claims the HMA: real mode may then read and write `100000h`–`10FFFFh`
through `FFFF:0010`, but only while the A20 gate is open (`e->a20_on`).
`noram_lin()` in `src/emu/emu.c` is where that is decided; before 1.11.0 every
address at or above `100000h` read `FFh`.

The HMA is real emulated RAM (`EMU_RAM_SIZE` is 16 MB, which is also what a
32-bit DPMI client can reach), *not* part of the sparse pool: on real hardware
the HMA is the first 64 KB-16 of extended memory, and DOS keeps it allocated for
itself, so treating it separately is the honest model.

## Test coverage

* `--itest`: 96 checks, unchanged and passing — the instruction set was not
  touched by this work.
* `XMSTEST.COM` in the harness (image `build/usbtest.img`): all eight steps pass.
* `MEM8` in the harness and in the release image: 8 GB claimed, 64 MB verified,
  top of the pool at 8191 MB verified.
* `VER` on the release image: `UEFI-DOS Version 1.11.0 (DOS 4.00 compatible,
  FAT32)`.

## Things that are deliberately not here

* **No flat 8 GB.**  A 16-bit guest has no 64-bit registers; anything claiming
  otherwise is windowing dressed up.
* **No EMS.**  LIM EMS 4.0 is a different (page-frame, 64 KB window) API.  The
  `INT E0h` window API is this system's native equivalent, and XMS covers the
  programs that matter.
* **No physical 8 GB allocation.**  `plat_alloc()` takes a `u32`, so a single
  block over 4 GB is impossible by construction; the pool allocates 64 KB pages
  instead and hands out 8 GB of address space.
* **Freed blocks are not reclaimed.**  The pool is a bump allocator from the top;
  freeing a block frees its address range for reuse but leaves its pages
  committed, which keeps an 8 GB pool cheap to manage and honest about what it
  cost.  A guest that allocates and frees 8 GB of *written* pages is asking the
  firmware for 8 GB of RAM, and that will fail loudly rather than silently.
