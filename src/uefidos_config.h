/* UEFI-DOS -- build configuration. */
#ifndef UEFIDOS_CONFIG_H
#define UEFIDOS_CONFIG_H

#define UEFIDOS_NAME       "UEFI-DOS"
#define UEFIDOS_VERSION    "0.8.0"
/* DOS 2.11 semantics.  INT 21h AH=30h reports COMPAT_MINOR instead: the
 * 2.11-era SYS and several tools reject a minor revision above 10. */
#define UEFIDOS_DOSVER_MAJOR 2
#define UEFIDOS_DOSVER_MINOR 11
#define UEFIDOS_DOSVER_COMPAT_MINOR 10

/* conventional memory emulated for 16-bit programs */
#define EMU_RAM_SIZE (16u * 1024u * 1024u) /* 16 MiB: 1 MiB real mode + room for 32-bit clients */

/* DOS 4.0/5.0 default is FILES=8 (handles 0..4 are the predefined devices);
 * CONFIG.SYS FILES= can raise the effective limit up to this ceiling. */
#define DOS_HANDLE_MAX 255
#define DOS_FILES_DEFAULT 8
#define DOS_MAX_DRIVES  4

#endif /* UEFIDOS_CONFIG_H */
