/*
 * UEFI-DOS -- FAT filesystem driver.
 *
 * Supports FAT32 read/write and FAT12/FAT16 read (the latter so DOS floppy
 * images can be mounted read-only, e.g. MS-DOS 2.11 media).
 */
#ifndef UEFIDOS_FAT_H
#define UEFIDOS_FAT_H

#include "uefidos.h"
#include "plat.h"

#define FAT_SEC 512

/* result codes -- mapped to DOS error codes by the kernel layer */
enum fat_err {
    FAT_OK = 0,
    FAT_ENOENT = 1,   /* entry not found */
    FAT_ENOPATH = 2,  /* path component missing / not a dir */
    FAT_EACCES = 3,   /* access denied (read-only volume etc.) */
    FAT_EEXIST = 4,
    FAT_ENOSPC = 5,
    FAT_EIO = 6,
    FAT_EINVAL = 7,
    FAT_ENOTDIR = 8,
    FAT_EISDIR = 9,
    FAT_EDIRFULL = 10,
    FAT_EROFS = 11,
    FAT_ECORRUPT = 12,
    FAT_ENOTEMPTY = 13,
    FAT_EMFILE = 14,   /* too many open files (DOS error 4) */
};

typedef struct fat_vol {
    blockdev *dev;
    u32  part_lba;
    u16  bytes_per_sector;
    u8   spc;                 /* sectors per cluster */
    u16  reserved;
    u8   nfats;
    u16  root_entries;
    u32  total_sectors;
    u32  fat_sectors;
    u32  fat_lba;             /* first FAT, absolute */
    u32  root_lba;            /* fixed root region (FAT12/16 only) */
    u32  data_lba;            /* first data sector (cluster 2) */
    u32  cluster_count;
    u32  root_cluster;        /* FAT32 root */
    u8   fs_type;             /* 12, 16, 32 */
    u8   read_only;
    /* FAT sector cache: a chain walk touches a handful of consecutive FAT
     * sectors, so a single slot thrashes and turns every step into disk I/O. */
#define FAT_CACHE_SLOTS 8
    u32  cache_lba[FAT_CACHE_SLOTS];
    u8   cache[FAT_CACHE_SLOTS][FAT_SEC];
    u8   cache_next;
    /* Allocation cursor: DOS 4.0-era FAT16 volumes have up to 65524 clusters;
     * a linear first-fit scan from cluster 2 for every allocation is O(n^2)
     * over a whole-file copy, so remember where the last allocation landed and
     * resume from there (wrapping once). */
    u32  alloc_hint;
    u32  free_count;
} fat_vol;

/* one directory entry (raw bytes, accessors below) */
typedef struct fat_dirent {
    u8  raw[32];
} fat_dirent;

static inline u32 fat_entry_cluster(const fat_dirent *d)
{
    return ((u32)d->raw[26] | ((u32)d->raw[27] << 8) |
            ((u32)d->raw[20] << 16) | ((u32)d->raw[21] << 24)) & 0x0FFFFFFF;
}
static inline void fat_entry_set_cluster(fat_dirent *d, u32 cl)
{
    d->raw[26] = (u8)cl; d->raw[27] = (u8)(cl >> 8);
    d->raw[20] = (u8)(cl >> 16); d->raw[21] = (u8)(cl >> 24);
}
static inline u32 fat_entry_size(const fat_dirent *d)
{
    return (u32)d->raw[28] | ((u32)d->raw[29] << 8) |
           ((u32)d->raw[30] << 16) | ((u32)d->raw[31] << 24);
}
static inline void fat_entry_set_size(fat_dirent *d, u32 sz)
{
    d->raw[28] = (u8)sz; d->raw[29] = (u8)(sz >> 8);
    d->raw[30] = (u8)(sz >> 16); d->raw[31] = (u8)(sz >> 24);
}
static inline u8 fat_entry_attr(const fat_dirent *d) { return d->raw[11]; }

/* scan callback: return nonzero to stop scanning */
typedef int (*fat_scan_cb)(fat_vol *v, const fat_dirent *d, void *arg);

int  fat_mount(fat_vol *v, blockdev *dev, u32 part_lba, int read_only);
int  fat_unmount(fat_vol *v);

int  fat_dir_scan(fat_vol *v, u32 start_cluster, fat_scan_cb cb, void *arg);
int  fat_find(fat_vol *v, u32 dir_cluster, const u8 name11[11],
              fat_dirent *out, u32 *out_cluster, u32 *out_ent_lba, u32 *out_ent_idx);
/* find a free entry slot; creates a new directory cluster when needed */
int  fat_find_free(fat_vol *v, u32 dir_cluster, u32 *out_lba, u32 *out_idx,
                   u32 *out_new_cluster);
int  fat_read_chain_byte(fat_vol *v, u32 start, u32 size, u32 offset, void *buf, u32 len,
                         u32 *io_bytes);
int  fat_write_chain(fat_vol *v, u32 *first_cluster, u32 *size, u32 offset,
                     const void *buf, u32 len);   /* grow as needed */
int  fat_free_chain(fat_vol *v, u32 first);
int  fat_get_cluster_lba(fat_vol *v, u32 cl, u32 *lba);
int  fat_load_fat_sector(fat_vol *v, u32 lba);
void fat_cache_invalidate(fat_vol *v);
int  fat_next_cluster(fat_vol *v, u32 cl, u32 *next);   /* next data cluster or END */

/* directory entry creation / removal */
int  fat_make_entry(fat_dirent *d, const char *name83_11, u8 attr, u32 cluster, u32 size);
int  fat_write_entry_at(fat_vol *v, u32 lba, u32 idx, const fat_dirent *d);
int  fat_add_entry(fat_vol *v, u32 dir_cluster, const u8 name11[11], u8 attr, u32 cluster, u32 size);
int  fat_remove_entry(fat_vol *v, u32 dir_cluster, const u8 name11[11]);
int  fat_rename_entry(fat_vol *v, u32 dir_cluster, const u8 old11[11], const u8 new11[11]);
int  fat_create_file(fat_vol *v, u32 dir_cluster, const u8 name11[11], u32 *first_cluster);
int  fat_delete_file(fat_vol *v, u32 dir_cluster, const u8 name11[11]);
int  fat_mkdir(fat_vol *v, u32 dir_cluster, const u8 name11[11], u32 *new_cluster);
int  fat_rmdir(fat_vol *v, u32 dir_cluster, const u8 name11[11]);
int  fat_set_attr(fat_vol *v, u32 dir_cluster, const u8 name11[11], u8 attr);

void fat_volume_label(fat_vol *v, char *out, int maxlen);

#endif
