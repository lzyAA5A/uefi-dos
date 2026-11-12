import io
p = r"F:\1231\UEFI-DOS\src\emu\emu.c"
s = io.open(p, "r", encoding="utf-8", newline="").read()
old = """#ifdef UEFIDOS_HOST
    fprintf(stderr, "[div] CS:IP=%04X:%04X"""
new = """#ifdef UEFIDOS_HOST
    if (getenv("UEFIDOS_TRACE_DIV"))
        fprintf(stderr, "[div] CS:IP=%04X:%04X"""
assert s.count(old) == 1
s = s.replace(old, new, 1)
# close the if: the fprintf ends with ');' followed by '#endif' -- add a brace pair instead
old2 = """            (unsigned)e->ds, (unsigned)e->es);
#endif"""
new2 = """            (unsigned)e->ds, (unsigned)e->es);
#endif"""
assert s.count(old2) == 1
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("gated")
