/*
 * UEFI-DOS -- native 32/64-bit DOS services API implementation.
 *
 * Thin, width-agnostic wrapper over kernel/dos.c.  The functions here are
 * exactly the operations INT 21h performs; error reporting is the DOS error
 * number (same values as INT 21h AL), not a UEFI status.
 */
#include "uefidos.h"
#include "uefidos_config.h"
#include "api/dosapi.h"
#include "kernel/dos.h"
#include "emu/emu.h"
#include "plat.h"

static u16 err_of(int rc)
{
    return rc == FAT_OK ? 0 : (u16)dos_error_code(rc);
}

/* one find state, like the DTA-based search state of INT 21h AH=4Eh */
static dos_find g_api_find;

/* ------------------------------------------------------------------ */
/* INT 21h counterparts                                                */
/* ------------------------------------------------------------------ */
static u16 dapi_get_version(void)
{
    return (u16)((UEFIDOS_DOSVER_COMPAT_MINOR << 8) | UEFIDOS_DOSVER_MAJOR);
}

static u16 dapi_open(const char *path, u16 mode, u16 *handle)
{
    int h = -1;
    int rc = dos_open(path, (int)mode, &h);
    if (rc != FAT_OK) return err_of(rc);
    if (handle) *handle = (u16)h;
    return 0;
}

static u16 dapi_create(const char *path, u8 attr, u16 *handle)
{
    int h = -1;
    int rc = dos_creat(path, &h);
    if (rc != FAT_OK) return err_of(rc);
    if (attr) dos_attr_set(path, attr);   /* AH=3Ch DL, applied after creation */
    if (handle) *handle = (u16)h;
    return 0;
}

static u16 dapi_close(u16 handle)
{
    return err_of(dos_close((int)handle));
}

static u16 dapi_read(u16 handle, void *buf, u32 len, u32 *got)
{
    u32 n = 0;
    int rc = dos_read((int)handle, buf, len, &n);
    if (got) *got = n;
    return err_of(rc);
}

static u16 dapi_write(u16 handle, const void *buf, u32 len, u32 *put)
{
    u32 n = 0;
    int rc = dos_write((int)handle, buf, len, &n);
    if (put) *put = n;
    return err_of(rc);
}

static u16 dapi_seek(u16 handle, i32 off, u8 whence, u32 *newpos)
{
    if (handle >= DOS_HANDLE_MAX || !g_dos.handles[handle].used) return 0x06; /* invalid handle */
    u32 base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = g_dos.handles[handle].pos; break;
    case 2: base = g_dos.handles[handle].size; break;
    default: return 0x0F;                      /* invalid sub-function */
    }
    i32 np = (i32)base + off;
    if (np < 0) return 0x19;                   /* seek on device error */
    int rc = dos_lseek((int)handle, (u32)np);
    if (rc != FAT_OK) return err_of(rc);
    if (newpos) *newpos = (u32)np;
    return 0;
}

static u16 dapi_unlink(const char *path) { return err_of(dos_unlink(path)); }
static u16 dapi_mkdir(const char *path)  { return err_of(dos_mkdir(path)); }
static u16 dapi_rmdir(const char *path)  { return err_of(dos_rmdir(path)); }
static u16 dapi_rename(const char *oldp, const char *newp) { return err_of(dos_rename(oldp, newp)); }

static u16 dapi_attr_get(const char *path, u8 *attr)
{
    return err_of(dos_attr_get(path, attr));
}

static u16 dapi_attr_set(const char *path, u8 attr)
{
    return err_of(dos_attr_set(path, attr));
}

static u16 dapi_chdir(const char *path) { return err_of(dos_chdir(path)); }

static u16 dapi_getcwd(char *buf, u32 max)
{
    if (!buf || max == 0) return 0x0F;
    dos_get_cwd(buf, (int)max);
    return 0;
}

static u8 dapi_get_drive(void) { return (u8)g_dos.cur_drive; }

static u16 dapi_set_drive(u8 drive)
{
    if (drive >= DOS_MAX_DRIVES || !g_dos.drives[drive].mounted) return 0x0F;
    g_dos.cur_drive = (int)drive;
    return (u16)DOS_MAX_DRIVES;                 /* AL = number of logical drives */
}

static void fill_findinfo(dosapi_findinfo *out, const char name[13], u8 attr,
                          u32 size, u16 tm, u16 dt)
{
    int i = 0;
    for (; i < 12 && name[i]; i++) out->name[i] = name[i];
    out->name[i] = 0;
    out->attr = attr;
    out->size = size;
    out->time = tm;
    out->date = dt;
}

static u16 dapi_find_first(const char *pattern, u8 attr_mask, dosapi_findinfo *out)
{
    char name[13]; u8 attr = 0; u32 size = 0; u16 tm = 0, dt = 0;
    int rc = dos_find_first_ex(pattern, attr_mask, &g_api_find, name, &attr, &size, &tm, &dt,
                               out ? out->long_name : NULL, DOSAPI_LFN_MAX);
    if (rc != FAT_OK) return err_of(rc);
    fill_findinfo(out, name, attr, size, tm, dt);
    if (out && !out->long_name[0]) out->long_name[0] = 0;
    return 0;
}

static u16 dapi_find_next(dosapi_findinfo *out)
{
    char name[13]; u8 attr = 0; u32 size = 0; u16 tm = 0, dt = 0;
    int rc = dos_find_next_ex(&g_api_find, name, &attr, &size, &tm, &dt,
                              out ? out->long_name : NULL, DOSAPI_LFN_MAX);
    if (rc != FAT_OK) return err_of(rc);
    fill_findinfo(out, name, attr, size, tm, dt);
    if (out && !out->long_name[0]) out->long_name[0] = 0;
    return 0;
}

static u16 dapi_longname(const char *path, char *out, u32 max)
{
    if (!out || max == 0) return 0x0B;
    int rc = dos_longname(path, out, (int)max);
    if (rc != FAT_OK) return err_of(rc);
    return 0;
}

static void dapi_get_date(u8 *year_dos, u8 *month, u8 *day)
{
    int y = 0, m = 0, d = 0;
    plat_get_date(&y, &m, &d);
    if (year_dos) *year_dos = (u8)((y >= 1980 ? y - 1980 : 0) & 0x7F);
    if (month) *month = (u8)(m & 0x0F);
    if (day) *day = (u8)(d & 0x1F);
}

static void dapi_get_time(u8 *hour, u8 *minute, u8 *second, u8 *centisecond)
{
    int h = 0, m = 0, s = 0;
    plat_get_time(&h, &m, &s);
    if (hour) *hour = (u8)(h & 0x1F);
    if (minute) *minute = (u8)(m & 0x3F);
    if (second) *second = (u8)(s & 0x3F);
    if (centisecond) *centisecond = 0;
}

static u32 dapi_get_ticks(void)
{
    return plat_ticks_ms() / 55u;
}

/* ------------------------------------------------------------------ */
/* 16-bit bridge                                                       */
/* ------------------------------------------------------------------ */
static u16 dapi_exec16(const char *prog, const char *args, i16 *exit_code)
{
    Emu *e = (Emu *)plat_alloc((u32)sizeof(Emu));
    if (!e) return 0x05;
    if (emu_init(e)) { plat_free(e); return 0x05; }
    int code = emu_exec_program(e, prog, args ? args : "");
    emu_free(e);
    plat_free(e);
    if (code == -1) return 0x02;                /* file not found */
    if (code == -2) return 0x0B;                /* bad format / too large */
    if (exit_code) *exit_code = (i16)code;
    return 0;
}

static u16 dapi_mount(u8 drive, const char *img_path)
{
    return err_of(dos_mount_image_drive((int)drive, img_path));
}

static u16 dapi_unmount(u8 drive)
{
    return err_of(dos_unmount_drive((int)drive));
}

static int dapi_fs_type(u8 drive)
{
    return dos_fs_type((int)drive);
}

/* ------------------------------------------------------------------ */
/* table                                                               */
/* ------------------------------------------------------------------ */
const dosapi_services g_dosapi = {
    DOSAPI_VERSION,
    dapi_get_version,
    dapi_open,
    dapi_create,
    dapi_close,
    dapi_read,
    dapi_write,
    dapi_seek,
    dapi_unlink,
    dapi_mkdir,
    dapi_rmdir,
    dapi_rename,
    dapi_attr_get,
    dapi_attr_set,
    dapi_chdir,
    dapi_getcwd,
    dapi_get_drive,
    dapi_set_drive,
    dapi_find_first,
    dapi_find_next,
    dapi_longname,
    dapi_get_date,
    dapi_get_time,
    dapi_get_ticks,
    dapi_exec16,
    dapi_mount,
    dapi_unmount,
    dapi_fs_type
};

const dosapi_exports g_dosapi_exports = {
    DOSAPI_VERSION,
    (u32)sizeof(void *),
    &g_dosapi
};
