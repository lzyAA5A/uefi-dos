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
        mov     word ptr [g_units], 0
        mov     word ptr [g_gapmax], 0
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
        inc     word ptr [g_units]
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

        /* Open B<yymmdd>.TXT first, so even the banner lands in the file: every
         * character that reaches the console is mirrored into it. */
        call    open_result_file
        call    init_data

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
        jmp     bench_more

bench_nodisk:
        mov     dx, offset msg_t_disk_skip
        call    puts
        call    crlf

bench_more:

        /* ---------------- 9: x87 arithmetic chain (6 ops/unit) ------ */
        BENCH   9, 32, msg_t_fpu
        fld     dword ptr [f_x]
        fadd    dword ptr [f_y]
        fmul    dword ptr [f_z]
        fsub    dword ptr [f_y]
        fdiv    dword ptr [f_w]
        fstp    dword ptr [g_fscratch]
        BENCHDONE 9, 6
        call    report

        /* ---------------- 10: FSQRT (8 per body) -------------------- */
        BENCH   10, 8, msg_t_sqrt
        fld     dword ptr [f_x]
        fsqrt
        fstp    dword ptr [g_fscratch]
        BENCHDONE 10, 8
        call    report

        /* ---------------- 11: 640x200x256 frame, written by hand ---- */
        /* Mode 13h is entered AFTER the label is printed, so the label still
         * reaches the text console; the window then runs in graphics mode and
         * the picture is written straight into the linear framebuffer, one
         * 64000-byte frame per unit. */
        mov     dx, offset msg_t_gfx
        call    puts
        mov     ax, 0x0013
        int     0x10
        call    el_reset
        mov     byte ptr [g_timeout], 0
        mov     word ptr [g_clo], 0
        mov     word ptr [g_chi], 0
        call    get_ticks
        mov     [g_t0], ax
.Lloop11:
        mov     bp, 1
.Lin11:
        push    es
        mov     ax, 0xA000
        mov     es, ax
        xor     di, di
        mov     ax, [g_floor]
        mov     cx, 32000
        rep     stosw
        inc     word ptr [g_floor]
        pop     es
        BENCHDONE 11, 1
        push    ax
        push    dx
        mov     ax, 0x0003
        int     0x10
        pop     dx
        pop     ax
        call    report

        /* ---------------- 12: CGA text VRAM fill, one screen ------- */
        /* 2000 words of B800:0000 per unit, i.e. 4000 bytes of emulated video
         * memory, which is what a scrolling text console lives on. */
        BENCH   12, 2, msg_t_vram
        push    es
        mov     ax, 0xB800
        mov     es, ax
        xor     di, di
        mov     ax, 0x0720
        mov     cx, 2000
        rep     stosw
        pop     es
        BENCHDONE 12, 1
        call    report

        /* ---------------- 13: sieve of Eratosthenes to 4096 -------- */
        BENCH   13, 1, msg_t_sieve
        call    sieve_unit
        BENCHDONE 13, 1
        call    report

        /* ---------------- 14: insertion sort of 256 words ---------- */
        BENCH   14, 1, msg_t_sort
        call    sort_unit
        BENCHDONE 14, 1
        call    report

        /* ---------------- 15: CRC16-CCITT over 1 KB ---------------- */
        BENCH   15, 1, msg_t_crc
        call    crc_unit
        BENCHDONE 15, 1
        call    report

        /* ---------------- 16: 8x8 int16 matrix multiply ------------ */
        BENCH   16, 1, msg_t_mat
        call    mat_unit
        BENCHDONE 16, 1
        call    report

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

        /* Everything above is already in B<yymmdd>.TXT; say so and close it. */
        call    close_result_file
        mov     dx, offset msg_saved
        call    puts
        mov     dx, offset fn_res
        call    puts_z
        call    crlf
        mov     dx, offset msg_native
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
        cmp     bx, [g_gapmax]
        jbe     .poll_nogap
        mov     [g_gapmax], bx
.poll_nogap:
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
        mov     dx, offset msg_diag_units
        call    puts
        mov     ax, [g_units]
        call    put_u16
        mov     dx, offset msg_diag_gap
        call    puts
        mov     ax, [g_gapmax]
        call    put_u16
        mov     dx, offset msg_diag_el
        call    puts
        mov     ax, [g_el_lo]
        mov     dx, [g_el_hi]
        call    put_u32
        mov     dx, offset msg_diag_end
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

/* put_ch: AL -> console, and into the result file when one is open.  The file
 * side is buffered (512 bytes) because a one-byte write per character would
 * turn every result line into a cluster of disk writes. */
put_ch:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     dl, al
        mov     ah, 2
        int     0x21
        cmp     word ptr [g_fh], 0xFFFF
        je      .pc_done
        mov     bx, [g_fbuf_n]
        mov     al, dl
        mov     [g_fbuf+bx], al
        inc     word ptr [g_fbuf_n]
        cmp     word ptr [g_fbuf_n], 512
        jb      .pc_done
        call    flush_file
.pc_done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* puts: DS:DX -> '$' terminated string, printed a character at a time so that
 * everything on the console also lands in the result file (AH=9 would bypass
 * put_ch and leave holes in the file). */
puts:
        push    ax
        push    dx
        push    si
        mov     si, dx
.pu_loop:
        mov     al, [si]
        cmp     al, '$'
        je      .pu_done
        inc     si
        call    put_ch
        jmp     .pu_loop
.pu_done:
        pop     si
        pop     dx
        pop     ax
        ret

/* puts_z: DX -> NUL terminated string, printed through put_ch so it is
 * mirrored into the result file as well */
puts_z:
        push    ax
        push    dx
        push    si
        mov     si, dx
.pz_loop:
        mov     al, [si]
        test    al, al
        jz      .pz_done
        inc     si
        call    put_ch
        jmp     .pz_loop
.pz_done:
        pop     si
        pop     dx
        pop     ax
        ret

/* ---- result file ---------------------------------------------------- */

/* bcd2: AL (0..99) -> two digits at [BX], BX advanced by two */
bcd2:
        push    ax
        push    cx
        mov     ah, 0
        mov     cl, 10
        div     cl                      /* AL = tens, AH = units */
        add     al, '0'
        add     ah, '0'
        mov     [bx], al
        mov     [bx+1], ah
        add     bx, 2
        pop     cx
        pop     ax
        ret

/* flush_file: write the pending characters to the result file */
flush_file:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     cx, [g_fbuf_n]
        jcxz    .ff_done
        cmp     word ptr [g_fh], 0xFFFF
        je      .ff_clear
        mov     bx, [g_fh]
        mov     dx, offset g_fbuf
        mov     ah, 0x40
        int     0x21
.ff_clear:
        mov     word ptr [g_fbuf_n], 0
.ff_done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* open_result_file: build B<yymmdd>.TXT from the DOS date and create it.  If
 * the volume is read-only the program carries on console-only, which is the
 * case for a benchmark run straight off a CD or a locked image. */
open_result_file:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     word ptr [g_fh], 0xFFFF
        mov     word ptr [g_fbuf_n], 0
        mov     ax, 0x2A00              /* CX = year, DH = month, DL = day */
        int     0x21
        mov     [g_month], dh
        mov     [g_day], dl
        mov     ax, cx                  /* year -> two digits (1980..2099) */
        xor     dx, dx
        mov     cx, 100
        div     cx
        mov     [g_year2], dl
        mov     bx, offset fn_res+1
        mov     al, [g_year2]
        call    bcd2
        mov     al, [g_month]
        call    bcd2
        mov     al, [g_day]
        call    bcd2
        /* Append, do not create: BENCH32/BENCH64 write their own sections into
         * the same file, and AH=3Ch would truncate whatever they left. */
        mov     dx, offset fn_res
        mov     ax, 0x3D02              /* open read/write */
        int     0x21
        jnc     .orf_have
        mov     dx, offset fn_res
        xor     cx, cx
        mov     ah, 0x3C                /* create it */
        int     0x21
        jc      .orf_open_fail
.orf_have:
        mov     [g_fh], ax
        mov     bx, ax
        xor     cx, cx
        xor     dx, dx
        mov     ax, 0x4202              /* seek to end of file */
        int     0x21
        mov     dx, offset msg_resfile
        call    puts
        mov     dx, offset fn_res
        call    puts_z
        mov     dx, offset msg_append
        call    puts
        call    crlf
        jmp     .orf_done
.orf_open_fail:
        mov     dx, offset msg_resfail
        call    puts
        call    crlf
.orf_done:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* close_result_file: flush the tail of the buffer and close the handle */
close_result_file:
        cmp     word ptr [g_fh], 0xFFFF
        je      .crf_done
        call    flush_file
        mov     bx, [g_fh]
        mov     ah, 0x3E
        int     0x21
        mov     word ptr [g_fh], 0xFFFF
.crf_done:
        ret

/* ---- algorithms ----------------------------------------------------- */

/* sieve_unit: sieve of Eratosthenes over SIEVE_N bytes; result folded into
 * g_sink so nothing can be optimised away (the emulator cannot optimise, but a
 * reader can check the count is real). */
sieve_unit:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     di, offset sieve_buf
        mov     cx, offset SIEVE_N
        xor     ax, ax
        rep     stosb
        mov     bx, 2
.sv_outer:
        cmp     bx, offset SIEVE_ROOT
        ja      .sv_count
        mov     di, offset sieve_buf
        add     di, bx
        cmp     byte ptr [di], 0
        jne     .sv_next
        mov     ax, bx
        mul     bx                      /* DX:AX = bx*bx */
        cmp     ax, offset SIEVE_N
        jae     .sv_next
        mov     di, offset sieve_buf
        add     di, ax
.sv_inner:
        cmp     di, offset sieve_buf + SIEVE_N
        jae     .sv_next
        mov     byte ptr [di], 1
        add     di, bx
        jmp     .sv_inner
.sv_next:
        inc     bx
        jmp     .sv_outer
.sv_count:
        mov     di, offset sieve_buf + 2
        mov     dx, offset sieve_buf + SIEVE_N
        mov     bx, 0
.sv_cc:
        cmp     di, dx
        jae     .sv_done
        cmp     byte ptr [di], 0
        jne     .sv_cc2
        inc     bx
.sv_cc2:
        inc     di
        jmp     .sv_cc
.sv_done:
        add     [g_sink], bx
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* sort_unit: refill 256 words from a template and insertion-sort them */
sort_unit:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     si, offset sort_tmpl
        mov     di, offset sort_arr
        mov     cx, offset SORT_N
        rep     movsw
        mov     bx, 2
.srt_outer:
        cmp     bx, offset SORT_N*2
        jae     .srt_done
        mov     ax, [sort_arr+bx]
        mov     di, bx
.srt_inner:
        test    di, di
        jz      .srt_place
        mov     dx, [sort_arr+di-2]
        cmp     dx, ax
        jbe     .srt_place
        mov     [sort_arr+di], dx
        sub     di, 2
        jmp     .srt_inner
.srt_place:
        mov     [sort_arr+di], ax
        add     bx, 2
        jmp     .srt_outer
.srt_done:
        mov     ax, [sort_arr]
        add     [g_sink], ax
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* crc_unit: CRC16-CCITT (0x1021, seed 0xFFFF) over 1 KB of buf */
crc_unit:
        push    bx
        push    cx
        push    dx
        push    si
        mov     si, offset buf
        mov     cx, 1024
        mov     ax, 0xFFFF
.crc_byte:
        mov     bl, [si]
        inc     si
        xor     ah, bl
        mov     dx, 8
.crc_bit:
        shl     ax, 1
        jnc     .crc_nx
        xor     ax, 0x1021
.crc_nx:
        dec     dx
        jnz     .crc_bit
        loop    .crc_byte
        add     [g_sink], ax
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

/* mat_unit: C(8x8) = A(8x8) * B(8x8) in 16-bit words -- 512 mul+add */
mat_unit:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     word ptr [g_mrow], 0
.m_r:
        mov     word ptr [g_mcol], 0
.m_c:
        mov     ax, [g_mcol]
        shl     ax, 1
        mov     [g_mcol2], ax
        mov     word ptr [g_mk], 0
        mov     word ptr [g_macc], 0
.m_k:
        mov     ax, [g_mrow]
        mov     cl, 4
        shl     ax, cl                  /* row * 16 bytes */
        mov     bx, [g_mk]
        shl     bx, 1                   /* k * 2 */
        add     ax, bx
        mov     [g_aoff], ax
        mov     ax, [g_mk]
        shl     ax, cl                  /* k * 16 bytes */
        add     ax, [g_mcol2]
        mov     [g_boff], ax
        mov     bx, [g_aoff]
        mov     ax, [mat_a+bx]
        mov     bx, [g_boff]
        mul     word ptr [mat_b+bx]
        add     [g_macc], ax
        inc     word ptr [g_mk]
        cmp     word ptr [g_mk], 8
        jb      .m_k
        mov     bx, [g_mrow]
        mov     cl, 4
        shl     bx, cl
        add     bx, [g_mcol2]
        mov     ax, [g_macc]
        mov     [mat_c+bx], ax
        inc     word ptr [g_mcol]
        cmp     word ptr [g_mcol], 8
        jb      .m_c
        inc     word ptr [g_mrow]
        cmp     word ptr [g_mrow], 8
        jb      .m_r
        mov     ax, [mat_c]
        add     [g_sink], ax
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* init_data: fill the sortable array from a 16-bit LCG (an all-zero array would
 * be sorted in one pass and measure nothing) and give the matrices small
 * non-zero entries. */
init_data:
        push    ax
        push    bx
        push    cx
        push    di
        mov     ax, 1234
        mov     di, offset sort_tmpl
        mov     cx, offset SORT_N
        mov     bx, 25173
.id_l:
        mul     bx                      /* x = x * 25173 + 13849 */
        add     ax, 13849
        mov     [di], ax
        add     di, 2
        loop    .id_l
        mov     di, offset mat_a
        mov     cx, 64
        mov     ax, 1
.id_m:
        mov     [di], ax
        add     di, 2
        inc     ax
        and     ax, 7
        loop    .id_m
        mov     di, offset mat_b
        mov     cx, 64
        mov     ax, 3
.id_n:
        mov     [di], ax
        add     di, 2
        inc     ax
        and     ax, 7
        loop    .id_n
        pop     di
        pop     cx
        pop     bx
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
msg_banner:     .ascii "AUTOBANCH 2.0 -- UEFI-DOS performance probe, 16-bit real mode\r\n$"
msg_rule:       .ascii "--------------------------------------------------------\r\n$"
msg_resfile:    .ascii "Result file ......... $"
msg_resfail:    .ascii "Result file ......... cannot create; console only\r\n$"
msg_append:     .ascii " (appending)$"
msg_saved:      .ascii "Results saved to $"
msg_native:     .ascii "Run BENCH32 / BENCH64 for the same box in 32- and 64-bit.\r\n$"
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
msg_t_fpu:      .ascii "x87 chain (6 FP ops)  $"
msg_t_sqrt:     .ascii "FSQRT ............... $"
msg_t_gfx:      .ascii "640x200 frame (64000B) $"
msg_t_vram:     .ascii "CGA text fill screens  $"
msg_t_sieve:    .ascii "Sieve to 4096 ....... $"
msg_t_sort:     .ascii "Insertion sort 256 .. $"
msg_t_crc:      .ascii "CRC16 over 1 KB ..... $"
msg_t_mat:      .ascii "8x8 matmul (512 mul)  $"
msg_per_s:      .ascii " /s$"
msg_timeout:    .ascii "<no PIT clock>$"
msg_diag_units: .ascii " units=$"
msg_diag_gap:   .ascii " maxgap=$"
msg_diag_el:    .ascii " el=$"
msg_diag_end:   .ascii ">"
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
g_units:        .word 0
g_gapmax:       .word 0
g_el_lo:        .word 0
g_el_hi:        .word 0
g_ticklast:     .word 0
g_tickcnt:      .word 0
g_started:      .byte 0
g_armed:        .byte 0
g_timeout:      .byte 0
g_year2:        .byte 0
g_month:        .byte 0
g_day:          .byte 0
        .align 2
g_fh:           .word 0xFFFF    /* result file handle, 0xFFFF = no file */
g_fbuf_n:       .word 0
g_fscratch:     .long 0
g_floor:        .word 0x0101
g_sink:         .word 0
g_mrow:         .word 0
g_mcol:         .word 0
g_mcol2:        .word 0
g_mk:           .word 0
g_macc:         .word 0
g_aoff:         .word 0
g_boff:         .word 0
f_x:            .float 1.5
f_y:            .float 2.25
f_z:            .float 0.75
f_w:            .float 3.5
fn_res:         .asciz "B000000.TXT"
        .align 2
powtab:         .long 1000000000, 100000000, 10000000, 1000000, 100000
        .long 10000, 1000, 100, 10, 1
        .align 2
buf:            .space 1024
buf2:           .space 1024
buf4:           .space 1024
g_fbuf:         .space 512
        .align 2
/* sieve of Eratosthenes up to 4096, root = 64 */
SIEVE_N = 4096
SIEVE_ROOT = 64
sieve_buf:      .space SIEVE_N
        .align 2
SORT_N = 256
sort_arr:       .space SORT_N*2
sort_tmpl:      .space SORT_N*2
mat_a:          .space 128
mat_b:          .space 128
mat_c:          .space 128
