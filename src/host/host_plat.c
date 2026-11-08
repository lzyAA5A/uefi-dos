/*
 * UEFI-DOS -- Windows host platform (test harness only).
 * Console via stdio, block device backed by an image file.
 */
#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "uefidos.h"
#include "plat.h"

int g_plat_shutdown_requested = 0;

static HANDLE g_img = INVALID_HANDLE_VALUE;
static int g_last_scan = 0;

void plat_banner(const char *s) { UFD_UNUSED(s); }

u64 plat_timer_units_per_ms(void) { return 1000u; }

int plat_abi_selftest(u32 *year_out)
{
    if (year_out) *year_out = 2026;
    return 0;
}

int plat_init(void *sys_table, void *image_handle)
{
    UFD_UNUSED(sys_table);
    UFD_UNUSED(image_handle);
    return 0;
}

void plat_putc(char c) { fputc((unsigned char)c, stdout); }
void plat_puts(const char *s) { fputs(s, stdout); }
void plat_putmem(const char *buf, u32 len) { fwrite(buf, 1, len, stdout); }

/* render the emulated 80x25 text screen (attributes ignored on the host) */
/* Only rows whose characters changed are printed: a program that repaints one
 * line per keypress used to dump the whole 25-row screen every time, which
 * buried the harness output under repeated full screens. */
static u8 g_vid_shadow[25 * 80];

void plat_video_show(const u8 *vram)
{
    for (int y = 0; y < 25; y++) {
        char line[81];
        int last = 0, changed = 0;
        for (int x = 0; x < 80; x++) {
            u8 c = vram[(y * 80 + x) * 2];
            if (g_vid_shadow[y * 80 + x] != c) { changed = 1; g_vid_shadow[y * 80 + x] = c; }
            if (c < 32 || c > 126) c = ' ';
            line[x] = (char)c;
            if (c != ' ') last = x + 1;
        }
        if (!changed) continue;
        line[last] = 0;
        fputs(line, stdout);
        fputc('\n', stdout);
    }
    fflush(stdout);
}

/* ---- scripted key injection (automated compatibility tests) ---------- *
 * UEFIDOS_KBD        comma separated keys, e.g. "DOWN,DOWN,ENTER,ESC"
 *                    single characters, or names: ENTER ESC SPACE TAB
 *                    BACKSPACE UP DOWN LEFT RIGHT
 * UEFIDOS_KBD_MAX_MS after this many ms without scripted keys left, poll
 *                    returns ESC forever so "wait for key" loops finish.
 */
static int  g_kb_keys[64];
static int  g_kb_n = 0;
static int  g_kb_idx = 0;
static unsigned long long g_kb_polls = 0;
static unsigned long long g_kb_next_poll = 0;
static unsigned long long g_kb_max_polls = 100000;
static int  g_kb_inited = 0;

static int key_token(const char *t)
{
    if (!t[0]) return -1;
    if (!t[1]) return (unsigned char)t[0];
    if (!_stricmp(t, "ENTER")) return 13;
    if (!_stricmp(t, "ESC")) return 27;
    if (!_stricmp(t, "SPACE")) return 32;
    if (!_stricmp(t, "TAB")) return 9;
    if (!_stricmp(t, "BACKSPACE")) return 8;
    if (!_stricmp(t, "UP")) return 0x100 | 0x48;
    if (!_stricmp(t, "DOWN")) return 0x100 | 0x50;
    if (!_stricmp(t, "LEFT")) return 0x100 | 0x4B;
    if (!_stricmp(t, "RIGHT")) return 0x100 | 0x4D;
    if (!_stricmp(t, "HOME")) return 0x100 | 0x47;
    if (!_stricmp(t, "END")) return 0x100 | 0x4F;
    if (!_stricmp(t, "PGUP")) return 0x100 | 0x49;
    if (!_stricmp(t, "PGDN")) return 0x100 | 0x51;
    if (!_stricmp(t, "DEL")) return 0x100 | 0x53;
    if (!_stricmp(t, "INS")) return 0x100 | 0x52;
    if (!_stricmp(t, "F1")) return 0x100 | 0x3B;
    if (!_stricmp(t, "F2")) return 0x100 | 0x3C;
    if (!_stricmp(t, "F3")) return 0x100 | 0x3D;
    if (!_stricmp(t, "F4")) return 0x100 | 0x3E;
    if (!_stricmp(t, "F5")) return 0x100 | 0x3F;
    if (!_stricmp(t, "F6")) return 0x100 | 0x40;
    if (!_stricmp(t, "F7")) return 0x100 | 0x41;
    if (!_stricmp(t, "F8")) return 0x100 | 0x42;
    if (!_stricmp(t, "F9")) return 0x100 | 0x43;
    if (!_stricmp(t, "F10")) return 0x100 | 0x44;
    /* CTRLA .. CTRLZ: control characters, e.g. CTRLX leaves the editor */
    if (!strnicmp(t, "CTRL", 4) && t[4] >= 'A' && t[4] <= 'Z' && !t[5])
        return t[4] - 'A' + 1;
    if (!_stricmp(t, "CTRLSPACE")) return 0;
    return (unsigned char)t[0];
}

/* wall-clock gate for the scripted key stream.  A program blocked in an
 * INT 16h wait only polls a handful of times (2 ms apart), so a pure
 * poll-count gate would never hand it a key; games that spin poll thousands
 * of times per millisecond, where the poll gate keeps the timing predictable. */
static unsigned long long g_kb_next_ms = 0;
static int kb_wall_ready(void)
{
    unsigned long long now = GetTickCount64();
    if (!g_kb_next_ms) g_kb_next_ms = now + 400;
    return now >= g_kb_next_ms;
}

static void kb_script_init(void)
{
    const char *s, *m;
    if (g_kb_inited) return;
    g_kb_inited = 1;
    s = getenv("UEFIDOS_KBD");
    if (s && *s) {
        char buf[512];
        char *p;
        strncpy(buf, s, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        p = buf;
        while (p && *p && g_kb_n < 64) {
            char *comma = strchr(p, ',');
            int k;
            if (comma) *comma = 0;
            k = key_token(p);
            if (k >= 0) g_kb_keys[g_kb_n++] = k;
            p = comma ? comma + 1 : 0;
        }
    }
    m = getenv("UEFIDOS_KBD_MAX_POLLS");
    if (m) g_kb_max_polls = strtoull(m, 0, 10);
    g_kb_next_poll = 5000;      /* let the program draw its first frame */
    g_kb_next_ms = GetTickCount64() + 400;
}

int plat_kbd_poll(void)
{
    kb_script_init();
    g_kb_polls++;
    /* Scripted keys are injected by poll count -- independent of wall clock and
     * of the instruction budget, so a spinning "any key?" loop still sees them.
     * The wall-clock gate is the fallback for programs that block on a key. */
    if (g_kb_idx < g_kb_n && (g_kb_polls >= g_kb_next_poll || kb_wall_ready())) {
        int k = g_kb_keys[g_kb_idx++];
        g_kb_next_poll = g_kb_polls + 20000;
        g_kb_next_ms = GetTickCount64() + 250;
        return k;
    }
    if (g_kb_idx >= g_kb_n && g_kb_polls > g_kb_max_polls) return 27;   /* ESC */
    if (_kbhit()) {
        int c = _getch();
        if (c == 0 || c == 0xE0) {
            /* extended key: the second byte is the PC scan code (arrows, F-keys) */
            g_last_scan = _getch();
            return 0x100 | (g_last_scan & 0xFF);
        }
        return c;
    }
    return -1;
}
int plat_kbd_scan(void) { return g_last_scan; }

void plat_shutdown(void) { g_plat_shutdown_requested = 1; }

u64 plat_ticks_ms(void) { return GetTickCount64(); }
void plat_sleep_ms(u32 ms) { Sleep(ms); }

void plat_get_date(int *y, int *m, int *d)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *y = st.wYear; *m = st.wMonth; *d = st.wDay;
}
void plat_get_time(int *h, int *mi, int *s)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *h = st.wHour; *mi = st.wMinute; *s = st.wSecond;
}

void *plat_alloc(u32 size) { return malloc(size); }
void plat_free(void *p) { free(p); }

/* ---- file-backed block device ---- */
static int hbd_read(blockdev *bd, u64 lba, void *buf, u32 count)
{
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)lba * bd->blk_size;
    if (!SetFilePointerEx(g_img, off, NULL, FILE_BEGIN)) return 1;
    DWORD want = count * bd->blk_size;
    DWORD got = 0;
    if (!ReadFile(g_img, buf, want, &got, NULL)) return 1;
    return got == want ? 0 : 1;
}

static int hbd_write(blockdev *bd, u64 lba, const void *buf, u32 count)
{
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)lba * bd->blk_size;
    if (!SetFilePointerEx(g_img, off, NULL, FILE_BEGIN)) return 1;
    DWORD want = count * bd->blk_size;
    DWORD put = 0;
    if (!WriteFile(g_img, buf, want, &put, NULL)) return 1;
    return put == want ? 0 : 1;
}

int plat_open_image(const char *path)
{
    g_img = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, NULL);
    return g_img == INVALID_HANDLE_VALUE ? 1 : 0;
}

int plat_boot_blockdev(blockdev *bd)
{
    if (g_img == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(g_img, &size)) return 1;
    bd->blk_size = 512;
    bd->nblocks = (u64)size.QuadPart / 512;
    bd->read = hbd_read;
    bd->write = hbd_write;
    bd->ctx = NULL;
    return 0;
}
