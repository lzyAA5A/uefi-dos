/*
 * UEFI-DOS native SDK example -- HELLO64.
 *
 * Build (see sdk/Makefile):
 *     make -C sdk                  # HELLO64.EXE (64-bit) and HELLO32.EXE
 * Copy the result into the boot image, then run it from the shell:
 *     C:\> HELLO64 HELLO.TXT
 *
 * What it demonstrates:
 *   - printing without a C library (handle 1 == console, like DOS)
 *   - the DOS file services (open/seek/read/close) on a real DOS path
 *   - directory search (find_first/find_next)
 *   - the clock
 *   - a 32/64 -> 16-bit downcall: run a real .COM through the 8086 interpreter
 *     and get its exit code back in a native register
 */
#include "uefidos/dosapi.h"
#include "uefidos/runtime.h"

int uefidos_main(const uefidos_exports *api, const char *args)
{
    uefidos_init(api);

    rt_puts("\n=== UEFI-DOS native SDK demo ===\n");
    rt_puts("pointer size : ");
    rt_putu(api->pointer_size * 8);
    rt_puts(" bit\n");
    rt_puts("API version  : ");
    rt_putx(api->version, 4);
    rt_puts("\n");

    /* DOS version: low byte = major, high byte = minor (AH=30h layout) */
    u16 ver = api->svc->get_version();
    rt_puts("DOS version  : ");
    rt_putu(ver & 0xFF);
    rt_putc('.');
    rt_putu(ver >> 8);
    rt_puts("\n");

    rt_puts("drive        : ");
    rt_putc((char)('A' + (api->svc->get_drive() & 0x7F)));
    rt_puts(":\n");

    /* ---- file I/O: read the file named on the command line ---- */
    const char *want = args;
    while (*want == ' ') want++;
    if (!*want) want = "HELLO.TXT";

    u16 h = 0;
    u16 err = api->svc->open(want, DOS_OPEN_READ, &h);
    if (err == 0) {
        u32 size = 0, pos = 0;
        api->svc->seek(h, 0, DOS_SEEK_END, &size);
        api->svc->seek(h, 0, DOS_SEEK_SET, &pos);
        rt_puts("reading      : ");
        rt_puts(want);
        rt_puts(" (");
        rt_putu(size);
        rt_puts(" bytes)\n");

        char buf[81];
        u32 got = 0;
        u32 total = 0;
        while (api->svc->read(h, buf, 80, &got) == 0 && got > 0) {
            buf[got] = 0;
            for (u32 i = 0; i < got; i++) {
                if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) rt_putc('\r');
                rt_putc(buf[i]);
            }
            total += got;
            if (total > 4096) break;
        }
        api->svc->close(h);
        rt_puts("\n");
    } else {
        rt_puts("open failed  : ");
        rt_puts(want);
        rt_puts(" (DOS error ");
        rt_putu(err);
        rt_puts(")\n");
    }

    /* ---- write a file, then read it back ---- */
    err = api->svc->create("NATIVE.TXT", DOS_ATTR_ARCHIVE, &h);
    if (err == 0) {
        const char *msg = "written by a 64-bit native UEFI-DOS program\n";
        u32 put = 0;
        api->svc->write(h, msg, rt_strlen(msg), &put);
        api->svc->close(h);
        rt_puts("wrote        : NATIVE.TXT (");
        rt_putu(put);
        rt_puts(" bytes)\n");
    } else {
        rt_puts("create failed: NATIVE.TXT (DOS error ");
        rt_putu(err);
        rt_puts(")\n");
    }

    /* ---- directory search ---- */
    dosapi_findinfo fi;
    u16 found = 0;
    err = api->svc->find_first("*.TXT", DOS_ATTR_ARCHIVE | DOS_ATTR_DIR, &fi);
    while (err == 0 && found < 6) {
        rt_puts(found ? "               " : "found        : ");
        rt_puts(fi.name);
        rt_puts("\n");
        found++;
        err = api->svc->find_next(&fi);
    }
    if (!found) rt_puts("found        : (no *.TXT)\n");

    /* ---- clock ---- */
    u8 y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0, cs = 0;
    api->svc->get_date(&y, &mo, &d);
    api->svc->get_time(&hh, &mi, &ss, &cs);
    rt_puts("date/time    : ");
    rt_putu(1980u + y);
    rt_putc('-');
    rt_putu(mo);
    rt_putc('-');
    rt_putu(d);
    rt_putc(' ');
    rt_putu(hh);
    rt_putc(':');
    rt_putu(mi);
    rt_putc(':');
    rt_putu(ss);
    rt_puts("\n");

    /* ---- downcall: run a real 16-bit .COM through the 8086 interpreter ---- */
    i16 code = 0;
    err = api->svc->exec16("HELLO.COM", "", &code);
    rt_puts("exec16       : HELLO.COM -> ");
    if (err == 0) {
        rt_puts("exit ");
        rt_putu((u32)code);
        rt_puts(" (16-bit program ran)\n");
    } else {
        rt_puts("DOS error ");
        rt_putu(err);
        rt_puts("\n");
    }

    rt_puts("NATIVE-OK\n");
    return 0;
}
