# Performance: what costs what, and how to measure it

This page records where the emulator's time goes, the knobs that change it, and
how each claim was measured. Everything here was measured on the Windows 1.01
tree (`WIN.COM` -> `WIN100.BIN`, CGA 640x200) under QEMU + OVMF x86_64, because
that is the workload that made the question come up.

## Measure, do not guess

`CONFIG.SYS` `PERFLOG=ON` makes the emulator print, every ~2M guest
instructions, to the serial port:

```
[perf] steps=0000000012a00000      guest instructions executed so far
 ms=00000000000287b9               milliseconds since emu_run() started
 ips=00000000001cc12c              instructions per second (steps / ms)
 frames=0000000000000104           frames presented so far
```

Watch `ips` to judge the interpreter and `frames` to judge the display path. A
program that is slow because the guest itself is busy shows a high `ips`; one
that is slow because of drawing shows a low `frames` with the CPU still busy.

On the host harness the same numbers are only meaningful for the interpreter
(the host "framebuffer" is a PPM dump), so use the QEMU run for anything
involving frames.

## Where the time went (1.2.1 -> 1.2.2)

| cost | before | after |
| --- | --- | --- |
| screen update | whole 640x200 frame expanded and blitted (4 MB of framebuffer writes at 1280x800) | only the rows/column spans that changed |
| frame pacing | one flush per 262144 instructions (~140 ms/frame) | wall-clock, ~33 fps ceiling, clean frames cost nothing |
| `INT` to an unserviced vector | push frame + jump + `IRET` (12 byte writes) | returned immediately |
| real-mode address translation | `seg_resolve()` call per byte accessed | two shifts and a mask |
| firmware event pump | `gBS->Stall(1 ms)` every 8 ms | every 16 ms |
| instruction ring | 13 register stores per instruction, always | opt-in (`CONFIG.SYS RINGLOG=ON`) |

Result on the same image: **1.88M -> 2.29M guest instructions/s** and
**1.6 -> 19 frames/s** while the mouse moves.

## Knobs

| knob | effect |
| --- | --- |
| `CONFIG.SYS` `PERFLOG=ON` | the `[perf]` line above |
| `CONFIG.SYS` `RINGLOG=ON` | keep the per-instruction register ring that fatal traces print (off under UEFI by default) |
| `CONFIG.SYS` `MOUSESPEED=n` | host counts per reported mouse count (default 2 = ~1 CGA pixel per count; 1 is twice as fast, 4 gives fine control) |
| `CONFIG.SYS` `EXECHIGH=ON` | load an EXE image flush against the top of its block (Windows 1.x needs it; costs nothing) |
| `UEFIDOS_TICK_STEPS=<n>` (host) | force a fixed instruction period per BIOS timer tick instead of following wall-clock time |
| `UEFIDOS_GFX_VERIFY=1` (host) | after every flush, compare the incremental frame with a full expansion (`[gfx-verify] frames=N ok (bad frames 0)`) |

## Rules for the video path

1. Any guest write that can land in video memory must go through `emu_wb` /
   `emu_wb32` so `video_mark_addr()` sees it. Code that writes the framebuffer
   *pointer* directly (the software pointer's undo path) must mark its own
   rectangle with `video_mark_span()`.
2. A mode change must force a whole frame: `video_flush_force()` sets
   `g_dirty_all`, and the first flush after `plat_video_gfx_begin()` repaints
   everything.
3. New platforms implement `plat_video_gfx_show_dirty()`; ignoring the row/span
   arguments (as the host build does) is always safe, just slower.
4. Run with `UEFIDOS_GFX_VERIFY=1` after touching anything in this path. It
   reports the first stale pixel instead of letting a visual bug ship.

## Why not go further

An interpreter that executes ~2.3M instructions per second is an AT-class
machine, and that is what Windows 1.x was written for; it feels the same on a
real AT. The remaining known costs are a byte-at-a-time `rep movs/stos`, a
decoder that re-fetches every byte, and word accesses that translate the address
once per byte. Each is a real rewrite with real regression risk, so they are
only worth doing when a program's slowness is actually traced to them.
