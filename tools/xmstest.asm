/* xmstest.asm -- XMSTEST: prove the UEFI-DOS XMS driver from inside a DOS
 * program, i.e. the way a 16-bit guest actually sees extended memory.
 *
 * UEFI-DOS keeps an 8 GB extended-memory pool that costs firmware RAM only for
 * the 64 KB pages a program really writes to.  A 16-bit machine cannot address
 * any of that directly, so it is reached the standard way: INT 0x2F AX=0x4310
 * hands out a far entry point, and the XMS functions below allocate a block,
 * move a pattern into it, move it back, compare, and free it again.
 *
 * Build (same toolchain detour as AUTOBANCH, see build/mkbench.ps1):
 *   as --32 -o xmstest.o tools/xmstest.asm
 *   ld -m i386pe -Ttext 0x100 --image-base 0 -nostdlib -o xm.exe xmstest.o
 *   objcopy -O binary --only-section=.text xm.exe XMSTEST.COM
 */

        .code16
        .intel_syntax noprefix
        .text
        .globl _start

/* call far through the XMS entry point stored at g_entry (FF 1E addr16) */
.macro XMSCALL
        .byte   0xFF, 0x1E
        .word   offset g_entry
.endm

_start:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cld
        mov     dx, offset msg_banner
        call    puts

/* ---- 1. is there a driver at all? ---------------------------------- */
        mov     ax, 0x4300
        int     0x2f
        cmp     al, 0x80
        je      have_driver
        mov     dx, offset msg_nodrv
        call    puts
        jmp     done
have_driver:
        mov     dx, offset msg_yes
        call    puts
        mov     ax, 0x4310
        int     0x2f
        mov     [g_entry], bx
        mov     word ptr [g_entry + 2], es
        mov     ax, cs
        mov     es, ax                  /* that call returns its answer in ES:BX,
                                         * so ES is not CS any more -- and cmpsw
                                         * compares against ES:DI */

/* ---- 2. version ---------------------------------------------------- */
        mov     ah, 0x00
        XMSCALL
        mov     [g_ver], ax
        mov     dx, offset msg_ver
        call    puts
        mov     dx, [g_ver]
        call    put_hex16
        call    crlf

/* ---- 3. free extended memory, in MB ------------------------------- */
        mov     ah, 0x08                 /* query free: AX = largest block in
                                         * KB, DX = total free KB (this is the
                                         * 16-bit view; the 32-bit one keeps its
                                         * answer in EAX/EDX) */
        XMSCALL
        mov     [g_free_lo], ax
        mov     [g_free_hi], dx
        mov     dx, offset msg_free
        call    puts
        mov     ax, [g_free_lo]
        call    put_u16
        mov     dx, offset msg_free2
        call    puts
        mov     ax, [g_free_hi]
        call    put_u16
        call    crlf

/* ---- 4. allocate 1 MB -------------------------------------------- */
        mov     ah, 0x09
        mov     dx, 1024                /* KB */
        XMSCALL
        or      ax, ax
        jnz     alloc_ok
        mov     dx, offset msg_alloc_fail
        call    puts
        mov     dx, bx
        call    put_hex16               /* BL = reason */
        call    crlf
        jmp     done
alloc_ok:
        mov     [g_handle], dx
        mov     dx, offset msg_alloc
        call    puts
        mov     dx, [g_handle]
        call    put_hex16
        call    crlf

/* ---- 5. fill a conventional buffer, move it into the block ------- */
        mov     si, offset g_buf1
        mov     cx, 2048                /* 2048 words = 4096 bytes */
        mov     ax, 0x0A55A
fill1:
        mov     [si], ax
        add     ax, 0x0101
        add     si, 2
        loop    fill1

        mov     word ptr [g_emb + 0], 4096      /* length in bytes */
        mov     word ptr [g_emb + 2], 0
        mov     word ptr [g_emb + 4], 0         /* source handle 0 = memory */
        mov     ax, offset g_buf1
        mov     [g_emb + 6], ax
        mov     [g_emb + 8], ds
        mov     ax, [g_handle]                  /* destination = the block */
        mov     [g_emb + 10], ax
        mov     word ptr [g_emb + 12], 0
        mov     word ptr [g_emb + 14], 0
        mov     si, offset g_emb
        mov     ah, 0x0B
        XMSCALL
        or      ax, ax
        jnz     move1_ok
        mov     dx, offset msg_move_fail
        call    puts
        mov     dx, bx
        call    put_hex16
        call    crlf
        jmp     cleanup
move1_ok:
        mov     dx, offset msg_move_in
        call    puts
        call    crlf

/* ---- 6. move it back into a second buffer and compare ------------ */
        mov     si, offset g_buf2
        mov     cx, 2048
        xor     ax, ax
zero2:
        mov     [si], ax
        add     si, 2
        loop    zero2

        mov     word ptr [g_emb + 0], 4096
        mov     word ptr [g_emb + 2], 0
        mov     ax, [g_handle]                  /* source = the block */
        mov     [g_emb + 4], ax
        mov     word ptr [g_emb + 6], 0
        mov     word ptr [g_emb + 8], 0
        mov     word ptr [g_emb + 10], 0        /* destination = memory */
        mov     ax, offset g_buf2
        mov     [g_emb + 12], ax
        mov     [g_emb + 14], ds
        mov     si, offset g_emb
        mov     ah, 0x0B
        XMSCALL
        or      ax, ax
        jnz     move2_ok
        mov     dx, offset msg_move_fail
        call    puts
        mov     dx, bx
        call    put_hex16
        call    crlf
        jmp     cleanup
move2_ok:
        mov     si, offset g_buf1
        mov     di, offset g_buf2
        mov     cx, 2048
        repe    cmpsw
        je      same
        mov     dx, offset msg_where
        call    puts
        mov     dx, cx
        call    put_hex16
        call    crlf
        jmp     cleanup
        mov     dx, offset msg_mismatch
        call    puts
        jmp     cleanup
same:
        mov     dx, offset msg_roundtrip
        call    puts
        call    crlf

/* ---- 7. offset inside the block is 32-bit: write at 512 KB ------ */
        mov     word ptr [g_emb + 0], 4         /* move 4 bytes */
        mov     word ptr [g_emb + 2], 0
        mov     word ptr [g_emb + 4], 0
        mov     ax, offset g_buf1
        mov     [g_emb + 6], ax
        mov     [g_emb + 8], ds
        mov     ax, [g_handle]
        mov     [g_emb + 10], ax
        mov     word ptr [g_emb + 12], 0        /* destination offset */
        mov     word ptr [g_emb + 14], 0x08      /* 0x0800:0x0000 = 512 KB */
        mov     si, offset g_emb
        mov     ah, 0x0B
        XMSCALL
        or      ax, ax
        jz      cleanup
        mov     dx, offset msg_offset
        call    puts
        call    crlf

cleanup:
        mov     ah, 0x0A
        mov     dx, [g_handle]
        XMSCALL
        mov     dx, offset msg_freed
        call    puts
        call    crlf
done:
        mov     ax, 0x4C00
        int     0x21

/* ---- helpers ------------------------------------------------------ */
puts:                                    /* DX -> '$' terminated string */
        mov     ah, 0x09
        int     0x21
        ret

crlf:
        mov     dx, offset msg_crlf
        mov     ah, 0x09
        int     0x21
        ret

put_hex16:                               /* DX = value, four hex digits */
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, dx
        mov     cx, 4
.Lhx:
        rol     bx, 4
        mov     ax, bx
        and     ax, 0x0F
        cmp     al, 10
        jb      .Ldig
        add     al, 'A' - 10
        jmp     .Lout
.Ldig:
        add     al, '0'
.Lout:
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        loop    .Lhx
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* DX:AX kilobytes -> decimal megabytes (a shift right by 10, then a plain
 * 16-bit decimal print; 8 GB is 8192 MB, which fits) */
put_u32_mb:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     cx, 10
.Lshr:
        shr     dx, 1
        rcr     ax, 1
        loop    .Lshr
        call    put_u16
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

put_u16:                                 /* AX = value, decimal, no padding */
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     bx, offset pow10
        mov     byte ptr [g_started], 0
.Lnext:
        mov     cx, [bx]
        cmp     cx, 0
        je      .Lend
        xor     dx, dx
        div     cx                      /* AX = digit, DX = remainder */
        cmp     al, 0
        jne     .Lemit
        cmp     byte ptr [g_started], 0
        je      .Lkeep_rem
.Lemit:
        mov     byte ptr [g_started], 1
        mov     si, dx                  /* keep the remainder: DL is about to
                                         * carry the digit character */
        add     al, '0'
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        mov     dx, si
.Lkeep_rem:
        mov     ax, dx
        add     bx, 2
        jmp     .Lnext
.Lend:
        cmp     byte ptr [g_started], 0
        jne     .Lfin
        mov     dl, '0'
        mov     ah, 0x02
        int     0x21
.Lfin:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

/* ---- data (must live in .text: PE pads sections to 4 KB) ---------- */
msg_banner:     .asciz  "XMSTEST -- UEFI-DOS extended memory (XMS) probe\r\n$"
msg_crlf:       .asciz  "\r\n$"
msg_nodrv:      .asciz  "xmstest: no XMS driver (INT 0x2F AX=0x4300 did not answer 0x80).\r\n$"
msg_yes:        .asciz  "xmstest: XMS driver present, entry point from INT 0x2F AX=0x4310.\r\n$"
msg_ver:        .asciz  "xmstest: XMS version ....... $"
msg_free:       .asciz  "xmstest: largest block ...... $"
msg_free2:      .asciz  " KB, total free $"

msg_alloc:      .asciz  "xmstest: allocated 1 MB .... handle $"
msg_alloc_fail: .asciz  "xmstest: allocation failed .. reason $"
msg_move_in:    .asciz  "xmstest: 4 KB moved into the block.\r\n$"
msg_move_fail:  .asciz  "xmstest: move failed ....... reason $"
msg_roundtrip:  .asciz  "xmstest: moved back, all 4096 bytes identical.\r\n$"
msg_mismatch:   .asciz  "xmstest: MOVED DATA DOES NOT MATCH.\r\n$"
msg_diag:       .asciz  "xmstest: buf1/buf2 first word $"
msg_where:      .asciz  "xmstest: differing words remaining $"

msg_offset:     .asciz  "xmstest: 32-bit offset inside the block works (512 KB in).\r\n$"
msg_freed:      .asciz  "xmstest: block freed.\r\n$"

        .align  2
pow10:          .word   10000, 1000, 100, 10, 1, 0
g_entry:        .word   0, 0
g_handle:       .word   0
g_ver:          .word   0
g_free_lo:      .word   0
g_free_hi:      .word   0
g_started:      .byte   0
        .align  2
g_emb:          .space  16, 0
g_buf1:         .space  4096, 0
g_buf2:         .space  4096, 0
