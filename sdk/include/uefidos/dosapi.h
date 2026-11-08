/*
 * UEFI-DOS native SDK -- public API for 32/64-bit native programs.
 *
 * A native UEFI-DOS application is a freestanding PE image (no CRT, no OS
 * imports) whose entry point is
 *
 *     int uefidos_main(const uefidos_exports *api, const char *args);
 *
 * The kernel maps the image, applies its base relocations and calls that entry
 * point directly on the real CPU -- no 8086 interpreter, no mode switch.  Every
 * service below performs the same action as the corresponding INT 21h call and
 * reports the same DOS error codes (0 = success, 2 = file not found,
 * 3 = path not found, 4 = too many open files, 5 = access denied,
 * 6 = invalid handle, 0x0B = invalid format, 0x19 = seek before start ...), so
 * DOS knowledge carries over; only the calling convention changes.
 *
 * This header is an ABI copy of src/api/dosapi.h -- the field order is the ABI.
 * Do not reorder or insert fields.
 */
#ifndef UEFIDOS_SDK_DOSAPI_H
#define UEFIDOS_SDK_DOSAPI_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- fixed-width types (freestanding: no <stdint.h> needed) ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed int         i32;
typedef signed short       i16;
typedef unsigned long long u64;

#define DOSAPI_VERSION 0x0100u      /* 1.00 */

/* UEFI configuration-table GUID -> dosapi_exports
 * 55454449-2D44-4F53-4150-4900-000000000001 */
#define UEFIDOS_API_GUID_HI  0x55454449u
#define UEFIDOS_API_GUID_LO  0x2D444F5341504900ull

/* ---- attribute / seek constants (identical values to DOS) ---- */
#define DOS_ATTR_READONLY 0x01
#define DOS_ATTR_HIDDEN   0x02
#define DOS_ATTR_SYSTEM   0x04
#define DOS_ATTR_VOLUME   0x08
#define DOS_ATTR_DIR      0x10
#define DOS_ATTR_ARCHIVE  0x20

#define DOS_OPEN_READ  0
#define DOS_OPEN_WRITE 1
#define DOS_OPEN_RDWR  2

#define DOS_SEEK_SET  0
#define DOS_SEEK_CUR  1
#define DOS_SEEK_END  2   /* also returns the file size in *newpos */

/* INT 21h AH=4Eh/4Fh result record. */
typedef struct dosapi_findinfo {
    char name[13];      /* 8.3 name, NUL padded */
    u8   attr;          /* directory-entry attributes */
    u32  size;          /* file size in bytes */
    u16  time;          /* packed DOS time */
    u16  date;          /* packed DOS date */
} dosapi_findinfo;

typedef struct dosapi_services {
    u32  version;                               /* DOSAPI_VERSION */

    /* INT 21h AH=30h -- (minor << 8) | major, e.g. 0x0A02 for 2.10 */
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

/* What a native app receives as its first argument (and what an EFI
 * application finds under UEFIDOS_API_GUID in the configuration table). */
typedef struct uefidos_exports {
    u32  version;           /* DOSAPI_VERSION */
    u32  pointer_size;      /* sizeof(void *) of the provider (4 or 8) */
    const dosapi_services *svc;
} uefidos_exports;

/* Entry point every native app must provide.  `args` is the command tail the
 * user typed after the program name (may be an empty string, never NULL).
 * The return value becomes the program's exit code. */
int uefidos_main(const uefidos_exports *api, const char *args);

#ifdef __cplusplus
}
#endif

#endif
