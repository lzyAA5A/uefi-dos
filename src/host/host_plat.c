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
static int  g_kb_guard = 27;    /* key returned once the script runs out */

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
    /* UEFIDOS_KBD_GUARD: the key handed out when the script is exhausted.
     * Menu-driven installers treat ESC as "abort", so tests that only want to
     * walk the default path set it to ENTER. */
    {
        const char *g = getenv("UEFIDOS_KBD_GUARD");
        g_kb_guard = 27;
        if (g && *g) {
            int k = key_token(g);
            if (k >= 0) g_kb_guard = k;
        }
    }
    g_kb_next_poll = 5000;      /* let the program draw its first frame */
    g_kb_next_ms = GetTickCount64() + 400;
}

/* ------------------------------------------------------------------ */
/* pointing device                                                     */
/* ------------------------------------------------------------------ */
/* UEFIDOS_MOUSE: scripted events "dx,dy,buttons; dx,dy,buttons; ..." so the
 * mouse path is testable without a physical device.  Without it the real
 * Windows cursor and buttons are used (relative to the previous sample). */
static char g_mouse_script[4096];
static int  g_mouse_loaded;
static POINT g_mouse_last;
static int  g_mouse_have_last;
static u64  g_mouse_next_ms;

int plat_mouse_present(void)
{
    if (!g_mouse_loaded) {
        const char *m = getenv("UEFIDOS_MOUSE");
        g_mouse_loaded = 1;
        if (m) {
            strncpy(g_mouse_script, m, sizeof(g_mouse_script) - 1);
            g_mouse_script[sizeof(g_mouse_script) - 1] = 0;
        }
    }
    return 1;
}

static int mouse_script_next(int *dx, int *dy, int *btn)
{
    char *s = g_mouse_script;
    while (*s == ' ' || *s == ';') s++;
    if (!*s) return 0;
    int v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        int sign = 1;
        while (*s == ' ') s++;
        if (*s == '-') { sign = -1; s++; }
        int n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
        v[i] = sign * n;
        while (*s == ' ') s++;
        if (*s == ',') s++;
        else break;
    }
    while (*s && *s != ';') s++;
    if (*s == ';') s++;
    memmove(g_mouse_script, s, strlen(s) + 1);
    *dx = v[0];
    *dy = v[1];
    *btn = v[2];
    return 1;
}

int plat_mouse_poll(int *dx, int *dy, int *buttons)
{
    if (!dx || !dy || !buttons) return 0;
    if (!g_mouse_loaded) plat_mouse_present();
    if (g_mouse_script[0]) {
        u64 now = plat_ticks_ms();
        /* UEFIDOS_MOUSE_WAIT=<ms>: hold the script back so that the events land
         * after a slowly starting guest (Windows takes minutes) has armed its
         * mouse interrupt, instead of being swallowed by the splash screen. */
        static int wait_loaded = 0;
        if (!wait_loaded) {
            const char *w = getenv("UEFIDOS_MOUSE_WAIT");
            wait_loaded = 1;
            if (w) g_mouse_next_ms = now + (u64)atol(w);
        }
        if (now < g_mouse_next_ms) return 0;
        int ok = mouse_script_next(dx, dy, buttons);
        if (!ok) return 0;
        g_mouse_next_ms = now + 120;    /* one scripted event every 120 ms */
        return 1;
    }
    /* real pointer: GetCursorPos works for the whole desktop */
    POINT p;
    if (!GetCursorPos(&p)) return 0;
    int ddx = 0, ddy = 0;
    if (g_mouse_have_last) {
        ddx = (int)(p.x - g_mouse_last.x);
        ddy = (int)(p.y - g_mouse_last.y);
    }
    g_mouse_last = p;
    g_mouse_have_last = 1;
    if (ddx == 0 && ddy == 0 && GetAsyncKeyState(VK_LBUTTON) == 0 &&
        GetAsyncKeyState(VK_RBUTTON) == 0 &&
        GetAsyncKeyState(VK_MBUTTON) == 0)
        return 0;
    *dx = ddx;
    *dy = ddy;
    *buttons = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0) |
               ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2 : 0) |
               ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 4 : 0);
    return 1;
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
    if (g_kb_idx >= g_kb_n && g_kb_polls > g_kb_max_polls) return g_kb_guard;
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
void plat_pump_events(void) { }   /* the host has no firmware event queue */

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
void *plat_alloc_code(u32 size)
{
    return VirtualAlloc(NULL, size ? size : 4096, MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
}
void plat_free_code(void *p, u32 size) { (void)size; if (p) VirtualFree(p, 0, MEM_RELEASE); }

/* ---- file-backed block device ---- */
static int hbd_read(blockdev *bd, u64 lba, void *buf, u32 count)
{
    HANDLE f = (HANDLE)bd->ctx;
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)lba * bd->blk_size;
    if (f == INVALID_HANDLE_VALUE || f == NULL) return 1;
    if (!SetFilePointerEx(f, off, NULL, FILE_BEGIN)) return 1;
    DWORD want = count * bd->blk_size;
    DWORD got = 0;
    if (!ReadFile(f, buf, want, &got, NULL)) return 1;
    return got == want ? 0 : 1;
}

static int hbd_write(blockdev *bd, u64 lba, const void *buf, u32 count)
{
    HANDLE f = (HANDLE)bd->ctx;
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)lba * bd->blk_size;
    if (bd->read_only || f == INVALID_HANDLE_VALUE || f == NULL) return 1;
    if (!SetFilePointerEx(f, off, NULL, FILE_BEGIN)) return 1;
    DWORD want = count * bd->blk_size;
    DWORD put = 0;
    if (!WriteFile(f, buf, want, &put, NULL)) return 1;
    return put == want ? 0 : 1;
}

int plat_open_image(const char *path)
{
    g_img = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, NULL);
    return g_img == INVALID_HANDLE_VALUE ? 1 : 0;
}

static int hbd_setup(blockdev *bd, HANDLE f)
{
    LARGE_INTEGER size;
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size)) return 1;
    bd->blk_size = 512;
    bd->nblocks = (u64)size.QuadPart / 512;
    bd->read_only = 0;
    bd->read = hbd_read;
    bd->write = hbd_write;
    bd->ctx = (void *)f;
    return 0;
}

int plat_boot_blockdev(blockdev *bd)
{
    if (g_img == INVALID_HANDLE_VALUE) return 1;
    return hbd_setup(bd, g_img);
}

/* Host stand-in for firmware device enumeration: the image passed on the
 * command line is device 0, and every image listed in UEFIDOS_EXTRA_DRIVES
 * (semicolon separated) becomes another drive.  That is how the multi-drive
 * mount path is exercised without QEMU. */
/* ---- graphics modes ------------------------------------------------ */
static int g_gfx_on = 0;
static int g_gfx_w = 0, g_gfx_h = 0;
static const char *g_gfx_dump = NULL;
static int g_gfx_frames = 0;

void plat_serial_puts(const char *s) { fputs(s, stdout); fflush(stdout); }
void plat_serial_hex(const char *tag, u64 v) { printf("%s%016llx\n", tag, (unsigned long long)v); }

int plat_video_gfx_begin(int w, int h)
{
    g_gfx_w = w;
    g_gfx_h = h;
    g_gfx_on = 1;
    if (!g_gfx_dump) g_gfx_dump = getenv("UEFIDOS_GFX_DUMP");
    printf("gfx: mode set %dx%dx8%s\n", w, h, g_gfx_dump ? " (frames go to UEFIDOS_GFX_DUMP)" : "");
    fflush(stdout);
    return 0;
}

/* dump the 8bpp framebuffer as a P6 PPM (the tests convert it to PNG) */
void plat_video_gfx_show(const u8 *fb, int w, int h, const u8 *dac)
{
    if (!g_gfx_dump) return;
    FILE *f = fopen(g_gfx_dump, "wb");
    if (!f) return;
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", w, h);
    fwrite(hdr, 1, (size_t)n, f);
    for (int i = 0; i < w * h; i++) {
        u8 idx = fb[i];
        u8 rgb[3];
        rgb[0] = (u8)(dac[idx * 3 + 0] << 2);
        rgb[1] = (u8)(dac[idx * 3 + 1] << 2);
        rgb[2] = (u8)(dac[idx * 3 + 2] << 2);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    if (g_gfx_frames == 0) {
        printf("gfx: %dx%d framebuffer -> %s (first pixel index %u = %u,%u,%u)\n",
               w, h, g_gfx_dump, (unsigned)fb[0],
               (unsigned)(dac[fb[0] * 3] << 2), (unsigned)(dac[fb[0] * 3 + 1] << 2),
               (unsigned)(dac[fb[0] * 3 + 2] << 2));
        fflush(stdout);
    }
    g_gfx_frames++;
}

void plat_video_text(void)
{
    if (!g_gfx_on) return;
    g_gfx_on = 0;
    printf("gfx: back to 80x25 text mode\n");
    fflush(stdout);
}

int plat_enum_blockdevs(blockdev *out, int max, char labels[][12])
{
    int n = 0;
    if (max <= 0) return 0;
    if (g_img != INVALID_HANDLE_VALUE && n < max) {
        if (!hbd_setup(&out[n], g_img)) {
            if (labels) strcpy(labels[n], "BOOT");
            n++;
        }
    }
    const char *extra = getenv("UEFIDOS_EXTRA_DRIVES");
    while (extra && *extra && n < max) {
        char path[MAX_PATH];
        int k = 0;
        while (*extra && *extra != ';' && k < (int)sizeof(path) - 1) path[k++] = *extra++;
        path[k] = 0;
        if (*extra == ';') extra++;
        if (!path[0]) continue;
        HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        int ro = 0;
        if (f == INVALID_HANDLE_VALUE) {
            f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            ro = 1;
        }
        if (f == INVALID_HANDLE_VALUE) continue;   /* unreadable: skip the device */
        if (hbd_setup(&out[n], f)) { CloseHandle(f); continue; }
        out[n].read_only = ro;
        if (labels) {
            const char *base = path;
            for (const char *p = path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
            int i = 0;
            while (*base && *base != '.' && i < 11) labels[n][i++] = *base++;
            labels[n][i] = 0;
            if (!labels[n][0]) strcpy(labels[n], "IMG");
        }
        n++;
    }
    return n;
}
