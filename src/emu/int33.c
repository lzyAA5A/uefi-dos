/*
 * UEFI-DOS -- INT 33h mouse driver (Microsoft-compatible serial/PS2 subset).
 *
 * The host side (firmware Simple Pointer protocol, or the Windows console for
 * the test harness) reports relative motion and the button state; this file is
 * the DOS-visible driver: functions 0..0x24, the event mask of AX=000Ch and the
 * far callback the client registers for it.
 *
 * The callback runs as a real-mode far procedure (the client returns with
 * RETF), so emu.c pushes a return frame and lets the normal instruction loop
 * execute it; the loop closes the callback when CS:IP:SS:SP come back to the
 * values recorded here.
 */
#include "uefidos.h"
#include "emu/emu.h"

/* Host-only INT 33h trace (UEFIDOS_TRACE_MOUSE=1 -> build/mouse_trace.txt).
 * The INT 21h trace cannot see this vector, so "Windows never touches the
 * mouse" was unprovable until every call got its own line. */
#ifdef UEFIDOS_HOST
#include <stdio.h>
#include <stdlib.h>
static FILE *g_mtrace;
static unsigned long g_mtrace_n;
static int g_mtrace_enabled = -1;
static void mtrace(Emu *e)
{
    if (g_mtrace_enabled < 0) {
        g_mtrace_enabled = getenv("UEFIDOS_TRACE_MOUSE") ? 1 : 0;
        if (g_mtrace_enabled) g_mtrace = fopen("build/mouse_trace.txt", "w");
    }
    if (!g_mtrace_enabled || !g_mtrace) return;
    g_mtrace_n++;
    if (g_mtrace_n <= 400 || (g_mtrace_n % 200) == 0) {
        fprintf(g_mtrace,
                "%7lu INT33 AX=%04X BX=%04X CX=%04X DX=%04X at %04X:%04X\n",
                g_mtrace_n, e->ax, e->bx, e->cx, e->dx, e->cs, e->ip);
        fflush(g_mtrace);
    }
}
#else
#define mtrace(e) ((void)0)
#endif

static void set_ax(Emu *e, u16 v) { e->ax = v; }

void int33_handler(Emu *e)
{
    u16 ax = e->ax;
    mtrace(e);
    switch (ax) {
    case 0x0000: {                  /* reset driver and read status */
        e->mouse_press[0] = e->mouse_press[1] = e->mouse_press[2] = 0;
        e->mouse_release[0] = e->mouse_release[1] = e->mouse_release[2] = 0;
        e->mouse_handler_off = 0;
        e->mouse_handler_seg = 0;
        e->mouse_mask = 0;
        e->mouse_pending_events = 0;
        e->mouse_dx = 0;
        e->mouse_dy = 0;
        e->mouse_show_count = 0;
        e->mouse_visible = 0;
        e->mouse_x = 320;
        e->mouse_y = 100;
        set_ax(e, emu_mouse_available() ? 0xFFFF : 0x0000);
        e->bx = 2;                  /* two buttons */
        break;
    }
    case 0x0001:                    /* show pointer */
        if (e->mouse_show_count < 0) e->mouse_show_count = 0;
        e->mouse_show_count++;
        e->mouse_visible = 1;
        break;
    case 0x0002:                    /* hide pointer */
        if (e->mouse_show_count > 0) e->mouse_show_count--;
        e->mouse_visible = e->mouse_show_count > 0;
        break;
    case 0x0003:                    /* read position and button state */
        e->cx = (u16)e->mouse_x;
        e->dx = (u16)e->mouse_y;
        e->bx = (u16)e->mouse_buttons;
        break;
    case 0x0004:                    /* set position */
        e->mouse_x = (int)e->cx;
        e->mouse_y = (int)e->dx;
        if (e->mouse_x < e->mouse_minx) e->mouse_x = e->mouse_minx;
        if (e->mouse_x > e->mouse_maxx) e->mouse_x = e->mouse_maxx;
        if (e->mouse_y < e->mouse_miny) e->mouse_y = e->mouse_miny;
        if (e->mouse_y > e->mouse_maxy) e->mouse_y = e->mouse_maxy;
        break;
    case 0x0005:                    /* button press counters */
        e->bx = e->mouse_press[0];
        e->cx = e->mouse_press[1];
        e->dx = e->mouse_press[2];
        break;
    case 0x0006:                    /* button release counters */
        e->bx = e->mouse_release[0];
        e->cx = e->mouse_release[1];
        e->dx = e->mouse_release[2];
        break;
    case 0x0007:                    /* horizontal/vertical range for the pointer */
        e->mouse_minx = (int)e->cx;
        e->mouse_miny = (int)e->dx;
        break;
    case 0x0008:                    /* vertical/horizontal range for the pointer */
        e->mouse_maxx = (int)e->cx;
        e->mouse_maxy = (int)e->dx;
        if (e->mouse_x > e->mouse_maxx) e->mouse_x = e->mouse_maxx;
        if (e->mouse_y > e->mouse_maxy) e->mouse_y = e->mouse_maxy;
        break;
    case 0x0009:                    /* define graphics cursor shape: our own arrow */
        set_ax(e, 0x0000);
        break;
    case 0x000A:                    /* define text cursor: screen attribute, ignored */
        break;
    case 0x000B:                    /* read motion counters since the last call */
        e->cx = (u16)e->mouse_dx;
        e->dx = (u16)e->mouse_dy;
        e->mouse_dx = 0;
        e->mouse_dy = 0;
        set_ax(e, 0x0000);
        break;
    case 0x000C:                    /* register the event handler */
        e->mouse_handler_off = e->dx;
        e->mouse_handler_seg = e->es;
        e->mouse_mask = e->cx;
        e->mouse_pending_events = 0;
        set_ax(e, 0x0000);
        break;
    case 0x000D:                    /* light pen on: unsupported */
        set_ax(e, 0x0000);
        break;
    case 0x000E:                    /* light pen off */
        set_ax(e, 0x0000);
        break;
    case 0x000F:                    /* mickey/pixel ratio: fixed */
        set_ax(e, 0x0000);
        break;
    case 0x0010:                    /* conditional off: region ignored */
        break;
    case 0x0013:                    /* light pen ratio */
        set_ax(e, 0x0000);
        break;
    case 0x0015:                    /* state save size */
        e->bx = 0;
        set_ax(e, 0x0000);
        break;
    case 0x0016: case 0x0017: case 0x0018:  /* save / restore / set sensitivity */
        set_ax(e, 0x0000);
        break;
    case 0x001A:                    /* set interrupt rate */
        break;
    case 0x001B:                    /* read interrupt rate */
        e->bx = 1;
        set_ax(e, 0x0000);
        break;
    case 0x001C:                    /* set alternate handler: unused */
        set_ax(e, 0x0000);
        break;
    case 0x001F:                    /* disable the driver */
        set_ax(e, 0xFFFF);
        break;
    case 0x0020:                    /* enable the driver */
        set_ax(e, 0xFFFF);
        break;
    case 0x0021:                    /* software reset */
        set_ax(e, 0xFAFF);
        e->bx = 2;
        break;
    case 0x0022:                    /* set language + get driver info */
        e->bx = 0x0600;
        break;
    case 0x0023:                    /* switch language: n/a */
        e->bx = 0;
        set_ax(e, 0x0000);
        break;
    case 0x0024:                    /* driver version + type */
        e->bx = 0x0600;             /* version 6.00 */
        e->cx = 0x0003;             /* bus mouse / PS2 style driver info */
        set_ax(e, 0x0600);
        break;
    default:
        set_ax(e, 0x0000);
        break;
    }
}
