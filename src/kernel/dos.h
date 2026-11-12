/*
 * UEFI-DOS -- DOS kernel layer (drives, paths, file handles, search).
 * Independent of the 8086 emulator: the shell calls it natively, INT 21h
 * emulation calls it too.
 */
#ifndef UEFIDOS_DOS_H
#define UEFIDOS_DOS_H

#include "uefidos.h"
#include "fs/fat.h"

#define DOS_MAX_PATH 128
/* DOS_HANDLE_MAX / DOS_MAX_DRIVES live in uefidos_config.h (included through
 * uefidos.h); keep these fallbacks for translation units that include dos.h
 * before the config. */
#ifndef DOS_HANDLE_MAX
#define DOS_HANDLE_MAX 20
#endif
#ifndef DOS_MAX_DRIVES
#define DOS_MAX_DRIVES 4
#endif

/* backing store for an image file mounted as a drive (e.g. floppy images) */
typedef struct dos_imgbd {
    fat_vol *vol;      /* volume holding the image file */
    u32 cluster;       /* image file's first cluster */
    u32 size;
} dos_imgbd;

typedef struct dos_drive {
    blockdev bd;          /* keeps the block device alive for the volume */
    fat_vol vol;
    dos_imgbd img;        /* valid when this drive is a mounted image file */
    int mounted;
    char label[13];
    u32 cwd_cluster;      /* first cluster of current directory */
    char cwd[DOS_MAX_PATH]; /* "" = root, "SUB\\DIR" */
} dos_drive;

/* DOS 4.0 SHARE-style compatibility modes (INT 21h AH=3Dh bits 4..6). */
#define DOS_SHARE_COMPAT     0   /* compat: exclusive towards other PSPs */
#define DOS_SHARE_DENY_RW    1
#define DOS_SHARE_DENY_WRITE 2
#define DOS_SHARE_DENY_READ  3
#define DOS_SHARE_DENY_NONE  4

/* per-program DOS version overrides (CONFIG.SYS SETVER= / shell SETVER) */
#ifndef DOS_SETVER_MAX
#define DOS_SETVER_MAX 12
#endif

#define DOS_LOCK_MAX 8

typedef struct dos_lock {
    u32 start;
    u32 len;
} dos_lock;

typedef struct dos_handle {
    int used;
    int console;          /* 0 = file, 1 = CON, 2 = PRN/AUX */
    int drive;
    u32 parent_cluster;
    u8  name11[11];
    u8  attr;
    u32 cluster;
    u32 size;
    u32 pos;
    /* directory timestamp, so INT 21h AH=57h can hand it back (Windows 1.x
     * asks for it right after opening its overlay and reads nothing when the
     * answer looks stale) */
    u16 time;
    u16 date;
    /* DOS 4.0 SHARE support: what the opener granted, what it asked for, and
     * the byte ranges it locked (INT 21h AH=5Ch). */
    u16 psp;              /* owning PSP (compat mode is per-PSP) */
    u8  share_mode;       /* DOS_SHARE_* */
    u8  open_mode;        /* 0 = read, 1 = write, 2 = read/write */
    u8  lock_count;
    dos_lock locks[DOS_LOCK_MAX];
} dos_handle;

typedef struct dos_find {
    int drive;
    u32 dir_cluster;
    u8  pat[11];
    u8  attr_mask;
    u32 index;            /* matches already returned */
} dos_find;

struct dos_state {
    dos_drive drives[DOS_MAX_DRIVES];
    int cur_drive;            /* 0 = A: */
    u16 dta_seg, dta_off;
    u16 psp_seg;
    u16 last_error;           /* DOS error code of last failed call */
    int last_error_valid;     /* nonzero after a failed INT 21h call */
    dos_handle handles[DOS_HANDLE_MAX];
    int  files_limit;         /* effective FILES= (handles 0..files_limit-1) */
    int  fcb_limit;           /* FCBS= first value */
    int  last_drive;          /* LASTDRIVE= (0-based), clamped to DOS_MAX_DRIVES-1 */
    /* SETVER-style per-program DOS version overrides.  Windows 1.x refuses to
     * start on "DOS 4.00" while a few DOS 2.11 tools want the old quirk, so the
     * reported version is settable per program instead of globally. */
    struct { char prog[13]; u8 major, minor; } ver_fix[DOS_SETVER_MAX];
    int  ver_fix_count;
    char path_env[DOS_MAX_PATH];
    char prompt[48];
    /* DOS memory arena (INT 21h AH=48h/49h/4Ah).  The loaded program owns
     * [psp_seg, mem_top); mem_base is the segment of the first MCB, i.e. the
     * free block that starts right after the program image. */
    u16 mem_base;
    u16 mem_top;
};

extern struct dos_state g_dos;

void dos_init(void);
int  dos_mount_boot(void);    /* mount the volume the OS booted from as C: */
int  dos_mount_all(void);     /* C: = boot volume, then D:/E:/... for every other device */
int  dos_mount_image_drive(int letter, const char *path); /* mount image file as A:/B: */
int  dos_unmount_drive(int letter);

int  dos_error_code(int fat_rc);

/* paths: returns selected drive index via *drive, absolute path via abs */
int  dos_parse_path(const char *path, int *drive, char *abs, int absmax);

/* directory ops */
int  dos_chdir(const char *path);
int  dos_mkdir(const char *path);
int  dos_rmdir(const char *path);
int  dos_unlink(const char *path);
int  dos_rename(const char *oldp, const char *newp);

/* file ops */
int  dos_creat(const char *path, int *handle);
int  dos_creat_ex(const char *path, int attr, int share, int open_mode, int *handle);
int  dos_open(const char *path, int mode, int *handle);
int  dos_open_ex(const char *path, int open_mode, int share, int *handle);
int  dos_close(int handle);
int  dos_read(int handle, void *buf, u32 len, u32 *got);
int  dos_write(int handle, const void *buf, u32 len, u32 *put);
int  dos_lseek(int handle, u32 pos);
int  dos_attr_get(const char *path, u8 *attr);
int  dos_attr_set(const char *path, u8 attr);

/* listing */
int  dos_find_first(const char *pattern, u8 attr_mask, dos_find *f,
                    char name_out[13], u8 *attr_out, u32 *size_out,
                    u16 *time_out, u16 *date_out);
int  dos_find_next(dos_find *f, char name_out[13], u8 *attr_out, u32 *size_out,
                   u16 *time_out, u16 *date_out);
/* same walk, but also hands back the VFAT long name ("" when the entry has only
 * an 8.3 name) -- the "old DOS sees 8.3, new code sees LFN" split */
int  dos_find_first_ex(const char *pattern, u8 attr_mask, dos_find *f,
                       char name_out[13], u8 *attr_out, u32 *size_out,
                       u16 *time_out, u16 *date_out, char *long_out, int long_max);
int  dos_find_next_ex(dos_find *f, char name_out[13], u8 *attr_out, u32 *size_out,
                      u16 *time_out, u16 *date_out, char *long_out, int long_max);

/* long name of an existing file; empty when it only has an 8.3 name */
int  dos_longname(const char *path, char *out, int max);
void dos_get_cwd(char *out, int max);      /* "C:\\SUB" or "C:" at root */
void dos_get_prompt(char *out, int max);
int  dos_current_drive(void);
int  dos_set_drive(int drive);              /* INT 21h AH=0Eh */
int  dos_fs_type(int drive);

/* helpers shared with shell / emulator */
void dos_fat_name(const char *name, u8 out11[11], int *ok);
void dos_dirname_to_dosname(const u8 name11[11], char out[13]);
u32  dos_cluster_lba(fat_vol *v, u32 cl);
int  dos_read_drive(fat_vol *v, u32 cl, u32 size, u32 off, void *buf, u32 len, u32 *got);

/* CONFIG.SYS (DOS 4.0 subset: FILES=, FCBS=, LASTDRIVE=, others ignored) */
void dos_read_config_sys(void);
/* per-program DOS version overrides (SETVER): returns 1 when `path` matches */
int  dos_setver_add(const char *prog, int major, int minor);
int  dos_setver_del(const char *prog);
int  dos_setver_lookup(const char *path, int *major, int *minor);

/* DOS 4.0 SHARE byte-range locks (INT 21h AH=5Ch) */
int  dos_lock_region(int handle, u32 off, u32 len);
int  dos_unlock_region(int handle, u32 off, u32 len);

#endif
