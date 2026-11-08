/*
 * UEFI-DOS -- INT 2Fh native multiplex: the 16-bit -> 32/64-bit upcall.
 *
 * A program running in the 8086 compatibility machine (e.g. a .COM file)
 * can ask the native UEFI-DOS side for services that only make sense in
 * 64/32-bit code (UEFI time, the native API banner, capability probing).
 *
 *   AX = 5544h ('UD', UEFI-DOS multiplex)
 *   DX = 0001h   get native API info
 *                -> AX = API version (0100h = 1.00), BX = pointer size of
 *                   the native side (4 or 8), CX = 0
 *   DX = 0002h   native side prints its API banner
 *                -> AX = banner length
 *   DX = 0003h   read the firmware RTC through the native platform layer
 *                -> ES:BX[0]=hour, [1]=minute, [2]=second, AX = 0
 *   other        -> AX = FFFFh (not supported)
 */
#include "uefidos.h"
#include "emu/emu.h"
#include "api/dosapi.h"
#include "plat.h"

#define INT2F_UEFIDOS 0x5544u

void int2f_handler(Emu *e)
{
    /* DPMI installation check (INT 2Fh AX=1687h): the UEFI-DOS emulator is
     * the DPMI host, so 32-bit clients can bootstrap here. */
    if (e->ax == 0x1687) { dpmi_install_check(e); return; }

    /* Not ours: leave every register untouched, exactly like a real DOS that
     * does not implement the multiplex function (the caller decides). */
    if (e->ax != INT2F_UEFIDOS) return;
    switch (e->dx) {
    case 0x0001:
        e->ax = (u16)(DOSAPI_VERSION & 0xFFFFu);
        e->bx = (u16)sizeof(void *);
        e->cx = 0;
        return;
    case 0x0002: {
        const char *s = "UEFI-DOS native API v1.00";
        plat_puts(s);
        plat_putc('\n');
        e->ax = (u16)strlen(s);
        return;
    }
    case 0x0003: {
        int hh = 0, mm = 0, ss = 0;
        plat_get_time(&hh, &mm, &ss);
        emu_wb(e, e->es, e->bx, (u8)hh);
        emu_wb(e, e->es, (u16)(e->bx + 1), (u8)mm);
        emu_wb(e, e->es, (u16)(e->bx + 2), (u8)ss);
        e->ax = 0;
        return;
    }
    default:
        /* unknown UEFI-DOS subfunction: report "not supported" in AX but keep
         * the other registers as the caller left them */
        e->ax = 0xFFFFu;
        return;
    }
}
