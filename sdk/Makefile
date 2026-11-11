# UEFI-DOS native SDK -- build 32/64-bit native apps.
#
# A native app is a freestanding PE image with no CRT and no imports; the
# UEFI-DOS loader maps it, applies relocations and calls uefidos_main() on the
# real CPU.  The only requirements are:
#
#   - a PE target (mingw-w64 compiler, natively on Windows or as a cross
#     compiler on Linux/macOS),
#   - -nostdlib -ffreestanding -fno-stack-protector,
#   - -Wl,--subsystem,10 (EFI application) and -Wl,--enable-reloc-section so the
#     image can be loaded at any address,
#   - -e uefidos_main to name the entry point.
#
# Usage:
#   make               # both examples, both bitnesses
#   make hello         # HELLO64.EXE + HELLO32.EXE
#   make filetool      # FILET64.EXE + FILET32.EXE
#   make check         # print the PE headers / machine type
#   make clean

CC64  ?= x86_64-w64-mingw32-gcc
CC32  ?= i686-w64-mingw32-gcc
# Native Windows (one compiler, -m32 for the 32-bit build)
ifeq ($(OS),Windows_NT)
  ifeq ($(wildcard $(shell command -v $(CC64) 2>/dev/null)),)
    CC64 := gcc
    CC32 := gcc -m32
  endif
endif

OUT   ?= ../build/sdk
CFLAGS = -O2 -std=gnu11 -ffreestanding -Wall -Wextra \
         -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
         -fno-unwind-tables -fno-ident -fno-pic -Iinclude
LDFLAGS = -nostdlib -Wl,--subsystem,10 -Wl,-e,uefidos_main \
          -Wl,--enable-reloc-section -Wl,--no-insert-timestamp

# 8.3 names only: DIR and DOS programs address the binary by its short name.
HELLO64 = $(OUT)/HELLO64.EXE
HELLO32 = $(OUT)/HELLO32.EXE
FILET64 = $(OUT)/FILET64.EXE
FILET32 = $(OUT)/FILET32.EXE

all: hello filetool

hello: $(HELLO64) $(HELLO32)
filetool: $(FILET64) $(FILET32)

$(OUT):
	mkdir -p $(OUT)

$(HELLO64): examples/hello64.c | $(OUT)
	$(CC64) $(CFLAGS) -m64 $(LDFLAGS) -o $@ $<
$(HELLO32): examples/hello64.c | $(OUT)
	$(CC32) $(CFLAGS) -m32 $(LDFLAGS) -o $@ $<
$(FILET64): examples/filetool.c | $(OUT)
	$(CC64) $(CFLAGS) -m64 $(LDFLAGS) -o $@ $<
$(FILET32): examples/filetool.c | $(OUT)
	$(CC32) $(CFLAGS) -m32 $(LDFLAGS) -o $@ $<

check: all
	@for f in $(HELLO64) $(HELLO32) $(FILET64) $(FILET32); do \
	  echo "== $$f"; file $$f 2>/dev/null || true; \
	  objdump -f $$f | head -n 6; done

clean:
	rm -rf $(OUT)

.PHONY: all hello filetool check clean
