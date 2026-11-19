/* Print the TSC frequency of this machine in Hz.
 *
 * bench/bench.c times the native worlds with RDTSC; the 16-bit world times
 * itself with the 18.2 Hz BIOS tick counter.  To compare them the driver needs
 * the TSC frequency, and measuring it is more reliable than trusting the CPU's
 * nominal clock (turbo, power states and invariant-TSC caps all move it).
 */
#include <stdio.h>
#include <windows.h>
#include <intrin.h>

int main(void)
{
    LARGE_INTEGER f, a, b;
    unsigned long long t0, t1;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) return 1;
    QueryPerformanceCounter(&a);
    t0 = __rdtsc();
    Sleep(400);
    QueryPerformanceCounter(&b);
    t1 = __rdtsc();
    double secs = (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
    if (secs <= 0.0) return 1;
    printf("%.0f\n", (double)(t1 - t0) / secs);
    return 0;
}
