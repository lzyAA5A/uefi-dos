/*
 * UEFI-DOS -- UEFI platform implementation.
 *
 * Console: ConOut (+ serial mirror when available). Input: ConIn polling.
 * Block device: the device the booted image lives on (LoadedImage->DeviceHandle).
 */
#include "uefidos.h"
u16 cp437_to_unicode(u8 c);
#include "efi/efi.h"
#include "plat.h"

EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_LOADED_IMAGE_PROTOCOL *gLoadedImage;

static EFI_SIMPLE_TEXT_OUTPUT *gOut;
static EFI_SIMPLE_TEXT_INPUT *gIn;
static EFI_BLOCK_IO *gBlk;
static int g_last_scan;

int g_plat_shutdown_requested = 0;

/* EFI_TIME (EDK2 layout) */
typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME_LOCAL;

static void con_char(UINT16 ch)
{
    CHAR16 s[3];
    if (!gOut) return;
    s[0] = (CHAR16)ch;
    s[1] = 0;
    gOut->OutputString(gOut, s);
}

/* NOTE: no separate serial mirror; OVMF's ConOut already covers serial,
 * and mirroring made every character appear twice in the serial log. */
void plat_putc(char c)
{
    if (c == '\n') {
        con_char('\r');
        con_char('\n');
        return;
    }
    con_char((UINT16)(UINT8)c);
}

void plat_puts(const char *s) { while (*s) plat_putc(*s++); }
void plat_putmem(const char *buf, u32 len)
{
    for (u32 i = 0; i < len; i++) plat_putc(buf[i]);
}

/* render the emulated 80x25 colour-text screen to console.
 *
 * Only rows whose characters changed are written: a game or editor that
 * repaints one line per keystroke used to rewrite all 25 rows, which made the
 * serial log enormous and the screen flicker. */
static u8 g_vid_shadow[25 * 80 * 2];

void plat_video_show(const u8 *vram)
{
    CHAR16 row[81];
    if (!gOut) return;
    for (int y = 0; y < 25; y++) {
        int changed = 0, last = 0;
        for (int x = 0; x < 80; x++) {
            u8 c = vram[(y * 80 + x) * 2];
            u8 a = vram[(y * 80 + x) * 2 + 1];
            u8 *sh = &g_vid_shadow[(y * 80 + x) * 2];
            if (sh[0] != c || sh[1] != a) { sh[0] = c; sh[1] = a; changed = 1; }
        }
        if (!changed) continue;
        for (int x = 0; x < 80; x++) {
            u8 c = vram[(y * 80 + x) * 2];
            CHAR16 ch;
            if (c == 0) c = ' ';
            ch = (c < 0x80) ? (CHAR16)c : cp437_to_unicode(c);
            row[x] = ch;
            if (ch != ' ') last = x + 1;
        }
        for (int x = last; x < 80; x++) row[x] = ' ';
        row[80] = 0;
        gOut->SetCursorPosition(gOut, 0, (UINTN)y);
        gOut->OutputString(gOut, row);
    }
    gOut->SetCursorPosition(gOut, 0, 24);
}

int plat_init(void *sys_table, void *image_handle)
{
    EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)sys_table;
    gST = st;
    gBS = st->BootServices;
    gRT = st->RuntimeServices;
    gOut = st->ConOut;
    gIn = st->ConIn;
    /* Prefer an 80x25 console: the guest screen is exactly 80x25 and the video
     * mirror rewrites those 25 rows, so a taller console (EDK2 defaults to
     * 80x50) leaves the guest screen floating above a separate console region. */
    if (gOut && gOut->Mode) {
        UINTN best = (UINTN)-1;
        for (UINTN i = 0; i < gOut->Mode->MaxMode; i++) {
            UINTN cols = 0, rows = 0;
            if (gOut->QueryMode(gOut, i, &cols, &rows) != EFI_SUCCESS) continue;
            if (cols == 80 && rows == 25) { best = i; break; }
        }
        if (best != (UINTN)-1) gOut->SetMode(gOut, best);
        else if (gOut->Mode->CursorRow >= 25) gOut->SetCursorPosition(gOut, 0, 0);
    }
    if (gBS->HandleProtocol((EFI_HANDLE)image_handle, &gEfiLoadedImageProtocolGuid,
                            (void **)&gLoadedImage) != EFI_SUCCESS)
        return 1;
    return 0;
}

/* EFI_INPUT_KEY.ScanCode -> PC/AT scan code set 1.
 *
 * DOS programs read the BIOS keyboard buffer (INT 16h, or 0040:001E directly)
 * and expect an extended key as "scan code, ASCII 0".  The EFI scan codes are
 * a different numbering, and an extended key carries no UnicodeChar at all, so
 * the raw value used to be dropped -- arrow keys did nothing in menus, games
 * and editors. */
static int efi_scan_to_pc(UINT16 sc)
{
    switch (sc) {
    case 0x01: return 0x48;     /* up */
    case 0x02: return 0x50;     /* down */
    case 0x03: return 0x4D;     /* right */
    case 0x04: return 0x4B;     /* left */
    case 0x05: return 0x47;     /* home */
    case 0x06: return 0x4F;     /* end */
    case 0x07: return 0x52;     /* insert */
    case 0x08: return 0x53;     /* delete */
    case 0x09: return 0x49;     /* page up */
    case 0x0A: return 0x51;     /* page down */
    case 0x17: return 0x01;     /* escape */
    default:
        if (sc >= 0x0B && sc <= 0x14) return 0x3B + ((int)sc - 0x0B); /* F1..F10 */
        if (sc == 0x15) return 0x57;    /* F11 */
        if (sc == 0x16) return 0x58;    /* F12 */
        return 0;
    }
}

int plat_kbd_poll(void)
{
    if (!gIn) return -1;
    if (gBS->CheckEvent(gIn->WaitForKey) != EFI_SUCCESS) return -1;
    EFI_INPUT_KEY key;
    if (gIn->ReadKeyStroke(gIn, &key) != EFI_SUCCESS) return -1;
    g_last_scan = key.ScanCode;
    if (!key.UnicodeChar) {
        /* Extended key: hand it over as 0x100 | pc_scan so the emulator pushes
         * a scan-code-only entry into the BIOS buffer. */
        int pc = efi_scan_to_pc(key.ScanCode);
        if (pc) return 0x100 | pc;
        return -1;
    }
    return (int)(UINT8)key.UnicodeChar;
}
int plat_kbd_scan(void) { return g_last_scan; }

void plat_shutdown(void)
{
    g_plat_shutdown_requested = 1;
    if (gRT && gRT->ResetSystem) {
        gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    }
    for (;;) { gBS->Stall(1000000); }
}

/* Millisecond clock.
 *
 * EFI_BOOT_SERVICES.GetNextMonotonicCount is NOT a clock: the EDK2/OVMF
 * implementation just bumps a counter on every call, so it cannot measure
 * elapsed time.  Use the CPU timestamp counter instead and calibrate it once
 * against Stall(); if the TSC turns out not to advance, fall back to the
 * firmware RTC (1 s granularity + EFI_TIME.Nanosecond). */
static UINT64 g_mono_base = 0;
static UINT64 g_mono_per_ms = 0;        /* TSC ticks per millisecond, 0 = no TSC */
static int    g_tsc_tried = 0;

static UINT64 tsc_read(void)
{
    UINT32 lo = 0, hi = 0;
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
#endif
    return ((UINT64)hi << 32) | (UINT64)lo;
}

/* Wall clock in milliseconds-since-midnight, rolled over monotonically. */
static UINT64 g_wall_last = 0, g_wall_day = 0;
static UINT64 wall_ms(void)
{
    EFI_TIME_LOCAL t;
    UINT64 ms;
    if (!gRT || !gRT->GetTime || gRT->GetTime(&t, NULL) != EFI_SUCCESS) return g_wall_day + g_wall_last;
    ms = (((UINT64)t.Hour * 60u + t.Minute) * 60u + t.Second) * 1000u + (UINT64)(t.Nanosecond / 1000000u);
    if (ms + 1u < g_wall_last) g_wall_day += 86400000u;   /* midnight rollover */
    g_wall_last = ms;
    return g_wall_day + ms;
}

static void tsc_calibrate(void)
{
    UINT64 a, b, per;
    if (g_tsc_tried) return;
    g_tsc_tried = 1;
    if (!gBS || !gBS->Stall) return;
    a = tsc_read();
    gBS->Stall(50000);                  /* 50 ms reference interval */
    b = tsc_read();
    if (b <= a) { g_mono_per_ms = 0; return; }
    per = (b - a + 25u) / 50u;
    g_mono_per_ms = per ? per : 0;
    g_mono_base = b;
}

u64 plat_ticks_ms(void)
{
    if (!g_tsc_tried) tsc_calibrate();
    if (g_mono_per_ms) return (tsc_read() - g_mono_base) / g_mono_per_ms;
    return wall_ms();
}

u64 plat_timer_units_per_ms(void) { return g_mono_per_ms; }

/* ABI / service smoke test run once at boot: both calls go through the
 * Microsoft x64 calling convention, so a wrong EFIAPI would corrupt the stack
 * or return nonsense here. */
int plat_abi_selftest(u32 *year_out)
{
    EFI_STATUS s1, s2;
    EFI_TIME_LOCAL t;
    UINT64 before, after;
    if (!gBS || !gBS->Stall) return 1;
    s1 = gBS->Stall(1000);              /* 1 ms */
    before = tsc_read();
    gBS->Stall(2000);
    after = tsc_read();
    s2 = gRT && gRT->GetTime ? gRT->GetTime(&t, NULL) : EFI_LOAD_ERROR;
    if (year_out) *year_out = (s2 == EFI_SUCCESS) ? (u32)t.Year : 0u;
    if (EFI_ERROR(s1) || EFI_ERROR(s2)) return 2;
    if (t.Year < 1980 || t.Month < 1 || t.Month > 12 || t.Day < 1) return 3;
    if (after <= before) return 4;      /* no usable TSC */
    return 0;
}

void plat_sleep_ms(u32 ms)
{
    if (gBS && gBS->Stall) gBS->Stall((UINTN)ms * 1000);
}

void plat_get_date(int *y, int *m, int *d)
{
    EFI_TIME_LOCAL t;
    *y = 2026; *m = 1; *d = 1;
    if (gRT && gRT->GetTime && gRT->GetTime(&t, NULL) == EFI_SUCCESS) {
        *y = t.Year; *m = t.Month; *d = t.Day;
    }
}
void plat_get_time(int *h, int *mi, int *s)
{
    EFI_TIME_LOCAL t;
    *h = 0; *mi = 0; *s = 0;
    if (gRT && gRT->GetTime && gRT->GetTime(&t, NULL) == EFI_SUCCESS) {
        *h = t.Hour; *mi = t.Minute; *s = t.Second;
    }
}

void *plat_alloc(u32 size)
{
    void *p = NULL;
    if (gBS->AllocatePool(2 /* EFI_LOADER_DATA */, size, &p) != EFI_SUCCESS) return NULL;
    return p;
}
void plat_free(void *p) { if (p && gBS->FreePool) gBS->FreePool(p); }

static int ubd_read(blockdev *bd, u64 lba, void *buf, u32 count)
{
    (void)bd;
    UINTN size = (UINTN)count * 512u;
    EFI_STATUS s = gBlk->ReadBlocks(gBlk, gBlk->Media->MediaId, lba, size, buf);
    return EFI_ERROR(s) ? 1 : 0;
}

static int ubd_write(blockdev *bd, u64 lba, const void *buf, u32 count)
{
    (void)bd;
    if (gBlk->Media->ReadOnly) return 1;
    UINTN size = (UINTN)count * 512u;
    EFI_STATUS s = gBlk->WriteBlocks(gBlk, gBlk->Media->MediaId, lba, size, (void *)buf);
    return EFI_ERROR(s) ? 1 : 0;
}

int plat_boot_blockdev(blockdev *bd)
{
    void *blk = NULL;
    if (!gBS || !gLoadedImage) return 1;
    EFI_HANDLE dev = gLoadedImage->DeviceHandle;
    EFI_STATUS s = gBS->HandleProtocol(dev, &gEfiBlockIoProtocolGuid, &blk);
    if (EFI_ERROR(s) || !blk) {
        /* maybe the image handle itself is on the disk (no partition handle) */
        s = gBS->HandleProtocol(gLoadedImage->ParentHandle, &gEfiBlockIoProtocolGuid, &blk);
        if (EFI_ERROR(s) || !blk) return 1;
    }
    gBlk = (EFI_BLOCK_IO *)blk;
    if (!gBlk->Media || gBlk->Media->BlockSize != 512) return 1;
    bd->blk_size = 512;
    bd->nblocks = gBlk->Media->LastBlock + 1;
    bd->read = ubd_read;
    bd->write = ubd_write;
    bd->ctx = gBlk;
    return 0;
}
