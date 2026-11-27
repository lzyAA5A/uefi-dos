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

/* ------------------------------------------------------------------ */
/* pointing device (INT 33h backing store)                             */
/* ------------------------------------------------------------------ */
/* A laptop routinely publishes MORE than one pointer: the built-in touchpad,
 * often as a handle that never reports anything, next to the USB mouse that
 * works.  Holding only the first one -- which is what this code used to do --
 * is then a mouse that does not move while a perfectly good device sits one
 * handle further along the list.  Keep them all and poll them all. */
#define MOUSE_MAX_DEV 8
static EFI_SIMPLE_POINTER_PROTOCOL *g_mouse_list[MOUSE_MAX_DEV];
static EFI_ABSOLUTE_POINTER_PROTOCOL *g_abs_list[MOUSE_MAX_DEV];
static int g_mouse_n_simple, g_mouse_n_abs;   /* handles the firmware published */
static int g_mouse_used, g_abs_used;          /* protocols we actually hold */
static int g_mouse_tried;
static int g_mouse_swept;       /* the ConnectController sweep was needed */
static int g_mouse_relisted;    /* polled long enough to look again */
static int g_mouse_last_buttons = -1;
static UINT64 g_abs_last_x[MOUSE_MAX_DEV], g_abs_last_y[MOUSE_MAX_DEV];
static int g_abs_have_last[MOUSE_MAX_DEV];
/* Per-device history.  A firmware handle that answers every poll with the same
 * non-zero "movement" is not a mouse that is moving, it is a device stuck
 * reporting a constant -- the touchpad half of "the pointer flies around when I
 * touch it".  Count identical repeats and stop listening to that device. */
static int g_dev_last_dx[MOUSE_MAX_DEV], g_dev_last_dy[MOUSE_MAX_DEV];
static int g_dev_same[MOUSE_MAX_DEV];
static int g_dev_moves[MOUSE_MAX_DEV + MOUSE_MAX_DEV];  /* diagnostics */
static int g_dev_dx_now[MOUSE_MAX_DEV + MOUSE_MAX_DEV];
static int g_dev_dy_now[MOUSE_MAX_DEV + MOUSE_MAX_DEV];
#define DEV_STUCK_REPEATS 128    /* identical reports in a row -> ignore */
#define MOUSE_POLL_CLAMP 96      /* counts we are willing to believe per poll */

void plat_serial_puts(const char *s);

/* Fill in the protocol pointers and the counts of handles that carry each one.
 * Every handle is kept, up to MOUSE_MAX_DEV: counting them is not enough, a
 * machine with a silent touchpad handle first and a working USB mouse second
 * only moves if the second one is polled too. */
static void mouse_enumerate(void)
{
    EFI_HANDLE *handles = NULL;
    UINTN n = 0;
    UINTN i;
    g_mouse_n_simple = 0;
    g_mouse_n_abs = 0;
    g_mouse_used = 0;
    g_abs_used = 0;
    if (!gBS || !gBS->LocateHandleBuffer) return;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(2 /* ByProtocol */,
                                          &gEfiSimplePointerProtocolGuid,
                                          NULL, &n, &handles))) {
        g_mouse_n_simple = (int)n;
        for (i = 0; i < n && g_mouse_used < MOUSE_MAX_DEV; i++) {
            void *p = NULL;
            if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiSimplePointerProtocolGuid, &p)) && p)
                g_mouse_list[g_mouse_used++] = (EFI_SIMPLE_POINTER_PROTOCOL *)p;
        }
    }
    n = 0;
    handles = NULL;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(2 /* ByProtocol */,
                                          &gEfiAbsolutePointerProtocolGuid,
                                          NULL, &n, &handles))) {
        g_mouse_n_abs = (int)n;
        for (i = 0; i < n && g_abs_used < MOUSE_MAX_DEV; i++) {
            void *p = NULL;
            if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiAbsolutePointerProtocolGuid, &p)) && p)
                g_abs_list[g_abs_used++] = (EFI_ABSOLUTE_POINTER_PROTOCOL *)p;
        }
    }
}

/* Reset every device we hold and forget the sample history. */
static void mouse_reset_all(void)
{
    int i;
    for (i = 0; i < g_mouse_used; i++)
        if (g_mouse_list[i]->Reset) g_mouse_list[i]->Reset(g_mouse_list[i], 0);
    for (i = 0; i < g_abs_used; i++) {
        if (g_abs_list[i]->Reset) g_abs_list[i]->Reset(g_abs_list[i], 0);
        g_abs_have_last[i] = 0;
    }
    for (i = 0; i < MOUSE_MAX_DEV; i++) {
        g_dev_last_dx[i] = 0;
        g_dev_last_dy[i] = 0;
        g_dev_same[i] = 0;
    }
    for (i = 0; i < MOUSE_MAX_DEV + MOUSE_MAX_DEV; i++) {
        g_dev_moves[i] = 0;
        g_dev_dx_now[i] = 0;
        g_dev_dy_now[i] = 0;
    }
    g_mouse_last_buttons = -1;
}

/* Ask DxeCore to run driver binding on every controller it has.  This is what
 * "connect -r" does in the UEFI shell: on a firmware that never bound
 * UsbMouseDxe during BDS, the mouse only appears after this.  Bounded, because
 * a machine with a lot of handles should not stall the boot. */
static void mouse_connect_sweep(void)
{
    EFI_HANDLE *all = NULL;
    UINTN n = 0;
    UINTN i;
    if (!gBS || !gBS->ConnectController || !gBS->LocateHandleBuffer) return;
    if (EFI_ERROR(gBS->LocateHandleBuffer(0 /* AllHandles */, NULL, NULL, &n, &all)))
        return;
    if (n > 512) n = 512;
    for (i = 0; i < n; i++)
        (void)gBS->ConnectController(all[i], NULL, NULL, TRUE);
}

int plat_mouse_present(void)
{
    if (!g_mouse_tried) {
        g_mouse_tried = 1;
        mouse_enumerate();
        if (!g_mouse_used && !g_abs_used) {
            /* BDS is supposed to have bound UsbMouseDxe (or the PS/2 driver) to
             * every pointing device before it handed us the console, and on a
             * lot of firmwares it does not: Fast Boot skips "full USB
             * initialisation", and a mouse plugged in after POST simply has no
             * driver attached.  Ask DxeCore to bind drivers to everything once;
             * if a USB mouse turns up, it arrives as a new protocol handle. */
            g_mouse_swept = 1;
            mouse_connect_sweep();
            mouse_enumerate();
        }
        mouse_reset_all();
    }
    return g_mouse_used > 0 || g_abs_used > 0;
}

/* One line, printed once, that says what the firmware actually offered.  On a
 * machine whose pointer does not work this is the whole diagnosis: no handle at
 * all is a firmware/driver question, whereas a handle that never reports a
 * state is the case the i8042 fallback exists for. */
void plat_mouse_report(void)
{
    (void)plat_mouse_present();
    plat_puts("mouse: simple-pointer=");
    plat_putc((char)('0' + (g_mouse_n_simple > 9 ? 9 : g_mouse_n_simple)));
    plat_puts(g_mouse_n_simple ? " (polling)" : " (none)");
    plat_puts("  absolute-pointer=");
    plat_putc((char)('0' + (g_mouse_n_abs > 9 ? 9 : g_mouse_n_abs)));
    plat_puts(g_mouse_n_abs ? " (polling)" : " (none)");
    if (g_mouse_swept) plat_puts("  [driver sweep]");
    plat_puts("\n");
    if (!g_mouse_used && !g_abs_used)
        plat_puts("mouse: no pointer protocol -- type MOUSE at the prompt to "
                  "probe the firmware\n");
}

/* device counts -> DOS mickeys: two counts make one pixel, but a single count
 * must still move the pointer or slow mice feel dead */
static int half_count(INT64 v)
{
    if (v > 0) return (int)((v + 1) / 2);
    if (v < 0) return -(int)(((0 - v) + 1) / 2);
    return 0;
}

static void put_int(int v)
{
    char buf[12];
    int n = 0;
    unsigned u;
    if (v < 0) {
        plat_putc('-');
        u = (unsigned)(-(v + 1)) + 1u;
    } else {
        u = (unsigned)v;
    }
    do {
        buf[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u && n < 11);
    while (n) plat_putc(buf[--n]);
}

/* Per-device diagnostics for the shell's MOUSE command.  The question "why does
 * the pointer misbehave on this laptop" cannot be answered from one summary
 * line: it needs to be visible which handle is moving and by how much. */
void plat_mouse_detail(void)
{
    int i;
    (void)plat_mouse_present();
    plat_puts("mouse: relative devices (winner-take-one, each poll)\n");
    for (i = 0; i < g_mouse_used; i++) {
        plat_puts("  dev ");
        put_int(i);
        plat_puts(": simple-pointer  moves=");
        put_int(g_dev_moves[i]);
        plat_puts(" last=");
        put_int(g_dev_dx_now[i]);
        plat_puts(",");
        put_int(g_dev_dy_now[i]);
        if (g_dev_same[i] >= DEV_STUCK_REPEATS) plat_puts("  [stuck, ignored]");
        plat_puts("\n");
    }
    for (i = 0; i < g_abs_used; i++) {
        plat_puts("  dev ");
        put_int(g_mouse_used + i);
        plat_puts(": absolute-pointer moves=");
        put_int(g_dev_moves[MOUSE_MAX_DEV + i]);
        plat_puts(" last=");
        put_int(g_dev_dx_now[MOUSE_MAX_DEV + i]);
        plat_puts(",");
        put_int(g_dev_dy_now[MOUSE_MAX_DEV + i]);
        if (g_abs_list[i] && g_abs_list[i]->Mode) {
            plat_puts("  range ");
            put_int((int)g_abs_list[i]->Mode->MinimumX);
            plat_puts("..");
            put_int((int)g_abs_list[i]->Mode->MaximumX);
            plat_puts(" x ");
            put_int((int)g_abs_list[i]->Mode->MinimumY);
            plat_puts("..");
            put_int((int)g_abs_list[i]->Mode->MaximumY);
        }
        plat_puts("\n");
    }
    if (!g_mouse_used && !g_abs_used) plat_puts("  (no device)\n");
}

/* ------------------------------------------------------------------ */
/* PS/2 (i8042 aux) mouse, polled straight from the controller          */
/*                                                                      */
/* Some firmware builds expose EFI_SIMPLE_POINTER_PROTOCOL but never     */
/* hand out a single state update: the 8042 collects the packets and     */
/* the firmware-side interrupt path stays silent.  Reading the          */
/* controller by hand works regardless, so this is armed as a fallback   */
/* once the protocol has gone a while with no event at all.             */
/* ------------------------------------------------------------------ */
static u8 io_inb(u16 port)
{
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void io_outb(u16 port, u8 v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static int ps2_wait_out(void)         /* input buffer (0x64 bit1) empty */
{
    int i;
    for (i = 0; i < 200000; i++)
        if (!(io_inb(0x64) & 0x02)) return 0;
    return -1;
}
static int ps2_wait_in(void)          /* output buffer (0x64 bit0) full */
{
    int i;
    for (i = 0; i < 200000; i++)
        if (io_inb(0x64) & 0x01) return 0;
    return -1;
}
static void ps2_aux_cmd(u8 v)
{
    ps2_wait_out();
    io_outb(0x64, 0xD4);
    ps2_wait_out();
    io_outb(0x60, v);
    if (!ps2_wait_in()) (void)io_inb(0x60);     /* the 0xFA acknowledge */
}

static int g_ps2_armed;               /* polling fallback active */
static u8  g_ps2_pkt[3];
static int g_ps2_len;
static int g_ps2_tries;               /* polls spent on an idle protocol */

static void ps2_mouse_arm(void)
{
    u8 cb;
    if (g_ps2_armed) return;
    ps2_wait_out();
    io_outb(0x64, 0xA8);              /* enable the aux (mouse) device */
    ps2_wait_out();
    io_outb(0x64, 0x20);              /* read the controller command byte */
    if (ps2_wait_in()) return;
    cb = io_inb(0x60);
    cb = (u8)(cb & ~0x02);            /* aux interrupt off: we poll it */
    cb = (u8)(cb & ~0x20);            /* aux clock on */
    ps2_wait_out();
    io_outb(0x64, 0x60);
    ps2_wait_out();
    io_outb(0x60, cb);
    ps2_aux_cmd(0xF6);                /* set defaults */
    ps2_aux_cmd(0xF4);                /* enable data reporting */
    g_ps2_armed = 1;
    g_ps2_len = 0;
}

/* Collect whole 3-byte packets.  Returns 1 when at least one packet was
 * consumed; the accumulated delta goes into *dx and *dy and the button state
 * into *buttons (bit0 left, bit1 right, bit2 middle). */
static int ps2_mouse_read(int *dx, int *dy, int *buttons)
{
    int got = 0, rx = 0, ry = 0, btn = -1;
    while ((io_inb(0x64) & 0x21) == 0x21) {
        u8 b = io_inb(0x60);
        /* A packet byte always has bit 3 set; the controller's own 0xFA/0xAA
         * responses have it set too but are not packets -- treating one as the
         * first byte desynchronises the stream and produces wild deltas. */
        if (g_ps2_len == 0 && ((b & 0x08) == 0 || b == 0xFA || b == 0xAA)) continue;
        g_ps2_pkt[g_ps2_len++] = b;
        if (g_ps2_len < 3) continue;
        g_ps2_len = 0;
        {
            u8 f = g_ps2_pkt[0];
            int mx = (int)g_ps2_pkt[1], my = (int)g_ps2_pkt[2];
            /* Byte 0 carries the signs in bits 4 (X) and 5 (Y); bits 6/7 are the
             * overflow flags.  Reading bit 7 as the Y sign turned a five-count
             * move into 251 and slammed the pointer into a corner. */
            if (f & 0x10) mx -= 256;
            if (f & 0x20) my -= 256;
            rx += mx;
            /* the controller counts Y upward, our card (and Windows) downward */
            ry += -my;
            btn = (f & 0x01 ? 1 : 0) | (f & 0x02 ? 2 : 0) | (f & 0x04 ? 4 : 0);
            got = 1;
        }
    }
    if (!got) return 0;
    *dx = rx;
    *dy = ry;
    if (btn >= 0) *buttons = btn;
    return 1;
}

int plat_mouse_poll(int *dx, int *dy, int *buttons)
{
    EFI_SIMPLE_POINTER_STATE st;
    EFI_ABSOLUTE_POINTER_STATE ast;
    static int ps2_buttons;
    static unsigned silent;
    int b, i;
    if (!dx || !dy || !buttons) return 0;
    if (plat_mouse_present()) {
        int btns = g_mouse_last_buttons < 0 ? 0 : g_mouse_last_buttons;
        int changed;
        int best = -1, bestmag = 0;
        int rdx = 0, rdy = 0;
        *dx = 0;
        *dy = 0;
        /* Every device the firmware published is asked, but only ONE of them
         * gets to move the pointer.  Adding them up (what this used to do) is
         * wrong on a laptop: the firmware often publishes the same physical
         * mouse -- or a touchpad handle that keeps repeating stale counts -- as
         * several handles, and the guest then sees two or three times the motion
         * the user made, which is exactly "the pointer flies across the screen".
         * The device that moved most this poll wins outright. */
        for (i = 0; i < g_mouse_used; i++) {
            int vx, vy, mag;
            if (g_mouse_list[i]->GetState(g_mouse_list[i], &st) != EFI_SUCCESS) continue;
            btns |= (st.LeftButton ? 1 : 0) | (st.RightButton ? 2 : 0);
            if (!st.RelativeMovementX && !st.RelativeMovementY) {
                g_dev_same[i] = 0;
                continue;
            }
            vx = half_count(st.RelativeMovementX);
            vy = half_count(st.RelativeMovementY);
            /* A handle that answers with the very same counts for a third of a
             * second is not a hand moving, it is a stuck device. */
            if (vx == g_dev_last_dx[i] && vy == g_dev_last_dy[i]) {
                if (g_dev_same[i] < DEV_STUCK_REPEATS) g_dev_same[i]++;
            } else {
                g_dev_same[i] = 0;
            }
            g_dev_last_dx[i] = vx;
            g_dev_last_dy[i] = vy;
            g_dev_dx_now[i] = vx;
            g_dev_dy_now[i] = vy;
            if (g_dev_same[i] >= DEV_STUCK_REPEATS) continue;
            mag = (vx < 0 ? -vx : vx) + (vy < 0 ? -vy : vy);
            if (mag > bestmag) {
                bestmag = mag;
                best = i;
                rdx = vx;
                rdy = vy;
            }
        }
        if (best >= 0) g_dev_moves[best]++;
        /* A device published in absolute coordinates (tablet, some touchpads):
         * turn the new position into a delta and scale it so the guest's
         * 640-pixel width maps onto the device's own range.  It only speaks when
         * no relative device moved -- an absolute device and a relative device
         * are the same hand far more often than they are two hands. */
        if (best < 0) {
            for (i = 0; i < g_abs_used; i++) {
                UINT64 minx = 0, maxx = 1, miny = 0, maxy = 1, spanx, spany;
                INT64 ax, ay;
                int vx, vy, mag;
                if (g_abs_list[i]->GetState(g_abs_list[i], &ast) != EFI_SUCCESS) continue;
                if (g_abs_list[i]->Mode) {
                    minx = g_abs_list[i]->Mode->MinimumX;
                    maxx = g_abs_list[i]->Mode->MaximumX;
                    miny = g_abs_list[i]->Mode->MinimumY;
                    maxy = g_abs_list[i]->Mode->MaximumY;
                }
                /* A Mode block with an empty or absurd range produces a scale
                 * factor of thousands and a pointer that teleports.  Demand a
                 * sane range before believing any of it. */
                if (maxx <= minx || maxy <= miny) continue;
                spanx = maxx - minx + 1;
                spany = maxy - miny + 1;
                if (spanx < 64 || spany < 64) continue;
                btns |= (int)(ast.ActiveButtons & 3u);
                if (!g_abs_have_last[i]) {
                    g_abs_have_last[i] = 1;
                    g_abs_last_x[i] = ast.CurrentX;
                    g_abs_last_y[i] = ast.CurrentY;
                    continue;
                }
                ax = (INT64)ast.CurrentX - (INT64)g_abs_last_x[i];
                ay = (INT64)ast.CurrentY - (INT64)g_abs_last_y[i];
                g_abs_last_x[i] = ast.CurrentX;
                g_abs_last_y[i] = ast.CurrentY;
                if (!ax && !ay) {
                    g_dev_same[MOUSE_MAX_DEV - 1 - i] = 0;
                    continue;
                }
                /* 1280 half-counts is the whole 640-pixel screen */
                vx = (int)((ax * 1280) / (INT64)spanx);
                vy = (int)((ay * 1280) / (INT64)spany);
                g_dev_dx_now[MOUSE_MAX_DEV + i] = vx;
                g_dev_dy_now[MOUSE_MAX_DEV + i] = vy;
                mag = (vx < 0 ? -vx : vx) + (vy < 0 ? -vy : vy);
                if (mag == 0) continue;
                if (mag > bestmag) {
                    bestmag = mag;
                    best = MOUSE_MAX_DEV + i;
                    rdx = vx;
                    rdy = vy;
                }
            }
            if (best >= 0) g_dev_moves[best]++;
        }
        if (best >= 0) {
            /* One wild sample -- a touchpad wake-up, a resume glitch -- should
             * nudge the pointer, not throw it into a corner. */
            if (rdx > MOUSE_POLL_CLAMP) rdx = MOUSE_POLL_CLAMP;
            if (rdx < -MOUSE_POLL_CLAMP) rdx = -MOUSE_POLL_CLAMP;
            if (rdy > MOUSE_POLL_CLAMP) rdy = MOUSE_POLL_CLAMP;
            if (rdy < -MOUSE_POLL_CLAMP) rdy = -MOUSE_POLL_CLAMP;
            *dx = rdx;
            *dy = rdy;
        }
        /* A button *release* carries no movement, so reporting only "something
         * moved or some button is held" swallowed every release and left the
         * guest holding a button down. */
        changed = (btns != g_mouse_last_buttons);
        g_mouse_last_buttons = btns;
        *buttons = btns;
        if (*dx || *dy || changed) return 1;
        /* The protocol is there but says nothing: either nothing is happening,
         * or the firmware bound a device that is not the mouse under test.  Look
         * again -- a mouse plugged in after the firmware's own scan does get a
         * handle of its own, since the USB bus driver binds on connect.  No
         * ConnectController sweep here: this branch can be running in graphics
         * mode, and re-binding every controller in the system could reset the
         * display out from under the picture. */
        if (!changed && ++silent == 64 && !g_mouse_relisted) {
            g_mouse_relisted = 1;
            mouse_enumerate();
            mouse_reset_all();
            return 0;
        }
        if (g_mouse_used || g_abs_used) return 0;
    }
    /* Nothing from the firmware protocol (some builds expose it and never
     * report a single state): after a moment of silence take the controller
     * over and poll the mouse by hand.  Silence here is the normal case, so
     * nothing is printed -- serial is the console mirror, and a diagnostic
     * line in the middle of a program's output breaks it. */
    if (!g_ps2_armed) {
        if (++g_ps2_tries > 4) ps2_mouse_arm();
        return 0;
    }
    b = ps2_buttons;
    if (ps2_mouse_read(dx, dy, &b)) {
        ps2_buttons = b;
        *buttons = b;
        return 1;
    }
    return 0;
}

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

/* set while a graphics mode owns the display (see plat_video_gfx_begin) */
static int g_gfx_on;
/* wipe the whole linear framebuffer (defined next to plat_video_text) */
static void gfx_clear(void);

static void ser_init(void);
void plat_serial_puts(const char *s);

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
    /* While a graphics mode owns the display, the console would repaint its
     * whole text screen over the framebuffer (that is how a game screen ended
     * up black as soon as the program printed anything).  Text output goes to
     * the serial port instead. */
    if (g_gfx_on) {
        char b[3];
        b[0] = c;
        b[1] = 0;
        if (c != '\n') plat_serial_puts(b);
        else plat_serial_puts("\r\n");
        return;
    }
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
        for (INT32 i = 0; i < gOut->Mode->MaxMode; i++) {
            UINTN cols = 0, rows = 0;
            if (gOut->QueryMode(gOut, (UINTN)i, &cols, &rows) != EFI_SUCCESS) continue;
            if (cols == 80 && rows == 25) { best = (UINTN)i; break; }
        }
        if (best != (UINTN)-1) gOut->SetMode(gOut, best);
        else if (gOut->Mode->CursorRow >= 25) gOut->SetCursorPosition(gOut, 0, 0);
    }
    /* Turn off the boot watchdog.  Firmware arms it before starting a boot
     * option (5 minutes is the usual value, MdeModulePkg BdsDxe does exactly
     * that) and resets the machine when it expires.  A DOS session -- and above
     * all a Windows 1.x desktop -- is expected to run far longer than that, and
     * the symptom without this call is that the machine simply reboots about
     * five minutes into the session, with nothing on screen, in the serial log
     * or in the emulator's step ring to explain it. */
    if (gBS->SetWatchdogTimer) gBS->SetWatchdogTimer(0, 0x0000, 0x00, NULL);

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

/* Give the firmware a slice of CPU so its own event groups run.  DxeCore's
 * Stall() arms a timer event and waits for it through WaitForEvent(), which
 * dispatches every ready notification at a lower TPL -- that is where the PS/2
 * and USB input drivers move their packets out of the controller and into the
 * protocol queue.  A tight emulation loop never reaches that code, so keys and
 * mouse packets simply pile up in the firmware and the guest sees no input. */
void plat_pump_events(void)
{
    if (gBS && gBS->Stall) gBS->Stall(1000);   /* 1 ms */
}

void plat_get_date(int *y, int *m, int *d)
{
    EFI_TIME_LOCAL t;
    *y = 2026; *m = 1; *d = 1;
    if (gRT && gRT->GetTime && gRT->GetTime(&t, NULL) == EFI_SUCCESS) {
        *y = t.Year; *m = t.Month; *d = t.Day;
    }
}
void plat_get_time_ms(int *h, int *mi, int *s, int *ms)
{
    EFI_TIME_LOCAL t;
    *h = 0; *mi = 0; *s = 0;
    if (ms) *ms = 0;
    if (gRT && gRT->GetTime && gRT->GetTime(&t, NULL) == EFI_SUCCESS) {
        *h = t.Hour; *mi = t.Minute; *s = t.Second;
        if (ms) *ms = (int)(t.Nanosecond / 1000000u);
    }
}

void plat_get_time(int *h, int *mi, int *s)
{
    int ms;
    plat_get_time_ms(h, mi, s, &ms);
}

void *plat_alloc(u32 size)
{
    void *p = NULL;
    if (gBS->AllocatePool(2 /* EFI_LOADER_DATA */, size, &p) != EFI_SUCCESS) return NULL;
    return p;
}
void plat_free(void *p) { if (p && gBS->FreePool) gBS->FreePool(p); }

/* remembered so plat_free_code() uses the matching deallocator */
static void *g_code_base;
static UINTN g_code_pages;

/* Executable memory for native PE images.  EfiLoaderCode (6) is mapped
 * read/write/execute by every conforming firmware, whereas EfiLoaderData is
 * NX on x86_64.  Page granularity: the loader sizes the block from
 * SizeOfImage, so rounding up here is harmless. */
void *plat_alloc_code(u32 size)
{
    UINTN pages = ((UINTN)size + 4095u) / 4096u;
    UINT64 mem = 0;
    void *pool = NULL;
    if (!pages) pages = 1;
    if (gBS->AllocatePages && gBS->AllocatePages(0 /* AnyPages */, 6 /* EfiLoaderCode */,
                                                 pages, &mem) == EFI_SUCCESS) {
        g_code_pages = pages;
        g_code_base = (void *)(UINTN)mem;
        return g_code_base;
    }
    /* some firmware maps pool pages of type EfiLoaderCode as executable where
     * it refuses to hand out whole pages; try that before giving up */
    if (gBS->AllocatePool && gBS->AllocatePool(6 /* EfiLoaderCode */, size, &pool) == EFI_SUCCESS) {
        g_code_pages = 0;
        g_code_base = pool;
        return pool;
    }
    return NULL;
}

void plat_free_code(void *p, u32 size)
{
    (void)size;
    if (!p) return;
    if (g_code_pages && p == g_code_base) {
        if (gBS->FreePages) gBS->FreePages((UINT64)(UINTN)p, g_code_pages);
    } else if (gBS->FreePool) {
        gBS->FreePool(p);
    }
    g_code_pages = 0;
    g_code_base = NULL;
}

/* One blockdevice shim for every volume: the protocol pointer rides in
 * bd->ctx, so the same two functions serve C:, D:, a USB stick and a second
 * disk.  (It used to read the single global gBlk, which is why only the boot
 * volume could ever be mounted.) */
static int ubd_read(blockdev *bd, u64 lba, void *buf, u32 count)
{
    EFI_BLOCK_IO *bio = (EFI_BLOCK_IO *)bd->ctx;
    if (!bio || !bio->Media) return 1;
    UINTN size = (UINTN)count * (UINTN)bd->blk_size;
    EFI_STATUS s = bio->ReadBlocks(bio, bio->Media->MediaId, lba, size, buf);
    return EFI_ERROR(s) ? 1 : 0;
}

static int ubd_write(blockdev *bd, u64 lba, const void *buf, u32 count)
{
    EFI_BLOCK_IO *bio = (EFI_BLOCK_IO *)bd->ctx;
    if (!bio || !bio->Media || bio->Media->ReadOnly) return 1;
    UINTN size = (UINTN)count * (UINTN)bd->blk_size;
    EFI_STATUS s = bio->WriteBlocks(bio, bio->Media->MediaId, lba, size, (void *)buf);
    return EFI_ERROR(s) ? 1 : 0;
}

static int ubd_init(blockdev *bd, EFI_BLOCK_IO *bio)
{
    if (!bio || !bio->Media) return 1;
    /* FAT needs 512-byte sectors; a 4096-byte-native medium is not usable yet */
    if (bio->Media->BlockSize != 512) return 1;
    if (!bio->Media->LastBlock) return 1;
    bd->blk_size = 512;
    bd->nblocks = (u64)bio->Media->LastBlock + 1;
    bd->read_only = bio->Media->ReadOnly ? 1 : 0;
    bd->read = ubd_read;
    bd->write = ubd_write;
    bd->ctx = bio;
    return 0;
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
    if (ubd_init(bd, gBlk)) return 1;
    return 0;
}

/* ---- graphics modes ------------------------------------------------ */
static EFI_GRAPHICS_OUTPUT *g_gop;
static UINT32 g_gfx_scale = 1;     /* horizontal integer upscale        */
static UINTN g_gfx_vy_num = 1;    /* vertical scale = vy_num / vy_den  */
static UINTN g_gfx_vy_den = 1;
static UINTN g_gfx_dx, g_gfx_dy;
static unsigned g_show_count;

/* ---- serial diagnostics ------------------------------------------ */
/* ConOut stops being readable once the display is a raw framebuffer, so
 * anything the graphics path wants to say goes straight to the serial port. */
static EFI_SERIAL_IO *g_ser;
static int g_ser_tried;

static void ser_init(void)
{
    if (g_ser_tried) return;
    g_ser_tried = 1;
    void *p = NULL;
    if (gBS && gBS->LocateProtocol &&
        !EFI_ERROR(gBS->LocateProtocol(&gEfiSerialIoProtocolGuid, NULL, &p)) && p) {
        g_ser = (EFI_SERIAL_IO *)p;
        /* leave the firmware's line settings alone: OVMF already mirrors
         * ConOut to serial, so the port is configured for the console */
    }
}

void plat_serial_puts(const char *s)
{
    ser_init();
    if (!g_ser || !g_ser->Write) return;
    while (*s) {
        const char *start = s;
        while (*s) s++;
        UINTN n = (UINTN)(s - start);
        UINTN sent = n;
        g_ser->Write(g_ser, &sent, (void *)start);
    }
}

void plat_serial_hex(const char *tag, u64 v)
{
    char b[24];
    int n = 0;
    plat_serial_puts(tag);
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)((v >> sh) & 0xF);
        b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[n++] = '\n';
    b[n] = 0;
    plat_serial_puts(b);
}

/* diagnostics for the framebuffer path (OVMF mirrors ConOut to serial) */

static int gop_find(void)
{
    if (g_gop) return 0;
    if (!gBS || !gBS->LocateHandleBuffer) return 1;
    EFI_HANDLE *handles = NULL;
    UINTN n = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(2, &gEfiGraphicsOutputProtocolGuid, NULL, &n, &handles)) || !handles)
        return 1;
    for (UINTN i = 0; i < n; i++) {
        void *p = NULL;
        if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiGraphicsOutputProtocolGuid, &p)) && p) {
            g_gop = (EFI_GRAPHICS_OUTPUT *)p;
            break;
        }
    }
    if (gBS->FreePool) gBS->FreePool(handles);
    return g_gop ? 0 : 1;
}

int plat_video_gfx_begin(int w, int h)
{
    /* locate the GOP on the first graphics mode: without this call g_gop is
     * provably NULL and the compiler drops every graphics path (the symptom was
     * a #UD inside the firmware the moment a game set a video mode) */
    if (gop_find() != 0) return 1;
    if (!g_gop->Mode || !g_gop->Mode->Info) return 1;

    UINT32 best = 0xFFFFFFFFu;
    UINT32 best_px = 0xFFFFFFFFu;
    UINT32 best_sc = 0;
    for (UINT32 m = 0; m < g_gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN size = 0;
        if (EFI_ERROR(g_gop->QueryMode(g_gop, m, &size, &info)) || !info) continue;
        if (info->PixelFormat == PixelBltOnly) continue;
        if (info->HorizontalResolution < (UINT32)w || info->VerticalResolution < (UINT32)h) continue;
        UINT32 px = info->HorizontalResolution * info->VerticalResolution;
        /* Rank by how many whole guest pixels fit across the panel (capped at
           4x: past that the picture gains nothing and the fill rate falls), and
           among equal scales take the smallest mode.  A 640x200 CGA screen is
           therefore 2x on a 1280-wide panel and 3x on a 1920-wide one instead of
           sitting in the boot logo's mode at 1x. */
        UINT32 sc = info->HorizontalResolution / (UINT32)w;
        if (sc > 4) sc = 4;
        if (sc > best_sc || (sc == best_sc && px < best_px)) { best_sc = sc; best_px = px; best = m; }
    }
    /* The search above used to be thrown away: the picture stayed in whatever
     * mode the firmware had set for its own boot logo.  Switch to the mode that
     * was chosen, then refresh the cached mode information (SetMode leaves
     * Mode->Info pointing at firmware-owned memory that some implementations
     * rewrite). */
    if (best != 0xFFFFFFFFu && g_gop->Mode->Mode != best) {
        if (!EFI_ERROR(g_gop->SetMode(g_gop, best))) {
            UINTN size = 0;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
            if (!EFI_ERROR(g_gop->QueryMode(g_gop, best, &size, &info)) && info) {
                g_gop->Mode->Info = info;
                g_gop->Mode->Mode = best;
            }
        }
    }
    if (!g_gop->Mode->Info) return 1;

    UINTN gw = g_gop->Mode->Info->HorizontalResolution;
    UINTN gh = g_gop->Mode->Info->VerticalResolution;
    /* Horizontal: integer upscale so a 320x200 game fills the width without
     * blurring.  Vertical: a 200-line mode (CGA 640x200, VGA 320x200) covered
     * the whole 4:3 screen on real hardware, i.e. its pixels were about 1:2.4
     * tall rather than square.  Scaling both axes by one integer therefore
     * either letterboxes the picture -- black bars above and below with the
     * text squashed, which is exactly how Windows 1.01 looked -- or stretches
     * it.  Target a 4:3 picture instead and let the vertical step be
     * fractional: row y covers rows [y*num/den, (y+1)*num/den), so a 200-line
     * frame in a 480-line mode alternates 2 and 3 destination rows. */
    UINTN sc = gw / (UINTN)w;
    if (sc < 1) sc = 1;
    if (sc > 4) sc = 4;
    UINTN out_w = (UINTN)w * sc;
    UINTN out_h = out_w * 3u / 4u;          /* 4:3 picture */
    if (out_h > gh) out_h = gh;
    if (out_h < (UINTN)h) out_h = (UINTN)h; /* never squeeze the guest */
    g_gfx_scale = (UINT32)sc;
    g_gfx_vy_num = out_h;
    g_gfx_vy_den = (UINTN)h;
    g_gfx_dx = (gw - out_w) / 2;
    g_gfx_dy = (gh - out_h) / 2;
    g_gfx_on = 1;
    g_show_count = 0;
    gfx_clear();
    plat_serial_puts("gfx: linear framebuffer ");
    plat_serial_puts(g_gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ? "BGR" : "RGB");
    plat_serial_hex(" guest=", ((u64)w << 32) | (u32)h);
    plat_serial_hex(" panel=", ((u64)gw << 32) | (u32)gh);
    return 0;
}

/* Frames are written straight into the linear framebuffer: the firmware Blt
 * (EfiBltVideoFill in particular) stalls on some implementations, and a plain
 * pixel loop is both simpler and predictable. */
/* One 32-bit store per output pixel: the DAC entry is pre-expanded into the
 * byte order the panel wants, so the inner loop does no shifts at all.  The
 * old code recomputed three 8-bit components for every pixel of every frame. */
static u32 gfx_pal[256];

static void gfx_pal_build(const u8 *dac, int bgr)
{
    for (int i = 0; i < 256; i++) {
        u8 c0 = (u8)((dac[i * 3 + 0] << 2) | (dac[i * 3 + 0] >> 4));
        u8 c1 = (u8)((dac[i * 3 + 1] << 2) | (dac[i * 3 + 1] >> 4));
        u8 c2 = (u8)((dac[i * 3 + 2] << 2) | (dac[i * 3 + 2] >> 4));
        u8 px[4];
        if (bgr) { px[0] = c2; px[1] = c1; px[2] = c0; }
        else     { px[0] = c0; px[1] = c1; px[2] = c2; }
        px[3] = 0;
        gfx_pal[i] = (u32)px[0] | ((u32)px[1] << 8) | ((u32)px[2] << 16);
    }
}

void plat_video_gfx_show_dirty(const u8 *fb, int w, int h, const u8 *dac,
                               const u8 *rows, const u16 *lo, const u16 *hi, int all)
{
    if (gop_find() != 0) return;
    if (!g_gfx_on || !g_gop || !g_gop->Mode || !g_gop->Mode->Info) return;
    if (!g_gop->Mode->FrameBufferBase || !g_gop->Mode->FrameBufferSize) return;
    g_show_count++;

    UINTN gw = g_gop->Mode->Info->HorizontalResolution;
    UINTN stride = g_gop->Mode->Info->PixelsPerScanLine;
    if (!stride) stride = gw;
    UINTN sc = g_gfx_scale;
    int bgr = (g_gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor);
    if ((g_show_count % 1000u) == 0u)
        plat_serial_hex("[show] n=", (u64)g_show_count);
    u8 *base = (u8 *)(UINTN)g_gop->Mode->FrameBufferBase;
    UINTN fbsize = (UINTN)g_gop->Mode->FrameBufferSize;
    gfx_pal_build(dac, bgr);        /* the program may have reprogrammed the DAC */

    for (int y = 0; y < h; y++) {
        int x0 = 0, x1 = w;
        if (!all && rows) {
            if (!rows[y]) continue;
            if (lo && hi) {
                x0 = (int)lo[y];
                x1 = (int)hi[y];
                if (x0 < 0) x0 = 0;
                if (x1 > w) x1 = w;
                if (x1 <= x0) continue;
            }
        }
        const u8 *row = fb + (UINTN)y * (UINTN)w;
        UINTN ry0 = (UINTN)y * g_gfx_vy_num / g_gfx_vy_den;
        UINTN ry1 = (UINTN)(y + 1) * g_gfx_vy_num / g_gfx_vy_den;
        if (ry1 <= ry0) ry1 = ry0 + 1;      /* never drop a source row */
        UINTN roff_x = (g_gfx_dx + (UINTN)x0 * sc) * 4u;
        UINTN need = ((UINTN)(x1 - x0) * sc + 1u) * 4u;
        for (UINTN ry = ry0; ry < ry1; ry++) {
            UINTN roff = (g_gfx_dy + ry) * stride * 4u + roff_x;
            if (roff + need > fbsize) {
                static int told;
                if (!told) {
                    told = 1;
                    plat_serial_puts("GFX-OOB off=");
                    plat_serial_hex("", roff);
                    plat_serial_hex(" size=", fbsize);
                    plat_serial_hex(" stride=", stride);
                    plat_serial_hex(" dx=", g_gfx_dx);
                    plat_serial_hex(" dy=", g_gfx_dy);
                    plat_serial_puts("\n");
                }
                continue;
            }
            u8 *dst = base + roff;
            for (int x = x0; x < x1; x++) {
                u32 px = gfx_pal[row[x]];
                for (UINTN rx = 0; rx < sc; rx++) {
                    *(u32 *)(void *)dst = px;
                    dst += 4;
                }
            }
        }
    }
}

/* Whole frame: the mode-set path and anything that cannot describe its
 * damage. */
void plat_video_gfx_show(const u8 *fb, int w, int h, const u8 *dac)
{
    if (gop_find() != 0) return;
    plat_video_gfx_show_dirty(fb, w, h, dac, NULL, NULL, NULL, 1);
}

/* Wipe the whole linear framebuffer.  Handing the display back to the text
 * console is not enough by itself: a scaled game frame only covers the middle
 * of the screen, the firmware console repaints just its own character grid, and
 * whatever was left around or under it stayed visible.  Windows 1.x's splash
 * survived the mode reset that way, so the prompt ended up drawn on top of two
 * white slabs of the abandoned logo. */
static void gfx_clear(void)
{
    if (!g_gop || !g_gop->Mode || !g_gop->Mode->Info) return;
    if (!g_gop->Mode->FrameBufferBase || !g_gop->Mode->FrameBufferSize) return;
    UINTN gw = g_gop->Mode->Info->HorizontalResolution;
    UINTN gh = g_gop->Mode->Info->VerticalResolution;
    UINTN stride = g_gop->Mode->Info->PixelsPerScanLine;
    if (!stride) stride = gw;
    if (!gw || !gh) return;
    UINTN fbsize = (UINTN)g_gop->Mode->FrameBufferSize;
    u8 *base = (u8 *)(UINTN)g_gop->Mode->FrameBufferBase;
    UINTN row_bytes = stride * 4u;
    if (row_bytes > fbsize) row_bytes = fbsize;
    for (UINTN y = 0; y < gh; y++) {
        UINTN off = y * stride * 4u;
        if (off + row_bytes > fbsize) break;
        gBS->SetMem(base + off, row_bytes, 0);
    }
}

void plat_video_text(void)
{
    plat_serial_puts("[app] plat_video_text\n");
    if (!g_gfx_on) return;
    g_gfx_on = 0;
    gfx_clear();
    /* Blank the console grid as well.  The text mirror only sends rows that are
     * not blank, so the firmware kept its previous characters in the empty ones
     * and would redraw them over the framebuffer on the next scroll.  One pass
     * of 25 blank lines leaves the console and the screen in sync. */
    if (gOut) {
        for (UINTN y = 0; y < 25; y++) {
            gOut->SetCursorPosition(gOut, 0, y);
            for (int x = 0; x < 80; x++)
                gOut->OutputString(gOut, (CHAR16 *)L" ");
        }
        gOut->SetCursorPosition(gOut, 0, 0);
    }
    /* repaint every text row from scratch when console output resumes */
    memset(g_vid_shadow, 0xFF, sizeof(g_vid_shadow));
}

/* ---- device path sniffing: what kind of port is this volume on? ---------- */
#define DP_TYPE_MESSAGING 0x03
#define DP_MSG_USB        0x05
#define DP_MSG_SATA       0x12
#define DP_MSG_NVME       0x17
#define DP_MSG_SD         0x1Au

static void udp_label(EFI_HANDLE handle, char out[12])
{
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    const char *name = "BLOCK";
    if (gBS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void **)&dp) == EFI_SUCCESS && dp) {
        const u8 *p = (const u8 *)dp;
        for (int guard = 0; guard < 64; guard++) {
            u8 type = p[0], sub = p[1];
            u16 len = (u16)(p[2] | ((u16)p[3] << 8));
            if (!len) break;
            if (type == DP_TYPE_MESSAGING) {
                if (sub == DP_MSG_USB) { name = "USB"; break; }
                if (sub == DP_MSG_SATA) { name = "SATA"; break; }
                if (sub == DP_MSG_NVME) { name = "NVME"; break; }
                if (sub == DP_MSG_SD) { name = "SD"; break; }
            }
            if (type == 0x7F /* end of path */) break;
            p += len;
        }
    }
    int i = 0;
    while (name[i] && i < 11) { out[i] = name[i]; i++; }
    out[i] = 0;
}

/* Every volume the firmware knows about.  Ordering rules:
 *   - the boot volume is always first (it becomes C:),
 *   - logical partitions are used when the firmware publishes them, and whole
 *     disks only when nothing else was found (a superfloppy, or firmware that
 *     does not split partitions).  Without that filter the same bytes show up
 *     twice (whole disk + partition) and D: would alias C:. */
int plat_enum_blockdevs(blockdev *out, int max, char labels[][12])
{
    int n = 0;
    if (!gBS || !gBS->LocateHandleBuffer || max <= 0) return 0;

    EFI_BLOCK_IO *boot = NULL;
    if (gLoadedImage) {
        void *blk = NULL;
        EFI_HANDLE dev = gLoadedImage->DeviceHandle;
        if (!EFI_ERROR(gBS->HandleProtocol(dev, &gEfiBlockIoProtocolGuid, &blk))) boot = (EFI_BLOCK_IO *)blk;
        else if (!EFI_ERROR(gBS->HandleProtocol(gLoadedImage->ParentHandle, &gEfiBlockIoProtocolGuid, &blk)))
            boot = (EFI_BLOCK_IO *)blk;
    }

    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    /* ByProtocol = 2 */
    if (EFI_ERROR(gBS->LocateHandleBuffer(2, &gEfiBlockIoProtocolGuid, NULL, &count, &handles)) || !handles)
        return 0;

    int have_partitions = 0;
    for (UINTN i = 0; i < count; i++) {
        void *blk = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid, &blk)) || !blk) continue;
        EFI_BLOCK_IO *bio = (EFI_BLOCK_IO *)blk;
        if (bio->Media && bio->Media->LogicalPartition) have_partitions = 1;
    }

    /* pass 1: the boot volume, so it always lands on C: */
    if (boot) {
        if (!ubd_init(&out[n], boot)) {
            if (labels) strcpy(labels[n], "BOOT");
            n++;
        }
    }

    /* pass 2: everything else */
    for (UINTN i = 0; i < count && n < max; i++) {
        void *blk = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid, &blk)) || !blk) continue;
        EFI_BLOCK_IO *bio = (EFI_BLOCK_IO *)blk;
        if (bio == boot) continue;
        if (have_partitions && bio->Media && !bio->Media->LogicalPartition) continue;
        if (ubd_init(&out[n], bio)) continue;
        /* avoid handing out the same media twice */
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (out[k].ctx == bio) { dup = 1; break; }
        if (dup) continue;
        if (labels) udp_label(handles[i], labels[n]);
        n++;
    }

    if (gBS->FreePool) gBS->FreePool(handles);
    return n;
}
