# Protected mode and DPMI (v0.4.0)

The 16-bit compatibility machine inside UEFI-DOS grew a 386 core: a program can
leave real mode, run 32-bit flat code, and 32-bit clients can bootstrap through
DPMI (INT 2Fh AX=1687h → host entry → INT 31h).  This document records what is
implemented, how it was verified, and what is still missing.

## Stage 0 — prerequisites

| Item | Result |
| --- | --- |
| EFIAPI ABI | `src/efi/efi.h` now spells it out: `__attribute__((ms_abi))` on non-Windows x86_64 builds, empty on MinGW (already ms_abi) and on IA32 (cdecl). Boot runs an ABI smoke test through `gBS->Stall` + `gRT->GetTime` and prints `UEFI-DOS: ABI-OK (Stall+GetTime, year YYYY)`. |
| Millisecond clock | `plat_ticks_ms()` used to return 0. `EFI_BOOT_SERVICES.GetNextMonotonicCount` turned out to be useless for timing (EDK2 just increments a counter per call), so the clock is the CPU TSC calibrated once against `Stall(50 ms)`, with the firmware RTC as fallback. Boot prints `UEFI-DOS: timer <units>/ms` (≈2.42e6 under QEMU/OVMF). `APITEST` now prints `ticks a -> b, ms c -> d TICKS-OK`. |
| Default DTA | `load_com_at` / `load_exe_at` set `g_dos.dta_seg = psp_seg; g_dos.dta_off = 0x80`, like DOS does at load time. Verified by `DTATEST.COM` (find-first with no `AH=1Ah` must fill `PSP:0080h`). |
| INT 2Fh passthrough | An INT 2Fh call that is not ours leaves **every** register untouched. Verified by `I2FTEST.COM`. |
| `-fno-builtin` | Added to all three build configurations so the freestanding `memcpy`/`memset` in `src/common/util.c` are never rewritten into calls to themselves. |

## Stage 1 — instruction core split

`step()` was one large switch. It is now:

* `step_8086()` — prefixes, the 8086/80186 instruction set, string ops,
  dispatch to the rest;
* `step_386()` — everything behind the `0F` prefix: `MOV CRn`, `LGDT`/`LIDT`/
  `SGDT`/`SIDT`, `LLDT`/`SLDT`/`LTR`/`STR`, `CLTS`, `Jcc rel32`, `SETcc`,
  `CMOVcc`, `MOVZX`/`MOVSX`, `SHLD`/`SHRD`, `BT`/`BTS`/`BTR`/`BTC`, `IMUL r,r/m`,
  `RDTSC`, `CPUID`, `PUSH`/`POP FS/GS`, `UD2`;
* `step_fpu()` — the `D8..DF` / `0F`-prefixed FPU escapes are decoded (so
  instruction length stays right) and otherwise ignored, exactly like an 8086
  with no coprocessor;
* `step()` — the dispatcher, plus `step_public()` for the DPMI reflection loops.

Registers are 32-bit with 16- and 8-bit aliases (`union { u32 eax; u16 ax;
struct { u8 al, ah; }; }`), so writing `ax` keeps the upper half and writing
`eax` replaces all 32 bits — which is what a 32-bit client dropping back to
16 bits expects.

**Operand-size rule (important):** in a 32-bit code segment the default operand
and address size is 32 and the `0x66`/`0x67` prefixes switch *back* to 16. The
emulator models this with `op_is32()` / `ad_is32()`, which XOR the prefix state
with the CS descriptor's D bit. Getting this wrong is invisible in real mode but
makes every flat 32-bit program decode garbage.

`decode_mrm()` understands 32-bit addressing (SIB byte, displacement forms,
`mod=0,rm=5` = disp32, `[ebp]` = SS default).

## Stage 1 — segment cache and limits

Every segment register has a hidden cache (`SegCache sel/base/limit/attr`).
Memory access resolves a selector through `seg_resolve()`:

* real mode → `sel << 4`, limit 64 KiB (no cache dependency, so a direct poke
  at `e->ds` can never desynchronise a cache);
* protected mode → descriptor walk through the GDT, or the DPMI host LDT when
  `sel & 4`, with the cache used only as a memo keyed on the selector value.

In protected mode an access beyond the segment limit latches `e->pm_fault`
instead of corrupting emulated RAM.

The emulated machine now has **16 MiB** of RAM (`EMU_RAM_SIZE`), so 32-bit
clients have room while real-mode code still sees 640 KiB / 1 MiB through the
BIOS data area.

## Stage 2 — 386 protected mode

* `cr0/cr2/cr3/cr4`, `gdtr`, `idtr`, `ldtr`, `tr`, `iopl`, `pm_fault`.
* Mode changes: `MOV CR0` reloads every segment cache, so a `PE` flip takes
  effect immediately.
* `LGDT`/`LIDT` install the tables; `LIDT` also runs `pm_patch_idt()`, which
  writes UEFI-DOS service gates (vectors 10h, 13h, 16h, 1Ah, 20h, 21h, 27h, 2Fh,
  31h) pointing at our per-vector trampoline into whatever IDT the client
  loaded. That is how `INT 21h` works in protected mode: the client keeps its own
  handlers on every other vector, while the DOS service vectors come back to us.
* `do_int_prot()` walks the IDT gate; our trampoline selector/offset means "run
  the DOS service inline" (no frame, like the real-mode path), anything else
  pushes `EFLAGS/CS/EIP` at the gate's size and jumps to the handler.
* Segment loading checks presence and uses descriptor bits for the 16/32-bit
  defaults; a limit violation reports `UEFI-DOS: PM int xx ... gate_sel=...` and
  halts instead of running wild.

## Stage 3 — DPMI host

`INT 2Fh AX=1687h` reports a DPMI 0.9 host with 32-bit client support and hands
out the real-mode entry point `F000:F000` (which is `INT 60h`).

`INT 60h AX=0001h` switches the caller to protected mode: it pops the far-call
return address, builds a host GDT (null, client code/data/stack/extra, LDT,
flat 32-bit code and data), installs a default IDT with service gates, sets
`CR0.PE`, and returns with CS:IP = selector:same-offset. The selectors carry
`base = realsegment << 4`, so every offset in the client keeps its meaning.

`INT 31h` implements:

| Function | Support |
| --- | --- |
| 0000h / 000Dh | allocate (first free / specific) LDT descriptor |
| 0001h | free descriptor |
| 0002h | real-mode segment → selector |
| 0003h | selector increment |
| 0006h–0009h | get/set descriptor base, limit (byte granularity), access rights |
| 000Ah–000Ch | alias / get / set raw descriptor |
| 0200h–0205h | real-mode vector get/set, PM vector set (also rebuilt for us) |
| 0300h | simulate a real-mode interrupt (register structure at ES:EDI) |
| 0301h | call a real-mode far procedure |
| 0302h/0303h | real-mode vector get/set |
| 0400h | version: AX=000Ah (0.9), BX=5A5Ah, CL=3 (386) |
| 0500h–0503h | DOS memory info / allocate / free / resize |
| 0600h–0604h, 0702h–0703h, 0800h, 0900h, 0A00h | paging/vendor stubs |

The host LDT lives at a private linear address (`DPMI_LDT_LIN = 0x00F00000`),
so a client's GDT is never touched and LDT selectors resolve without an LDTR
descriptor walk. DOS memory for `0501h` comes from a first-fit pool between
64 KiB and 636 KiB, so the blocks stay real-mode addressable for `0300h`.

AX=0300h works by snapshotting the whole `Emu`, loading the client's real-mode
register structure, dropping `CR0.PE`, calling the DOS service (or running the
program's own real-mode handler to completion), then writing the 16-bit results
back into the structure and restoring the protected-mode machine state. Memory
changes stay, which is what a real DPMI reflection does.

## Verification

`DPMITEST.COM` (generated by `tools/make_pm_tests.py`) is a real DPMI client:

```
DPMI-HOST-OK      INT 2Fh AX=1687h found the host
PM16-OK           16-bit protected mode after the host entry switched us
PM32-DOS-OK       far jump to the flat 32-bit selector, INT 21h AH=9 from 32-bit code
REFLECTED-21H-OK  printed *by* a real-mode INT 21h reached through INT 31h AX=0300h
RMREFLECT-OK      the reflection returned to protected mode
DOSMEM-OK         INT 31h AX=0501h allocated 16 paragraphs of DOS memory
```

It runs in the host harness (`hosttest.exe --shell build/UEFIDOS.img DPMITEST`)
and in both QEMU smoke tests (x86_64 + IA32), and its output is part of the
`run_tests.ps1` assertions.

## Performance note (found while testing)

Writing to a mounted floppy image took **32.7 s** for 8 sectors in QEMU. The FAT
sector cache had a single slot, so every chain-walk step that alternated between
two FAT sectors went back to the disk. `fat_vol` now keeps an eight-entry FAT
sector cache (`fat_sec_ptr()`); the same copy takes **113 ms** (≈290×). This was
a pre-existing bug, not a protected-mode regression — the old autoexec simply
fit inside the 120 s smoke-test timeout.

## Limitations / next

* No paging (`CR0.PG` is refused), no virtual-8086 mode, no task switching.
* Ring 3 clients are not isolated: everything runs at CPL 0 and DPL checks are
  minimal (descriptor DPL is not enforced on far jumps).
* `0300h`/`0301h` run program-supplied real-mode handlers to completion with a
  simple guard; hardware interrupts are not reflected while that happens.
* No `INT 15h`/`INT 33h`/XMS/EMS services for extenders that probe for them.
* `AX=0501h` blocks are not merged back on free (no coalescing).

## 阶段 5 设计草案（ring 3 + 分页 + 完整 DPMI，尚未实现）

现状：DPMI 客户进入保护模式后仍运行在 **ring 0**，用分段（4 GB 平段）而不是分页隔离；
`INT 31h AX=0600h`（分页）与 `AX=0B00h`（硬件中断）是桩，`AX=0300h` 反射不处理硬件中断。

### 5.1 TSS + 门 + 跨 ring 切栈

1. `dpmi_enter_pm()` 构建 GDT 时增加：
   - TSS 描述符（type 0x89，base = 一块 104 字节的 TSS），`esp0` 指向 0x9000 附近的宿主栈，
     `ss0 = SEL_DATA16`（或 0x10），`iomap_base = sizeof(TSS)`；
   - 客户端代码/数据段 DPL 改为 3（`0x00FA` / `0x00F2`），宿主服务门 DPL 保持 0；
   - `LTR SEL_TSS` 之后 ring 3 → ring 0 的栈切换由硬件语义完成。
2. 模拟器需要新增：
   - `IRET` 跨 ring：弹出 ESP/SS 后再弹 EFLAGS/CS/EIP，并校验 CPL 变化；
   - `RETF` 跨 ring、`CALL/JMP far` 的 DPL 检查（违反 → `#GP`，向量 0x0D）；
   - 中断/异常入 ring 0 时清 `IF`、用第 5 类门（任务门）或 TSS 的 `ss0:esp0` 压栈；
   - `#GP`/`#PF` 的 delivered 路径复用现有 `do_int_prot()`。
3. 验证程序 `RING3TEST.COM`：`INT 31h AX=0001` 进入后读 CS 的 RPL 应为 3，
   `INT 31h AX=0300h` 反射一次 INT 21h AH=9 能回到 ring 0 打印并正确返回 ring 3。

### 5.2 CR3 页表 + #PF

1. `Emu` 增加 `cr3` 已存在，需要实现 2 级（无 PAE）页表遍历：
   `lin → pde(0x3FF) → pte(0x3FF) → 物理页 + offset`，尊重 U/S、R/W、P 位；
2. `#PF`（向量 0x0E）压入错误码（页错误码 + EIP），`cr2` = 线性地址；
3. 宿主侧给 16 MiB 模拟 RAM 建一张恒等映射的页表（4 MB 页可选，减少页表数量），
   DPMI 客户端拿到的线性地址与物理一致，`CR0.PG=1` 后可验证“访问未映射页触发 #PF”。

### 5.3 DPMI 分页服务

- `AX=0600h` 分配内存（返回线性地址）与 `0601h` 释放；
- `AX=0602h` 标记为分页候选、`0603h` 重新分页、`0604h` 取页大小；
- `AX=0B00h` 挂硬件中断向量、`0B01h` 取状态、`0B02h`/`0B03h` 复位；
- 验证：一个 DOS4GW 风格的 32 位 extender 程序能跑 `INT 21h` + 图形写屏。

### 5.4 完整 DPMI 服务

- `AX=0500h` 取内存信息（返回最大可用线性空间、页数等真实值）；
- `AX=0502h` 释放 DOS 内存块（当前 0501h 有，0502h 缺）；
- `AX=0A00h` 取厂商信息；`AX=0702h`/`0703h` 标记页为可换出；
- 验证：复杂 extender（CWSDPMI 风格客户端）加载并运行。

依赖顺序固定：**5.1 → 5.2 → 5.3 → 5.4**（没有栈切换就无法正确处理 #PF，没有 #PF 就无法
验证页表）。阶段 4 与阶段 6 与本阶段完全解耦，已完成并独立验证。
