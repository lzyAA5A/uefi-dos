# UEFI-DOS native SDK — writing 32/64-bit programs

UEFI-DOS boots as a UEFI application, so the CPU is already in 64-bit long mode
(or 32-bit protected mode on `BOOTIA32.EFI`). A *native program* is an ordinary
PE image that the DOS shell loads and calls **directly on the real CPU**: no 8086
interpreter, no mode switch, no reboot, and access to the same DOS services that
`INT 21h` programs use — through a wide (native) calling convention.

```
      INT 21h program (16-bit)          native program (32/64-bit PE)
              |                                    |
      8086 interpreter                     loader maps + relocates
              |                                    |
              +----------------+-------------------+
                               |
                     kernel/dos.c + fs/fat.c
                     (one DOS, one filesystem)
```

Both worlds share the file handles, the current directory, the search state and
the volumes, because both call the same kernel. Going *down* is
`api->svc->exec16(...)`; going *up* from 16-bit code is `INT 2Fh AX=5544h`
(see `docs/protected-mode.md`).

## 1. Build

```sh
make -C sdk                 # build/HELLO64.EXE and build/HELLO32.EXE
make -C sdk CC64=x86_64-w64-mingw32-gcc CC32=i686-w64-mingw32-gcc
```

On Windows one compiler covers both targets:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File sdk\build_sdk.ps1
```

The flags are the whole trick — a native app is a *freestanding* PE image:

| flag | why |
| --- | --- |
| `-ffreestanding -nostdlib` | no CRT, no PEB, no imports: nothing to resolve at load time |
| `-fno-stack-protector -fno-asynchronous-unwind-tables` | no `__stack_chk_fail`, no `.pdata` dependency |
| `-Wl,--subsystem,10` | EFI application subsystem (what the loader expects) |
| `-Wl,--enable-reloc-section` | image can be loaded at any address; without it only `.reloc`-less fixed-base images work |
| `-Wl,-e,uefidos_main` | entry point is `uefidos_main`, not `main` |
| `-mno-red-zone` (x64) | the kernel runs with interrupts on; a red zone would be clobbered |

`-fno-pic` is *not* required (mingw x64 is position-dependent by default and the
loader applies `DIR64` relocations), but it keeps images smaller.

## 2. Entry point

```c
#include "uefidos/dosapi.h"

int uefidos_main(const uefidos_exports *api, const char *args)
{
    api->svc->write(1, "hello\n", 6, &got);   /* handle 1 == console */
    return 0;                                  /* becomes the exit code */
}
```

* `api` is the service table; `api->pointer_size` tells you if the *kernel* is
  64-bit (8) or 32-bit (4) — useful for shared binaries.
* `args` is the raw command tail after the program name, never NULL.
* the return value is the exit code that `RUN64` prints and that the shell sees.

`#include "uefidos/runtime.h"` adds the tiny freestanding helpers used by the
example: `rt_puts`, `rt_putu`, `rt_putx`, `rt_strlen`.

### Printing

DOS exposes the console as handle 1 (`STDOUT`); `write(1, ...)` goes straight
through the kernel to the console. Emit `\n` only — the platform layer expands it
to CRLF, exactly like the `INT 21h` path.

## 3. Services

Everything mirrors an `INT 21h` function and returns **0 or a DOS error code**
(2 = file not found, 3 = path not found, 4 = too many open files, 5 = access
denied, 0x19 = seek before start …).

| service | `INT 21h` | notes |
| --- | --- | --- |
| `open(path, mode, &h)` | 3Dh | mode 0=read 1=write 2=read/write |
| `create(path, attr, &h)` | 3Ch | truncates/creates |
| `close`, `read`, `write` | 3Eh/3Fh/40h | `*got`/`*put` = bytes transferred |
| `seek(h, off, whence, &pos)` | 42h | whence 0/1/2; `SEEK_END` also returns the size |
| `unlink`, `mkdir`, `rmdir`, `rename` | 41h/39h/3Ah/56h | |
| `chdir`, `getcwd` | 3Bh/47h | |
| `attr_get`, `attr_set` | 43h | DOS attribute byte |
| `find_first(pat, mask, &fi)`, `find_next(&fi)` | 4Eh/4Fh | **one** search state, do not nest |
| `get_drive`, `set_drive` | 19h/0Eh | 0 = `A:` |
| `get_date`, `get_time`, `get_ticks` | 2Ah/2Ch/1Ah | date year is DOS-relative (year-1980) |
| `exec16(prog, args, &code)` | 4Bh | runs a 16-bit `.COM`/`.EXE`, blocks, returns its exit code |
| `mount(drive, img)`, `unmount`, `fs_type` | — | UEFI-DOS extension: floppy/hard-disk images as drives |

The ABI is `sdk/include/uefidos/dosapi.h`; it is a field-for-field copy of
`src/api/dosapi.h`, and the field order **is** the ABI — never reorder it.

## 4. Loading

`RUN64 <file.exe> [args]` (shell builtin) or `native_run_file()`:

1. the kernel reads the image from the current drive (max 64 MB),
2. parses `MZ` → `e_lfanew` → `PE\0\0`, the COFF header and the optional header
   (PE32+ `0x20B` or PE32 `0x10B`),
3. allocates `SizeOfImage` bytes of **executable** memory (`EfiLoaderCode`,
   `PAGE_EXECUTE_READWRITE` on the host),
4. copies headers and sections, zero-fills `.bss`,
5. applies base relocations (`DIR64` for x64, `HIGHLOW` for 32-bit) from
   `.reloc`,
6. calls `entry_point(api, args)`.

Bitness is enforced: a 32-bit image is refused by the 64-bit build and vice
versa (`native loader: image bitness does not match this UEFI-DOS build`) —
32-bit code cannot execute inside a 64-bit address space, so this is a hard rule,
not a limitation of the loader. `HELLO32.EXE` is built for exactly that case and
runs under `BOOTIA32.EFI` (the IA32 QEMU smoke test asserts
`pointer size : 32 bit` from it); the x86_64 build runs `HELLO64.EXE` and prints
the bitness refusal for the 32-bit image, so both paths are covered.

Images stay mapped after exit (a later program may still reference shared state);
the 64 MB cap keeps a stray image from eating the pool.

## 5. Verified behaviour

`build/HELLO64.EXE` (7.2 KB, PE32+ / `pei-x86-64`) run from the shell:

```
C:\> RUN64 HELLO64.EXE
RUN64: loading HELLO64.EXE as a native 64-bit image
=== UEFI-DOS native SDK demo ===
pointer size : 64 bit
API version  : 0100
DOS version  : 4.00
drive        : C:
reading      : HELLO.TXT (15 bytes)
Hello UEFI-DOS!
wrote        : NATIVE.TXT (44 bytes)
found        : HELLO.TXT
               NATIVE.TXT
date/time    : 2026-11-8 16:29:12
Hello from 8086 code!
exec16       : HELLO.COM -> exit 0 (16-bit program ran)
NATIVE-OK
RUN64: exit code 0 -> NATIVE-OK
```

The `Hello from 8086 code!` line is the interpreter printing from a 16-bit `.COM`
that a 64-bit program started — the downcall path, end to end.

## 6. Limitations

* No CRLF translation: emit `\n` and the console expands it.
* No `argv` splitting: `args` is the raw tail; parse it yourself.
* `find_first`/`find_next` share one global state (like DOS), so recursion into a
  directory listing needs the caller to save what it needs first.
* No threads, no signal handling, no `malloc` — allocate from your own buffers or
  use the DOS memory services in the native API table as they grow.
* Zig/Rust: any toolchain that can emit a freestanding PE image with a
  `--subsystem,10`-style header and a `uefidos_main` entry works. `sdk/Makefile`
  shows the flags; there is no language-specific glue yet.
