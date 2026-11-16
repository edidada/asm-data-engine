;
; calc_rdtsc.asm - V7: serializing time-stamp counter reads.
;
; System V AMD64 ABI: no arguments, 64-bit cycle count in rax.
;
; `lfence` before RDTSC and after RDTSCP keeps the measurement from being
; reordered around the measured region by the out-of-order core.
;
default rel
section .text

global calc_cycles_begin:function
global calc_cycles_end:function

; uint64_t calc_cycles_begin(void)
calc_cycles_begin:
    lfence
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    ret

; uint64_t calc_cycles_end(void)
calc_cycles_end:
    rdtscp                  ; EDX:EAX = TSC, ECX = IA32_TSC_AUX
    shl     rdx, 32
    or      rax, rdx
    lfence
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
