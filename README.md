# asm-data-engine

High-performance data analytics engine using **x86-64 Assembly (NASM)**, **SIMD (SSE2 / AVX2 / AVX-512)**, and **C**, targeting **Linux**.

拿到一份真实的公开数据集（纽约出租车订单），把金额列转成二进制 double 流，然后用 **手写汇编** 在 CPU 上完成 `SUM / AVG / MIN / MAX / COUNT` 统计，并与 C 语言实现做性能对比。

```text
NYC Parquet（或合成数据） → convert.py → amounts.bin (raw doubles)
                                        ↓
                          asm-stat / asm-bench（C 读文件 + NASM SIMD 计算）
                                        ↓
                             统计报表 / 性能对比表
```

## 仓库结构

```text
asm-data-engine/
├── README.md
├── CMakeLists.txt
├── .github/workflows/ci.yml     # Linux CI: build + ctest + smoke + benchmark
├── data/                        # 数据目录（大文件不进库）
├── scripts/
│   ├── convert.py               # Parquet/合成数据 → amounts.bin
│   └── download_data.sh         # 下载 NYC TLC 数据
├── include/
│   └── calc.h                   # 公共 API
├── src/
│   ├── main.c                   # asm-stat CLI（C 读文件、输出报表）
│   ├── calc_scalar.c            # 标量 C 参考实现（正确性基准）
│   ├── calc_dispatch.c          # CPUID 运行时检测 + 分发
│   ├── calc_sse2.asm            # SSE2：一次 2 个 double
│   ├── calc_avx2.asm            # AVX2：一次 4 个 double
│   └── calc_avx512.asm          # AVX-512：一次 8 个 double
├── benchmarks/
│   └── benchmark.c              # C -O3 vs SSE2 vs AVX2 vs AVX-512
└── tests/
    └── test_calc.c              # 各引擎与标量参考对拍（含边界长度）
```

## 快速开始

```bash
sudo apt-get install -y build-essential cmake nasm python3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

生成数据并运行统计报表：

```bash
# 合成数据（任何机器可用）
python3 scripts/convert.py --synthetic 1000000 amounts.bin
./build/asm-stat --input amounts.bin --engine auto

# 真实 NYC 数据
./scripts/download_data.sh 2024-01
./build/asm-stat --input yellow_tripdata_2024-01.bin --engine auto
```

选择引擎：`--engine auto|scalar|sse2|avx2|avx512`（CPU 不支持时自动降级并提示）。

## 性能对比

```bash
./build/asm-bench --count 1000000 --repeat 5
```

输出示例（数值取决于机器）：

```text
== asm-data-engine benchmark ==
elements: 1000000 (~7.6 MB)   repeats per kernel: 5
cpu: sse2=1 avx2=1 avx512f=0

kernel         best time    throughput   checksum
----------------------------------------------------------------
C scalar -O3   ...
ASM SSE2       ...
ASM AVX2       ...
ASM AVX-512    (skipped, no AVX-512)
```

路线图（见 `README.txt` 备注：中级改进 main 分支，高级改进其他分支）：

- [x] V1 SSE2 基础
- [x] V3 AVX2 一次处理 4 个 double
- [x] V4 AVX-512 一次处理 8 个 double（运行时 CPUID 检测）
- [ ] V5 多线程分块计算（pthread）
- [ ] V6 mmap / 分块 I/O 处理上亿条数据
- [ ] V7 perf / CPU cycles 细粒度测量

## CI

GitHub Actions（`.github/workflows/ci.yml`）在 `ubuntu-latest` 上执行：安装 NASM → CMake Release 构建 → 单元测试（ctest）→ 合成数据冒烟测试 → 基准测试冒烟。AVX-512 在 CI 机器不支持时测试会自动跳过对应内核，不会导致失败。
