/*
 * UEFI-DOS -- native (PE) application loader, internal interface.
 *
 * Public SDK headers live in sdk/include/uefidos/.  These two entry points are
 * what the shell uses; the loader itself is src/api/native.c.
 */
#ifndef UEFIDOS_NATIVE_H
#define UEFIDOS_NATIVE_H

#include "uefidos.h"

/* Map a PE image held in memory, apply its base relocations and call
 * `int uefidos_main(const uefidos_exports *, const char *args)` on the real
 * CPU.  Returns 0 on success (with *exit_code set), 1 = not a PE image,
 * 2 = unsupported machine/optional header or unusable image, 3 = out of
 * executable memory. */
int native_run_image(const u8 *img, u32 size, const char *args, int *exit_code);

/* Open `dos_path` on the current drive, then native_run_image() it.
 * Returns a FAT_* status code. */
int native_run_file(const char *dos_path, const char *args, int *exit_code);

#endif
