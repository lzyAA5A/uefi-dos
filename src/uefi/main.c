/*
 * UEFI-DOS -- entry point.
 */
#include "uefidos.h"
#include "efi/efi.h"
#include "plat.h"
#include "kernel/dos.h"
#include "shell/shell.h"
#include "api/dosapi.h"

/* Globals owned here, used by efi_plat.c and the drivers. */
EFI_GUID gEfiLoadedImageProtocolGuid     = { 0x5B1B31A1, 0x9562, 0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
EFI_GUID gEfiSimpleFileSystemProtocolGuid= { 0x964E5B22, 0x6459, 0x11d2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
EFI_GUID gEfiBlockIoProtocolGuid         = { 0x964E5B21, 0x6459, 0x11d2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };
EFI_GUID gEfiSerialIoProtocolGuid        = { 0xBB25CF6F, 0xF1D4, 0x11D2, {0x9A,0x0C,0x00,0x90,0x27,0x3F,0xC1,0xFD} };
EFI_GUID gEfiGraphicsOutputProtocolGuid      = { 0x9042A9DE, 0x23DC, 0x4A38, {0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A} };
EFI_GUID gEfiDevicePathProtocolGuid      = { 0x09576E91, 0x6D3F, 0x11D2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B} };

/* Configuration table entry published for external EFI apps: the value is a
 * pointer to const dosapi_exports (see api/dosapi.h). */
EFI_GUID gEfiUefiDosApiGuid              = UEFIDOS_API_GUID;

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    if (plat_init(system_table, image_handle) != 0) return EFI_LOAD_ERROR;

    plat_puts("UEFI-DOS v");
    plat_puts(UEFIDOS_VERSION);
    plat_puts(" -- UEFI-native DOS shell (boot OK)\n");

    /* ABI smoke test: gBS->Stall and gRT->GetTime both go through the
     * Microsoft x64 convention, so this catches a wrong EFIAPI early. */
    {
        u32 abi_year = 0;
        int abi_rc = plat_abi_selftest(&abi_year);
        plat_puts("UEFI-DOS: ABI-OK (Stall+GetTime, year ");
        {
            char digits[8]; int n = 0; u32 v = abi_year;
            if (v < 1980 || v > 2200) { plat_puts("n/a)"); }
            else {
                while (v && n < 7) { digits[n++] = (char)('0' + v % 10u); v /= 10u; }
                while (n > 0) plat_putc(digits[--n]);
                plat_puts(")");
            }
        }
        if (abi_rc != 0) {
            plat_puts(" but service probe rc=");
            plat_putc((char)('0' + (abi_rc % 10)));
            plat_puts(")");
        }
        plat_puts("\n");
        /* Force the millisecond-clock calibration now and show its scale, so a
         * wrong monotonic-counter unit shows up in the boot log. */
        plat_ticks_ms();
        {
            u64 upm = plat_timer_units_per_ms();
            char d[24]; int n = 0;
            if (upm == 0) upm = 1;
            while (upm && n < 23) { d[n++] = (char)('0' + (int)(upm % 10u)); upm /= 10u; }
            plat_puts("UEFI-DOS: timer ");
            while (n > 0) plat_putc(d[--n]);
            plat_puts(" units/ms\n");
        }
    }

    dos_init();
    int rc = dos_mount_boot();
    if (rc == 0) dos_mount_all();   /* D:, E:, ... for every other UEFI volume */
    if (rc != 0) {
        plat_puts("UEFI-DOS: fatal: cannot mount the boot volume");
        plat_puts(" (rc=");
        /* print rc as a decimal digit sequence */
        char num[8]; int n = 0;
        int v = rc;
        if (v == 0) num[n++] = '0';
        while (v > 0) { num[n++] = (char)('0' + v % 10); v /= 10; }
        while (n > 0) plat_putc(num[--n]);
        plat_puts(")\n");
        plat_shutdown();
        return EFI_SUCCESS;
    }

    /* DOS 4.0: CONFIG.SYS (FILES=, FCBS=, LASTDRIVE=) */
    dos_read_config_sys();

    plat_puts("UEFI-DOS: BOOT OK\n");
    plat_puts("Drive C: mounted (FAT");
    if (dos_fs_type(2) == 32) plat_puts("32");
    else if (dos_fs_type(2) == 16) plat_puts("16");
    else plat_puts("12");
    plat_puts("), volume ");
    plat_puts(g_dos.drives[2].label);
    plat_puts("\n");
    for (int d = 3; d < DOS_MAX_DRIVES; d++) {
        if (!g_dos.drives[d].mounted) continue;
        plat_puts("Drive ");
        plat_putc((char)('A' + d));
        plat_puts(": mounted (FAT");
        int t = dos_fs_type(d);
        plat_puts(t == 32 ? "32" : t == 16 ? "16" : "12");
        plat_puts("), volume ");
        plat_puts(g_dos.drives[d].label);
        plat_puts(g_dos.drives[d].vol.read_only ? " (read-only)\n" : "\n");
    }

    /* Publish the native 32/64-bit DOS API (UEFIDOS_API_GUID). */
    gBS->InstallConfigurationTable(&gEfiUefiDosApiGuid, (void *)&g_dosapi_exports);

    shell_init();
    shell_run();

    if (g_plat_shutdown_requested) return EFI_SUCCESS;
    plat_puts("UEFI-DOS: exit to firmware.\n");
    return EFI_SUCCESS;
}
