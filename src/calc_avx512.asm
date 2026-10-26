;
; calc_avx512.asm - AVX-512 kernels (process 8 doubles per instruction).
;
; System V AMD64 ABI:
;   rdi = data pointer, rsi = element count, xmm0 = return value.
;
; Only executed after runtime CPUID dispatch confirmed AVX-512F support.
; Loads use vmovupd so no 64-byte alignment is required on the data.
;
default rel
section .text

global calc_sum_avx512:function
global calc_max_avx512:function
global calc_min_avx512:function

; -------------------------------------------------------------------
; double calc_sum_avx512(const double *data, size_t n)
; -------------------------------------------------------------------
calc_sum_avx512:
    vxorpd  zmm0, zmm0, zmm0
    mov     rax, rsi
    shr     rax, 3                  ; number of 8-double vectors
    test    rax, rax
    jz      .horizontal
.vector_loop:
    vmovupd zmm1, [rdi]
    vaddpd  zmm0, zmm0, zmm1
    add     rdi, 64
    dec     rax
    jnz     .vector_loop
.horizontal:                        ; reduce zmm0 -> ymm0 -> xmm0
    vextractf64x4 ymm1, zmm0, 1
    vaddpd  ymm0, ymm0, ymm1
    vextractf128 xmm1, ymm0, 1
    vaddpd  xmm0, xmm0, xmm1
    movhlps xmm1, xmm0
    vaddsd  xmm0, xmm0, xmm1
.tail:
    and     rsi, 7                  ; scalar tail (0-7 elements)
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
; double calc_max_avx512(const double *data, size_t n)
; -------------------------------------------------------------------
calc_max_avx512:
    test    rsi, rsi
    jz      .empty
    vbroadcastsd zmm0, [rdi]        ; seed all 8 lanes with element 0
    mov     rax, rsi
    shr     rax, 3
    test    rax, rax
    jz      .tail
.vector_loop:
    vmovupd zmm1, [rdi]
    vmaxpd  zmm0, zmm0, zmm1
    add     rdi, 64
    dec     rax
    jnz     .vector_loop
.horizontal:
    vextractf64x4 ymm1, zmm0, 1
    vmaxpd  ymm0, ymm0, ymm1
    vextractf128 xmm1, ymm0, 1
    vmaxpd  xmm0, xmm0, xmm1
    movhlps xmm1, xmm0
    vmaxsd  xmm0, xmm0, xmm1
.tail:
    and     rsi, 7
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
; double calc_min_avx512(const double *data, size_t n)
; -------------------------------------------------------------------
calc_min_avx512:
    test    rsi, rsi
    jz      .empty
    vbroadcastsd zmm0, [rdi]
    mov     rax, rsi
    shr     rax, 3
    test    rax, rax
    jz      .tail
.vector_loop:
    vmovupd zmm1, [rdi]
    vminpd  zmm0, zmm0, zmm1
    add     rdi, 64
    dec     rax
    jnz     .vector_loop
.horizontal:
    vextractf64x4 ymm1, zmm0, 1
    vminpd  ymm0, ymm0, ymm1
    vextractf128 xmm1, ymm0, 1
    vminpd  xmm0, xmm0, xmm1
    movhlps xmm1, xmm0
    vminsd  xmm0, xmm0, xmm1
.tail:
    and     rsi, 7
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
