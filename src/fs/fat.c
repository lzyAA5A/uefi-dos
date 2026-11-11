/*
 * UEFI-DOS -- FAT filesystem driver (FAT32 read/write, FAT12/16 read).
 *
 * Sector size is fixed at 512 bytes.  All disk access goes through the
 * blockdev abstraction so the same driver runs inside UEFI and in the
 * Windows host test harness.
 */
#include "uefidos.h"
#include "fs/fat.h"

#define CLUSTER_BYTES(v) ((u32)(v)->spc * FAT_SEC)

#define CHAIN_END  0xFFFFFFFFu
#define CHAIN_BAD  0xFFFFFFFEu
#define CHAIN_FREE 0xFFFFFFFDu

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p)
{
    return ((u32)p[0]) | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int read_sec(fat_vol *v, u32 lba, void *buf)
{
    return v->dev->read(v->dev, lba, buf, 1);
}
static int write_sec(fat_vol *v, u32 lba, const void *buf)
{
    if (v->read_only) return FAT_EROFS;
    return v->dev->write(v->dev, lba, buf, 1);
}

void fat_volume_label(fat_vol *v, char *out, int maxlen)
{
    u8 buf[FAT_SEC];
    out[0] = 0;
    if (v->fs_type == 32) {
        u32 lba;
        if (fat_get_cluster_lba(v, v->root_cluster, &lba)) return;
        if (read_sec(v, lba, buf)) return;
    } else {
        if (read_sec(v, v->root_lba, buf)) return;
    }
    for (int i = 0; i < 512; i += 32) {
        u8 *e = buf + i;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if ((e[11] & 0x3F) == 0x08) {
            int n = 0;
            for (int k = 0; k < 11 && n < maxlen - 1; k++)
                if (e[k] != ' ') out[n++] = (char)e[k];
            out[n] = 0;
            return;
        }
    }
}


/* ------------------------------------------------------------------ */
/* long file names (VFAT 0x0F entries)                                 */
/* ------------------------------------------------------------------ */

/* the 8.3 checksum every LFN entry of a chain carries in byte 13 */
static u8 lfn_checksum(const u8 *name11)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    return sum;
}

struct lfn_state {
    u16 ch[FAT_LFN_MAX];
    int max_order;   /* highest sequence number seen in this chain */
    int have;
    u8 sum;
};

static void lfn_reset(struct lfn_state *st)
{
    memset(st->ch, 0, sizeof(st->ch));
    st->max_order = 0;
    st->have = 0;
    st->sum = 0;
}

/* 13 UTF-16 code units live at these offsets of an LFN entry */
static const u8 g_lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static void lfn_take(struct lfn_state *st, const u8 *raw)
{
    int order = raw[0] & 0x1F;
    if ((raw[0] & 0x40) || order <= 0 || order > FAT_LFN_MAX / 13) {
        lfn_reset(st);              /* first (0x40) entry starts a new chain */
        if (order <= 0 || order > FAT_LFN_MAX / 13) return;
    }
    st->sum = raw[13];
    int base = (order - 1) * 13;
    for (int i = 0; i < 13; i++) {
        u16 c = (u16)(raw[g_lfn_off[i]] | ((u16)raw[g_lfn_off[i] + 1] << 8));
        if (base + i < FAT_LFN_MAX) st->ch[base + i] = c;
    }
    if (order > st->max_order) st->max_order = order;
    st->have = 1;
}

/* assemble the collected chain; non-ASCII code units become '_' (the DOS side
 * of this system is CP437, so a full Unicode round trip is out of scope) */
static void lfn_build(const struct lfn_state *st, char *out, int maxlen)
{
    int n = 0, total = st->max_order * 13;
    for (int i = 0; i < total && n < maxlen - 1; i++) {
        u16 c = st->ch[i];
        if (c == 0) break;
        out[n++] = (c < 0x80) ? (char)c : '_';
    }
    out[n] = 0;
}

/* enumerate directory sectors: cb returns non-zero to stop early */
typedef int (*dir_sec_cb)(fat_vol *v, u32 lba, void *arg);

static int dir_foreach_sector(fat_vol *v, u32 start_cluster, dir_sec_cb cb, void *arg)
{
    if (v->fs_type != 32 && start_cluster == 0) {
        u32 nsec = (v->root_entries * 32 + FAT_SEC - 1) / FAT_SEC;
        for (u32 sec = 0; sec < nsec; sec++)
            if (cb(v, v->root_lba + sec, arg)) return FAT_OK;
        return FAT_OK;
    }
    u32 cl = start_cluster, guard = 0;
    while (cl >= 2 && cl < v->cluster_count + 2 && guard++ < 65536) {
        u32 lba;
        if (fat_get_cluster_lba(v, cl, &lba)) return FAT_EIO;
        for (u32 sec = 0; sec < v->spc; sec++)
            if (cb(v, lba + sec, arg)) return FAT_OK;
        u32 nxt;
        if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
        if (nxt >= CHAIN_FREE) break;
        cl = nxt;
    }
    return FAT_OK;
}

struct scan_ex_ctx {
    fat_scan_ex_cb cb;
    void *arg;
    struct lfn_state lfn;
};

static int scan_ex_sector(fat_vol *v, u32 lba, void *arg)
{
    struct scan_ex_ctx *c = (struct scan_ex_ctx *)arg;
    u8 buf[FAT_SEC];
    if (read_sec(v, lba, buf)) return 1;
    for (u32 e = 0; e + 32 <= FAT_SEC; e += 32) {
        fat_dirent d;
        memcpy(d.raw, buf + e, 32);
        if (d.raw[0] == 0x00) return 1;                     /* end of directory */
        if (d.raw[0] == 0xE5) { lfn_reset(&c->lfn); continue; }
        if ((d.raw[11] & 0x3F) == 0x0F) { lfn_take(&c->lfn, d.raw); continue; }
        char longname[FAT_LFN_MAX];
        const char *lp = NULL;
        if (c->lfn.have && lfn_checksum(d.raw) == c->lfn.sum) {
            lfn_build(&c->lfn, longname, sizeof(longname));
            if (longname[0]) lp = longname;
        }
        lfn_reset(&c->lfn);
        if (c->cb(v, &d, lba, e, lp, c->arg)) return 1;
    }
    return 0;
}

int fat_dir_scan_ex(fat_vol *v, u32 start_cluster, fat_scan_ex_cb cb, void *arg)
{
    struct scan_ex_ctx c;
    memset(&c, 0, sizeof(c));
    c.cb = cb;
    c.arg = arg;
    lfn_reset(&c.lfn);
    return dir_foreach_sector(v, start_cluster, scan_ex_sector, &c);
}

/* case-insensitive ASCII compare (DOS long names are matched that way) */
static int lfn_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (u8)*a, cb2 = (u8)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb2 >= 'a' && cb2 <= 'z') cb2 -= 32;
        if (ca != cb2) return 0;
        if (!ca) return 1;
    }
}

struct find_long_ctx {
    const char *name;
    fat_dirent *out;
    u32 cl, lba, idx;
    int found;
};

static int find_long_trampoline(fat_vol *v, const fat_dirent *d, u32 lba, u32 off,
                                const char *longname, void *arg)
{
    struct find_long_ctx *f = (struct find_long_ctx *)arg;
    UFD_UNUSED(v);
    if (!longname) return 0;
    if (!lfn_ieq(longname, f->name)) return 0;
    memcpy(f->out, d, sizeof(fat_dirent));
    f->cl = fat_entry_cluster(d);
    f->lba = lba;
    f->idx = off / 32;
    f->found = 1;
    return 1;
}

int fat_find_long(fat_vol *v, u32 dir_cluster, const char *longname, fat_dirent *out,
                  u32 *out_cluster, u32 *out_ent_lba, u32 *out_ent_idx)
{
    struct find_long_ctx f;
    memset(&f, 0, sizeof(f));
    f.name = longname;
    f.out = out;
    int rc = fat_dir_scan_ex(v, dir_cluster, find_long_trampoline, &f);
    if (rc != FAT_OK) return rc;
    if (!f.found) return FAT_ENOENT;
    if (out_cluster) *out_cluster = f.cl;
    if (out_ent_lba) *out_ent_lba = f.lba;
    if (out_ent_idx) *out_ent_idx = f.idx;
    return FAT_OK;
}

struct run_ctx {
    int need;
    u32 lba, idx;
    int found;
};

static int run_sector(fat_vol *v, u32 lba, void *arg)
{
    struct run_ctx *r = (struct run_ctx *)arg;
    u8 buf[FAT_SEC];
    if (read_sec(v, lba, buf)) return 1;
    int run = 0, start = -1;
    for (int e = 0; e + 32 <= FAT_SEC; e += 32) {
        int free_slot = (buf[e] == 0x00 || buf[e] == 0xE5);
        if (free_slot) {
            if (run == 0) start = e;
            run++;
            if (run >= r->need) {
                r->lba = lba;
                r->idx = (u32)(start / 32);
                r->found = 1;
                return 1;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

/* need consecutive free slots (a whole LFN chain plus its 8.3 entry) */
static int fat_find_free_run(fat_vol *v, u32 dir_cluster, int need, u32 *out_lba, u32 *out_idx)
{
    if (need <= 0) return FAT_EINVAL;
    if (need > (int)(FAT_SEC / 32)) return FAT_EDIRFULL;
    struct run_ctx r;
    memset(&r, 0, sizeof(r));
    r.need = need;
    int rc = dir_foreach_sector(v, dir_cluster, run_sector, &r);
    if (rc != FAT_OK) return rc;
    if (r.found) {
        if (out_lba) *out_lba = r.lba;
        if (out_idx) *out_idx = r.idx;
        return FAT_OK;
    }
    /* grow the directory once (FAT32 only) and use the fresh cluster */
    if (v->fs_type != 32 || dir_cluster == 0) return FAT_EDIRFULL;
    u32 lba = 0, idx = 0, newcl = 0;
    rc = fat_find_free(v, dir_cluster, &lba, &idx, &newcl);
    if (rc != FAT_OK) return rc;
    if (!newcl) return FAT_EDIRFULL;   /* last sector was tight; caller falls back */
    if (out_lba) *out_lba = lba;
    if (out_idx) *out_idx = 0;
    return FAT_OK;
}

int fat_create_file_long(fat_vol *v, u32 dir_cluster, const u8 name11[11],
                         const char *longname, u8 attr)
{
    if (v->read_only) return FAT_EROFS;
    if (!longname || !longname[0]) return fat_create_file(v, dir_cluster, name11, NULL);
    int len = (int)strlen(longname);
    if (len > FAT_LFN_MAX - 1) len = FAT_LFN_MAX - 1;
    int slots = (len + 12) / 13;
    fat_dirent chk;
    int rc = fat_find(v, dir_cluster, name11, &chk, NULL, NULL, NULL);
    if (rc == FAT_OK) return FAT_EEXIST;
    if (rc != FAT_ENOENT) return rc;

    u32 lba = 0, idx = 0;
    rc = fat_find_free_run(v, dir_cluster, slots + 1, &lba, &idx);
    if (rc != FAT_OK) {
        /* no room for the chain: store the short name only */
        return fat_add_entry(v, dir_cluster, name11, attr, 0, 0);
    }
    u8 sum = lfn_checksum(name11);
    for (int k = 0; k < slots; k++) {
        int order = slots - k;              /* VFAT stores the chain backwards */
        u8 raw[32];
        memset(raw, 0xFF, 32);
        raw[0] = (u8)order;
        if (order == slots) raw[0] |= 0x40;
        raw[11] = 0x0F;
        raw[12] = 0;
        raw[13] = sum;
        raw[26] = raw[27] = 0;              /* first cluster must be zero */
        for (int i = 0; i < 13; i++) {
            int ci = (order - 1) * 13 + i;
            u16 c = 0xFFFF;
            if (ci < len) c = (u16)(u8)longname[ci];
            else if (ci == len) c = 0;      /* terminator */
            raw[g_lfn_off[i]] = (u8)(c & 0xFF);
            raw[g_lfn_off[i] + 1] = (u8)(c >> 8);
        }
        fat_dirent d;
        memcpy(d.raw, raw, 32);
        rc = fat_write_entry_at(v, lba, idx + (u32)k, &d);
        if (rc != FAT_OK) return rc;
    }
    fat_dirent sd;
    fat_make_entry(&sd, (const char *)name11, attr, 0, 0);
    return fat_write_entry_at(v, lba, idx + (u32)slots, &sd);
}

/* ------------------------------------------------------------------ */
/* mount / unmount                                                     */
/* ------------------------------------------------------------------ */
static u32 count_free(fat_vol *v);

int fat_mount(fat_vol *v, blockdev *dev, u32 part_lba, int read_only)
{
    u8 bs[FAT_SEC];
    memset(v, 0, sizeof(*v));
    v->dev = dev;
    v->part_lba = part_lba;
    v->read_only = (u8)read_only;
    fat_cache_invalidate(v);
    if (!dev || dev->blk_size != FAT_SEC) return FAT_EINVAL;
    if (read_sec(v, part_lba, bs)) return FAT_EIO;
    if (bs[510] != 0x55 || bs[511] != 0xAA) return FAT_EINVAL;
    v->bytes_per_sector = rd16(bs + 11);
    if (v->bytes_per_sector != FAT_SEC) return FAT_EINVAL;
    v->spc = bs[13];
    if (v->spc == 0 || (v->spc & (v->spc - 1)) || v->spc > 128) return FAT_EINVAL;
    v->reserved = rd16(bs + 14);
    v->nfats = bs[16];
    if (v->nfats == 0) return FAT_EINVAL;
    v->root_entries = rd16(bs + 17);
    u16 total16 = rd16(bs + 19);
    u16 fat16sz = rd16(bs + 22);
    u32 total32 = rd32(bs + 32);
    u32 fat32sz = rd32(bs + 36);

    if (v->root_entries == 0 && fat16sz == 0 && fat32sz != 0) {
        v->fs_type = 32;
        v->fat_sectors = fat32sz;
        v->root_cluster = rd32(bs + 44);
        v->total_sectors = total32 ? total32 : total16;
    } else if (fat16sz != 0) {
        v->fat_sectors = fat16sz;
        v->total_sectors = total16 ? total16 : total32;
    } else {
        return FAT_EINVAL;
    }
    if (v->total_sectors < 16 || v->fat_sectors == 0) return FAT_EINVAL;

    u32 root_dir_secs = v->fs_type == 32 ? 0 : (u32)((v->root_entries * 32 + 511) / 512);
    if (v->total_sectors <= v->reserved + v->nfats * v->fat_sectors + root_dir_secs)
        return FAT_EINVAL;

    v->fat_lba = v->part_lba + v->reserved;
    v->root_lba = v->part_lba + v->reserved + (u32)v->nfats * v->fat_sectors;
    v->data_lba = v->root_lba + root_dir_secs;
    u64 clusters = (v->total_sectors - v->reserved - (u64)v->nfats * v->fat_sectors - root_dir_secs)
                   / v->spc;
    if (clusters < 1) return FAT_EINVAL;
    if (clusters > 0x0FFFFFF5u) clusters = 0x0FFFFFF5u;
    v->cluster_count = (u32)clusters;

    if (v->fs_type == 0) {
        v->fs_type = clusters < 4085 ? 12 : 16;
    }
    if (v->fs_type == 32) {
        if (v->root_cluster < 2 || v->root_cluster >= v->cluster_count + 2) return FAT_ECORRUPT;
    } else {
        v->root_cluster = 0;
    }
    v->alloc_hint = 2;
    v->free_count = count_free(v);
    return FAT_OK;
}

int fat_unmount(fat_vol *v)
{
    memset(v, 0, sizeof(*v));
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* FAT table access                                                    */
/* ------------------------------------------------------------------ */
/* Return a pointer to a cached FAT sector, reading it from the device only
 * when it is not already resident.  A missing sector evicts round-robin, so a
 * chain walk (which touches a few consecutive FAT sectors) stays in cache
 * instead of turning every step into a disk read. */
static u8 *fat_sec_ptr(fat_vol *v, u32 lba)
{
    int i, victim = -1;
    for (i = 0; i < FAT_CACHE_SLOTS; i++) {
        if (v->cache_lba[i] == lba) return v->cache[i];
    }
    for (i = 0; i < FAT_CACHE_SLOTS; i++) {
        if (v->cache_lba[i] == 0xFFFFFFFFu) { victim = i; break; }
    }
    if (victim < 0) {
        victim = (int)(v->cache_next % FAT_CACHE_SLOTS);
        v->cache_next = (u8)((v->cache_next + 1u) % FAT_CACHE_SLOTS);
    }
    if (read_sec(v, lba, v->cache[victim])) return NULL;
    v->cache_lba[victim] = lba;
    return v->cache[victim];
}

void fat_cache_invalidate(fat_vol *v)
{
    int i;
    for (i = 0; i < FAT_CACHE_SLOTS; i++) v->cache_lba[i] = 0xFFFFFFFFu;
    v->cache_next = 0;
}

int fat_load_fat_sector(fat_vol *v, u32 lba)
{
    return fat_sec_ptr(v, lba) ? FAT_OK : FAT_EIO;
}

static int get_fat_entry_raw(fat_vol *v, u32 cl, u32 *out)
{
    u32 byte = (v->fs_type == 32) ? cl * 4u : (v->fs_type == 16) ? cl * 2u : (cl * 3u) >> 1;
    u32 lba = v->fat_lba + byte / FAT_SEC;
    u32 off = byte % FAT_SEC;
    u8 *p;
    u16 w;
    *out = 0;
    p = fat_sec_ptr(v, lba);
    if (!p) return FAT_EIO;
    if (off + 2 > FAT_SEC) {
        u8 *p2 = fat_sec_ptr(v, lba + 1);
        if (!p2) return FAT_EIO;
        w = (u16)(p[off] | ((u16)p2[0] << 8));
    } else {
        w = rd16(p + off);
    }
    if (v->fs_type == 12) *out = (cl & 1) ? (u32)(w >> 4) : (u32)(w & 0xFFF);
    else if (v->fs_type == 16) *out = w;
    else *out = rd32(p + off) & 0x0FFFFFFF;
    return FAT_OK;
}

static int set_fat_entry(fat_vol *v, u32 cl, u32 val)
{
    u32 byte = (v->fs_type == 32) ? cl * 4u : (v->fs_type == 16) ? cl * 2u : (cl * 3u) >> 1;
    u32 lba = v->fat_lba + byte / FAT_SEC;
    u32 off = byte % FAT_SEC;
    u8 *p;
    if (v->read_only) return FAT_EROFS;
    if (v->fs_type == 32) {
        u32 old;
        p = fat_sec_ptr(v, lba);
        if (!p) return FAT_EIO;
        old = rd32(p + off);
        val = (val & 0x0FFFFFFF) | (old & 0xF0000000);
        p[off] = (u8)val; p[off + 1] = (u8)(val >> 8);
        p[off + 2] = (u8)(val >> 16); p[off + 3] = (u8)(val >> 24);
        return write_sec(v, lba, p);
    }
    if (v->fs_type == 16) {
        u16 val16 = (u16)val;
        p = fat_sec_ptr(v, lba);
        if (!p) return FAT_EIO;
        p[off] = (u8)val16; p[off + 1] = (u8)(val16 >> 8);
        return write_sec(v, lba, p);
    }
    /* FAT12: an entry may straddle two sectors */
    {
        u8 *p2 = NULL;
        u16 w;
        p = fat_sec_ptr(v, lba);
        if (!p) return FAT_EIO;
        if (off + 2 > FAT_SEC) {
            p2 = fat_sec_ptr(v, lba + 1);
            if (!p2) return FAT_EIO;
            w = (u16)(p[off] | ((u16)p2[0] << 8));
        } else {
            w = rd16(p + off);
        }
        if (cl & 1) w = (u16)((w & 0x000F) | ((val & 0xFFF) << 4));
        else        w = (u16)((w & 0xF000) | (val & 0xFFF));
        p[off] = (u8)w;
        if (off + 1 < FAT_SEC) {
            p[off + 1] = (u8)(w >> 8);
            if (write_sec(v, lba, p)) return FAT_EIO;
        } else {
            if (p2) p2[0] = (u8)(w >> 8);
            if (write_sec(v, lba, p)) return FAT_EIO;
            if (p2 && write_sec(v, lba + 1, p2)) return FAT_EIO;
        }
        return FAT_OK;
    }
}

int fat_next_cluster(fat_vol *v, u32 cl, u32 *next)
{
    u32 raw;
    if (cl < 2 || cl >= v->cluster_count + 2) return FAT_EINVAL;
    if (get_fat_entry_raw(v, cl, &raw)) return FAT_EIO;
    u32 end = v->fs_type == 32 ? 0x0FFFFFF8u : v->fs_type == 16 ? 0xFFF8u : 0xFF8u;
    u32 bad = v->fs_type == 32 ? 0x0FFFFFF7u : v->fs_type == 16 ? 0xFFF7u : 0xFF7u;
    if (raw >= end) *next = CHAIN_END;
    else if (raw == bad) *next = CHAIN_BAD;
    else if (raw < 2) *next = CHAIN_FREE;
    else *next = raw;
    return FAT_OK;
}

int fat_get_cluster_lba(fat_vol *v, u32 cl, u32 *lba)
{
    if (cl < 2 || cl >= v->cluster_count + 2) return FAT_EINVAL;
    *lba = v->data_lba + (cl - 2) * v->spc;
    return FAT_OK;
}

/* allocate one free cluster, chain-marked END; returns 0 on success */
static int alloc_cluster(fat_vol *v, u32 *out)
{
    if (v->read_only) return FAT_EROFS;
    if (v->free_count == 0) return FAT_ENOSPC;
    u32 first = v->cluster_count + 2;
    if (v->alloc_hint < 2 || v->alloc_hint >= first) v->alloc_hint = 2;
    /* single wrap: start at the cursor, then scan from cluster 2 up to it */
    u32 tries = first - 2;
    u32 cl = v->alloc_hint;
    for (u32 i = 0; i < tries; i++) {
        u32 raw;
        if (get_fat_entry_raw(v, cl, &raw)) return FAT_EIO;
        if (raw == 0) {
            if (set_fat_entry(v, cl, (v->fs_type == 32) ? 0x0FFFFFFF :
                                  (v->fs_type == 16) ? 0xFFFF : 0xFFF)) return FAT_EIO;
            /* zero the cluster so reads are deterministic */
            u32 lba;
            if (fat_get_cluster_lba(v, cl, &lba)) return FAT_EIO;
            u8 zero[FAT_SEC];
            memset(zero, 0, sizeof(zero));
            for (u32 s = 0; s < v->spc; s++)
                if (write_sec(v, lba + s, zero)) return FAT_EIO;
            v->alloc_hint = (cl + 1 < first) ? cl + 1 : 2;
            if (v->free_count != 0xFFFFFFFFu) v->free_count--;
            *out = cl;
            return FAT_OK;
        }
        cl++;
        if (cl >= first) cl = 2;
    }
    v->free_count = 0;
    return FAT_ENOSPC;
}

/* count free clusters (one linear pass at mount; FAT16 worst case 65524) */
static u32 count_free(fat_vol *v)
{
    u32 n = 0;
    u32 end = v->cluster_count + 2;
    for (u32 cl = 2; cl < end; cl++) {
        u32 raw;
        if (get_fat_entry_raw(v, cl, &raw)) break;
        if (raw == 0) n++;
    }
    return n;
}

int fat_free_chain(fat_vol *v, u32 first)
{
    /* NOTE: each cleared entry increments free_count below */
    u32 cl = first;
    u32 guard = 0;
    while (cl >= 2 && cl < v->cluster_count + 2 && guard++ < 2000000) {
        u32 nxt;
        if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
        if (set_fat_entry(v, cl, 0)) return FAT_EIO;
        if (v->free_count != 0xFFFFFFFFu) v->free_count++;
        if (nxt == CHAIN_END) break;
        /* A free or bad marker where a link was expected means the chain is
         * already broken (a half-deleted file, or a volume written by a buggy
         * tool).  Treat it as the end of the chain instead of refusing to free:
         * that way the file can still be deleted or overwritten, which is what a
         * rescue tool has to be able to do. */
        if (nxt >= CHAIN_FREE) break;
        cl = nxt;
    }
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                  */
/* ------------------------------------------------------------------ */
/* cb returns nonzero to stop */
typedef int (*dir_raw_cb)(fat_vol *v, const fat_dirent *d, u32 lba, u32 off, void *arg);

static int iterate_dir(fat_vol *v, u32 start_cluster, dir_raw_cb cb, void *arg, int raw)
{
    if (v->fs_type != 32 && start_cluster == 0) {
        u32 nsec = (v->root_entries * 32 + FAT_SEC - 1) / FAT_SEC;
        for (u32 s = 0; s < nsec; s++) {
            u8 buf[FAT_SEC];
            if (read_sec(v, v->root_lba + s, buf)) return FAT_EIO;
            for (u32 e = 0; e + 32 <= FAT_SEC; e += 32) {
                fat_dirent d;
                memcpy(d.raw, buf + e, 32);
                if (d.raw[0] == 0x00) {
                    if (raw) cb(v, &d, v->root_lba + s, e, arg);
                    return FAT_OK; /* end marker (also a free slot in raw mode) */
                }
                if (d.raw[0] == 0xE5 && !raw) continue;
                if ((d.raw[11] & 0x3F) == 0x0F) continue; /* LFN */
                if (cb(v, &d, v->root_lba + s, e, arg)) return FAT_OK;
            }
        }
        return FAT_OK;
    }
    u32 cl = start_cluster;
    u32 guard = 0;
    while (cl >= 2 && cl < v->cluster_count + 2) {
        u32 lba;
        if (fat_get_cluster_lba(v, cl, &lba)) return FAT_EIO;
        for (u32 s = 0; s < v->spc; s++) {
            u8 buf[FAT_SEC];
            if (read_sec(v, lba + s, buf)) return FAT_EIO;
            for (u32 e = 0; e + 32 <= FAT_SEC; e += 32) {
                fat_dirent d;
                memcpy(d.raw, buf + e, 32);
                if (d.raw[0] == 0x00) {
                    if (raw) cb(v, &d, lba + s, e, arg);
                    return FAT_OK; /* end marker (also a free slot in raw mode) */
                }
                if (d.raw[0] == 0xE5 && !raw) continue;
                if ((d.raw[11] & 0x3F) == 0x0F) continue;
                if (cb(v, &d, lba + s, e, arg)) return FAT_OK;
            }
        }
        u32 nxt;
        if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
        if (nxt >= CHAIN_FREE) break;
        cl = nxt;
        if (++guard > 1u << 20) return FAT_ECORRUPT;
    }
    return FAT_OK;
}

struct scan_wrap {
    fat_scan_cb cb;
    void *arg;
};

static int scan_trampoline(fat_vol *v, const fat_dirent *d, u32 lba, u32 off, void *arg)
{
    (void)lba; (void)off;
    struct scan_wrap *w = (struct scan_wrap *)arg;
    return w->cb(v, d, w->arg);
}

int fat_dir_scan(fat_vol *v, u32 start_cluster, fat_scan_cb cb, void *arg)
{
    struct scan_wrap w;
    w.cb = cb;
    w.arg = arg;
    return iterate_dir(v, start_cluster, scan_trampoline, &w, 0);
}

/* ------------------------------------------------------------------ */
/* entry lookup                                                        */
/* ------------------------------------------------------------------ */
struct find_ctx {
    const u8 *name11;
    fat_dirent *out;
    u32 cl, lba, idx;
    int found;
};

static int find_trampoline(fat_vol *v, const fat_dirent *d, u32 lba, u32 off, void *arg)
{
    struct find_ctx *f = (struct find_ctx *)arg;
    UFD_UNUSED(v);
    if (memcmp(d->raw, f->name11, 11) == 0) {
        memcpy(f->out, d, sizeof(fat_dirent));
        f->cl = fat_entry_cluster(d);
        f->lba = lba;
        f->idx = off / 32;
        f->found = 1;
        return 1;
    }
    return 0;
}

int fat_find(fat_vol *v, u32 dir_cluster, const u8 name11[11], fat_dirent *out,
             u32 *out_cluster, u32 *out_ent_lba, u32 *out_ent_idx)
{
    struct find_ctx f;
    memset(&f, 0, sizeof(f));
    f.name11 = name11;
    f.out = out;
    int rc = iterate_dir(v, dir_cluster, find_trampoline, &f, 1);
    if (rc != FAT_OK) return rc;
    if (!f.found) return FAT_ENOENT;
    if (out_cluster) *out_cluster = f.cl;
    if (out_ent_lba) *out_ent_lba = f.lba;
    if (out_ent_idx) *out_ent_idx = f.idx;
    return FAT_OK;
}

/* free entry slot: 0x00 or 0xE5 entries (skip LFN chains) */
struct free_ctx {
    int found;
    u32 lba, off;
};

static int free_trampoline(fat_vol *v, const fat_dirent *d, u32 lba, u32 off, void *arg)
{
    struct free_ctx *f = (struct free_ctx *)arg;
    (void)v;
    if (d->raw[0] == 0x00 || d->raw[0] == 0xE5) {
        if (d->raw[0] == 0x00) {
            if ((d->raw[11] & 0x3F) == 0x0F && d->raw[0] == 0x00) {
                /* shouldn't happen: LFN terminator handled by iterate */
            }
        }
        f->found = 1; f->lba = lba; f->off = off;
        return 1;
    }
    return 0;
}

int fat_find_free(fat_vol *v, u32 dir_cluster, u32 *out_lba, u32 *out_idx, u32 *out_new_cluster)
{
    struct free_ctx f;
    memset(&f, 0, sizeof(f));
    int rc = iterate_dir(v, dir_cluster, free_trampoline, &f, 1);
    if (rc != FAT_OK) return rc;
    if (f.found) {
        if (out_lba) *out_lba = f.lba;
        if (out_idx) *out_idx = f.off / 32;
        if (out_new_cluster) *out_new_cluster = 0;
        return FAT_OK;
    }
    if (v->fs_type != 32 || dir_cluster == 0) return FAT_EDIRFULL;
    /* grow the directory by one cluster */
    u32 last = dir_cluster, cl = dir_cluster;
    for (;;) {
        u32 nxt;
        if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
        if (nxt >= CHAIN_FREE) { last = cl; break; }
        cl = nxt;
    }
    u32 newcl;
    if (alloc_cluster(v, &newcl) != FAT_OK) return FAT_ENOSPC;
    if (set_fat_entry(v, last, newcl)) return FAT_EIO;
    if (out_lba) {
        u32 lba;
        if (fat_get_cluster_lba(v, newcl, &lba)) return FAT_EIO;
        *out_lba = lba;
    }
    if (out_idx) *out_idx = 0;
    if (out_new_cluster) *out_new_cluster = newcl;
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* directory entry creation / removal                                  */
/* ------------------------------------------------------------------ */
static void entry_time_now(u8 *e)
{
    int y, m, d, h, mi, s;
    plat_get_date(&y, &m, &d);
    plat_get_time(&h, &mi, &s);
    u16 dos_date = (u16)(((y - 1980) << 9) | ((m & 0xF) << 5) | (d & 0x1F));
    u16 dos_time = (u16)(((h & 0x1F) << 11) | ((mi & 0x3F) << 5) | ((s / 2) & 0x1F));
    e[14] = (u8)dos_time; e[15] = (u8)(dos_time >> 8);
    e[16] = (u8)dos_date; e[17] = (u8)(dos_date >> 8);
    e[22] = (u8)dos_time; e[23] = (u8)(dos_time >> 8);
    e[24] = (u8)dos_date; e[25] = (u8)(dos_date >> 8);
}

int fat_make_entry(fat_dirent *d, const char *name83_11, u8 attr, u32 cluster, u32 size)
{
    memset(d->raw, 0, 32);
    memcpy(d->raw, name83_11, 11);
    d->raw[11] = attr;
    d->raw[12] = 0;  /* NT reserved */
    d->raw[13] = 0;  /* creation tenths */
    entry_time_now(d->raw);
    fat_entry_set_cluster(d, cluster);
    fat_entry_set_size(d, size);
    return FAT_OK;
}

int fat_write_entry_at(fat_vol *v, u32 lba, u32 idx, const fat_dirent *d)
{
    u8 buf[FAT_SEC];
    if (idx * 32 + 32 > FAT_SEC) return FAT_EINVAL;
    if (read_sec(v, lba, buf)) return FAT_EIO;
    memcpy(buf + idx * 32, d->raw, 32);
    return write_sec(v, lba, buf);
}

int fat_add_entry(fat_vol *v, u32 dir_cluster, const u8 name11[11], u8 attr, u32 cluster, u32 size)
{
    u32 lba = 0, idx = 0, newcl = 0;
    int rc = fat_find_free(v, dir_cluster, &lba, &idx, &newcl);
    if (rc != FAT_OK) return rc;
    fat_dirent d;
    fat_make_entry(&d, (const char *)name11, attr, cluster, size);
    return fat_write_entry_at(v, lba, idx, &d);
}

int fat_remove_entry(fat_vol *v, u32 dir_cluster, const u8 name11[11])
{
    fat_dirent d;
    u32 lba, idx;
    int rc = fat_find(v, dir_cluster, name11, &d, NULL, &lba, &idx);
    if (rc != FAT_OK) return rc;
    u8 buf[FAT_SEC];
    if (read_sec(v, lba, buf)) return FAT_EIO;
    buf[idx * 32] = 0xE5;
    return write_sec(v, lba, buf);
}

int fat_rename_entry(fat_vol *v, u32 dir_cluster, const u8 old11[11], const u8 new11[11])
{
    fat_dirent d;
    u32 lba, idx;
    int rc = fat_find(v, dir_cluster, old11, &d, NULL, &lba, &idx);
    if (rc != FAT_OK) return rc;
    u8 buf[FAT_SEC];
    if (read_sec(v, lba, buf)) return FAT_EIO;
    memcpy(buf + idx * 32, new11, 11);
    return write_sec(v, lba, buf);
}

/* ------------------------------------------------------------------ */
/* file data read / write                                              */
/* ------------------------------------------------------------------ */
int fat_read_chain_byte(fat_vol *v, u32 start, u32 size, u32 offset, void *buf, u32 len,
                        u32 *io_bytes)
{
    u32 cl = start;
    u32 cbytes = CLUSTER_BYTES(v);
    u32 pos = 0;
    u32 guard = 0;
    u32 done = 0;
    u8 *out = (u8 *)buf;
    *io_bytes = 0;
    while (len > 0 && pos < size && cl >= 2 && guard++ < 2000000) {
        u32 lba;
        if (fat_get_cluster_lba(v, cl, &lba)) return FAT_ECORRUPT;
        if (offset >= pos + cbytes) {
            u32 nxt;
            if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
            if (nxt >= CHAIN_FREE) break;
            cl = nxt; pos += cbytes;
            continue;
        }
        u32 in_off = (offset > pos) ? (offset - pos) : 0;
        u32 n = cbytes - in_off;
        u32 remain_file = size - (pos + in_off > size ? size : pos + in_off);
        if (n > remain_file) n = remain_file;
        if (n > len) n = len;
        if (n) {
            u32 sec = in_off / FAT_SEC;   /* sector index within this cluster */
            u32 so = in_off % FAT_SEC;    /* intra-sector offset */
            u32 got = 0;
            while (got < n) {
                u8 sbuf[FAT_SEC];
                if (read_sec(v, lba + sec, sbuf)) return FAT_EIO;
                u32 chunk = FAT_SEC - so;
                if (chunk > n - got) chunk = n - got;
                memcpy(out + done, sbuf + so, chunk);
                got += chunk;
                done += chunk;
                so = 0;
                sec += 1;
            }
            len -= n;
            offset += n;
        }
        if (len == 0) break;
        u32 nxt;
        if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
        if (nxt >= CHAIN_FREE) break;
        cl = nxt; pos += cbytes;
    }
    *io_bytes = done;
    return FAT_OK;
}

int fat_write_chain(fat_vol *v, u32 *first_cluster, u32 *size, u32 offset,
                    const void *buf, u32 len)
{
    if (v->read_only) return FAT_EROFS;
    u32 cbytes = CLUSTER_BYTES(v);
    u32 need_end = offset + len;
    u32 need_clusters = (need_end + cbytes - 1) / cbytes;
    if (need_clusters == 0) need_clusters = 1;
    u32 first = *first_cluster;
    u32 have = 0;
    if (first >= 2) {
        u32 cl = first;
        while (cl >= 2 && have < need_clusters) {
            have++;
            u32 nxt;
            if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
            if (nxt >= CHAIN_FREE) break;
            cl = nxt;
        }
    }
    if (have < need_clusters) {
        u32 add = need_clusters - have;
        u32 tail = first;
        if (first >= 2) {
            u32 cl = first;
            while (1) {
                u32 nxt;
                if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
                if (nxt >= CHAIN_FREE) break;
                cl = nxt;
                tail = cl;
            }
        }
        for (u32 i = 0; i < add; i++) {
            u32 cl;
            if (alloc_cluster(v, &cl) != FAT_OK) return FAT_ENOSPC;
            if (first < 2) {
                first = cl;
                tail = cl;
            } else {
                if (set_fat_entry(v, tail, cl)) return FAT_EIO;
                tail = cl;
            }
        }
    }
    /* write the data */
    u32 cl = first;
    u32 pos = 0;
    u32 written = 0;
    u32 guard = 0;
    while (written < len && cl >= 2 && guard++ < 2000000) {
        u32 lba;
        if (fat_get_cluster_lba(v, cl, &lba)) return FAT_ECORRUPT;
        if (offset >= pos + cbytes) {
            u32 nxt;
            if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
            if (nxt >= CHAIN_FREE) return FAT_ECORRUPT;
            cl = nxt; pos += cbytes;
            continue;
        }
        u32 in_off = (offset > pos) ? (offset - pos) : 0;
        u32 n = cbytes - in_off;
        if (n > len - written) n = len - written;
        u32 sec = in_off / FAT_SEC;
        u32 so = in_off % FAT_SEC;
        u32 done = 0;
        while (done < n) {
            u8 sbuf[FAT_SEC];
            u32 chunk = FAT_SEC - so;
            if (chunk > n - done) chunk = n - done;
            if (so != 0 || chunk != FAT_SEC) {
                if (read_sec(v, lba + sec, sbuf)) return FAT_EIO;
            } else {
                memset(sbuf, 0, FAT_SEC);
            }
            memcpy(sbuf + so, (const u8 *)buf + written + done, chunk);
            if (write_sec(v, lba + sec, sbuf)) return FAT_EIO;
            done += chunk;
            sec++;
            so = 0;
        }
        offset += n;
        written += n;
        u32 nxt;
        if (fat_next_cluster(v, cl, &nxt)) return FAT_EIO;
        if (nxt >= CHAIN_FREE) break;
        cl = nxt; pos += cbytes;
    }
    if (written != len) return FAT_EIO;
    if (*size < need_end) *size = need_end;
    *first_cluster = first;
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* high-level file/dir operations                                     */
/* ------------------------------------------------------------------ */
int fat_create_file(fat_vol *v, u32 dir_cluster, const u8 name11[11], u32 *first_cluster)
{
    fat_dirent d;
    u32 lba, idx;
    int rc;
    if (v->read_only) return FAT_EROFS;
    rc = fat_find(v, dir_cluster, name11, &d, NULL, &lba, &idx);
    if (rc == FAT_OK) return FAT_EEXIST;
    if (rc != FAT_ENOENT) return rc;
    rc = fat_add_entry(v, dir_cluster, name11, 0x20, 0, 0);
    if (rc != FAT_OK) return rc;
    if (first_cluster) *first_cluster = 0;
    return FAT_OK;
}

int fat_delete_file(fat_vol *v, u32 dir_cluster, const u8 name11[11])
{
    fat_dirent d;
    u32 lba, idx, cl = 0;
    int rc;
    if (v->read_only) return FAT_EROFS;
    rc = fat_find(v, dir_cluster, name11, &d, &cl, &lba, &idx);
    if (rc != FAT_OK) return rc;
    if (d.raw[11] & 0x10) return FAT_EISDIR;
    if (cl >= 2) {
        rc = fat_free_chain(v, cl);
        if (rc != FAT_OK) return rc;
    }
    return fat_remove_entry(v, dir_cluster, name11);
}

int fat_mkdir(fat_vol *v, u32 dir_cluster, const u8 name11[11], u32 *new_cluster)
{
    fat_dirent d;
    u32 lba, idx;
    int rc;
    if (v->read_only) return FAT_EROFS;
    rc = fat_find(v, dir_cluster, name11, &d, NULL, &lba, &idx);
    if (rc == FAT_OK) return FAT_EEXIST;
    if (rc != FAT_ENOENT) return rc;
    u32 newcl;
    rc = alloc_cluster(v, &newcl);
    if (rc != FAT_OK) return FAT_ENOSPC;
    u32 cl_lba;
    if (fat_get_cluster_lba(v, newcl, &cl_lba)) return FAT_EIO;
    u8 buf[FAT_SEC];
    if (read_sec(v, cl_lba, buf)) return FAT_EIO;
    memset(buf, 0, FAT_SEC);
    u8 nm[11];
    memset(nm, ' ', 11);
    nm[0] = '.';
    fat_make_entry(&d, (const char *)nm, 0x10, newcl, 0);
    memcpy(buf, d.raw, 32);
    memset(nm, ' ', 11);
    nm[0] = '.';
    nm[1] = '.';
    u32 parent = (dir_cluster == 0 || dir_cluster == v->root_cluster) ? 0 : dir_cluster;
    fat_make_entry(&d, (const char *)nm, 0x10, parent, 0);
    memcpy(buf + 32, d.raw, 32);
    if (write_sec(v, cl_lba, buf)) return FAT_EIO;
    rc = fat_add_entry(v, dir_cluster, name11, 0x10, newcl, 0);
    if (rc != FAT_OK) {
        fat_free_chain(v, newcl);
        return rc;
    }
    if (new_cluster) *new_cluster = newcl;
    return FAT_OK;
}

typedef struct { int nonempty; } empty_ctx;

static int empty_trampoline(fat_vol *v, const fat_dirent *d, u32 lba, u32 off, void *arg)
{
    UFD_UNUSED(v); UFD_UNUSED(lba); UFD_UNUSED(off);
    if (d->raw[0] == '.' && (d->raw[1] == ' ' || d->raw[1] == '.'))
        return 0;
    ((empty_ctx *)arg)->nonempty = 1;
    return 1;
}

int fat_rmdir(fat_vol *v, u32 dir_cluster, const u8 name11[11])
{
    fat_dirent d;
    u32 lba, idx, cl = 0;
    int rc;
    if (v->read_only) return FAT_EROFS;
    rc = fat_find(v, dir_cluster, name11, &d, &cl, &lba, &idx);
    if (rc != FAT_OK) return rc;
    if (!(d.raw[11] & 0x10)) return FAT_ENOTDIR;
    if (cl == 0 || cl == v->root_cluster) return FAT_EACCES;
    empty_ctx ec;
    ec.nonempty = 0;
    rc = iterate_dir(v, cl, empty_trampoline, &ec, 0);
    if (rc != FAT_OK) return rc;
    if (ec.nonempty) return FAT_ENOTEMPTY;
    rc = fat_free_chain(v, cl);
    if (rc != FAT_OK) return rc;
    return fat_remove_entry(v, dir_cluster, name11);
}

int fat_set_attr(fat_vol *v, u32 dir_cluster, const u8 name11[11], u8 attr)
{
    fat_dirent d;
    u32 lba, idx;
    int rc;
    if (v->read_only) return FAT_EROFS;
    rc = fat_find(v, dir_cluster, name11, &d, NULL, &lba, &idx);
    if (rc != FAT_OK) return rc;
    u8 buf[FAT_SEC];
    if (read_sec(v, lba, buf)) return FAT_EIO;
    buf[idx * 32 + 11] = attr;
    return write_sec(v, lba, buf);
}
