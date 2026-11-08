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

/* render the emulated 80x25 colour-text screen to console */
void plat_video_show(const u8 *vram)
{
    CHAR16 row[81];
    if (!gOut) return;
    for (int y = 0; y < 25; y++) {
        int last = 0;
        gOut->SetCursorPosition(gOut, 0, (UINTN)y);
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
    if (gBS->HandleProtocol((EFI_HANDLE)image_handle, &gEfiLoadedImageProtocolGuid,
                            (void **)&gLoadedImage) != EFI_SUCCESS)
        return 1;
    return 0;
}

int plat_kbd_poll(void)
{
    if (!gIn) return -1;
    if (gBS->CheckEvent(gIn->WaitForKey) != EFI_SUCCESS) return -1;
    EFI_INPUT_KEY key;
    if (gIn->ReadKeyStroke(gIn, &key) != EFI_SUCCESS) return -1;
    g_last_scan = key.ScanCode;
    if (key.UnicodeChar) return (int)(UINT8)key.UnicodeChar;
    return -1;
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
