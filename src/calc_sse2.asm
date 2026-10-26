;
; calc_sse2.asm - SSE2 kernels (process 2 doubles per instruction).
;
; System V AMD64 ABI:
;   rdi = data pointer, rsi = element count, xmm0 = return value.
;
default rel
section .text

global calc_sum_sse2:function
global calc_max_sse2:function
global calc_min_sse2:function

; -------------------------------------------------------------------
; double calc_sum_sse2(const double *data, size_t n)
; -------------------------------------------------------------------
calc_sum_sse2:
    xorpd   xmm0, xmm0              ; packed accumulator
    mov     rax, rsi
    shr     rax, 1                  ; number of pairs
    test    rax, rax
    jz      .horizontal
.pair_loop:
    movapd  xmm1, [rdi]
    addpd   xmm0, xmm1
    add     rdi, 16
    dec     rax
    jnz     .pair_loop
.horizontal:
    movhlps xmm1, xmm0              ; high lane -> xmm1
    addsd   xmm0, xmm1              ; horizontal add
    test    rsi, 1                  ; odd tail element
    jz      .done
    addsd   xmm0, [rdi]
.done:
    ret

; -------------------------------------------------------------------
; double calc_sse2_max/min common shape:
;   seed both 128-bit lanes with element 0, then max/min packed pairs.
;   (Duplicating the seed lane avoids the 0.0 in the high lane leading
;    to wrong results on all-negative inputs.)
; -------------------------------------------------------------------
calc_max_sse2:
    test    rsi, rsi
    jz      .empty
    movsd   xmm0, [rdi]
    movlhps xmm0, xmm0              ; broadcast seed to both lanes
    mov     rax, rsi
    shr     rax, 1
    test    rax, rax
    jz      .tail
.pair_loop:
    movapd  xmm1, [rdi]
    maxpd   xmm0, xmm1
    add     rdi, 16
    dec     rax
    jnz     .pair_loop
.tail:
    test    rsi, 1
    jz      .reduce
    maxsd   xmm0, [rdi]
.reduce:
    movhlps xmm1, xmm0
    maxsd   xmm0, xmm1
    ret
.empty:
    xorpd   xmm0, xmm0
    ret

calc_min_sse2:
    test    rsi, rsi
    jz      .empty
    movsd   xmm0, [rdi]
    movlhps xmm0, xmm0
    mov     rax, rsi
    shr     rax, 1
    test    rax, rax
    jz      .tail
.pair_loop:
    movapd  xmm1, [rdi]
    minpd   xmm0, xmm1
    add     rdi, 16
    dec     rax
    jnz     .pair_loop
.tail:
    test    rsi, 1
    jz      .reduce
    minsd   xmm0, [rdi]
.reduce:
    movhlps xmm1, xmm0
    minsd   xmm0, xmm1
    ret
.empty:
    xorpd   xmm0, xmm0
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
