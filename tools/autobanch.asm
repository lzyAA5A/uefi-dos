/* autobanch.asm -- AUTOBANCH: performance probe for UEFI-DOS on real hardware.
 *
 * A 16-bit DOS .COM program (8086-only instructions) that measures how many
 * operations per second the emulator can deliver on the machine it runs on.
 * UEFI-DOS emulates a 16-bit PC, so "operations per second" is in effect the
 * emulated-CPU throughput of the host: a bigger number means a faster host, a
 * livelier Windows 1.x desktop and shorter disk waits.
 *
 * Clock: the 8253 channel-0 counter (out 43h,0 to latch, in 40h twice).  It is
 * a real 1.193182 MHz clock, which is what makes the figures repeatable; the
 * BIOS tick at 0040:006C is only used as a watchdog (20 ticks would be 55 ms of
 * resolution and its rate is the one the wall-clock pacer negotiated).
 *
 * Each test: 0.25 s of warm-up (the pacer re-converges after the previous test
 * changed the instruction mix), then a window of exactly 1193182 counts =
 * 1.000 s, so the raw count IS the per-second figure printed.
 *
 * Build (no ELF emulation in this mingw binutils, so the PE detour is used):
 *   as --32 -o autobanch.o tools/autobanch.asm
 *   ld -m i386pe -Ttext 0x100 --image-base 0 -nostdlib -o ab.exe autobanch.o
 *   objcopy -O binary --only-section=.text ab.exe AUTOBANC.COM
 * See build/mkbench.ps1.
 */

        .code16
        .intel_syntax noprefix
        .text
        .globl _start

/* ---- test driver macros -------------------------------------------- */
.macro BENCH id, inner, label
        mov     dx, offset \label
        call    puts
        call    el_reset
        mov     byte ptr [g_timeout], 0
        mov     word ptr [g_clo], 0
        mov     word ptr [g_chi], 0
        call    get_ticks
        mov     [g_t0], ax
.Lloop\id:
        mov     bp, \inner
.Lin\id:
.endm

/* ends the window; leaves the 32-bit count in DX:AX and prints nothing */
.macro BENCHDONE id, unit
        dec     bp
        jnz     .Lin\id
        add     word ptr [g_clo], \unit
        adc     word ptr [g_chi], 0
        call    el_poll
        call    el_ge_window
        jc      .Ldone\id
        call    get_ticks
        sub     ax, [g_t0]
        cmp     ax, 200                 /* 11 s: the clock never reached 1 s */
        jb      .Lloop\id
        mov     byte ptr [g_timeout], 1
.Ldone\id:
        mov     ax, [g_clo]
        mov     dx, [g_chi]
.endm

/* ---- program -------------------------------------------------------- */
_start:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld

        mov     dx, offset msg_banner
        call    puts
        mov     dx, offset msg_rule
        call    puts

        /* One second of mixed work first: the pacer that keeps guest time on the
         * wall clock starts from a default steps-per-tick figure and re-learns it
         * every 125 ms, so anything measured during the first second after boot
         * would be measuring the convergence, not the machine. */
        mov     dx, offset msg_calib
        call    puts
        call    el_reset
.cal_loop:
        call    el_poll
        call    el_ge_window
        jnc     .cal_loop
        mov     dx, offset msg_ok
        call    puts
        call    crlf

        /* ---------------- environment ---------------- */
        mov     dx, offset msg_dosver
        call    puts
        mov     ah, 0x30
        int     0x21
        push    ax
        xor     ah, ah
        call    put_u16
        mov     al, '.'
        call    put_ch
        pop     ax
        mov     al, ah
        xor     ah, ah
        call    put_u16
        call    crlf

        mov     dx, offset msg_video
        call    puts
        mov     ah, 0x0F
        int     0x10
        push    ax
        xor     ah, ah
        call    put_u16
        mov     dx, offset msg_cols
        call    puts
        pop     ax
        mov     al, ah
        xor     ah, ah
        call    put_u16
        call    crlf

        mov     dx, offset msg_mem
        call    puts
        call    get_ram_kb
        call    put_u16
        mov     dx, offset msg_kb
        call    puts
        call    crlf

        /* BIOS tick rate, measured against the PIT (18.2 Hz expected) */
        mov     dx, offset msg_tickrate
        call    puts
        call    el_reset
        call    get_ticks
        mov     [g_ticklast], ax
        mov     word ptr [g_tickcnt], 0
.tr_loop:
        call    el_poll
        call    get_ticks
        cmp     byte ptr [g_armed], 0
        jne     .tr_count
        mov     [g_ticklast], ax         /* warm-up: only follow the counter */
        jmp     .tr_same
.tr_count:
        cmp     ax, [g_ticklast]
        je      .tr_same
        mov     [g_ticklast], ax
        inc     word ptr [g_tickcnt]
.tr_same:
        call    el_ge_window
        jnc     .tr_loop
        mov     ax, [g_tickcnt]
        call    put_u16
        mov     dx, offset msg_per_s
        call    puts
        call    crlf

        mov     dx, offset msg_rule
        call    puts

        mov     dx, offset msg_legend
        call    puts

        /* ---------------- 1: ALU chain (8 ops/unit) ---------------- */
        BENCH   1, 128, msg_t_alu
        add     ax, bx
        xor     bx, dx
        add     ax, 1
        xor     dx, ax
        mov     bx, ax
        add     dx, bx
        xor     ax, bx
        add     dx, 1
        BENCHDONE 1, 1024
        call    report

        /* ---------------- 2: MUL (2 muls/unit) ---------------- */
        BENCH   2, 64, msg_t_mul
        mov     ax, 3
        mov     bx, 5
        mul     bx
        mul     dx
        BENCHDONE 2, 128
        call    report

        /* ---------------- 3: DIV (1 div/unit) ---------------- */
        BENCH   3, 64, msg_t_div
        mov     ax, 1000
        xor     dx, dx
        mov     bx, 7
        div     bx
        BENCHDONE 3, 64
        call    report

        /* ---------------- 4: REP MOVSW copy, 4 KB/unit ---------------- */
        BENCH   4, 4, msg_t_copy
        mov     si, offset buf
        mov     di, offset buf2
        mov     cx, 512
        rep     movsw
        BENCHDONE 4, 4
        call    report

        /* ---------------- 5: REP STOSW fill, 4 KB/unit ---------------- */
        BENCH   5, 4, msg_t_fill
        mov     di, offset buf2
        mov     ax, 0x1234
        mov     cx, 512
        rep     stosw
        BENCHDONE 5, 4
        call    report

        /* ---------------- 6: REP LODSW read, 4 KB/unit ---------------- */
        BENCH   6, 4, msg_t_read
        mov     si, offset buf
        mov     cx, 512
        rep     lodsw
        BENCHDONE 6, 4
        call    report

        /* ---------------- 7: INT 21h call overhead ---------------- */
        BENCH   7, 8, msg_t_int21
        mov     ax, 0x3000
        int     0x21
        BENCHDONE 7, 8
        call    report

        /* ---------------- 8: disk read throughput, 64 KB/unit ------- */
        mov     dx, offset fn_bench
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      bench_nodisk
        mov     [g_handle], ax
        mov     word ptr [g_rd], 64
.ab_write:
        mov     bx, [g_handle]
        mov     dx, offset buf4
        mov     cx, 1024
        mov     ah, 0x40
        int     0x21
        jc      bench_nodisk
        dec     word ptr [g_rd]
        jnz     .ab_write
        mov     bx, [g_handle]
        mov     ah, 0x3E
        int     0x21

        BENCH   8, 1, msg_t_disk
        mov     dx, offset fn_bench
        mov     ax, 0x3D00
        int     0x21
        jc      .ab_stop
        mov     [g_handle], ax
        mov     word ptr [g_rd], 64
.ab_read:
        mov     bx, [g_handle]
        mov     dx, offset buf4
        mov     cx, 1024
        mov     ah, 0x3F
        int     0x21
        jc      .ab_stop
        dec     word ptr [g_rd]
        jnz     .ab_read
        mov     bx, [g_handle]
        mov     ah, 0x3E
        int     0x21
.ab_stop:
        BENCHDONE 8, 64
        call    report
        jmp     bench_tail

bench_nodisk:
        mov     dx, offset msg_t_disk_skip
        call    puts
        call    crlf

bench_tail:
        mov     dx, offset fn_bench
        mov     ah, 0x41
        int     0x21

        mov     dx, offset msg_rule
        call    puts
        mov     dx, offset msg_total
        call    puts
        mov     ax, [g_sum_lo]
        mov     dx, [g_sum_hi]
        call    put_u32
        call    crlf
        mov     dx, offset msg_footer
        call    puts

        mov     ax, 0x4C00
        int     0x21

/* ---- subroutines ---------------------------------------------------- */

/* get_ticks: AX = low word of the BIOS tick counter (0040:006C) */
get_ticks:
        push    ds
        mov     ax, 0x0040
        mov     ds, ax
        mov     ax, [0x006C]
        pop     ds
        ret

/* get_ram_kb: AX = conventional memory in KB, from the BIOS data area
 * (0040:0013).  INT 12h is not implemented by every BIOS and UEFI-DOS does not
 * trap it, which reported a leftover register instead of a memory size. */
get_ram_kb:
        push    ds
        mov     ax, 0x0040
        mov     ds, ax
        mov     ax, [0x0013]
        pop     ds
        ret

/* pit_read: AX = 8253 channel 0 counter (latch, then low byte, high byte) */
pit_read:
        push    dx
        mov     al, 0
        out     0x43, al
        in      al, 0x40
        mov     dl, al
        in      al, 0x40
        mov     ah, al
        mov     al, dl
        pop     dx
        ret

/* el_reset: start an elapsed-count measurement */
el_reset:
        mov     word ptr [g_el_lo], 0
        mov     word ptr [g_el_hi], 0
        mov     byte ptr [g_armed], 0
        call    pit_read
        mov     [g_prev], ax
        ret

/* el_poll: add the counts elapsed since the previous poll to g_el.
 * The counter runs downwards, so the delta is (previous - now) mod 65536.
 * Until a quarter of the window has gone by, the accumulator (and the caller's
 * operation counter) is kept at zero: that is the warm-up, during which the
 * pacer is still re-converging after the previous test's instruction mix. */
el_poll:
        call    pit_read
        mov     bx, [g_prev]
        mov     [g_prev], ax
        sub     bx, ax
        add     [g_el_lo], bx
        adc     word ptr [g_el_hi], 0
        cmp     byte ptr [g_armed], 0
        jne     .poll_done
        call    el_quarter
        jnc     .poll_done
        mov     byte ptr [g_armed], 1
        mov     word ptr [g_el_lo], 0
        mov     word ptr [g_el_hi], 0
        mov     word ptr [g_clo], 0
        mov     word ptr [g_chi], 0
.poll_done:
        ret

/* el_ge_window: CF set once a full second (1193182 counts) is in g_el */
el_ge_window:
        mov     ax, [g_el_hi]
        cmp     ax, 0x0012
        jb      .w_no
        ja      .w_yes
        mov     ax, [g_el_lo]
        cmp     ax, 0x34DE
        jb      .w_no
.w_yes:
        stc
        ret
.w_no:
        clc
        ret

/* el_quarter: CF set once a quarter second (298295 counts) is in g_el */
el_quarter:
        mov     ax, [g_el_hi]
        cmp     ax, 0x0004
        jb      .q_no
        ja      .q_yes
        mov     ax, [g_el_lo]
        cmp     ax, 0x8D37
        jb      .q_no
.q_yes:
        stc
        ret
.q_no:
        clc
        ret

/* report: DX:AX = operations per second for the test just finished */
report:
        cmp     byte ptr [g_timeout], 0
        je      .r_ok
        mov     dx, offset msg_timeout
        call    puts
        call    crlf
        ret
.r_ok:
        add     [g_sum_lo], ax
        adc     [g_sum_hi], dx
        call    put_u32
        mov     dx, offset msg_per_s
        call    puts
        call    crlf
        ret

/* put_u32: DX:AX = hi:lo -> decimal, suppresses leading zeros */
put_u32:
        push    si
        push    di
        push    bx
        push    cx
        push    bp
        mov     si, ax
        mov     di, dx
        mov     bp, offset powtab
        mov     cx, 10
        mov     byte ptr [g_started], 0
.p_dig:
        xor     bx, bx
.p_sub:
        mov     ax, [bp+2]
        cmp     di, ax
        ja      .p_take
        jb      .p_skip
        mov     ax, [bp]
        cmp     si, ax
        jb      .p_skip
.p_take:
        mov     ax, [bp]
        sub     si, ax
        mov     ax, [bp+2]
        sbb     di, ax
        inc     bx
        jmp     .p_sub
.p_skip:
        cmp     byte ptr [g_started], 0
        jne     .p_out
        cmp     cx, 1
        je      .p_out
        test    bx, bx
        jz      .p_next
.p_out:
        mov     byte ptr [g_started], 1
        mov     al, bl
        add     al, '0'
        call    put_ch
.p_next:
        add     bp, 4
        dec     cx
        jnz     .p_dig
        cmp     byte ptr [g_started], 0
        jne     .p_done
        mov     al, '0'
        call    put_ch
.p_done:
        pop     bp
        pop     cx
        pop     bx
        pop     di
        pop     si
        ret

/* put_u16: AX -> decimal */
put_u16:
        xor     dx, dx
        jmp     put_u32

/* put_ch: AL -> console */
put_ch:
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 2
        int     0x21
        pop     dx
        pop     ax
        ret

/* puts: DS:DX -> '$' terminated string */
puts:
        push    ax
        mov     ah, 9
        int     0x21
        pop     ax
        ret

crlf:
        push    dx
        mov     dx, offset msg_crlf
        call    puts
        pop     dx
        ret

/* ---- data -----------------------------------------------------------
 * Kept inside .text: a .COM is one flat image loaded at offset 0x100, and a
 * separate .data section would be padded onto a 4 KB boundary by the linker,
 * leaving holes in the middle of the file. */
msg_crlf:       .ascii "\r\n$"
msg_banner:     .ascii "AUTOBANCH 1.0 -- UEFI-DOS emulated CPU performance probe\r\n$"
msg_rule:       .ascii "--------------------------------------------------------\r\n$"
msg_dosver:     .ascii "DOS version ......... $"
msg_video:      .ascii "Video mode .......... $"
msg_cols:       .ascii "   columns $"
msg_mem:        .ascii "Conventional RAM .... $"
msg_kb:         .ascii " KB$"
msg_tickrate:   .ascii "BIOS ticks .......... $"
msg_calib:      .ascii "Calibrating the clock 1 s . $"
msg_ok:         .ascii "ok$"
msg_legend:     .ascii "Each test: 0.25 s warm-up, then a 1.000 s window.\r\n$"
msg_t_alu:      .ascii "ALU chain (8 ops) ... $"
msg_t_mul:      .ascii "MUL r/m16 x2 ........ $"
msg_t_div:      .ascii "DIV r/m16 ........... $"
msg_t_copy:     .ascii "REP MOVSW copy KB ... $"
msg_t_fill:     .ascii "REP STOSW fill KB ... $"
msg_t_read:     .ascii "REP LODSW read KB ... $"
msg_t_int21:    .ascii "INT 21h call ........ $"
msg_t_disk:     .ascii "Disk read KB ........ $"
msg_t_disk_skip:.ascii "Disk read ........... skipped (media is read-only)$"
msg_per_s:      .ascii " /s$"
msg_timeout:    .ascii "<no usable PIT clock>$"
msg_total:      .ascii "TOTAL SUMMED RATE ... $"
msg_footer:     .ascii "Bigger figures = faster host. Run this on two machines and\r\ncompare line by line; TOTAL is a rough single-figure index.\r\n$"
fn_bench:       .asciz "BENCH.TMP"
g_t0:           .word 0
g_handle:       .word 0
g_rd:           .word 0
g_clo:          .word 0
g_chi:          .word 0
g_sum_lo:       .word 0
g_sum_hi:       .word 0
g_prev:         .word 0
g_el_lo:        .word 0
g_el_hi:        .word 0
g_ticklast:     .word 0
g_tickcnt:      .word 0
g_started:      .byte 0
g_armed:        .byte 0
g_timeout:      .byte 0
        .align 2
powtab:         .long 1000000000, 100000000, 10000000, 1000000, 100000
        .long 10000, 1000, 100, 10, 1
        .align 2
buf:            .space 1024
buf2:           .space 1024
buf4:           .space 1024
