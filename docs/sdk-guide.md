# UEFI-DOS native SDK -- developer guide

Write a 32- or 64-bit DOS program that runs at full speed on the real CPU
behind a UEFI-DOS shell. No emulator, no CRT, no imports: you get a flat
address space, a DOS-shaped service table and the ability to call back into
16-bit DOS code.

---

## 1. What a native program is

```
C:\> RUN64 HELLO64.EXE arg1
```

`RUN64` reads the file, checks its PE header, maps it (headers + sections),
applies base relocations, and calls

```c
int uefidos_main(const uefidos_exports *api, const char *args);
```

on the real CPU. `api` is the DOS service table; `args` is the raw command tail
(everything after the program name, may be `""`, never `NULL`). The return value
becomes the program's exit code, which the shell prints:

```
RUN64: loading HELLO64.EXE as a native 64-bit image
...
RUN64: exit code 0 -> NATIVE-OK
```

Bitness is not emulated: a 64-bit kernel runs 64-bit images (`0x8664`), the
IA32 kernel runs 32-bit images (`0x14C`), and a mismatch is refused with a clear
message instead of a crash. `api->pointer_size` tells you which you are.

## 2. Hello world

```c
#include "uefidos/dosapi.h"
#include "uefidos/runtime.h"

int uefidos_main(const uefidos_exports *api, const char *args)
{
    uefidos_init(api, args);
    rt_puts("hello from native mode\n");
    rt_puts("command tail: ");
    rt_puts(g_args);
    rt_putc('\n');
    return 0;
}
```

Build (Windows, MinGW-w64):

```powershell
gcc -m64 -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector `
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -fno-pic `
    -Isdk/include -nostdlib -Wl,--subsystem,10 -Wl,-e,uefidos_main `
    -Wl,--enable-reloc-section -Wl,--no-insert-timestamp `
    -o HELLO64.EXE hello.c
```

or just use the helper makefiles:

```powershell
powershell -File sdk\build_sdk.ps1      # Windows: build\sdk\*.EXE
make -C sdk                             # Linux/macOS: needs mingw-w64 cross gcc
make -C sdk CC64=x86_64-w64-mingw32-gcc CC32=i686-w64-mingw32-gcc
```

Then put the image together and run it:

```powershell
python tools\make_image.py build\my.img --size-mb 64 --boot build\BOOTX64.EFI `
  --file build\sdk\HELLO64.EXE=HELLO64.EXE
build\hosttest.exe --shell build\my.img "RUN64 HELLO64.EXE"
```

`build.ps1` already ships `HELLO64/HELLO32/FILET64/FILET32` in `UEFIDOS.img`, so
`RUN64 FILET64.EXE -l *.TXT` works out of the box.

### Why those flags

| flag | why |
| --- | --- |
| `-ffreestanding -nostdlib` | there is no CRT to link against |
| `-fno-stack-protector`, `-fno-asynchronous-unwind-tables`, `-fno-unwind-tables` | otherwise gcc emits calls to `__stack_chk_fail` / unwind tables that do not exist |
| `-fno-builtin` | stops gcc from turning your `rt_puts`-style loops into `memcpy`/`strlen` calls |
| `-fno-pic` | the loader relocates the image itself; no GOT/PLT |
| `-Wl,--subsystem,10` | EFI application subsystem, so the PE header looks like a loader-friendly image |
| `-Wl,--enable-reloc-section` | **required**: the image must be relocatable, the loader depends on `.reloc` |
| `-Wl,-e,uefidos_main` | the entry point name the loader looks for |
| `-Wl,--no-insert-timestamp` | reproducible builds |

`-m32` needs `-mno-stack-arg-probe` on some toolchains; the SDK makefile adds
what is needed for both bitnesses.

## 3. The service table

Everything is `INT 21h` semantics with 32-bit-clean prototypes. Return value is
`0` on success, otherwise a **DOS error code** (`2` not found, `3` path not
found, `5` access denied, `4` too many open files, `6` bad handle, ...).

```c
typedef struct dosapi_services {
    u32  version;
    u16  (*get_version)(void);                      /* minor<<8 | major */
    u16  (*open)(const char *path, u16 mode, u16 *handle);
    u16  (*create)(const char *path, u8 attr, u16 *handle);
    u16  (*close)(u16 handle);
    u16  (*read)(u16 handle, void *buf, u32 len, u32 *got);
    u16  (*write)(u16 handle, const void *buf, u32 len, u32 *put);
    u16  (*seek)(u16 handle, i32 off, u8 whence, u32 *newpos);
    u16  (*unlink)(const char *path);
    u16  (*mkdir)(const char *path);
    u16  (*rmdir)(const char *path);
    u16  (*rename)(const char *oldp, const char *newp);
    u16  (*attr_get)(const char *path, u8 *attr);
    u16  (*attr_set)(const char *path, u8 attr);
    u16  (*chdir)(const char *path);
    u16  (*getcwd)(char *buf, u32 max);
    u8   (*get_drive)(void);
    u16  (*set_drive)(u8 drive);
    u16  (*find_first)(const char *pattern, u8 attr_mask, dosapi_findinfo *out);
    u16  (*find_next)(dosapi_findinfo *out);
    u16  (*longname)(const char *path, char *out, u32 max);   /* API 1.01 */
    void (*get_date)(u8 *year_dos, u8 *month, u8 *day);
    void (*get_time)(u8 *hour, u8 *minute, u8 *second, u8 *centisecond);
    u32  (*get_ticks)(void);
    u16  (*exec16)(const char *prog, const char *args, i16 *exit_code);
    u16  (*mount)(u8 drive, const char *img_path);
    u16  (*unmount)(u8 drive);
    int  (*fs_type)(u8 drive);
} dosapi_services;
```

Notes that save time:

* **Offsets and sizes are `u32`.** No 64 KB segment limit, no `CX:DX` games.
  `seek(h, 0, DOS_SEEK_END, &size)` yields the file size and leaves the position
  at EOF; use `DOS_SEEK_SET` afterwards to rewind.
* **Handle 1 is the console.** There is no "print" service; write to handle 1,
  exactly like `AH=40h` with `BX=1`.
* **`find_first`/`find_next` keep one search state per process.** Iterate; do
  not nest searches, and copy `dosapi_findinfo` fields you need (the long name
  lives in `long_name[DOSAPI_LFN_MAX]`, `""` when the entry has no LFN).
* **Paths are DOS paths**: `C:\SUB\FILE.TXT` or relative; `/` also works, case
  is insensitive. 8.3 names always work; long names work for create/open/rename
  and show up in `long_name`.
* **`get_version`** returns `minor<<8 | major` (DOS 4.00 is `0x0400`).

## 4. Runtime helpers (`sdk/include/uefidos/runtime.h`)

Header-only, `static`, free to ignore. Everything goes through `g_api`, which
`uefidos_init(api, args)` sets.

| helper | purpose |
| --- | --- |
| `uefidos_init(api, args)` | store the API pointer and the command tail (`g_args`) |
| `rt_putc/rt_puts/rt_putu/rt_putx/rt_putu_pad` | console output, hex, right-aligned numbers |
| `rt_strlen/rt_streq/rt_strncmp/rt_strcpy/rt_memcpy/rt_memset` | the minimal string/memory set |
| `rt_read_file(path, buf, max, &got)` | whole-file read |
| `rt_write_file(path, buf, len)` | create/truncate and write |
| `rt_file_size(path)` | size, `0` if missing |
| `rt_list(pattern)` | print one line per match: 8.3 name, size, long name |
| `rt_exec16(prog, args, &code)` | run a 16-bit `.COM`/`.EXE` and get its exit code |

They are `static`, so including the header costs nothing for what you do not
call.

## 5. Memory model

* Your image is mapped into memory the kernel owns, with relocations applied;
  any address the compiler computes is valid for the whole image.
* There is **no `malloc`**. Use static/global buffers, or the stack (the UEFI
  boot-services stack, generous but finite -- keep big buffers `static`).
* Sections are mapped with their `SizeOfImage` footprint; the image base is
  whatever the loader chose, so **never** assume a base address.
* No paging, no ring transitions: your code runs at the same privilege as the
  kernel shell. That is what makes calls into the service table cheap.

## 6. The two-way bridge

**Native -> 16-bit (downcall).** `exec16` runs a real DOS program through the
8086 interpreter, synchronously, in its own PSP:

```c
i16 code = 0;
if (rt_exec16("HELLO.COM", "", &code) == 0) { /* child did not fail to start */ }
```

The child gets its own PSP, DTA, environment and file table; your handles and
current directory survive. The child's exit code lands in `code`.

**16-bit -> native (upcall).** A 16-bit program probes the DOS API with
`INT 2Fh, AX=5544h`:

| call | result |
| --- | --- |
| `DX=1` | `AX=DOSAPI_VERSION`, `BX=sizeof(void*)` of the provider, `CX=0` |
| `DX=2` | prints `UEFI-DOS native API vX.YY`, returns length in `AX` |
| `DX=3` | RTC time bytes into `ES:BX` |

That is how a real-mode program discovers that a native API exists (see
`UPCALL.COM` in the image). Stub `F000:FFF0` is the marker vector for services
the shell intercepts; `INT 21h AH=35h` hands out the real vector.

## 7. Debugging

* `RUN64` prints the load result, the exit code and, on failure, why
  (`native loader: image bitness does not match ...`, `out of memory`, ...).
* `INFO.COM` prints the DOS version, the drive, the cwd and the native API
  version/pointer size -- handy to confirm you are where you think you are.
* The **host harness** runs the same kernel on Windows:
  `build\hosttest.exe --shell build\UEFIDOS.img "RUN64 FILET64.EXE -l *.TXT"`.
  Host-only knobs: `UEFIDOS_TRACE_INTS=1`, `UEFIDOS_TRACE_STEPS=n`,
  `UEFIDOS_TRACE_READ=<handle>`, `UEFIDOS_MAX_STEPS`, `UEFIDOS_HARD_STEPS`,
  `UEFIDOS_DUMP_VRAM`, `UEFIDOS_KBD=...`.
* Run your program twice in a row: creating a file that already exists exercises
  `AH=3Ch` truncation, the classic place where a DOS clone leaves a file whose
  directory entry points at a freed cluster.

## 8. Sample: FILETOOL

`sdk/examples/filetool.c` is the reference "real work" program -- it is shipped
in the boot image as `FILET64.EXE` / `FILET32.EXE`:

```
C:\> RUN64 FILET64.EXE -l *.TXT
FILETOOL (native 64-bit, API 0101)
README.TXT        1228
HELLO.TXT           16
FILETOOL-DONE
RUN64: exit code 0 -> NATIVE-OK

C:\> RUN64 FILET64.EXE -w NATBIG.TXT 3000
wrote 3000 bytes to NATBIG.TXT
read back 3000 bytes, mismatches: 0
FILEIO-OK

C:\> RUN64 FILET64.EXE -d
drive  fs      status
C:     FAT32    current

C:\> RUN64 FILET64.EXE -x HELLO.COM
Hello from 8086 code!
child exit code: 0  -> EXEC16-OK
```

Options: (none) list the directory, `-l PATTERN`, `-r FILE` hex dump,
`-s FILE` size + short/long name, `-w FILE [SIZE]` write and verify a pattern
file, `-d` drives and file-system types, `-x PROG` downcall.

## 9. Limitations to plan around

* **No console input yet.** There is no read service, and the firmware keyboard
  is owned by the shell, so a native program cannot wait for a keystroke. Work
  around it by `exec16`-ing a 16-bit program for interactive parts (that is what
  the API is for). A native `get_key` service is on the roadmap.
* **No threads, no interrupts, no timers** beyond `get_ticks`.
* **No floating point ABI guarantees** -- x87/SSE work (the CPU is real), but
  the kernel does not initialise anything for you.
* **No dynamic imports**: everything you need comes from the service table or
  your own code.
* **`exec16` is synchronous** and reuses one interpreter slot per nesting level
  (depth limit 7). Do not hold file handles open across a downcall if the child
  needs many of them: they share the DOS handle table.
* **Long names are VFAT** (255 chars on disk, 64 in the API); name lookups
  still need a legal 8.3 alias, which the kernel synthesises (`LONGNA~1.TXT`).

## 10. Where to look in the source

| file | what |
| --- | --- |
| `sdk/include/uefidos/dosapi.h` | the ABI: types, constants, service table, exports |
| `sdk/include/uefidos/runtime.h` | the header-only helpers |
| `sdk/examples/hello64.c` | minimal tour: printing, files, clock, find, `exec16` |
| `sdk/examples/filetool.c` | real utility: 32-bit file I/O, LFN, drives, downcall |
| `src/api/native.c` | the PE loader (mapping, relocations, bitness check) |
| `src/api/dosapi.c` | the service table implementation over the DOS kernel |
| `src/emu/int2f.c` | the 16-bit upcall entry (`INT 2Fh AX=5544h`) |
| `src/shell/shell.c` (`cmd_run64`) | the `RUN64` command |

## 11. Roadmap

Near-term, in the order they unblock things: a native keyboard/console-input
service, `mount`/`unmount` in the samples (the service already exists), a
native `exec32` chain (run a second native image and get its exit code), and
`get/set drive` in `rt_*` helpers.
