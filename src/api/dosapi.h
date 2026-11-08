/*
 * UEFI-DOS -- native 32/64-bit DOS services API.
 *
 * This is the "same semantics, wider registers" view of INT 21h: every call
 * performs the same action as its INT 21h counterpart and reports the same
 * DOS error codes (0 = success, 2/3/5/0x0B/0x27/... see kernel/dos.c
 * dos_error_code()), but arguments are native UTF-8/ASCII pointers and
 * handles are native u16 values.  Code compiled for either UEFI class
 * (BOOTX64.EFI, BOOTIA32.EFI) links the same g_dosapi table.
 *
 * 16-bit bridge:
 *   - exec16() runs a 16-bit .COM/.EXE synchronously (native -> 16-bit
 *     "downcall", the V86-style fallback; the caller resumes afterwards),
 *   - INT 2Fh with AX=5544h lets the emulated 16-bit world call back into
 *     the native 32/64-bit side (16-bit -> native "upcall").
 *
 * External EFI applications can obtain the table through the UEFI
 * configuration table under UEFIDOS_API_GUID (see dosapi_exports).
 */
#ifndef UEFIDOS_DOSAPI_H
#define UEFIDOS_DOSAPI_H

#include "uefidos.h"

#define DOSAPI_VERSION 0x0100u      /* 1.00 */

/* UEFI configuration table GUID -> dosapi_exports */
#define UEFIDOS_API_GUID { 0x55454449u, 0x2D44, 0x4F53, \
                           { 0x41, 0x50, 0x49, 0x00, 0x00, 0x00, 0x00, 0x01 } }

/* INT 21h AH=4Eh/4Fh result record. */
typedef struct dosapi_findinfo {
    char name[13];      /* 8.3 name, NUL padded */
    u8   attr;          /* directory-entry attributes */
    u32  size;          /* file size in bytes */
    u16  time;          /* packed DOS time (HHHHHMM MMMSSSSS) */
    u16  date;          /* packed DOS date (YYYYYYYM MMMDDDDD) */
} dosapi_findinfo;

typedef struct dosapi_services {
    u32  version;                               /* DOSAPI_VERSION */

    /* INT 21h AH=30h -- (minor << 8) | major, e.g. 0x0A02 */
    u16  (*get_version)(void);
    /* AH=3Dh / AH=3Ch: mode 0=read 1=write 2=read/write; handle out */
    u16  (*open)(const char *path, u16 mode, u16 *handle);
    u16  (*create)(const char *path, u8 attr, u16 *handle);
    /* AH=3Eh / AH=3Fh / AH=40h / AH=42h (whence 0=SET 1=CUR 2=END) */
    u16  (*close)(u16 handle);
    u16  (*read)(u16 handle, void *buf, u32 len, u32 *got);
    u16  (*write)(u16 handle, const void *buf, u32 len, u32 *put);
    u16  (*seek)(u16 handle, i32 off, u8 whence, u32 *newpos);
    /* AH=41h / AH=39h / AH=3Ah / AH=43h */
    u16  (*unlink)(const char *path);
    u16  (*mkdir)(const char *path);
    u16  (*rmdir)(const char *path);
    u16  (*rename)(const char *oldp, const char *newp);
    u16  (*attr_get)(const char *path, u8 *attr);
    u16  (*attr_set)(const char *path, u8 attr);
    /* AH=3Bh / AH=47h */
    u16  (*chdir)(const char *path);
    u16  (*getcwd)(char *buf, u32 max);
    /* AH=19h (0=A:) / AH=0Eh (returns number of logical drives) */
    u8   (*get_drive)(void);
    u16  (*set_drive)(u8 drive);
    /* AH=4Eh/4Fh -- one find state per process, do not nest */
    u16  (*find_first)(const char *pattern, u8 attr_mask, dosapi_findinfo *out);
    u16  (*find_next)(dosapi_findinfo *out);
    /* AH=2Ah (year_dos = year-1980) / AH=2Ch / INTERRUPT 1Ah AH=00h */
    void (*get_date)(u8 *year_dos, u8 *month, u8 *day);
    void (*get_time)(u8 *hour, u8 *minute, u8 *second, u8 *centisecond);
    u32  (*get_ticks)(void);                    /* 55 ms BIOS ticks */

    /* INT 21h AH=4Bh: run a 16-bit .COM/.EXE synchronously (downcall).
     * Blocks until the child exits; 0 on success, 2 not found, 0x0B bad. */
    u16  (*exec16)(const char *prog, const char *args, i16 *exit_code);

    /* image drives (UEFI-DOS extension): drive 0 = A: */
    u16  (*mount)(u8 drive, const char *img_path);
    u16  (*unmount)(u8 drive);
    int  (*fs_type)(u8 drive);                  /* 12/16/32, 0 = absent */
} dosapi_services;

/* What an EFI application finds under UEFIDOS_API_GUID. */
typedef struct dosapi_exports {
    u32  version;           /* DOSAPI_VERSION */
    u32  pointer_size;      /* sizeof(void *) of the provider (4 or 8) */
    const dosapi_services *svc;
} dosapi_exports;

extern const dosapi_services g_dosapi;
extern const dosapi_exports  g_dosapi_exports;

#endif
