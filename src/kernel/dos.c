/*
 * UEFI-DOS -- DOS kernel layer implementation.
 *
 * The kernel keeps a small DOS-like model: drives with current directories,
 * a 20-entry file handle table, 8.3 names, and DOS-style error codes.
 * Everything is built directly on the FAT driver (src/fs/fat.c).
 */
#include "uefidos.h"
#include "kernel/dos.h"
#include "plat.h"

struct dos_state g_dos;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static char up(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}
static int is_alpha_upper(char c) { return c >= 'A' && c <= 'Z'; }

static int drive_of_letter(char c)
{
    char u = up(c);
    if (u < 'A' || u > 'Z') return -1;
    return u - 'A';
}

static fat_vol *vol_of(int drive)
{
    return &g_dos.drives[drive].vol;
}
static int mounted(int drive)
{
    return drive >= 0 && drive < DOS_MAX_DRIVES && g_dos.drives[drive].mounted;
}

static u32 root_cluster_of(int drive)
{
    fat_vol *v = vol_of(drive);
    return v->fs_type == 32 ? v->root_cluster : 0;
}

int dos_error_code(int fat_rc)
{
    switch (fat_rc) {
    case FAT_ENOENT:   return 0x02; /* file not found */
    case FAT_ENOPATH:  return 0x03; /* path not found */
    case FAT_ENOTDIR:  return 0x03;
    case FAT_EACCES:   return 0x05; /* access denied */
    case FAT_EEXIST:   return 0x50; /* file exists */
    case FAT_ENOSPC:   return 0x27; /* insufficient space */
    case FAT_EDIRFULL: return 0x27;
    case FAT_EROFS:    return 0x05;
    case FAT_ENOTEMPTY:return 0x05;
    case FAT_EISDIR:   return 0x05;
    case FAT_EMFILE:   return 0x04; /* too many open files */
    case FAT_EINVAL:   return 0x0F; /* invalid drive-ish */
    case FAT_ECORRUPT: return 0x1E; /* read fault */
    default:           return 0x1E;
    }
}

/* convert a DOS 8.3 name into an 11-byte FAT name */
void dos_fat_name(const char *name, u8 out11[11], int *ok)
{
    char base[9] = {0}, ext[4] = {0};
    int b = 0, e = 0;
    const char *p = name;
    int okv = 1;
    if (!name || !*name) { if (ok) *ok = 0; memset(out11, ' ', 11); return; }
    /* leading dot names like . and .. are rejected here (handled by caller) */
    for (; *p && *p != '.'; p++) {
        if (*p == '/' || *p == '\\' || *p == ' ' || *p == '*' || *p == '?') { okv = 0; }
        if (b < 8) base[b++] = up(*p);
        else { okv = 0; break; }
    }
    if (*p == '.') {
        p++;
        for (; *p; p++) {
            if (*p == ' ' || *p == '?' || *p == '*' || *p == '/' || *p == '\\') { okv = 0; }
            if (e < 3) ext[e++] = up(*p);
            else { okv = 0; }
        }
    }
    memset(out11, ' ', 11);
    for (int i = 0; i < b; i++) out11[i] = (u8)base[i];
    for (int i = 0; i < e; i++) out11[8 + i] = (u8)ext[i];
    if (ok) *ok = okv;
}

/* 11-byte FAT name -> "NAME.EXT" (trimmed, upper case) */
void dos_dirname_to_dosname(const u8 name11[11], char out[13])
{
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < 8; i++) {
        if (name11[i] == ' ') break;
        out[n++] = (char)name11[i];
    }
    for (int i = 8; i < 11; i++) {
        if (name11[i] != ' ') {
            if (n == 0 || out[n - 1] != '.') out[n++] = '.';
            /* append remaining ext chars */
            for (; i < 11 && name11[i] != ' '; i++) out[n++] = (char)name11[i];
            break;
        }
    }
    out[n] = 0;
}

u32 dos_cluster_lba(fat_vol *v, u32 cl)
{
    u32 lba = 0;
    fat_get_cluster_lba(v, cl, &lba);
    return lba;
}

int dos_read_drive(fat_vol *v, u32 cl, u32 size, u32 off, void *buf, u32 len, u32 *got)
{
    return fat_read_chain_byte(v, cl, size, off, buf, len, got);
}

/* ------------------------------------------------------------------ */
/* init / mounts                                                       */
/* ------------------------------------------------------------------ */
void dos_init(void)
{
    memset(&g_dos, 0, sizeof(g_dos));
    g_dos.cur_drive = 2;   /* C: */
    for (int i = 0; i < DOS_HANDLE_MAX; i++) {
        g_dos.handles[i].used = 0;
    }
    /* DOS 4.0 defaults: FILES=8 (handles 0..4 predefined => 3 free),
     * FCBS=4, LASTDRIVE=E; CONFIG.SYS may raise FILES=/FCBS=. */
    g_dos.files_limit = DOS_FILES_DEFAULT;
    g_dos.fcb_limit   = 4;
    g_dos.last_drive  = 3;
    g_dos.handles[0].used = 1; g_dos.handles[0].console = 1;
    g_dos.handles[1].used = 1; g_dos.handles[1].console = 1;
    g_dos.handles[2].used = 1; g_dos.handles[2].console = 1;
    g_dos.handles[3].used = 1; g_dos.handles[3].console = 2;
    g_dos.handles[4].used = 1; g_dos.handles[4].console = 2;
    g_dos.dta_seg = 0;
    g_dos.dta_off = 0x80;
    g_dos.path_env[0] = 0;
    g_dos.prompt[0] = 0;   /* default = $P$G */
}

static u32 find_part_lba(blockdev *bd)
{
    u8 sec[512];
    if (bd->read(bd, 0, sec, 1)) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    u8 *e = sec + 446;
    for (int i = 0; i < 4; i++, e += 16) {
        u8 status = e[0];
        u8 type = e[4];
        if (status != 0x00 && status != 0x80) continue;
        if (type != 0x01 && type != 0x04 && type != 0x06 && type != 0x07 &&
            type != 0x0B && type != 0x0C && type != 0x0E && type != 0x0F && type != 0xEF)
            continue;
        u32 lba = (u32)e[8] | ((u32)e[9] << 8) | ((u32)e[10] << 16) | ((u32)e[11] << 24);
        u32 cnt = (u32)e[12] | ((u32)e[13] << 8) | ((u32)e[14] << 16) | ((u32)e[15] << 24);
        if (lba == 0 || cnt == 0) continue;
        u8 bs2[512];
        if (bd->read(bd, lba, bs2, 1)) continue;
        if (bs2[510] == 0x55 && bs2[511] == 0xAA) return lba;
    }
    return 0;
}

/* mount one block device on one drive letter: try each MBR partition, then the
 * superfloppy layout (whole device = one volume). */
static int mount_device(int drive, blockdev *bd, const char *hint)
{
    dos_drive *dr = &g_dos.drives[drive];
    dr->bd = *bd;
    u32 part = find_part_lba(&dr->bd);
    int ro = dr->bd.read_only ? 1 : 0;
    /* a volume can live in an MBR partition or start at sector 0 (superfloppy);
     * both layouts must be attempted, so rc starts pessimistic. */
    int rc = FAT_EINVAL;
    if (part) rc = fat_mount(&dr->vol, &dr->bd, part, ro);
    if (rc != FAT_OK) rc = fat_mount(&dr->vol, &dr->bd, 0, ro);
    if (rc != FAT_OK) {
        memset(dr, 0, sizeof(*dr));
        return rc;
    }
    dr->mounted = 1;
    dr->cwd_cluster = root_cluster_of(drive);
    dr->cwd[0] = 0;
    fat_volume_label(&dr->vol, dr->label, sizeof(dr->label));
    if (!dr->label[0]) {
        if (hint && hint[0]) {
            int i = 0;
            for (; hint[i] && i < (int)sizeof(dr->label) - 1; i++) dr->label[i] = hint[i];
            dr->label[i] = 0;
        } else {
            strcpy(dr->label, "NO NAME");
        }
    }
    return FAT_OK;
}

int dos_mount_boot(void)
{
    blockdev bd;
    if (plat_boot_blockdev(&bd)) return FAT_EIO;
    int rc = mount_device(2, &bd, "BOOT");
    return rc;
}

/* Mount every volume the platform can see: the boot volume is C:, the others
 * take the next free letters (D:, E:, ...).  That is what turns "one drive
 * called C:" into "plug in a USB stick and it shows up as D:". */
int dos_mount_all(void)
{
    blockdev bds[DOS_MAX_DRIVES];
    static char labels[DOS_MAX_DRIVES][12];
    int n = plat_enum_blockdevs(bds, DOS_MAX_DRIVES, labels);
    int mounted = 0;

    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        int drive = -1;
        if (i == 0) {
            drive = 2;                                  /* boot volume -> C: */
        } else {
            for (int d = 3; d < DOS_MAX_DRIVES; d++) {
                if (!g_dos.drives[d].mounted) { drive = d; break; }
            }
        }
        if (drive < 0) break;                           /* no free letters left */
        if (mount_device(drive, &bds[i], labels[i]) == FAT_OK) mounted++;
    }
    return mounted;
}

/* mount a file image as A: or B: (e.g. a floppy image); writes go back
 * into the image file through the host volume's FAT chain */
static int img_bd_read(blockdev *bd, u64 lba, void *buf, u32 count)
{
    dos_imgbd *fb = (dos_imgbd *)bd->ctx;
    u8 *out = (u8 *)buf;
    for (u32 i = 0; i < count; i++) {
        u32 off = (u32)(lba + i) * 512u;
        u32 got = 0;
        if (off >= fb->size) {
            memset(out + i * 512u, 0, 512);
            continue;
        }
        u32 want = fb->size - off;
        if (want > 512) want = 512;
        int rc = fat_read_chain_byte(fb->vol, fb->cluster, fb->size, off, out + i * 512u, want, &got);
        if (rc != FAT_OK) return 1;
        if (got < 512) memset(out + i * 512u + got, 0, 512 - got);
    }
    return 0;
}

static int img_bd_write(blockdev *bd, u64 lba, const void *buf, u32 count)
{
    dos_imgbd *fb = (dos_imgbd *)bd->ctx;
    const u8 *in = (const u8 *)buf;
    for (u32 i = 0; i < count; i++) {
        u32 off = (u32)(lba + i) * 512u;
        if (off + 512u > fb->size) return 1;      /* fixed-size image */
        u32 cl = fb->cluster;
        u32 size = fb->size;
        int rc = fat_write_chain(fb->vol, &cl, &size, off, in + i * 512u, 512);
        if (rc != FAT_OK) return 1;
        if (cl != fb->cluster || size != fb->size) return 1; /* never move/grow */
    }
    return 0;
}

int dos_mount_image_drive(int letter, const char *path)
{
    if (letter < 0 || letter > 1) return FAT_EINVAL;
    if (!mounted(2)) return FAT_ENOENT;
    dos_drive *dr = &g_dos.drives[letter];
    int h;
    int rc = dos_open(path, 0, &h);
    if (rc != FAT_OK) return rc;
    u32 cluster = g_dos.handles[h].cluster;
    u32 size = g_dos.handles[h].size;
    dos_close(h);
    if (size < 512) return FAT_EINVAL;
    dr->img.vol = &g_dos.drives[2].vol;
    dr->img.cluster = cluster;
    dr->img.size = size;
    dr->bd.blk_size = 512;
    dr->bd.nblocks = (size + 511u) / 512u;
    dr->bd.read = img_bd_read;
    dr->bd.write = img_bd_write;
    dr->bd.ctx = &dr->img;
    u32 part = find_part_lba(&dr->bd);
    rc = fat_mount(&dr->vol, &dr->bd, part, 0);
    if (rc != FAT_OK) return rc;
    dr->mounted = 1;
    dr->cwd_cluster = root_cluster_of(letter);
    dr->cwd[0] = 0;
    fat_volume_label(&dr->vol, dr->label, sizeof(dr->label));
    if (!dr->label[0]) strcpy(dr->label, "NO NAME");
    return FAT_OK;
}

int dos_unmount_drive(int letter)
{
    if (letter < 0 || letter > 1) return FAT_EINVAL;
    if (!mounted(letter)) return FAT_ENOENT;
    memset(&g_dos.drives[letter], 0, sizeof(dos_drive));
    return FAT_OK;
}

int dos_current_drive(void) { return g_dos.cur_drive; }

/* Select the default drive (INT 21h AH=0Eh).  Like DOS we accept a letter
   with no medium mounted -- the following file operation then fails instead
   of the drive switch failing. */
int dos_set_drive(int drive)
{
    if (drive < 0 || drive >= DOS_MAX_DRIVES) return -1;
    g_dos.cur_drive = drive;
    return 0;
}

int dos_fs_type(int drive)
{
    if (!mounted(drive)) return 0;
    return vol_of(drive)->fs_type;
}

/* ------------------------------------------------------------------ */
/* path parsing                                                        */
/* ------------------------------------------------------------------ */
/* normalize a raw path (relative to base) into "A\\B" form (no drive) */
static int normalize(const char *raw, char comps[][FAT_LFN_MAX], int maxc, int *ncomp)
{
    int n = 0;
    const char *p = raw;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '\\' && *p != '/') p++;
        int len = (int)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n > 0) n--;
            continue;
        }
        if (len > FAT_LFN_MAX - 1) return -1;
        if (n >= maxc) return -1;
        /* case is preserved here: the 8.3 conversion upper-cases on its own, and
         * long names keep whatever the user typed */
        for (int i = 0; i < len; i++) comps[n][i] = start[i];
        comps[n][len] = 0;
        n++;
    }
    *ncomp = n;
    return 0;
}

static void join_comps(const char comps[][FAT_LFN_MAX], int n, char *out, int max)
{
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(comps[i]);
        if (o + len + 2 > max) break;
        if (o > 0) out[o++] = '\\';
        memcpy(out + o, comps[i], (size_t)len);
        o += len;
    }
    out[o] = 0;
}

/* parse "X:path" into drive + normalized absolute path (drive-relative) */
int dos_parse_path(const char *path, int *drive, char *abs, int absmax)
{
    int d = g_dos.cur_drive;
    const char *p = path;
    while (*p == ' ') p++;
    if (is_alpha_upper(up(p[0])) && p[1] == ':') {
        d = drive_of_letter(p[0]);
        if (d < 0 || d >= DOS_MAX_DRIVES || !mounted(d)) return FAT_EINVAL;
        p += 2;
    } else if (up(p[0]) >= 'A' && up(p[0]) <= 'Z' && p[1] == ':') {
        d = drive_of_letter(p[0]);
        if (d < 0 || d >= DOS_MAX_DRIVES || !mounted(d)) return FAT_EINVAL;
        p += 2;
    }
    char comps[16][FAT_LFN_MAX];
    int n = 0;
    char raw[DOS_MAX_PATH];
    if (*p == '\\' || *p == '/') {
        raw[0] = 0;
        strcpy(raw, p);
    } else {
        const char *cwd = g_dos.drives[d].cwd;
        if (strlen(cwd) + strlen(p) + 2 >= sizeof(raw)) return FAT_ENOPATH;
        strcpy(raw, cwd);
        if (*p) {
            if (raw[0]) strcat(raw, "\\");
            strcat(raw, p);
        }
    }
    if (normalize(raw, comps, 16, &n)) return FAT_ENOPATH;
    join_comps(comps, n, abs, absmax);
    if (drive) *drive = d;
    return FAT_OK;
}

/* resolve an absolute relative path to a directory cluster */
static int resolve_dir(int drive, const char *abs, u32 *out_cluster)
{
    char comps[16][FAT_LFN_MAX];
    int n = 0;
    if (normalize(abs, comps, 16, &n)) return FAT_ENOPATH;
    u32 cur = root_cluster_of(drive);
    for (int i = 0; i < n; i++) {
        u8 name11[11];
        int ok;
        dos_fat_name(comps[i], name11, &ok);
        fat_dirent d;
        u32 cl2 = 0;
        int rc = FAT_ENOENT;
        if (ok) rc = fat_find(vol_of(drive), cur, name11, &d, &cl2, NULL, NULL);
        if (rc != FAT_OK) rc = fat_find_long(vol_of(drive), cur, comps[i], &d, &cl2, NULL, NULL);
        if (rc != FAT_OK) return rc;
        if (!(d.raw[11] & 0x10)) return FAT_ENOTDIR;
        cur = cl2;
    }
    *out_cluster = cur;
    return FAT_OK;
}

/* resolve an entry: fill dirent, parent cluster, 11-byte name.
 * Returns FAT_OK or error. abs="" means "the root itself" -> *is_root=1 */
static int resolve_entry(int drive, const char *abs, fat_dirent *out, u32 *parent,
                         u8 name11_out[11], int *is_root)
{
    char comps[16][FAT_LFN_MAX];
    int n = 0;
    if (normalize(abs, comps, 16, &n)) return FAT_ENOPATH;
    *is_root = (n == 0);
    if (n == 0) {
        if (out) memset(out->raw, 0, 32), out->raw[11] = 0x10;
        if (parent) *parent = 0;
        if (name11_out) memset(name11_out, ' ', 11);
        return FAT_OK;
    }
    u32 cur = root_cluster_of(drive);
    for (int i = 0; i < n - 1; i++) {
        u8 nm[11];
        int ok;
        dos_fat_name(comps[i], nm, &ok);
        fat_dirent d;
        u32 cl2 = 0;
        int rc = FAT_ENOENT;
        if (ok) rc = fat_find(vol_of(drive), cur, nm, &d, &cl2, NULL, NULL);
        if (rc != FAT_OK) rc = fat_find_long(vol_of(drive), cur, comps[i], &d, &cl2, NULL, NULL);
        if (rc != FAT_OK) return rc;
        if (!(d.raw[11] & 0x10)) return FAT_ENOTDIR;
        cur = cl2;
    }
    u8 nm[11];
    int ok;
    dos_fat_name(comps[n - 1], nm, &ok);
    fat_dirent d;
    u32 cl = 0;
    int rc = FAT_ENOENT;
    if (ok) rc = fat_find(vol_of(drive), cur, nm, &d, &cl, NULL, NULL);
    if (rc != FAT_OK) rc = fat_find_long(vol_of(drive), cur, comps[n - 1], &d, &cl, NULL, NULL);
    /* fill parent/name even if the entry was not found (create paths need it) */
    if (parent) *parent = cur;
    if (name11_out) memcpy(name11_out, nm, 11);
    if (rc != FAT_OK) return rc;
    if (out) *out = d;
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* directory operations                                                */
/* ------------------------------------------------------------------ */
int dos_chdir(const char *path)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    u32 cl;
    rc = resolve_dir(drive, abs, &cl);
    if (rc != FAT_OK) return rc;
    g_dos.drives[drive].cwd_cluster = cl;
    strcpy(g_dos.drives[drive].cwd, abs);
    g_dos.cur_drive = drive;
    return FAT_OK;
}

int dos_mkdir(const char *path)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    if (vol_of(drive)->read_only) return FAT_EROFS;
    char comps[16][FAT_LFN_MAX];
    int n = 0;
    if (normalize(abs, comps, 16, &n)) return FAT_ENOPATH;
    if (n == 0) return FAT_EINVAL; /* cannot create root */
    u8 nm[11];
    int ok;
    dos_fat_name(comps[n - 1], nm, &ok);
    if (!ok) return FAT_EINVAL;
    char parent_abs[DOS_MAX_PATH];
    join_comps((const char (*)[FAT_LFN_MAX])comps, n - 1, parent_abs, sizeof(parent_abs));
    u32 parent;
    rc = resolve_dir(drive, parent_abs, &parent);
    if (rc != FAT_OK) return rc;
    u32 newcl;
    return fat_mkdir(vol_of(drive), parent, nm, &newcl);
}

int dos_rmdir(const char *path)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    if (vol_of(drive)->read_only) return FAT_EROFS;
    fat_dirent d;
    u32 parent;
    u8 nm[11];
    int root;
    rc = resolve_entry(drive, abs, &d, &parent, nm, &root);
    if (rc != FAT_OK) {
#ifdef UEFIDOS_HOST
        plat_puts("[open-fail] ");
        plat_puts(path);
        plat_puts("  (abs ");
        plat_puts(abs);
        plat_puts(")\n");
#endif
        return rc;
    }
    if (root) return FAT_EACCES;
    if (!(d.raw[11] & 0x10)) return FAT_ENOTDIR;
    return fat_rmdir(vol_of(drive), parent, nm);
}

int dos_unlink(const char *path)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    if (vol_of(drive)->read_only) return FAT_EROFS;
    fat_dirent d;
    u32 parent;
    u8 nm[11];
    int root;
    rc = resolve_entry(drive, abs, &d, &parent, nm, &root);
    if (rc != FAT_OK) {
#ifdef UEFIDOS_HOST
        plat_puts("[open-fail] ");
        plat_puts(path);
        plat_puts("  (abs ");
        plat_puts(abs);
        plat_puts(")\n");
#endif
        return rc;
    }
    if (root) return FAT_EACCES;
    if (d.raw[11] & 0x01) return FAT_EACCES;   /* read-only attribute */
    return fat_delete_file(vol_of(drive), parent, nm);
}

int dos_rename(const char *oldp, const char *newp)
{
    int d1, rc;
    char abs1[DOS_MAX_PATH];
    rc = dos_parse_path(oldp, &d1, abs1, sizeof(abs1));
    if (rc != FAT_OK) return rc;
    if (vol_of(d1)->read_only) return FAT_EROFS;
    fat_dirent od;
    u32 parent;
    u8 onm[11];
    int root;
    rc = resolve_entry(d1, abs1, &od, &parent, onm, &root);
    if (rc != FAT_OK) return rc;
    if (root) return FAT_EACCES;

    int d2 = d1;
    char abs2[DOS_MAX_PATH];
    if (strchr(newp, ':') || strchr(newp, '\\') || strchr(newp, '/')) {
        rc = dos_parse_path(newp, &d2, abs2, sizeof(abs2));
        if (rc != FAT_OK) return rc;
        if (d2 != d1) return FAT_EACCES; /* cross-drive move unsupported */
    } else {
        /* plain target name: rename within old directory */
        int n = (int)strlen(abs1);
        while (n > 0 && abs1[n - 1] != '\\') n--;
        if ((size_t)(n + (int)strlen(newp) + 1) > sizeof(abs2)) return FAT_ENOPATH;
        memcpy(abs2, abs1, (size_t)n);
        strcpy(abs2 + n, newp);
    }

    char c1[16][FAT_LFN_MAX], c2[16][FAT_LFN_MAX];
    int n1 = 0, n2 = 0;
    if (normalize(abs1, c1, 16, &n1) || normalize(abs2, c2, 16, &n2)) return FAT_ENOPATH;
    if (n2 == 0) return FAT_EINVAL;
    char p1[DOS_MAX_PATH], p2[DOS_MAX_PATH];
    join_comps((const char (*)[FAT_LFN_MAX])c1, n1 - 1, p1, sizeof(p1));
    join_comps((const char (*)[FAT_LFN_MAX])c2, n2 - 1, p2, sizeof(p2));
    if (strcmp(p1, p2) != 0) return FAT_EACCES; /* rename across dirs unsupported */

    u8 nnm[11];
    int ok;
    dos_fat_name(c2[n2 - 1], nnm, &ok);
    if (!ok) return FAT_EINVAL;
    fat_dirent chk;
    if (fat_find(vol_of(d1), parent, nnm, &chk, NULL, NULL, NULL) == FAT_OK)
        return FAT_EEXIST;
    return fat_rename_entry(vol_of(d1), parent, onm, nnm);
}

/* ------------------------------------------------------------------ */
/* file handles                                                        */
/* ------------------------------------------------------------------ */
static int alloc_handle(int *out)
{
    int limit = g_dos.files_limit;
    if (limit > DOS_HANDLE_MAX) limit = DOS_HANDLE_MAX;
    if (limit < 5) limit = 5;
    for (int i = 5; i < limit; i++) {
        if (!g_dos.handles[i].used) {
            memset(&g_dos.handles[i], 0, sizeof(dos_handle));
            g_dos.handles[i].used = 1;
            *out = i;
            return FAT_OK;
        }
    }
    return FAT_EMFILE; /* DOS error 4: too many open files */
}

/* DOS 4.0 SHARE compatibility check: is a new open (open_mode/share) allowed
 * given the handles already open on the same file? */
static int share_conflict(int drive, u32 parent, const u8 nm[11],
                          int open_mode, int share)
{
    int want_read  = (open_mode != 1);
    int want_write = (open_mode != 0);
    int deny_rw = 0, deny_write = 0, deny_read = 0;
    int others_read = 0, others_write = 0;
    int others_other_psp = 0;
    for (int i = 5; i < DOS_HANDLE_MAX; i++) {
        dos_handle *H = &g_dos.handles[i];
        if (!H->used || H->console) continue;
        if (H->drive != drive || H->parent_cluster != parent) continue;
        if (memcmp(H->name11, nm, 11) != 0) continue;
        if (H->psp != g_dos.psp_seg) others_other_psp = 1;
        switch (H->share_mode) {
        case DOS_SHARE_DENY_RW:    deny_rw = 1;    break;
        case DOS_SHARE_DENY_WRITE: deny_write = 1; break;
        case DOS_SHARE_DENY_READ:  deny_read = 1;  break;
        default: break;                     /* DENY_NONE: no restriction */
        }
        if (H->open_mode != 1) others_read = 1;
        if (H->open_mode != 0) others_write = 1;
    }
    /* (a) existing share modes vs the access this open requests */
    if (deny_rw) return 1;
    if (want_read && deny_read) return 1;
    if (want_write && (deny_write || deny_rw)) return 1;
    /* (b) the share mode this open requests vs accesses already granted */
    if (others_read && (share == DOS_SHARE_DENY_READ || share == DOS_SHARE_DENY_RW)) return 1;
    if (others_write && (share == DOS_SHARE_DENY_WRITE || share == DOS_SHARE_DENY_RW)) return 1;
    /* compat mode only conflicts with opens from a *different* PSP */
    if (share == DOS_SHARE_COMPAT && others_other_psp) return 1;
    return 0;
}

int dos_open(const char *path, int mode, int *out)
{
    return dos_open_ex(path, (mode != 0) ? 2 : 0, DOS_SHARE_COMPAT, out);
}

int dos_open_ex(const char *path, int open_mode, int share, int *out)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    fat_dirent d;
    u32 parent;
    u8 nm[11];
    int root;
    rc = resolve_entry(drive, abs, &d, &parent, nm, &root);
    if (rc != FAT_OK) {
#ifdef UEFIDOS_HOST
        plat_puts("[open-fail] ");
        plat_puts(path);
        plat_puts("  (abs ");
        plat_puts(abs);
        plat_puts(")\n");
#endif
        return rc;
    }
    if (root) return FAT_EISDIR;
    if (d.raw[11] & 0x10) return FAT_EISDIR;
    if (open_mode != 0) {
        if (vol_of(drive)->read_only) return FAT_EROFS;
        if (d.raw[11] & 0x01) return FAT_EACCES; /* read-only attribute */
    }
    if (share_conflict(drive, parent, nm, open_mode, share)) return FAT_EACCES;
    int h;
    rc = alloc_handle(&h);
    if (rc != FAT_OK) return rc;
    dos_handle *H = &g_dos.handles[h];
    H->drive = drive;
    H->parent_cluster = parent;
    memcpy(H->name11, nm, 11);
    H->attr = d.raw[11];
    H->cluster = fat_entry_cluster(&d);
    H->size = fat_entry_size(&d);
    H->pos = 0;
    H->open_mode = (u8)open_mode;
    H->share_mode = (u8)share;
    H->psp = g_dos.psp_seg;
    H->lock_count = 0;
    *out = h;
    return FAT_OK;
}

int dos_creat(const char *path, int *out)
{
    return dos_creat_ex(path, 0x20, DOS_SHARE_COMPAT, 2, out);
}

/* last component of an absolute relative path ("SUB\Name.txt" -> "Name.txt") */
static void leaf_of(const char *abs, char *out, int max)
{
    const char *start = abs;
    for (const char *p = abs; *p; p++)
        if (*p == '\\' || *p == '/') start = p + 1;
    int i = 0;
    for (; start[i] && i < max - 1; i++) out[i] = start[i];
    out[i] = 0;
}

/* Build the 8.3 alias VFAT pairs with a long name: up to six usable characters
 * of the base name, "~1".."~9", and the first three characters of the extension. */
static int make_alias(int drive, u32 parent, const char *longname, u8 out11[11])
{
    char base[9];
    char ext[4];
    int b = 0, e = 0;
    const char *p = longname;
    while (*p && *p != '.') {
        char c = up(*p);
        if (c > ' ' && c != '\\' && c != '/' && c != '+' && c != ',' && c != ';' &&
            c != '=' && c != '[' && c != ']' && b < 6)
            base[b++] = c;
        p++;
    }
    if (b == 0) { base[b++] = 'F'; base[b++] = 'I'; base[b++] = 'L'; base[b++] = 'E'; }
    while (*p && *p != '.') p++;
    if (*p == '.') {
        p++;
        while (*p && e < 3) {
            char c = up(*p);
            if (c > ' ' && c != '.') ext[e++] = c;
            p++;
        }
    }
    for (int n = 1; n <= 9; n++) {
        u8 cand[11];
        memset(cand, ' ', 11);
        for (int i = 0; i < b; i++) cand[i] = (u8)base[i];
        cand[b] = '~';
        cand[b + 1] = (u8)('0' + n);
        for (int i = 0; i < e; i++) cand[8 + i] = (u8)ext[i];
        fat_dirent d;
        if (fat_find(vol_of(drive), parent, cand, &d, NULL, NULL, NULL) == FAT_ENOENT) {
            memcpy(out11, cand, 11);
            return 0;
        }
    }
    return -1;   /* every alias taken */
}

int dos_creat_ex(const char *path, int attr, int share, int open_mode, int *out)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    if (vol_of(drive)->read_only) return FAT_EROFS;
    fat_dirent d;
    u32 parent = 0;
    u8 nm[11];
    int root;
    rc = resolve_entry(drive, abs, &d, &parent, nm, &root);
    if (rc == FAT_OK) {
        if (root || (d.raw[11] & 0x10)) return FAT_EISDIR;
        /* AH=3Ch truncates an existing file: shrink the directory entry first,
         * then release the chain.  The entry location must be looked up into a
         * SCRATCH dirent -- fat_find() overwrites whatever it is given, so
         * passing &d here used to restore the original cluster/size and leave a
         * freed cluster pointed to by a 44-byte file (later writes then found
         * FAT[cluster] == 0 and reported "corrupt"). */
        u32 cl = fat_entry_cluster(&d);
        u32 lba = 0, idx = 0;
        fat_dirent scratch;
        d.raw[26] = d.raw[27] = 0;
        d.raw[20] = d.raw[21] = 0;
        fat_entry_set_size(&d, 0);
        fat_entry_set_cluster(&d, 0);
        if (fat_find(vol_of(drive), parent, nm, &scratch, NULL, &lba, &idx) == FAT_OK) {
            rc = fat_write_entry_at(vol_of(drive), lba, idx, &d);
            if (rc != FAT_OK) return rc;
        }
        if (cl >= 2) {
            rc = fat_free_chain(vol_of(drive), cl);
            if (rc != FAT_OK) return rc;
        }
    } else if (rc == FAT_ENOENT) {
        /* a name that cannot be expressed in 8.3 gets an LFN chain plus a
         * generated alias, so DOS programs still see a legal short name */
        char leaf[FAT_LFN_MAX];
        leaf_of(abs, leaf, sizeof(leaf));
        u8 cn[11];
        int cok = 0;
        dos_fat_name(leaf, cn, &cok);
        const char *lname = NULL;
        if (!cok) {
            if (make_alias(drive, parent, leaf, cn) != 0) return FAT_EACCES;
            lname = leaf;
        } else {
            memcpy(cn, nm, 11);
        }
        rc = fat_create_file_long(vol_of(drive), parent, cn, lname, 0x20);
        if (rc == FAT_OK && attr && attr != 0x20)
            fat_set_attr(vol_of(drive), parent, cn, (u8)attr);
        if (rc != FAT_OK) return rc;
        fat_dirent d2;
        u32 p2;
        rc = resolve_entry(drive, abs, &d2, &p2, nm, &root);
        if (rc != FAT_OK) return rc;
        d = d2;
        /* an LFN-created file is tracked by its generated 8.3 alias: writes and
         * the final size update look the entry up by that short name */
        if (lname) memcpy(nm, cn, 11);
    } else {
        return rc;
    }
    if (share_conflict(drive, parent, nm, open_mode, share)) return FAT_EACCES;
    int h;
    rc = alloc_handle(&h);
    if (rc != FAT_OK) return rc;
    dos_handle *H = &g_dos.handles[h];
    H->drive = drive;
    H->parent_cluster = parent;
    memcpy(H->name11, nm, 11);
    H->attr = (u8)(d.raw[11] ? d.raw[11] : 0x20);
    H->cluster = fat_entry_cluster(&d);
    H->size = fat_entry_size(&d);
    H->pos = 0;
    H->open_mode = (u8)open_mode;
    H->share_mode = (u8)share;
    H->psp = g_dos.psp_seg;
    H->lock_count = 0;
    *out = h;
    return FAT_OK;
}

static int flush_handle(dos_handle *H)
{
    if (H->console) return FAT_OK;
    fat_vol *v = vol_of(H->drive);
    if (v->read_only) return FAT_OK;
    fat_dirent d;
    u32 lba, idx;
    int rc = fat_find(v, H->parent_cluster, H->name11, &d, NULL, &lba, &idx);
    if (rc != FAT_OK) return rc;
    fat_entry_set_cluster(&d, H->cluster);
    fat_entry_set_size(&d, H->size);
    return fat_write_entry_at(v, lba, idx, &d);
}

int dos_close(int h)
{
    if (h < 0 || h >= DOS_HANDLE_MAX) return FAT_EINVAL;
    dos_handle *H = &g_dos.handles[h];
    if (!H->used) return FAT_EINVAL;
    int rc = flush_handle(H);
    H->used = 0;
    return rc;
}

int dos_read(int h, void *buf, u32 len, u32 *got)
{
    *got = 0;
    if (h < 0 || h >= DOS_HANDLE_MAX) return FAT_EINVAL;
    dos_handle *H = &g_dos.handles[h];
    if (!H->used) return FAT_EINVAL;
    if (H->console) {
        /* read a line from the console (echoed) */
        char *p = (char *)buf;
        u32 n = 0;
        for (;;) {
            int c = -1;
            while (c < 0) {
                c = plat_kbd_poll();
                if (c < 0) plat_sleep_ms(10);
            }
            if (c == 8) {
                if (n > 0) { n--; plat_putc(8); plat_putc(' '); plat_putc(8); }
                continue;
            }
            if (c == '\r' || c == '\n') {
                plat_putc('\r'); plat_putc('\n');
                if (n + 2 < len) { p[n++] = '\r'; p[n++] = '\n'; }
                else if (n < len) p[n++] = '\r';
                break;
            }
            if (c >= 32 && c < 127 && n < len) { p[n++] = (char)c; plat_putc((char)c); }
        }
        *got = n;
        return FAT_OK;
    }
    if (H->pos >= H->size) { *got = 0; return FAT_OK; }
    u32 max = H->size - H->pos;
    if (len > max) len = max;
    int rc = fat_read_chain_byte(vol_of(H->drive), H->cluster, H->size, H->pos, buf, len, got);
    if (rc == FAT_OK) H->pos += *got;
    return rc;
}

int dos_write(int h, const void *buf, u32 len, u32 *put)
{
    *put = 0;
    if (h < 0 || h >= DOS_HANDLE_MAX) return FAT_EINVAL;
    dos_handle *H = &g_dos.handles[h];
    if (!H->used) return FAT_EINVAL;
    if (H->console) {
        const char *p = (const char *)buf;
        for (u32 i = 0; i < len; i++) plat_putc(p[i]);
        *put = len;
        return FAT_OK;
    }
    fat_vol *v = vol_of(H->drive);
    if (v->read_only) return FAT_EROFS;
    int rc = fat_write_chain(v, &H->cluster, &H->size, H->pos, buf, len);
    if (rc == FAT_OK) {
        H->pos += len;
        *put = len;
        flush_handle(H);
    }
    return rc;
}

int dos_lseek(int h, u32 pos)
{
    if (h < 0 || h >= DOS_HANDLE_MAX) return FAT_EINVAL;
    dos_handle *H = &g_dos.handles[h];
    if (!H->used || H->console) return FAT_EINVAL;
    H->pos = pos;
    return FAT_OK;
}

int dos_attr_get(const char *path, u8 *attr)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    fat_dirent d;
    u32 parent;
    u8 nm[11];
    int root;
    rc = resolve_entry(drive, abs, &d, &parent, nm, &root);
    if (rc != FAT_OK) {
#ifdef UEFIDOS_HOST
        plat_puts("[open-fail] ");
        plat_puts(path);
        plat_puts("  (abs ");
        plat_puts(abs);
        plat_puts(")\n");
#endif
        return rc;
    }
    *attr = root ? 0x10 : d.raw[11];
    return FAT_OK;
}

int dos_attr_set(const char *path, u8 attr)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    if (vol_of(drive)->read_only) return FAT_EROFS;
    fat_dirent d;
    u32 parent;
    u8 nm[11];
    int root;
    rc = resolve_entry(drive, abs, &d, &parent, nm, &root);
    if (rc != FAT_OK) {
#ifdef UEFIDOS_HOST
        plat_puts("[open-fail] ");
        plat_puts(path);
        plat_puts("  (abs ");
        plat_puts(abs);
        plat_puts(")\n");
#endif
        return rc;
    }
    if (root) return FAT_EACCES;
    return fat_set_attr(vol_of(drive), parent, nm, attr);
}

/* ------------------------------------------------------------------ */
/* search (DIR / FindFirst / FindNext)                                 */
/* ------------------------------------------------------------------ */
static int pattern_to_11(const char *pat, u8 out[11])
{
    char base[9] = {0}, ext[4] = {0};
    const char *p = pat;
    int b = 0, e = 0, ine = 0;
    if (!pat || !*pat) return -1;
    /* name field */
    while (*p && *p != '.') {
        if (*p == '*') {
            while (b < 8) base[b++] = '?';
            p++;
            continue;
        }
        if (*p == '?') { if (b < 8) base[b++] = '?'; p++; continue; }
        if (*p == ' ' || *p == '/' || *p == '\\') return -1;
        if (b < 8) base[b++] = up(*p); else return -1;
        p++;
    }
    if (*p == '.') {
        ine = 1;
        p++;
        while (*p) {
            if (*p == '*') {
                while (e < 3) ext[e++] = '?';
                p++;
                continue;
            }
            if (*p == '?') { if (e < 3) ext[e++] = '?'; p++; continue; }
            if (*p == ' ' || *p == '/' || *p == '\\') return -1;
            if (e < 3) ext[e++] = up(*p); else return -1;
            p++;
        }
    }
    memset(out, ' ', 11);
    for (int i = 0; i < b; i++) out[i] = (u8)base[i];
    for (int i = 0; i < e; i++) out[8 + i] = (u8)ext[i];
    (void)ine;
    return 0;
}

static int name_match(const u8 pat[11], const u8 nm[11])
{
    for (int i = 0; i < 11; i++) {
        if (pat[i] != '?' && pat[i] != nm[i]) return 0;
    }
    return 1;
}

static void fill_find_out(const fat_dirent *d, char name[13], u8 *attr, u32 *size,
                          u16 *dt, u16 *dd)
{
    dos_dirname_to_dosname(d->raw, name);
    if (attr) *attr = d->raw[11];
    if (size) *size = fat_entry_size(d);
    if (dt) *dt = (u16)(d->raw[22] | (d->raw[23] << 8));
    if (dd) *dd = (u16)(d->raw[24] | (d->raw[25] << 8));
}

struct find_pass {
    const u8 *pat;
    u8 attr_mask;
    u32 want;       /* index of the match we want */
    u32 count;
    int found;
    fat_dirent d;
    char lname[FAT_LFN_MAX];
};

static int find_pass_take(struct find_pass *f, const fat_dirent *d, const char *longname)
{
    u8 attr = d->raw[11];
    if (attr & 0x08) return 0;          /* volume label */
    if (d->raw[0] == '.') return 0;     /* . and .. */
    if ((attr & 0x10) && !(f->attr_mask & 0x10)) return 0;
    if ((attr & 0x18) && !(attr & f->attr_mask)) return 0;
    if (!name_match(f->pat, d->raw)) return 0;
    if (f->count == f->want) {
        f->d = *d;
        if (longname) {
            int i = 0;
            for (; longname[i] && i < FAT_LFN_MAX - 1; i++) f->lname[i] = longname[i];
            f->lname[i] = 0;
        } else {
            f->lname[0] = 0;
        }
        f->found = 1;
        return 1;
    }
    f->count++;
    return 0;
}

static int find_pass_cb_ex(fat_vol *v, const fat_dirent *d, u32 lba, u32 off,
                           const char *longname, void *arg)
{
    UFD_UNUSED(v); UFD_UNUSED(lba); UFD_UNUSED(off);
    return find_pass_take((struct find_pass *)arg, d, longname);
}

int dos_find_first(const char *pattern, u8 attr_mask, dos_find *f,
                   char name_out[13], u8 *attr_out, u32 *size_out,
                   u16 *time_out, u16 *date_out)
{
    return dos_find_first_ex(pattern, attr_mask, f, name_out, attr_out, size_out,
                             time_out, date_out, NULL, 0);
}

int dos_find_first_ex(const char *pattern, u8 attr_mask, dos_find *f,
                      char name_out[13], u8 *attr_out, u32 *size_out,
                      u16 *time_out, u16 *date_out, char *long_out, int long_max)
{
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(pattern, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    char comps[16][FAT_LFN_MAX];
    int n = 0;
    if (normalize(abs, comps, 16, &n)) return FAT_ENOPATH;
    if (n == 0) return FAT_EINVAL;
    u8 nm[11];
    if (pattern_to_11(comps[n - 1], nm)) return FAT_EINVAL;
    char parent_abs[DOS_MAX_PATH];
    join_comps((const char (*)[FAT_LFN_MAX])comps, n - 1, parent_abs, sizeof(parent_abs));
    u32 dir;
    rc = resolve_dir(drive, parent_abs, &dir);
    if (rc != FAT_OK) return rc;
    f->drive = drive;
    f->dir_cluster = dir;
    memcpy(f->pat, nm, 11);
    f->attr_mask = attr_mask;
    f->index = 0;
    return dos_find_next_ex(f, name_out, attr_out, size_out, time_out, date_out,
                            long_out, long_max);
}

int dos_find_next(dos_find *f, char name_out[13], u8 *attr_out, u32 *size_out,
                  u16 *time_out, u16 *date_out)
{
    return dos_find_next_ex(f, name_out, attr_out, size_out, time_out, date_out, NULL, 0);
}

int dos_find_next_ex(dos_find *f, char name_out[13], u8 *attr_out, u32 *size_out,
                     u16 *time_out, u16 *date_out, char *long_out, int long_max)
{
    struct find_pass fp;
    memset(&fp, 0, sizeof(fp));
    fp.pat = f->pat;
    fp.attr_mask = f->attr_mask;
    fp.want = f->index;
    int src = fat_dir_scan_ex(vol_of(f->drive), f->dir_cluster, find_pass_cb_ex, &fp);
    if (src != FAT_OK) return src;
    if (!fp.found) return FAT_ENOENT;
    f->index++;
    fill_find_out(&fp.d, name_out, attr_out, size_out, time_out, date_out);
    if (long_out && long_max > 0) {
        int i = 0;
        for (; fp.lname[i] && i < long_max - 1; i++) long_out[i] = fp.lname[i];
        long_out[i] = 0;
    }
    return FAT_OK;
}

/* long name of an existing entry; out is "" when it only has an 8.3 name */
struct lname_ctx {
    const u8 *nm;
    char *out;
    int max;
    int found;
};

static int lname_cb(fat_vol *v, const fat_dirent *d, u32 lba, u32 off,
                    const char *longname, void *arg)
{
    struct lname_ctx *c = (struct lname_ctx *)arg;
    UFD_UNUSED(v); UFD_UNUSED(lba); UFD_UNUSED(off);
    if (memcmp(d->raw, c->nm, 11) != 0) return 0;
    int i = 0;
    if (longname) for (; longname[i] && i < c->max - 1; i++) c->out[i] = longname[i];
    c->out[i] = 0;
    c->found = 1;
    return 1;
}

int dos_longname(const char *path, char *out, int max)
{
    if (!out || max <= 0) return FAT_EINVAL;
    out[0] = 0;
    int drive, rc;
    char abs[DOS_MAX_PATH];
    rc = dos_parse_path(path, &drive, abs, sizeof(abs));
    if (rc != FAT_OK) return rc;
    char comps[16][FAT_LFN_MAX];
    int n = 0;
    if (normalize(abs, comps, 16, &n)) return FAT_ENOPATH;
    if (n == 0) return FAT_EINVAL;
    char parent_abs[DOS_MAX_PATH];
    join_comps((const char (*)[FAT_LFN_MAX])comps, n - 1, parent_abs, sizeof(parent_abs));
    u32 dir = root_cluster_of(drive);
    rc = resolve_dir(drive, parent_abs, &dir);
    if (rc != FAT_OK) return rc;
    u8 nm[11];
    int ok = 0;
    dos_fat_name(comps[n - 1], nm, &ok);
    if (!ok) return FAT_ENOENT;         /* caller has to use the long name directly */
    struct lname_ctx c;
    memset(&c, 0, sizeof(c));
    c.nm = nm;
    c.out = out;
    c.max = max;
    rc = fat_dir_scan_ex(vol_of(drive), dir, lname_cb, &c);
    if (rc != FAT_OK) return rc;
    return c.found ? FAT_OK : FAT_ENOENT;
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */
void dos_get_cwd(char *out, int max)
{
    int d = g_dos.cur_drive;
    if (!mounted(d)) { strncpy(out, "C:\\", (size_t)max); return; }
    int n = 0;
    if (n < max - 3) { out[n++] = (char)('A' + d); out[n++] = ':'; }
    const char *cwd = g_dos.drives[d].cwd;
    if (*cwd) {
        if (n < max - 1) out[n++] = '\\';
        int len = (int)strlen(cwd);
        if (n + len >= max) len = max - n - 1;
        memcpy(out + n, cwd, (size_t)len);
        n += len;
    } else if (n < max - 1) {
        out[n++] = '\\';
    }
    out[n] = 0;
}

void dos_get_prompt(char *out, int max)
{
    if (g_dos.prompt[0] && strcmp(g_dos.prompt, "$P$G") != 0) {
        strncpy(out, g_dos.prompt, (size_t)max);
        out[max - 1] = 0;
        return;
    }
    char cwd[DOS_MAX_PATH];
    dos_get_cwd(cwd, sizeof(cwd));
    int n = 0;
    for (const char *p = cwd; *p && n < max - 2; p++) out[n++] = *p;
    if (n < max - 1) out[n++] = '>';
    out[n] = 0;
}

/* ------------------------------------------------------------------ *
 *  DOS 4.0 CONFIG.SYS subset: FILES=, FCBS=, LASTDRIVE=
 *  (BUFFERS=, DEVICE=, SHELL=, STACKS=, SWITCHES=, REM/; are accepted
 *   and ignored - UEFI-DOS does not need a configurable BIOS layer.)
 * ------------------------------------------------------------------ */
static char cfg_upper(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static int cfg_num(const char *p, int dflt)
{
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return dflt;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 100000) break;
        p++;
    }
    return v;
}

/* ------------------------------------------------------------------ *
 *  SETVER: per-program DOS version overrides
 * ------------------------------------------------------------------ */
/* keep only the file name, upper-cased, so "C:\WINDOWS\WIN.COM" matches "WIN.COM" */
static void setver_name(const char *path, char out[13])
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    int n = 0;
    while (base[n] && n < 12) {
        char c = base[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[n] = c;
        n++;
    }
    out[n] = 0;
}

int dos_setver_lookup(const char *path, int *major, int *minor)
{
    char want[13];
    if (!path || !*path) return 0;
    setver_name(path, want);
    for (int i = 0; i < g_dos.ver_fix_count; i++) {
        if (strcmp(g_dos.ver_fix[i].prog, want) == 0) {
            if (major) *major = g_dos.ver_fix[i].major;
            if (minor) *minor = g_dos.ver_fix[i].minor;
            return 1;
        }
    }
    return 0;
}

int dos_setver_add(const char *prog, int major, int minor)
{
    char nm[13];
    setver_name(prog, nm);
    if (!nm[0]) return FAT_EINVAL;
    if (major < 0 || major > 9 || minor < 0 || minor > 99) return FAT_EINVAL;
    for (int i = 0; i < g_dos.ver_fix_count; i++) {
        if (strcmp(g_dos.ver_fix[i].prog, nm) == 0) {
            g_dos.ver_fix[i].major = (u8)major;
            g_dos.ver_fix[i].minor = (u8)minor;
            return FAT_OK;
        }
    }
    if (g_dos.ver_fix_count >= DOS_SETVER_MAX) return FAT_ENOSPC;
    int i = g_dos.ver_fix_count++;
    memcpy(g_dos.ver_fix[i].prog, nm, sizeof(nm));
    g_dos.ver_fix[i].major = (u8)major;
    g_dos.ver_fix[i].minor = (u8)minor;
    return FAT_OK;
}

int dos_setver_del(const char *prog)
{
    char nm[13];
    setver_name(prog, nm);
    for (int i = 0; i < g_dos.ver_fix_count; i++) {
        if (strcmp(g_dos.ver_fix[i].prog, nm) == 0) {
            for (int j = i; j + 1 < g_dos.ver_fix_count; j++)
                g_dos.ver_fix[j] = g_dos.ver_fix[j + 1];
            g_dos.ver_fix_count--;
            return FAT_OK;
        }
    }
    return FAT_ENOENT;
}

void dos_read_config_sys(void)
{
    int h;
    char buf[1024];
    u32 got = 0;
    if (!mounted(2)) return;
    if (dos_open("C:\\CONFIG.SYS", 0, &h) != FAT_OK) return;
    if (dos_read(h, buf, sizeof(buf) - 1, &got) != FAT_OK) got = 0;
    dos_close(h);
    buf[got] = 0;

    char *p = buf;
    while (*p) {
        char line[160];
        int n = 0;
        while (*p && *p != '\r' && *p != '\n' && n < (int)sizeof(line) - 1)
            line[n++] = *p++;
        line[n] = 0;
        while (*p == '\r' || *p == '\n') p++;

        /* keyword = text (case-insensitive) */
        char key[16];
        int kn = 0;
        const char *q = line;
        while (*q == ' ' || *q == '\t' || *q == '@') q++;
        while (*q && *q != '=' && *q != ' ' && *q != '\t' && kn < (int)sizeof(key) - 1)
            key[kn++] = cfg_upper(*q++);
        key[kn] = 0;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '=') q++;
        while (*q == ' ' || *q == '\t') q++;

        if (strcmp(key, "FILES") == 0) {
            int v = cfg_num(q, DOS_FILES_DEFAULT);
            if (v < 5) v = 5;
            if (v > DOS_HANDLE_MAX) v = DOS_HANDLE_MAX;
            g_dos.files_limit = v;
        } else if (strcmp(key, "FCBS") == 0) {
            g_dos.fcb_limit = cfg_num(q, 4);
            if (g_dos.fcb_limit > 255) g_dos.fcb_limit = 255;
        } else if (strcmp(key, "LASTDRIVE") == 0) {
            int c = cfg_upper((char)*q);
            if (c >= 'A' && c <= 'Z') {
                int d = c - 'A';
                if (d > DOS_MAX_DRIVES - 1) d = DOS_MAX_DRIVES - 1;
                if (d < 0) d = 0;
                g_dos.last_drive = d;
            }
        } else if (strcmp(key, "SETVER") == 0 || strcmp(key, "DOSVER") == 0) {
            /* SETVER NAME 3.30  -- report another DOS version to that program */
            char nm[16];
            int nn = 0;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != ' ' && *q != '\t' && *q != ',' && nn < 15)
                nm[nn++] = *q++;
            nm[nn] = 0;
            while (*q == ' ' || *q == '\t' || *q == ',') q++;
            int maj = 0, min = 0;
            while (*q >= '0' && *q <= '9') { maj = maj * 10 + (*q - '0'); q++; }
            while (*q && *q != '.' && *q != ' ') q++;
            if (*q == '.') {
                q++;
                int digits = 0;
                while (*q >= '0' && *q <= '9') { min = min * 10 + (*q - '0'); q++; digits++; }
                if (digits == 1) min *= 10;     /* "3.3" means 3.30 */
            }
            if (nn > 0 && maj > 0) dos_setver_add(nm, maj, min);
        }
        /* everything else: ignored */
    }
}

/* ------------------------------------------------------------------ *
 *  DOS 4.0 SHARE byte-range locks (INT 21h AH=5Ch)
 * ------------------------------------------------------------------ */
static u32 lock_end(u32 off, u32 len)
{
    u64 e = (u64)off + (u64)len;
    if (e > 0xFFFFFFFFull) e = 0xFFFFFFFFull;
    return (u32)e;
}

int dos_lock_region(int h, u32 off, u32 len)
{
    if (h < 0 || h >= DOS_HANDLE_MAX) return FAT_EINVAL;
    dos_handle *H = &g_dos.handles[h];
    if (!H->used || H->console) return FAT_EINVAL;
    if (H->lock_count >= DOS_LOCK_MAX) return FAT_EMFILE;
    u32 a1 = off, b1 = lock_end(off, len);
    for (int i = 5; i < DOS_HANDLE_MAX; i++) {
        dos_handle *O = &g_dos.handles[i];
        if (!O->used || O->console || i == h) continue;
        if (O->drive != H->drive || O->parent_cluster != H->parent_cluster) continue;
        if (memcmp(O->name11, H->name11, 11) != 0) continue;
        for (int j = 0; j < O->lock_count; j++) {
            u32 a2 = O->locks[j].start;
            u32 b2 = lock_end(a2, O->locks[j].len);
            if (a1 < b2 && a2 < b1) return FAT_EACCES; /* region already locked */
        }
    }
    H->locks[H->lock_count].start = off;
    H->locks[H->lock_count].len = len;
    H->lock_count++;
    return FAT_OK;
}

int dos_unlock_region(int h, u32 off, u32 len)
{
    if (h < 0 || h >= DOS_HANDLE_MAX) return FAT_EINVAL;
    dos_handle *H = &g_dos.handles[h];
    if (!H->used || H->console) return FAT_EINVAL;
    for (int j = 0; j < H->lock_count; j++) {
        if (H->locks[j].start == off && H->locks[j].len == len) {
            for (int k = j; k + 1 < H->lock_count; k++) H->locks[k] = H->locks[k + 1];
            H->lock_count--;
            return FAT_OK;
        }
    }
    return FAT_EACCES; /* not locked by this handle */
}
