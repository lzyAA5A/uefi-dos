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
#   make                 # HELLO64.EXE (64-bit) and HELLO32.EXE (32-bit)
#   make hello64         # only the 64-bit one
#   make check           # print the PE headers / machine type
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

TARGETS = $(OUT)/HELLO64.EXE $(OUT)/HELLO32.EXE

all: $(TARGETS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/HELLO64.EXE: examples/hello64.c | $(OUT)
	$(CC64) $(CFLAGS) -m64 $(LDFLAGS) -o $@ $<

$(OUT)/HELLO32.EXE: examples/hello64.c | $(OUT)
	$(CC32) $(CFLAGS) -m32 $(LDFLAGS) -o $@ $<

check: $(TARGETS)
	@for f in $(TARGETS); do echo "== $$f"; file $$f 2>/dev/null || true; \
	  objdump -f $$f | head -n 6; done

clean:
	rm -rf $(OUT)

.PHONY: all check clean
