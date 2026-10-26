cmake_minimum_required(VERSION 3.16)

project(asm-data-engine
    VERSION 0.1.0
    DESCRIPTION "High-performance data analytics engine using x86-64 Assembly, SIMD (SSE2/AVX2/AVX-512), and C"
    LANGUAGES C ASM_NASM
)

# This project contains hand-written x86-64 assembly and only supports
# 64-bit x86 hosts via System V AMD64 ABI (Linux).
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
    message(FATAL_ERROR
        "asm-data-engine only supports x86-64 hosts (found: ${CMAKE_SYSTEM_PROCESSOR})")
endif()
if(NOT UNIX OR APPLE)
    message(FATAL_ERROR
        "asm-data-engine currently targets Linux only (System V AMD64 ABI).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# NASM object format for Linux
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
enable_language(ASM_NASM)

# Silence NASM "warning: label alone on a line" style noise
set(CMAKE_ASM_NASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -w+all")

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

# Hand-written SIMD kernels (SSE2 / AVX2 / AVX-512)
add_library(calc STATIC
    src/calc_sse2.asm
    src/calc_avx2.asm
    src/calc_avx512.asm
)
target_include_directories(calc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Scalar C reference implementation
add_library(calc_scalar STATIC src/calc_scalar.c)
target_include_directories(calc_scalar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(calc_scalar PRIVATE -O3)

# Runtime CPU feature detection + engine dispatch
add_library(calc_dispatch STATIC src/calc_dispatch.c)
target_include_directories(calc_dispatch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(calc_dispatch PUBLIC calc calc_scalar)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

# Main CLI: file -> statistics report
add_executable(asm-stat src/main.c)
target_link_libraries(asm-stat PRIVATE calc_dispatch)

# Benchmark: scalar C vs SSE2 vs AVX2 vs AVX-512
add_executable(asm-bench benchmarks/benchmark.c)
target_link_libraries(asm-bench PRIVATE calc_dispatch)

# Correctness tests
add_executable(calc-tests tests/test_calc.c)
target_link_libraries(calc-tests PRIVATE calc_dispatch)

enable_testing()
add_test(NAME calc-tests COMMAND calc-tests)
