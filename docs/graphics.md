# Graphics modes: mode 13h and VESA VBE on a UEFI framebuffer

UEFI-DOS has no BIOS, so the classic path "INT 10h switches to a video mode, the
program writes pixels to `A0000`, the card scans them out" had to be rebuilt on
top of the firmware's `EFI_GRAPHICS_OUTPUT_PROTOCOL` (GOP). This note documents
what is emulated, how the pieces fit, and where the limits are.

## What a DOS program sees

| INT 10h call | Behaviour |
| --- | --- |
| `AH=00h AL=13h` | 320x200, 256 colours, framebuffer at `A0000` (64 000 bytes) |
| `AH=00h AL=03h/07h/00h` | back to text mode (80x25 mirror at `B8000`) |
| `AH=0Fh` | current mode in `AL`, 80 columns in `AH` |
| `AH=10h AL=00h/10h` | set one DAC register (`BX` index, `DH` red, `CH` green, `CL` blue) |
| `AH=10h AL=12h` | set a block of DAC registers (`BX` first, `CX` count, `ES:DX` = 3-byte triples) |
| `AH=10h AL=15h` | read one DAC register |
| `AX=4F00h` | VBE info block: signature `VESA`, VBE 2.0, mode list `100h 101h 103h` |
| `AX=4F01h CX=mode` | mode information block (attributes, 640x480/800x600, 8 bpp, LFB flag, `A0000` window) |
| `AX=4F02h BX=mode\|4000h` | set the mode (linear framebuffer bit accepted) |
| `AX=4F03h` | current mode in `BX` |
| `AX=4F05h/4F06h/4F07h` | accepted and ignored: the framebuffer is linear, no banking |
| `AX=4F08h`, `AX=4F0Ah` | palette format query; protected-mode interface reported unsupported |

Palette ports also work: `3C8h` (write index), `3C9h` (write/read 6-bit
components), `3C7h` (read index). The DAC starts as a CGA-like 16-colour set
plus a grey ramp; programs are expected to load their own palette.

Supported modes: `13h` (320x200), `100h` (640x400), `101h` (640x480),
`103h` (800x600), all 8 bits per pixel with a packed-pixel memory model.

## How a frame reaches the screen

```
DOS program            emulator (src/emu)                  platform (src/uefi)
------------           --------------------                -------------------
writes A0000    -->    emu_wb() marks the frame dirty  -->  plat_video_gfx_show()
INT 10h 4F02h   -->    emu_set_video_mode()            -->  plat_video_gfx_begin()
                        (gfx_w/gfx_h + DAC state)           QueryMode/SetMode (GOP)
```

* `emu_set_video_mode()` stores the geometry, clears the emulated framebuffer,
  and asks the platform for a matching display mode.
* Frames are pushed at most every 30 ms, at every intercepted `INT`, and once
  right before the emulator blocks waiting for a keystroke (otherwise a program
  that draws and then waits would leave a half-painted screen).
* `plat_video_gfx_begin()` picks the *smallest* GOP mode that is at least as
  large as the DOS mode, then computes an integer upscale (`min(gw/w, gh/h)`,
  capped at 4) and centres the image with letterbox borders.
* `plat_video_gfx_show()` converts 8-bit palette indices to the GOP pixel
  format (`BlueGreenRedReserved8BitPerColor` or `RedGreenBlue…`) and writes the
  pixels straight into `Mode->FrameBufferBase`, honouring `PixelsPerScanLine`.

## Two things that did not work

* `Blt(EfiBltVideoFill, ...)` **stalls** in the OVMF/QEMU GOP implementation
  used here (the guest froze inside the firmware with the screen already
  switched). The full-screen clear was dropped and the first frame simply
  paints every pixel.
* `Blt(EfiBltBufferToVideo, ...)` did not put anything visible on the screen in
  the same environment, so direct linear-framebuffer writes are used instead.
  They are also cheaper: no intermediate 4-byte-per-pixel buffer.

## Text output while a graphics mode is active

The firmware console would repaint its whole text grid over the framebuffer as
soon as anything printed (that is how the first graphics screen turned black
again). So while a graphics mode owns the display, `plat_putc()` sends text to
the **serial port** instead. `EXIT`/mode 03h bring the text mirror back.

## Signals

| Where | What |
| --- | --- |
| `gfx: mode set 320x200x8` | host platform: a graphics mode was selected |
| `gfx: 320x200 framebuffer -> file.ppm` | host platform wrote a PPM dump (`UEFIDOS_GFX_DUMP`) |
| `gfx: back to 80x25 text mode` | text mode restored |
| `gfx: linear framebuffer RGB/BGR` | firmware side: GOP mode chosen and mapped |
| `GFX-OK 320x200x256` | `GFXTEST.COM` painted the mode-13h framebuffer |
| `VESA-OK`, `VESA-MODE-OK 640x480x256` | `VESATEST.COM` walked the VBE calls |

`tools/gfx_shot.ps1 -Image build/gfxdemo.img -Out build/shot.png` boots an image
in QEMU, screenshots the framebuffer through the monitor and converts it to PNG.

## Limits

* 8 bits per pixel only (no 16/24/32 bpp VESA modes, no 4-bit planar modes).
* No vertical-retrace synchronisation (`3DAh` alternates forever, which is what
  most DOS programs poll, but there is no real vblank pacing).
* The framebuffer is a plain RAM window: no hardware scrolling, no page
  flipping (`4F07h` is accepted and ignored), no bank switching.
* A 16-bit DOS program can only address 64 KB of framebuffer at a time; real
  VESA programs bank or use a 32-bit extender, and `VESATEST.COM` shows the
  bank-free way (five 64 KB passes with `DS` moved between them).
* 320x200 is displayed with square pixels (integer upscale), not the classic
  320x200-on-4:3 stretch.
