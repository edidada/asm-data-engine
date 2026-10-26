;
; calc_avx2.asm - AVX2 kernels (process 4 doubles per instruction).
;
; System V AMD64 ABI:
;   rdi = data pointer, rsi = element count, xmm0 = return value.
;
; Loads use vmovupd so no 32-byte alignment is required on the data.
;
default rel
section .text

global calc_sum_avx2:function
global calc_max_avx2:function
global calc_min_avx2:function

; -------------------------------------------------------------------
; double calc_sum_avx2(const double *data, size_t n)
; -------------------------------------------------------------------
calc_sum_avx2:
    vxorpd  ymm0, ymm0, ymm0
    mov     rax, rsi
    shr     rax, 2                  ; number of 4-double vectors
    test    rax, rax
    jz      .horizontal
.vector_loop:
    vmovupd ymm1, [rdi]
    vaddpd  ymm0, ymm0, ymm1
    add     rdi, 32
    dec     rax
    jnz     .vector_loop
.horizontal:                        ; reduce ymm0 -> xmm0
    vextractf128 xmm1, ymm0, 1
    vaddpd  xmm0, xmm0, xmm1
    movhlps xmm1, xmm0
    vaddsd  xmm0, xmm0, xmm1
.tail:
    and     rsi, 3                  ; scalar tail (0-3 elements)
    jz      .done
.tail_loop:
    vaddsd  xmm0, xmm0, [rdi]
    add     rdi, 8
    dec     rsi
    jnz     .tail_loop
.done:
    vzeroupper
    ret
.empty:
    vxorpd  xmm0, xmm0, xmm0
    vzeroupper
    ret

; -------------------------------------------------------------------
; double calc_max_avx2(const double *data, size_t n)
; -------------------------------------------------------------------
calc_max_avx2:
    test    rsi, rsi
    jz      .empty
    vbroadcastsd ymm0, [rdi]        ; seed all 4 lanes with element 0
    mov     rax, rsi
    shr     rax, 2
    test    rax, rax
    jz      .tail
.vector_loop:
    vmovupd ymm1, [rdi]
    vmaxpd  ymm0, ymm0, ymm1
    add     rdi, 32
    dec     rax
    jnz     .vector_loop
.horizontal:
    vextractf128 xmm1, ymm0, 1
    vmaxpd  xmm0, xmm0, xmm1
    movhlps xmm1, xmm0
    vmaxsd  xmm0, xmm0, xmm1
.tail:
    and     rsi, 3
    jz      .done
.tail_loop:
    vmaxsd  xmm0, xmm0, [rdi]
    add     rdi, 8
    dec     rsi
    jnz     .tail_loop
.done:
    vzeroupper
    ret
.empty:
    vxorpd  xmm0, xmm0, xmm0
    vzeroupper
    ret

; -------------------------------------------------------------------
; double calc_min_avx2(const double *data, size_t n)
; -------------------------------------------------------------------
calc_min_avx2:
    test    rsi, rsi
    jz      .empty
    vbroadcastsd ymm0, [rdi]
    mov     rax, rsi
    shr     rax, 2
    test    rax, rax
    jz      .tail
.vector_loop:
    vmovupd ymm1, [rdi]
    vminpd  ymm0, ymm0, ymm1
    add     rdi, 32
    dec     rax
    jnz     .vector_loop
.horizontal:
    vextractf128 xmm1, ymm0, 1
    vminpd  xmm0, xmm0, xmm1
    movhlps xmm1, xmm0
    vminsd  xmm0, xmm0, xmm1
.tail:
    and     rsi, 3
    jz      .done
.tail_loop:
    vminsd  xmm0, xmm0, [rdi]
    add     rdi, 8
    dec     rsi
    jnz     .tail_loop
.done:
    vzeroupper
    ret
.empty:
    vxorpd  xmm0, xmm0, xmm0
    vzeroupper
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
