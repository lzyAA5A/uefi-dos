# Roadmap: VESA linear framebuffer, UEFI drive enumeration, long file names

**Status in 0.8.0: all three are implemented and covered by the regression
suite.** This document is kept as the design record — it is what the
implementation was built against, and it explains *why* each trap exists, which
the final code no longer shows once it works around them. What shipped:

| Section | Shipped in 0.8.0 | Where the result is documented |
| --- | --- | --- |
| 1. VESA on GOP | `INT 10h` modes `13h`, `100h`, `101h`, `103h` + `AX=4F00h/4F01h/4F02h/4F03h/4F05h-4F08h`, DAC palette via ports `3C7h-3C9h` and `AX=1010h/1012h/1015h` | `docs/graphics.md` |
| 2. Extra drives | `plat_enum_blockdevs()` enumerates every `EFI_BLOCK_IO` handle, `dos_mount_all()` mounts them as `D:`, `E:`, …; `DRIVES` shell command | README "Graphics, extra drives, long names" |
| 3. Long file names | VFAT LFN read + create + delete, `DIR`/`TYPE`/`COPY` by long name, `dosapi findinfo.long_name`, `dosapi.longname()`, `AX=0101h` | README, `sdk/examples/hello64.c` |

Deviations from this design, decided during implementation:

- Section 1 proposed `Blt()` as the frame path. Measured on OVMF/QEMU:
  `EfiBltVideoFill` **hangs** the guest inside firmware and
  `EfiBltBufferToVideo` completes without drawing anything. The shipped path
  writes pixels straight into `Mode->FrameBufferBase`; see `docs/graphics.md`.
- Section 1 also proposed dirty rectangles. Shipped instead: a full-frame blit
  throttled to 30 ms plus a forced flush at the points where the guest can
  observe the screen (blocking key read, `INT 10h` mode set, program exit).
  Correctness first; a 640x480 frame copy is cheap enough here.
- Section 2 anticipated `SimpleFileSystem`. Shipped on top of
  `EFI_BLOCK_IO` instead, so a stick is mounted as a *DOS disk* (FAT12/16/32
  with an MBR or a superfloppy BPB) rather than as a host-filesystem mirror.
  That is the semantics a rescue tool needs.
- Section 3 proposed `find_first_ex`/`find_next_ex`. Shipped under those names
  in the kernel plus public `findinfo.long_name` and `longname()` in the native
  API, so a 64-bit program can ask for the long name of a file that a 16-bit
  program only knows as `LONGNA~1.TXT`.

---

## 1. INT 10h graphics: VESA linear framebuffer (640x480x256)

### Why it is feasible now

UEFI gives us the framebuffer directly: `EFI_GRAPHICS_OUTPUT_PROTOCOL`
(`gEfiGraphicsOutputProtocolGuid`, `EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE` →
`FrameBufferBase`, `FrameBufferSize`,
`Info->HorizontalResolution/VerticalResolution/PixelsPerScanLine/PixelFormat`).
`gOP->SetMode(index)` picks a mode. OVMF offers `640x480` (mode 0, `BltOnly`
only — no framebuffer!), then `800x600`, `1024x768`, `1280x1024`. **Trap #1:**
mode 0 is often `PixelBltOnly`, i.e. `FrameBufferBase == 0`; always check
`Info->PixelFormat != PixelBltOnly` before using the linear buffer, and prefer
`PixelBlueGreenRedReserved8BitPerColor` (BGRX) since that is what VBE
`640x480x8bpp` games expect when they write `RGB` triples.

### Guest-visible contract

Real DOS programs reach SVGA in two steps, both of which must work:

1. **VBE 2.0 via `INT 10h AX=4F00h/4F01h/4F02h`.** The block the guest expects at
   `ES:DI` after `4F00h` is 512 bytes: `VbeSignature='VESA'`, `VbeVersion=0x0200`,
   `VideoModePtr` → a 16-bit far pointer to a 0xFFFF-terminated mode list. Our
   list: `{ 0x0101 /*640x480x8*/, 0x0103 /*800x600x8*/, 0x0118 /*1024x768x8*/,
   0xFFFF }`. `4F01h` fills `ModeInfoBlock` (256 bytes) with
   `BytesPerScanLine`, `XResolution`, `YResolution`, `BitsPerPixel=8`,
   `MemoryModel=4 (packed pixel)`, `PhysBasePtr = framebuffer linear address`,
   `LinBytesPerScanLine`, and `ModeAttributes` with bit 0 (`supported`),
   bit 3 (`color`), bit 4 (`graphics`), bit 7 (`linear framebuffer available`).
2. **Mode set via `INT 10h AX=4F02h BX=mode|0x4000`** (bit 14 = use linear
   framebuffer). Then the program does `int 10h AX=0000h` no more — it writes
   `0xA0000` (banked) or `PhysBasePtr` (linear) directly.

The banked path (`WinFuncPtr`, `INT 10h AX=4F05h` = set bank) is what old
programs use when bit 14 is absent. Supporting both means: keep a 64 KB window at
`0xA0000` and let `4F05h` copy window ← framebuffer on switch (slow), *or*
advertise linear-only and accept that some titles need `-vbe:linear`.

### Emulator changes

* `Emu`: add `u8 *lfb; u32 lfb_pitch; u16 gfx_w, gfx_h; u8 gfx_bpp; u16 gfx_mode;
  u16 vbe_bank;` and a `gfx_active` flag. `video_mode` already exists.
* `emu_wb`/`emu_wd` currently mark the screen dirty only for
  `[0xB8000, 0xB8000+4000)`. Add: writes into the LFB range mark a rectangle
  dirty (track `dirty_x0/y0/x1/y1` to avoid a full 300 KB blit per instruction).
* `int10_handler`: `AH=00h` with `AL>=0x0D` (or after `4F02h`) switches
  `gfx_active`, sets the mode through a new `plat_video_mode(w,h,bpp)` hook.
* New `plat_video_frame(const u8 *lfb, int w, int h, int pitch, u32 dirty_x0,
  dirty_y0, u32 dirty_x1, dirty_y1)` in `plat.h`, implemented in
  `src/uefi/efi_plat.c` as a per-row `CopyMem` into `gOP->Mode->FrameBufferBase`
  with an 8-bit-palette→BGRX expansion; the host implementation writes a PPM.
* Palette: `INT 10h AX=1010h` (set one DAC entry) / `1012h` (set block) →
  keep a `u8 dac[256][3]`, and expand indices through it during the blit. Without
  this, 256-colour games render as garbage.

### Risks / effort

| item | effort | risk |
| --- | --- | --- |
| GOP mode pick + blit + palette | 1 day | low |
| VBE `4F00`/`4F01`/`4F02` block layout (offsets are easy to get wrong) | 1 day | medium — the mode list must be a *far pointer into guest memory*, not a host pointer |
| `4F05` banking emulation | 0.5 day | medium (per-write cost) |
| SVGA-aware mode restore on program exit (DOS returns to text mode; games often do not) | 0.5 day | medium — the shell must force text mode after any `.EXE` whose parent mode was text |
| DOOM specifically | 1 day of testing | high — needs `INT 10h 4A00h` (VBE state save) to not corrupt, and a 0.9 MB DOS4GW extender (DPMI works, but `INT 31h 0500h` blocks must be big and *contiguous*) |

Bottom line: 640x480x256 with VBE 2.0 discovery is realistic in ~3 days, and that
is exactly the mode 仙剑奇侠传 (PAL) uses (`VBE 101h`, banked, with a linear
fallback). DOOM needs the DPMI memory pool work too.

---

## 2. "Modern USB stick" — enumerate UEFI SimpleFileSystem as D:, E:, …

### Current state

`plat_boot_blockdev()` (`src/uefi/efi_plat.c`) walks the *loaded image's* device
path to `EFI_BLOCK_IO_PROTOCOL` and returns that one volume. That is why the OS
only ever sees `C:`.

### Design

Two independent layers, and it matters which one you use:

**A. Block level (`EFI_BLOCK_IO_PROTOCOL`) — the right one for us.**
`LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, ...)` returns every
block device the firmware knows: USB mass storage, NVMe, SATA, virtio, and *also*
the ESP partitions. For each handle:

1. `HandleProtocol(handle, &gEfiDevicePathProtocolGuid, &dp)` — keep the device
   path, it is how you name the drive in `MOUNT`/`DIR` output.
2. `HandleProtocol(handle, &gEfiBlockIoProtocolGuid, &bio)`; skip
   `bio->Media->LogicalPartition` handles **or** skip whole disks — pick
   partitions when `Media->LogicalPartition` is set, otherwise the raw disk, and
   remember the parent so a disk appears once.
3. Read LBA 0 (and, if it parses as an MBR, each partition entry) and try
   `fat_mount()` on the offsets `{0} ∪ {MBR partitions}`. A volume that mounts is
   a drive letter; one that does not is skipped (no error message per stick).
4. `bio->Media->ReadOnly` → pass `read_only=1` to `fat_mount`, so `DEL` reports
   "access denied" instead of corrupting a write-protected stick.

Drive letters: 26 max (`DOS_MAX_DRIVES`). Allocation policy: `C:` stays the boot
volume, then partitions in handle order — deterministic, so a scripted
`COPY X.TXT D:` keeps working across boots. Store the mapping in
`g_dos.drives[i].bd` (already `blockdev`), plus a `char label[12]` and an
`int removable` flag for `VOL`/`DIR` output.

**B. File-system level (`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`) — useful for a
different reason.** It gives *long* names and Unicode for free, and it is the only
way to read files on a volume whose FAT we cannot parse (exFAT, or a FAT32 with a
weird boot sector). The catch: mixing two file systems over the same disk means
two caches and two notions of "the current directory". So the plan is:

* **Default:** block level only (one FAT implementation, one cache, DOS
  semantics, images and real disks behave identically).
* **Optional:** `MOUNT U: fs` mounts a `SimpleFileSystem` volume in a
  *pass-through* mode where `open/read/find` call the firmware directly and
  write is refused. This is for rescue work ("read a file off an exFAT stick"),
  not for a DOS drive.

### Traps

* `EFI_BLOCK_IO_PROTOCOL` `MediaId` changes when media is inserted/changed. Cache
  it and, on `EFI_MEDIA_CHANGED`, drop the mounted volume and re-probe (a USB
  stick pulled mid-`COPY` must not write to the wrong blocks).
* `Media->BlockSize` is *not* necessarily 512. `blockdev.blk_size` exists —
  use it, and when it is 4096 wrap it so FAT still sees 512-byte sectors.
* OVMF enumerates the ESP twice (whole disk + partition); without the
  `LogicalPartition` filter you get `C:` and `D:` pointing at the same bytes.
* The block cache in `fs/fat.c` is keyed by LBA only; with several volumes it
  must be per-`fat_vol` (it already is) *and* invalidated on media change.
* USB enumeration happens at boot. `INT 13h AH=00h` on a dead drive should
  re-probe, not hang.

### Effort

`plat_enum_blockdevs(blockdev *out, int max, char labels[][12])` = ~120 lines in
`efi_plat.c`; `dos_mount_all()` = ~60 lines in `kernel/dos.c`; `DIR`/`VOL`
output + `MOUNT` by device path = ~80 lines. Half a day, low risk, high payoff:
this is what turns the thing into a rescue tool.

---

## 3. Long file names

### Current state

`dos_fat_name()` uppercases to 8.3 and stores an 11-byte name; directory
scanning *skips* entries with `0x0F` (LFN) attribute; `dos_find_first` returns
8.3 only. So `DIR` shows `ABCDEF~1.TXT` and `TYPE Long file name.txt` fails.

### Design: one namespace internally, two views outwards

```
        native API (64-bit)                INT 21h (16-bit)
        long names + 8.3                  8.3 only, as DOS 6 does
                |                               |
                +------------- dos_vfs ---------+
                       (path → dirent, with LFN)
```

1. **Read LFN.** A `dirent` with `attr & 0x3F == 0x0F` holds 13 UCS-2 chars
   (offsets 1,3,5,7,9,14,16,18,20,22,24,28,30) plus a sequence byte (ordinal,
   `0x40 | n` on the last) and a checksum byte that must match
   `dos_lfn_checksum(short_name11)`. Assemble the sequence backwards into
   `char lfn[256]` (UTF-8; ASCII is the 99% case — convert UCS-2 → UTF-8 and
   store a byte-length).
2. **`dos_handle`/`dirent` gain `char lfn[DOS_LFN_MAX]; u8 has_lfn;`.** `dos_find`
   already carries a directory cursor, so `dos_find_first`/`next` just fill
   `lfn` too. The 16-bit FCB path ignores it (DOS 3.x/4.0 semantics).
3. **`dos_lookup()` tries three forms, in this order:** exact short name
   (`ABCDEF~1.TXT`, case-insensitive) → exact LFN (case-insensitive, so
   `type hello.txt` works) → and if the name is *not* 8.3-legal, the LFN only.
   This keeps existing 16-bit programs (which only ever pass 8.3) on the fast
   path.
4. **Create with a long name** needs: generate a unique 8.3 alias
   (`NAME~1.EXT`, probing `~1`..`~9`, then `~10`... using the *tail* form when
   the base is longer than 6), write the LFN entries before the short entry,
   update `LFN` checksums, and (for FAT32) leave the volume dirty flag logic
   alone. Creation from the *native* API gets the long name; creation from
   `INT 21h AH=3Ch` should stay 8.3 (that is what a 16-bit program expects and
   what DOS 6 did — LFN came from Windows 95).
5. **`dosapi` additions** (ABI-compatible, append at the end of
   `dosapi_services` and bump `DOSAPI_VERSION` to `0x0101`):
   * `dosapi_findinfo` gains `char longname[260]` — but that changes the struct
     size, so add `find_first_ex`/`find_next_ex` returning a new
     `dosapi_findinfo_ex`, and keep the old pair untouched;
   * `longname(const char *path, char *out, u32 max)` — the LFN of an existing
     file given either name form.
6. **`DIR` output** (`src/shell/shell.c`): print the long name in a `NAME` column,
   the 8.3 name in a second column (DOS 7 style), and only switch to the compact
   8.3 layout when the terminal is 40 columns.

### Traps

* **Sort order:** DOS sorts by 8.3 name; Windows Explorer sorts by long name.
  Keep DOS order (8.3) so `DIR` output stays stable, and document it.
* **Checksum:** if it mismatches, the LFN is stale garbage (a deleted file
  reused the short name) — ignore the LFN and show the 8.3 name. Never trust an
  LFN whose checksum fails.
* **`~1` collisions** after `DEL`: DOS keeps the alias with the file; a new file
  with the same base gets the next free number, so aliases are not stable across
  sessions. Do not cache them.
* **Case:** FAT is case-insensitive but case-*preserving*; the LFN carries the
  case, the 8.3 name is uppercased by convention (the NT `0x18` lowercase flags
  in the short entry can be honoured cheaply).
* **Path separators:** accept both `/` and `\` in the native API (UEFI habits),
  emit `\` in `getcwd`.
* **Romanised 8.3 for CJK names:** `仙剑奇侠传.EXE` has no ASCII base. The DOS
  convention is to mangle to `_____~1.EXE` using `_` per non-ASCII char — that is
  what Windows does and what any DOS program that cannot read the LFN will see.

### Effort

Read path (LFN assembly + lookup + `find` + `DIR` columns) ≈ 1 day. Create path
(alias generation + LFN write + checksum) ≈ 1 day and it must be tested against
real Windows/mkfs.vfat volumes, not just our own writer. Half a day for the
native-API extension and docs.

---

## Suggested order

1. **Drive enumeration** (`D:`/`E:` from every block device) — half a day, no
   emulator risk, immediately turns the OS into a rescue tool.
2. **LFN read** — one day, improves every directory listing, no write risk.
3. **VESA 640x480x256** (GOP + palette + `4F00/4F01/4F02`) — three days, then
   PAL/仙剑 becomes a compatibility target.
4. **LFN create**, **VBE banking**, **DOOM/DOS4GW** — diminishing returns, do
   them when a specific program needs them.
