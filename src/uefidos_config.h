/* UEFI-DOS -- build configuration. */
#ifndef UEFIDOS_CONFIG_H
#define UEFIDOS_CONFIG_H

#define UEFIDOS_NAME       "UEFI-DOS"
#define UEFIDOS_VERSION    "0.9.0"
/* DOS API level.  The kernel implements MS-DOS 4.0 semantics (SHARE modes,
 * FCB calls, FILES=, extended error reporting, AH=6Ch extended open), and
 * INT 21h AH=30h now reports that level: AL=major, AH=minor -> AX=0x0004
 * for DOS 4.00.
 *
 * History: up to 0.8.0 this reported 2.10 (AX=0x0A02) because the Compaq DOS
 * 2.11 SYS.COM rejects any minor above 10 (its gate accepts 0x0136..0x020A
 * only).  Reporting 4.00 makes modern DOS tools take their DOS 4+ paths; the
 * cost is that SYS.COM from that 2.11 disk now refuses to run.  Set
 * UEFIDOS_DOSVER_MAJOR/MINOR/COMPAT_MINOR back to 2/11/10 to get the old
 * behaviour (VER and AH=30h both follow these numbers). */
#define UEFIDOS_DOSVER_MAJOR 4
#define UEFIDOS_DOSVER_MINOR 0
#define UEFIDOS_DOSVER_COMPAT_MINOR 0

/* conventional memory emulated for 16-bit programs */
#define EMU_RAM_SIZE (16u * 1024u * 1024u) /* 16 MiB: 1 MiB real mode + room for 32-bit clients */

/* DOS 4.0/5.0 default is FILES=8 (handles 0..4 are the predefined devices);
 * CONFIG.SYS FILES= can raise the effective limit up to this ceiling. */
#define DOS_HANDLE_MAX 255
#define DOS_FILES_DEFAULT 8
#define DOS_MAX_DRIVES  4

#endif /* UEFIDOS_CONFIG_H */
