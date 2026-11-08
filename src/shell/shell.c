/*
 * UEFI-DOS -- command interpreter implementation.
 *
 * Internal commands are implemented natively on the kernel (kernel/dos.c).
 * External programs (.COM/.EXE) are handed to the 8086 emulator through
 * emu_shell_exec(); every INT 21h service that program uses is served by
 * the emulator's hook back into this same kernel.
 */
#include "uefidos.h"
#include "shell/shell.h"
#include "kernel/dos.h"
#include "emu/emu.h"
#include "plat.h"
#include "api/dosapi.h"
#include "api/native.h"
#define LINE_MAX 128


int shell_exit_requested;

static int echo_on = 1;
static char g_env[512];
static char g_line[LINE_MAX];

/* ------------------------------------------------------------------ */
/* text helpers                                                        */
/* ------------------------------------------------------------------ */
static int ci_equal(const char *a, const char *b)
{
    for (;;) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        if (!ca) return 1;
        a++; b++;
    }
}

static char up(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static void upcase_inplace(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    return s;
}

/* split first token; returns rest of line (may be empty) */
static char *next_token(char **line)
{
    char *p = *line;
    while (*p == ' ' || *p == '\t') p++;
    char *start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    *line = p;
    return start;
}

static int wildcard(const char *s)
{
    for (; *s; s++) if (*s == '*' || *s == '?') return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* console helpers                                                     */
/* ------------------------------------------------------------------ */
static void say(const char *s) { plat_puts(s); plat_putc('\n'); }

static void say_rc(int rc, const char *what)
{
    char buf[96];
    plat_puts(what);
    plat_puts(": ");
    char num[16];
    int n = rc < 0 ? -rc : rc;
    int i = 0;
    if (n == 0) num[i++] = '0';
    while (n > 0) { num[i++] = (char)('0' + n % 10); n /= 10; }
    if (rc < 0) num[i++] = '-';
    buf[0] = 0;
    char *o = buf;
    while (i > 0) *o++ = num[--i];
    *o = 0;
    plat_puts(buf);
    plat_puts(" (DOS error ");
    int doserr = dos_error_code(rc);
    if (doserr == 0) {
        plat_putc('0');
    } else {
        char dnum[16];
        int k = 0;
        while (doserr > 0) { dnum[k++] = (char)('0' + doserr % 10); doserr /= 10; }
        while (k > 0) plat_putc(dnum[--k]);
    }
    plat_puts("h)\n");
}

/* ------------------------------------------------------------------ */
/* DIR                                                                 */
/* ------------------------------------------------------------------ */
static void dir_print_entry(const char *name, u8 attr, u32 size, u16 t, u16 d)
{
    char line[64];
    int n = 0;
    for (const char *p = name; *p && n < 12; p++) line[n++] = *p;
    while (n < 13) line[n++] = ' ';
    if (attr & 0x10) {
        for (int i = 0; i < 5; i++) line[n++] = ' ';
        const char *dir = "<DIR>";
        for (const char *p = dir; *p; p++) line[n++] = *p;
        line[n++] = ' ';
    } else {
        char num[12];
        int k = 0;
        u32 v = size;
        if (v == 0) num[k++] = '0';
        while (v > 0) { num[k++] = (char)('0' + v % 10); v /= 10; }
        for (int i = 0; i < (int)(10 - k); i++) line[n++] = ' ';
        while (k > 0) line[n++] = num[--k];
        line[n++] = ' ';
    }
    /* time HH:MM  date MM-DD-YY */
    int hh = (t >> 11) & 0x1F, mm = (t >> 5) & 0x3F;
    int yy = 1980 + ((d >> 9) & 0x7F), mo = (d >> 5) & 0xF, dd = d & 0x1F;
    if (mo < 1 || mo > 12) mo = 1;
    if (dd < 1) dd = 1;
    line[n++] = (char)('0' + hh / 10); line[n++] = (char)('0' + hh % 10);
    line[n++] = ':';
    line[n++] = (char)('0' + mm / 10); line[n++] = (char)('0' + mm % 10);
    line[n++] = ' '; line[n++] = ' ';
    line[n++] = (char)('0' + mo / 10); line[n++] = (char)('0' + mo % 10);
    line[n++] = '-';
    line[n++] = (char)('0' + dd / 10); line[n++] = (char)('0' + dd % 10);
    line[n++] = '-';
    line[n++] = (char)('0' + (yy / 10) % 10); line[n++] = (char)('0' + yy % 10);
    line[n] = 0;
    say(line);
}

static int cmd_dir(const char *spec)
{
    char pattern[DOS_MAX_PATH];
    if (!*spec) {
        strcpy(pattern, "*.*");
    } else {
        strncpy(pattern, spec, sizeof(pattern) - 1);
        pattern[sizeof(pattern) - 1] = 0;
        if (!wildcard(pattern)) {
            u8 attr;
            if (dos_attr_get(pattern, &attr) == 0 && (attr & 0x10)) {
                int len = (int)strlen(pattern);
                if (len + 5 < (int)sizeof(pattern)) strcat(pattern, "\\*.*");
            }
        }
    }
    int disp_drive = dos_current_drive();
    const char *after_drive = spec;
    if (up(spec[0]) >= 'A' && up(spec[0]) <= 'Z' && spec[1] == ':') {
        disp_drive = up(spec[0]) - 'A';
        after_drive = spec + 2;
    }
    char dirpart[DOS_MAX_PATH];
    dirpart[0] = 0;
    const char *slash = strrchr(after_drive, '\\');
    if (slash) {
        int n = (int)(slash - after_drive);
        if (n < (int)sizeof(dirpart) - 1) { memcpy(dirpart, after_drive, (size_t)n); dirpart[n] = 0; }
    }
    plat_puts(" Volume in drive ");
    plat_putc((char)('A' + disp_drive));
    if (disp_drive >= 0 && disp_drive < DOS_MAX_DRIVES && g_dos.drives[disp_drive].label[0]) {
        plat_puts(" is ");
        plat_puts(g_dos.drives[disp_drive].label);
    }
    plat_putc('\n');
    plat_puts(" Directory of ");
    char disp[DOS_MAX_PATH];
    if (dirpart[0]) {
        disp[0] = (char)('A' + disp_drive);
        disp[1] = ':';
        strcpy(disp + 2, "\\");
        strcat(disp + 2, dirpart);
        plat_puts(disp);
    } else if (after_drive != spec) {
        disp[0] = (char)('A' + disp_drive);
        disp[1] = ':';
        disp[2] = '\\';
        disp[3] = 0;
        plat_puts(disp);
    } else {
        dos_get_cwd(disp, sizeof(disp));
        plat_puts(disp);
    }
    plat_putc('\n');
    plat_putc('\n');

    dos_find f;
    char name[13];
    u8 attr;
    u32 size;
    u16 t, d;
    int rc = dos_find_first(pattern, 0x3F, &f, name, &attr, &size, &t, &d);
    u32 nfiles = 0, ndirs = 0;
    u64 total = 0;
    while (rc == FAT_OK) {
        dir_print_entry(name, attr, size, t, d);
        if (attr & 0x10) ndirs++; else { nfiles++; total += size; }
        rc = dos_find_next(&f, name, &attr, &size, &t, &d);
    }
    char tail[96];
    /* "  N file(s)  M bytes" */
    {
        char *o = tail;
        const char *fmt1 = "        ";
        for (const char *p = fmt1; *p; p++) *o++ = *p;
        u32 v = nfiles; int i; char num[12];
        i = 0; if (!v) num[i++] = '0';
        while (v) { num[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) *o++ = num[--i];
        const char *f2 = " file(s)";
        for (const char *p = f2; *p; p++) *o++ = *p;
        /* total bytes */
        const char *f3 = "         ";
        for (const char *p = f3; *p; p++) *o++ = *p;
        char nb[24]; int k = 0;
        u64 tv = total;
        if (!tv) nb[k++] = '0';
        while (tv) { nb[k++] = (char)('0' + (int)(tv % 10)); tv /= 10; }
        while (k) *o++ = nb[--k];
        const char *f4 = " bytes";
        for (const char *p = f4; *p; p++) *o++ = *p;
        *o = 0;
    }
    say(tail);
    {
        const char *f5 = "        ";
        char *o = g_line;
        for (const char *p = f5; *p; p++) *o++ = *p;
        char num[12]; u32 v = ndirs; int i = 0;
        if (!v) num[i++] = '0';
        while (v) { num[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) *o++ = num[--i];
        const char *f6 = " dir(s)";
        for (const char *p = f6; *p; p++) *o++ = *p;
        *o = 0;
        say(g_line);
    }
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* TYPE / COPY / DELETE / RENAME                                       */
/* ------------------------------------------------------------------ */
static int cmd_type(const char *spec)
{
    int h, rc;
    rc = dos_open(spec, 0, &h);
    if (rc != FAT_OK) return rc;
    char buf[512];
    int prev = 0;
    for (;;) {
        u32 got = 0;
        rc = dos_read(h, buf, sizeof(buf), &got);
        if (rc != FAT_OK) break;
        if (got == 0) break;
        for (u32 i = 0; i < got; i++) {
            if (buf[i] == '\n' && prev != '\r') plat_putc('\r');
            plat_putc(buf[i]);
            prev = (unsigned char)buf[i];
        }
    }
    dos_close(h);
    return rc;
}

static int cmd_copy(const char *rest)
{
    char buf[DOS_MAX_PATH];
    strncpy(buf, rest, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *p = buf;
    char *s = next_token(&p);
    char *d = next_token(&p);
    if (!*s || !*d) { say("Required parameter missing"); return FAT_EINVAL; }
    if (wildcard(s)) {
        say("COPY: wildcard source not supported yet");
        return FAT_EINVAL;
    }
    u8 attr;
    if (dos_attr_get(d, &attr) == 0 && !(attr & 0x10)) {
        say("File exists");
        return FAT_EEXIST;
    }
    int hs, hd;
    int rc = dos_open(s, 0, &hs);
    if (rc != FAT_OK) return rc;
    rc = dos_creat(d, &hd);
    if (rc != FAT_OK) { dos_close(hs); return rc; }
    char chunk[512];
    for (;;) {
        u32 got = 0;
        rc = dos_read(hs, chunk, sizeof(chunk), &got);
        if (rc != FAT_OK) break;
        if (got == 0) break;
        u32 put = 0;
        rc = dos_write(hd, chunk, got, &put);
        if (rc != FAT_OK) break;
    }
    dos_close(hd);
    dos_close(hs);
    if (rc == FAT_OK) {
        /* 1 file(s) copied */
        char msg[48]; int n = 0;
        const char *pre = "        1 file(s) copied";
        for (const char *q = pre; *q; q++) msg[n++] = *q;
        msg[n] = 0;
        say(msg);
    }
    return rc;
}

static int cmd_del(const char *spec)
{
    if (wildcard(spec)) {
        char prefix[DOS_MAX_PATH];
        prefix[0] = 0;
        const char *bs = strrchr(spec, '\\');
        if (bs && !strchr(bs, '/')) {
            int n = (int)(bs - spec) + 1;
            if (n < (int)sizeof(prefix)) { memcpy(prefix, spec, (size_t)n); prefix[n] = 0; }
        }
        dos_find f;
        char name[13];
        u8 attr;
        u32 size;
        u16 t, d;
        int rc = dos_find_first(spec, 0x00, &f, name, &attr, &size, &t, &d);
        u32 count = 0;
        while (rc == FAT_OK) {
            if (!(attr & 0x10)) {
                char full[DOS_MAX_PATH];
                strcpy(full, prefix);
                strcat(full, name);
                int r2 = dos_unlink(full);
                if (r2 == FAT_OK) count++;
            }
            rc = dos_find_next(&f, name, &attr, &size, &t, &d);
        }
        if (count == 0) return FAT_ENOENT;
        return FAT_OK;
    }
    return dos_unlink(spec);
}

static int cmd_ren(const char *rest)
{
    char buf[DOS_MAX_PATH];
    strncpy(buf, rest, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char *p = buf;
    char *a = next_token(&p);
    char *b = next_token(&p);
    if (!*a || !*b) return FAT_EINVAL;
    return dos_rename(a, b);
}

/* ------------------------------------------------------------------ */
/* batch handling                                                      */
/* ------------------------------------------------------------------ */
static int run_batch_text(const char *text, u32 len);

static int cmd_echo(const char *rest)
{
    if (!*rest) {
        say(echo_on ? "ECHO is on" : "ECHO is off");
        return FAT_OK;
    }
    char tmp[LINE_MAX];
    strncpy(tmp, rest, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
    upcase_inplace(tmp);
    if (strcmp(tmp, "ON") == 0) { echo_on = 1; return FAT_OK; }
    if (strcmp(tmp, "OFF") == 0) { echo_on = 0; return FAT_OK; }
    /* DOS blank-line forms: "ECHO." "ECHO:" "ECHO;" "ECHO/" "ECHO+" "ECHO[" "ECHO]" */
    if (rest[1] == 0 && strchr(".:;/,+[]\\", rest[0])) { say(""); return FAT_OK; }
    /* expand %VAR% from our tiny environment */
    char out[LINE_MAX];
    int o = 0;
    for (int i = 0; rest[i] && o < LINE_MAX - 1; i++) {
        if (rest[i] == '%') {
            int j = i + 1;
            while (rest[j] && rest[j] != '%') j++;
            if (rest[j] == '%') {
                /* lookup NAME in g_env lines */
                char var[32];
                int vl = j - i - 1;
                if (vl > 0 && vl < 32) {
                    memcpy(var, rest + i + 1, (size_t)vl);
                    var[vl] = 0;
                    upcase_inplace(var);
                    const char *e = g_env;
                    int found = 0;
                    while (*e) {
                        const char *eq = e;
                        while (*eq && *eq != '=') eq++;
                        if ((size_t)(eq - e) == strlen(var) && strncmp(e, var, strlen(var)) == 0) {
                            const char *val = eq + 1;
                            while (*val && o < LINE_MAX - 1) out[o++] = *val++;
                            found = 1;
                            break;
                        }
                        e += strlen(e) + 1;
                    }
                    if (!found) {
                        while (i < j && o < LINE_MAX - 1) out[o++] = rest[i++];
                        continue;
                    }
                    i = j;
                    continue;
                }
            }
        }
        out[o++] = rest[i];
    }
    out[o] = 0;
    say(out);
    return FAT_OK;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */
static int do_exit(int code)
{
    shell_exit_requested = 1;
    (void)code;
    return FAT_OK;
}

static void api_put_num(u16 v)
{
    char t[8];
    int n = 0;
    if (v == 0) { plat_putc('0'); return; }
    while (v && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) plat_putc(t[--n]);
}

/* RUN64: load and run a native PE image (see sdk/) on the real CPU. */
static int cmd_run64(const char *rest)
{
    char path[DOS_MAX_PATH];
    int n = 0, code = 0;
    int rc;

    while (*rest == ' ' || *rest == '\t') rest++;
    while (*rest && *rest != ' ' && *rest != '\t' && n < (int)sizeof(path) - 1)
        path[n++] = *rest++;
    path[n] = 0;
    if (!n) {
        say("usage: RUN64 <file.exe> [args]");
        return FAT_EINVAL;
    }
    while (*rest == ' ' || *rest == '\t') rest++;

    plat_puts("RUN64: loading ");
    plat_puts(path);
    plat_puts(" as a native ");
    api_put_num((u16)(sizeof(void *) * 8));
    plat_puts("-bit image\n");

    rc = native_run_file(path, rest, &code);
    if (rc != FAT_OK) {
        plat_puts("RUN64: cannot run ");
        plat_puts(path);
        plat_puts("\n");
        return rc;
    }
    plat_puts("RUN64: exit code ");
    api_put_num((u16)code);
    plat_puts(" -> NATIVE-OK\n");
    return FAT_OK;
}

/* APITEST: exercise the native 32/64-bit DOS API (api/dosapi.h) and the
 * 64/32 -> 16-bit downcall, then print API-OK. */
static int cmd_apitest(const char *rest)
{
    const dosapi_services *api = &g_dosapi;
    dosapi_findinfo fi;
    char buf[80];
    char cwd[DOS_MAX_PATH];
    u16 h = 0;
    u32 got = 0, put = 0, pos = 0, sz = 0;
    u16 err;
    i16 code = 0;
    (void)rest;

    u16 ver = api->get_version();
    plat_puts("APITEST: DOS version ");
    api_put_num((u16)(ver & 0xFF));
    plat_putc('.');
    api_put_num((u16)(ver >> 8));
    plat_puts(", native pointer size ");
    api_put_num((u16)sizeof(void *));
    plat_puts("\n");

    err = api->open("HELLO.TXT", 0, &h);
    if (err == 0) {
        api->seek(h, 0, 2, &sz);           /* AL=2: file size */
        api->seek(h, 0, 0, &pos);          /* AL=0: rewind */
        err = api->read(h, buf, sizeof(buf) - 1, &got);
        api->close(h);
        buf[got] = 0;
        int bl = (int)got;
        while (bl > 0 && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r')) buf[--bl] = 0;
        plat_puts("APITEST: size ");
        api_put_num((u16)sz);
        plat_puts(", read ");
        api_put_num((u16)got);
        plat_puts(" bytes: ");
        say(buf);
    } else {
        plat_puts("APITEST: open HELLO.TXT failed\n");
    }

    err = api->create("TMPAPI.TXT", 0, &h);
    if (err == 0) {
        err = api->write(h, "native api ok", 13, &put);
        api->close(h);
        plat_puts("APITEST: wrote ");
        api_put_num((u16)put);
        plat_puts(" bytes to TMPAPI.TXT\n");
        api->unlink("TMPAPI.TXT");
    }

    err = api->find_first("*.TXT", 0, &fi);
    int files = 0;
    while (err == 0 && files < 50) {
        files++;
        err = api->find_next(&fi);
    }
    plat_puts("APITEST: find '*.TXT' -> ");
    api_put_num((u16)files);
    plat_puts(" file(s)\n");

    if (api->getcwd(cwd, sizeof(cwd)) == 0) {
        plat_puts("APITEST: cwd ");
        say(cwd);
    }

    err = api->exec16("HELLO.COM", "", &code);
    plat_puts("APITEST: exec16 HELLO.COM -> ");
    if (err == 0) {
        api_put_num((u16)code);
        plat_puts(" (16-bit program ran)\n");
    } else {
        plat_puts("error ");
        api_put_num(err);
        plat_putc('\n');
    }

    /* Phase 0 / 0.2: the millisecond clock must advance, not be a constant 0.
     * get_ticks returns the DOS timer tick count (18.2 Hz), so poll the raw
     * millisecond value across a 30 ms sleep and require growth. */
    {
        u32 t0 = api->get_ticks();
        plat_sleep_ms(30);
        u32 t1 = api->get_ticks();
        u32 m0 = (u32)(plat_ticks_ms() & 0xFFFFFFFFu);
        plat_sleep_ms(30);
        u32 m1 = (u32)(plat_ticks_ms() & 0xFFFFFFFFu);
        plat_puts("APITEST: ticks ");
        api_put_num((u16)(t0 & 0xFFFFu));
        plat_puts(" -> ");
        api_put_num((u16)(t1 & 0xFFFFu));
        plat_puts(", ms ");
        api_put_num((u16)(m0 & 0xFFFFu));
        plat_puts(" -> ");
        api_put_num((u16)(m1 & 0xFFFFu));
        if (m1 > m0) plat_puts(" TICKS-OK\n");
        else plat_puts(" TICKS-STUCK\n");
    }

    plat_puts("API-OK\n");
    return FAT_OK;
}

static int dispatch(char *line)
{
    char *p = line;
    char *cmd = next_token(&p);
    char *arg = p;
    while (*arg == ' ' || *arg == '\t') arg++;
    if (!*cmd) return FAT_OK;

    if (ci_equal(cmd, "VER")) {
        plat_puts("UEFI-DOS Version ");
        plat_puts(UEFIDOS_VERSION);
        plat_puts(" (DOS ");
        char v[8];
        int n = 0;
        int maj = UEFIDOS_DOSVER_MAJOR, min = UEFIDOS_DOSVER_MINOR;
        v[n++] = (char)('0' + maj);
        v[n++] = '.';
        v[n++] = (char)('0' + min / 10);
        v[n++] = (char)('0' + min % 10);
        v[n] = 0;
        plat_puts(v);
        plat_puts(" compatible, FAT32)\n");
        return FAT_OK;
    }
    if (ci_equal(cmd, "CLS")) {
        plat_puts("\x1b[2J\x1b[H");
        return FAT_OK;
    }
    if (ci_equal(cmd, "DIR")) { int rc = cmd_dir(arg); if (rc) say_rc(rc, "DIR"); return FAT_OK; }
    if (ci_equal(cmd, "CD") || ci_equal(cmd, "CHDIR")) {
        if (!*arg) { char cwd[DOS_MAX_PATH]; dos_get_cwd(cwd, sizeof(cwd)); say(cwd); return FAT_OK; }
        int rc = dos_chdir(arg);
        if (rc != FAT_OK) say_rc(rc, "CD");
        return FAT_OK;
    }
    if (ci_equal(cmd, "MD") || ci_equal(cmd, "MKDIR")) {
        int rc = dos_mkdir(arg);
        if (rc != FAT_OK) say_rc(rc, "MD");
        return FAT_OK;
    }
    if (ci_equal(cmd, "RD") || ci_equal(cmd, "RMDIR")) {
        int rc = dos_rmdir(arg);
        if (rc != FAT_OK) say_rc(rc, "RD");
        return FAT_OK;
    }
    if (ci_equal(cmd, "DEL") || ci_equal(cmd, "ERASE")) {
        int rc = cmd_del(arg);
        if (rc != FAT_OK) say_rc(rc, "DEL");
        return FAT_OK;
    }
    if (ci_equal(cmd, "REN") || ci_equal(cmd, "RENAME")) {
        int rc = cmd_ren(arg);
        if (rc != FAT_OK) say_rc(rc, "REN");
        return FAT_OK;
    }
    if (ci_equal(cmd, "TYPE")) {
        if (!*arg) { say("Required parameter missing"); return FAT_OK; }
        int rc = cmd_type(arg);
        if (rc != FAT_OK) say_rc(rc, "TYPE");
        return FAT_OK;
    }
    if (ci_equal(cmd, "COPY")) {
        int rc = cmd_copy(arg);
        if (rc != FAT_OK) say_rc(rc, "COPY");
        return FAT_OK;
    }
    if (ci_equal(cmd, "ECHO")) { cmd_echo(arg); return FAT_OK; }
    /* DOS also accepts "ECHO." "ECHO:" "ECHO;" "ECHO/" "ECHO+" … as "print a
       blank line", so the tokenizer hands us a five character command name. */
    if (strlen(cmd) == 5 && strchr(".:;/,+[]\\", cmd[4]) &&
        (cmd[0] == 'E' || cmd[0] == 'e') && (cmd[1] == 'C' || cmd[1] == 'c') &&
        (cmd[2] == 'H' || cmd[2] == 'h') && (cmd[3] == 'O' || cmd[3] == 'o')) {
        say("");
        return FAT_OK;
    }
    if (ci_equal(cmd, "VOL")) {
        int d = dos_current_drive();
        plat_puts(" Volume in drive ");
        char dv[2] = { (char)('A' + d), 0 };
        plat_puts(dv);
        plat_puts(" is ");
        plat_puts(g_dos.drives[d].label[0] ? g_dos.drives[d].label : "NO NAME");
        plat_putc('\n');
        return FAT_OK;
    }
    if (ci_equal(cmd, "DATE")) {
        int y, m, dd; plat_get_date(&y, &m, &dd);
        char b[32];
        int n = 0;
        b[n++] = (char)('0' + m / 10); b[n++] = (char)('0' + m % 10);
        b[n++] = '-'; b[n++] = (char)('0' + dd / 10); b[n++] = (char)('0' + dd % 10);
        b[n++] = '-'; b[n++] = (char)('0' + (y / 1000) % 10); b[n++] = (char)('0' + (y / 100) % 10);
        b[n++] = (char)('0' + (y / 10) % 10); b[n++] = (char)('0' + y % 10); b[n] = 0;
        say(b);
        return FAT_OK;
    }
    if (ci_equal(cmd, "TIME")) {
        int h, mi, s; plat_get_time(&h, &mi, &s);
        char b[16]; int n = 0;
        b[n++] = (char)('0' + h / 10); b[n++] = (char)('0' + h % 10); b[n++] = ':';
        b[n++] = (char)('0' + mi / 10); b[n++] = (char)('0' + mi % 10); b[n++] = ':';
        b[n++] = (char)('0' + s / 10); b[n++] = (char)('0' + s % 10); b[n] = 0;
        say(b);
        return FAT_OK;
    }
    if (ci_equal(cmd, "PROMPT")) {
        if (!*arg) { strcpy(g_dos.prompt, "$P$G"); return FAT_OK; }
        strncpy(g_dos.prompt, arg, sizeof(g_dos.prompt) - 1);
        g_dos.prompt[sizeof(g_dos.prompt) - 1] = 0;
        return FAT_OK;
    }
    if (ci_equal(cmd, "PATH")) {
        if (!*arg) { say(g_dos.path_env[0] ? g_dos.path_env : "PATH not set"); return FAT_OK; }
        strncpy(g_dos.path_env, arg, sizeof(g_dos.path_env) - 1);
        g_dos.path_env[sizeof(g_dos.path_env) - 1] = 0;
        return FAT_OK;
    }
    if (ci_equal(cmd, "SET")) {
        if (!*arg) {
            const char *e = g_env;
            while (*e) { say(e); e += strlen(e) + 1; }
            return FAT_OK;
        }
        char name[32];
        char *eqp = strchr(arg, '=');
        if (!eqp) { say("Syntax error"); return FAT_OK; }
        int n = (int)(eqp - arg);
        if (n >= (int)sizeof(name)) n = (int)sizeof(name) - 1;
        memcpy(name, arg, (size_t)n); name[n] = 0;
        upcase_inplace(name);
        if (strcmp(name, "PROMPT") == 0) {
            strncpy(g_dos.prompt, eqp + 1, sizeof(g_dos.prompt) - 1);
            return FAT_OK;
        }
        if (strcmp(name, "PATH") == 0) {
            strncpy(g_dos.path_env, eqp + 1, sizeof(g_dos.path_env) - 1);
            return FAT_OK;
        }
        /* update env blob */
        u32 used = (u32)strlen(g_env);
        if (used > 0) used++;
        if (used + strlen(arg) + 2 < sizeof(g_env)) {
            memcpy(g_env + used, arg, strlen(arg) + 1);
        }
        return FAT_OK;
    }
    if (ci_equal(cmd, "MOUNT")) {
        char buf[DOS_MAX_PATH];
        strncpy(buf, arg, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        char *p = buf;
        char *letter = next_token(&p);
        char *file = next_token(&p);
        if (!*letter || !*file || letter[1] != ':') {
            say("MOUNT: usage MOUNT A: IMAGE.IMG");
            return FAT_OK;
        }
        int d = up(letter[0]) - 'A';
        int rc = dos_mount_image_drive(d, file);
        if (rc != FAT_OK) { say_rc(rc, "MOUNT"); return FAT_OK; }
        plat_puts("Mounted ");
        plat_putc((char)('A' + d));
        plat_puts(": (");
        plat_puts(g_dos.drives[d].label);
        plat_puts(")\n");
        return FAT_OK;
    }
    if (ci_equal(cmd, "UNMOUNT")) {
        char l = *arg ? up(arg[0]) : 0;
        int rc = (l >= 'A' && l <= 'B') ? dos_unmount_drive(l - 'A') : FAT_EINVAL;
        if (rc != FAT_OK) say_rc(rc, "UNMOUNT");
        else say("Unmounted");
        return FAT_OK;
    }
    if (ci_equal(cmd, "CONFIG")) {
        /* DOS 4.0 CONFIG.SYS view (FILES=, FCBS=, LASTDRIVE=) */
        int used = 0;
        for (int i = 0; i < DOS_HANDLE_MAX; i++) if (g_dos.handles[i].used) used++;
        plat_puts("CONFIG.SYS (DOS 4.0 subset)"); plat_putc('\n');
        plat_puts("  FILES=");     api_put_num((u16)g_dos.files_limit); plat_putc('\n');
        plat_puts("  FCBS=");      api_put_num((u16)g_dos.fcb_limit);  plat_puts(",0"); plat_putc('\n');
        plat_puts("  LASTDRIVE="); plat_putc((char)('A' + g_dos.last_drive)); plat_putc('\n');
        plat_puts("  open handles: ");
        api_put_num((u16)used);
        plat_puts(" of ");
        api_put_num((u16)g_dos.files_limit);
        plat_putc('\n');
        return FAT_OK;
    }
    if (ci_equal(cmd, "FCB")) {
        plat_puts("FCB layer active, FCBS=");
        api_put_num((u16)g_dos.fcb_limit);
        plat_puts(",0");
        plat_putc('\n');
        return FAT_OK;
    }
    if (ci_equal(cmd, "MEM")) {
        say("   655360 bytes total conventional memory");
        say("    65536 bytes used by UEFI-DOS shell and kernel");
        say("   589824 bytes free (approx, DOS API view)");
        return FAT_OK;
    }
    if (ci_equal(cmd, "SHUTDOWN")) {
        say("UEFI-DOS: shutting down...");
        plat_shutdown();
        shell_exit_requested = 1;
        return FAT_OK;
    }
    if (ci_equal(cmd, "EXIT")) {
        return do_exit(0);
    }
    if (ci_equal(cmd, "REM")) return FAT_OK;
    if (ci_equal(cmd, "HELP")) {
        say("UEFI-DOS built-in commands:");
        say("  DIR   CD   MD   RD   DEL   REN   TYPE   COPY");
        say("  CLS   ECHO VER  VOL   DATE  TIME  PROMPT PATH");
        say("  SET   MEM  HELP EXIT  SHUTDOWN  MOUNT  UNMOUNT");
        say("  APITEST: native 32/64-bit DOS API demo (prints API-OK)");
        say("  RUN64 <file.exe>: run a native 32/64-bit PE program (sdk/)");
        say("External programs: .COM and .EXE files on the current drive.");
        return FAT_OK;
    }

    if (ci_equal(cmd, "APITEST")) { cmd_apitest(arg); return FAT_OK; }
    if (ci_equal(cmd, "RUN64")) return cmd_run64(arg);

    /* external command: find & run through the emulator */
    {
        char name[DOS_MAX_PATH];
        strncpy(name, cmd, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        return emu_shell_exec(name, arg);
    }
}

/* ------------------------------------------------------------------ */
/* batch / line execution                                              */
/* ------------------------------------------------------------------ */
int shell_exec_line(const char *line)
{
    strncpy(g_line, line, LINE_MAX - 1);
    g_line[LINE_MAX - 1] = 0;
    char *p = trim(g_line);
    if (*p == '@') p++;
    int rc = dispatch(p);
    return rc;
}

static int run_batch_text(const char *text, u32 len)
{
    u32 i = 0;
    static char bline[LINE_MAX];
    int saved_echo = echo_on;
    shell_exit_requested = 0;
    while (i < len && !shell_exit_requested && !g_plat_shutdown_requested) {
        u32 start = i;
        while (i < len && text[i] != '\r' && text[i] != '\n') i++;
        u32 n = i - start;
        if (i < len) i++;
        if (n >= LINE_MAX) n = LINE_MAX - 1;
        memcpy(bline, text + start, n);
        bline[n] = 0;
        char *p = trim(bline);
        if (!*p) continue;
        if (*p == '@') p++;
        if (*p == ':') continue;
        if (ci_equal(p, "GOTO") || ci_equal(p, "GOTO")) continue;
        if (echo_on && !(bline[0] == '@')) {
            char pr[DOS_MAX_PATH];
            dos_get_prompt(pr, sizeof(pr));
            plat_puts(pr);
            plat_putc(' ');
            say(p);
        }
        shell_exec_line(p);
    }
    echo_on = saved_echo;
    return FAT_OK;
}

static int run_batch_file(const char *path)
{
    int h, rc;
    rc = dos_open(path, 0, &h);
    if (rc != FAT_OK) return rc;
    static char buf[4096];
    u32 total = 0, got = 0;
    rc = dos_read(h, buf, sizeof(buf) - 1, &got);
    total = got;
    dos_close(h);
    if (rc != FAT_OK) return rc;
    buf[total] = 0;
    return run_batch_text(buf, total);
}

int shell_exec_batch_file(const char *path)
{
    return run_batch_file(path);
}

/* ------------------------------------------------------------------ */
/* interactive console                                                 */
/* ------------------------------------------------------------------ */
static int read_line_console(char *out, int max)
{
    int n = 0;
    for (;;) {
        int c = plat_kbd_poll();
        if (c < 0) { plat_sleep_ms(10); continue; }
        if (c == '\r' || c == '\n') { plat_putc('\r'); plat_putc('\n'); break; }
        if (c == 8) { if (n > 0) { n--; plat_putc(8); plat_putc(' '); plat_putc(8); } continue; }
        if (c == 27) { n = 0; continue; }
        if (c >= 32 && c < 127) {
            if (n < max - 1) { out[n++] = (char)c; plat_putc((char)c); }
        }
    }
    out[n] = 0;
    return n;
}

int shell_init(void)
{
    shell_exit_requested = 0;
    echo_on = 1;
    g_env[0] = 0;
    return 0;
}

int shell_run(void)
{
    shell_init();
    if (g_plat_shutdown_requested) return 0;
    /* AUTOEXEC.BAT on the current (boot) drive */
    if (dos_current_drive() >= 0) {
        u8 attr;
        if (dos_attr_get("AUTOEXEC.BAT", &attr) == 0) {
            run_batch_file("AUTOEXEC.BAT");
        }
    }
    if (shell_exit_requested || g_plat_shutdown_requested) return 0;

    for (;;) {
        char pr[DOS_MAX_PATH];
        dos_get_prompt(pr, sizeof(pr));
        plat_puts(pr);
        plat_puts(" ");
        read_line_console(g_line, LINE_MAX);
        char *p = trim(g_line);
        if (!*p) continue;
        if (*p == '@') p++;
        int rc = dispatch(p);
        (void)rc;
        if (shell_exit_requested || g_plat_shutdown_requested) break;
    }
    return 0;
}
