/*
 * UEFI-DOS -- platform abstraction.
 *
 * Implemented twice: src/uefi/efi_plat.c (real firmware) and
 * src/host/host_plat.c (Windows console + image file, for fast tests).
 */
#ifndef UEFIDOS_PLAT_H
#define UEFIDOS_PLAT_H

#include "uefidos.h"

typedef struct blockdev {
    u32  blk_size;      /* 512 for FAT */
    u64  nblocks;
    int  read_only;     /* 1 = media/volume is write protected */
    int  (*read)(struct blockdev *bd, u64 lba, void *buf, u32 count);
    int  (*write)(struct blockdev *bd, u64 lba, const void *buf, u32 count);
    void *ctx;
} blockdev;

int  plat_init(void *sys_table, void *image_handle);
void plat_putc(char c);
void plat_puts(const char *s);
void plat_putmem(const char *buf, u32 len);
/* serial-console diagnostics: keeps working while the display is a raw
 * framebuffer (ConOut is not readable then) */
void plat_serial_puts(const char *s);
void plat_serial_hex(const char *tag, u64 v);
int  plat_kbd_poll(void);        /* -1 = no key, else CP437 code */
int  plat_kbd_scan(void);        /* last scan code seen (0 if none) */
void plat_shutdown(void);
u64  plat_ticks_ms(void);
void plat_sleep_ms(u32 ms);
void plat_get_date(int *y, int *m, int *d);
void plat_get_time(int *h, int *mi, int *s);
u64  plat_timer_units_per_ms(void); /* 0 = not calibrated yet */
int  plat_abi_selftest(u32 *year_out); /* 0 = ABI/services OK */
int  plat_boot_blockdev(blockdev *bd);  /* block device holding the OS image */
/* Every block device the firmware exposes (USB sticks, extra disks, other
 * partitions), the boot volume first.  Returns how many were filled in, or 0.
 * `labels` receives a short description per device ("BOOT", "USB", "SATA"). */
int  plat_enum_blockdevs(blockdev *out, int max, char labels[][12]);
void *plat_alloc(u32 size);
void plat_free(void *p);
/* Executable memory for native (PE) images: the returned block must be
 * readable, writable and executable.  plat_free_code takes the same size. */
void *plat_alloc_code(u32 size);
void plat_free_code(void *p, u32 size);
void plat_banner(const char *s);        /* serial/mirror output hook */
void plat_video_show(const u8 *vram);   /* render emulated 80x25 text screen */
/* Graphics mode support (INT 10h mode 13h / VESA linear framebuffer).
 * plat_video_gfx_begin is called when a graphics mode is selected (w x h,
 * 8 bits per pixel); frames then arrive through plat_video_gfx_show.
 * plat_video_text switches back to the 80x25 text mirror. */
int  plat_video_gfx_begin(int w, int h);
void plat_video_gfx_show(const u8 *fb, int w, int h, const u8 *dac);
void plat_video_text(void);

extern int g_plat_shutdown_requested;

#endif
