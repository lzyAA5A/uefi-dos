/*
 * UEFI-DOS -- 64-bit integer division helpers.
 *
 * Needed when building for IA32 (-m32 -nostdlib): gcc emits calls to these
 * libgcc helpers for 64-bit / and %.  Implemented here so the freestanding
 * build never needs libgcc.
 */
#include "uefidos.h"

u64 __udivmoddi4(u64 n, u64 d, u64 *rem)
{
    if (rem) *rem = 0;
    if (d == 0) return 0;
    if (d == 1) return n;
    u64 q = 0;
    u64 r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (1ull << i);
        }
    }
    if (rem) *rem = r;
    return q;
}

u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, NULL); }
u64 __umoddi3(u64 n, u64 d)
{
    u64 r;
    __udivmoddi4(n, d, &r);
    return r;
}

i64 __divdi3(i64 a, i64 b)
{
    int neg = 0;
    u64 ua = (u64)a, ub = (u64)b;
    if (a < 0) { ua = (u64)(-a); neg ^= 1; }
    if (b < 0) { ub = (u64)(-b); neg ^= 1; }
    u64 q = __udivmoddi4(ua, ub, NULL);
    return neg ? -(i64)q : (i64)q;
}

i64 __moddi3(i64 a, i64 b)
{
    u64 ua = (u64)(a < 0 ? -a : a);
    u64 ub = (u64)(b < 0 ? -b : b);
    u64 r = __umoddi3(ua, ub);
    return a < 0 ? -(i64)r : (i64)r;
}

/* gcc >= 10 calls this for "a / b" when both the quotient and the remainder
 * are needed. */
i64 __divmoddi4(i64 a, i64 b, i64 *rem)
{
    int neg = 0;
    u64 ua = (u64)a, ub = (u64)b, r = 0, q;
    if (a < 0) { ua = (u64)(-a); neg ^= 1; }
    if (b < 0) { ub = (u64)(-b); neg ^= 1; }
    q = __udivmoddi4(ua, ub, &r);
    if (rem) *rem = (a < 0) ? -(i64)r : (i64)r;
    return neg ? -(i64)q : (i64)q;
}

u64 __udivmoddi4_wrap(u64 n, u64 d, u64 *rem) { return __udivmoddi4(n, d, rem); }
