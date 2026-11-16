# asm-data-engine

High-performance data analytics engine using x86-64 Assembly (NASM), SIMD (SSE2 / AVX2 / AVX-512), pthreads, mmap, and C, targeting Linux.

拿到一份真实的公开数据集（纽约出租车订单），把金额列转成二进制 double 流，然后用 手写汇编 在 CPU 上完成 `SUM / AVG / MIN / MAX / COUNT` 统计，并与 C 语言实现做性能对比。

V5 使用 pthread 分块并行，V6 使用 mmap / 分块 I/O 处理上亿条记录，V7 使用 TSC（`rdtsc`/`rdtscp`）与 `perf` 做细粒度测量。

```text
NYC Parquet（或合成数据）
        │
        ├── convert.py（真实数据）──┐
        └── asm-gen（快速生成大文件）┤
                                   ▼
                         amounts.bin (raw doubles, 8 B/条)
                                   │
                                   ▼
              asm-stat（pthread + mmap/分块 I/O + NASM SIMD）
                                   │
                 ┌─────────────────┼─────────────────┐
                 ▼                 ▼                 ▼
            统计报表          asm-bench          scripts/perf.sh
          SUM/AVG/MIN/MAX   多引擎性能对比      perf stat 缓存/IPC
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
│   ├── download_data.sh         # 下载 NYC TLC 数据
│   └── perf.sh                  # V7: perf stat 缓存/IPC/分支
├── include/
│   ├── calc.h                   # SIMD API + 聚合/线程 API
│   ├── calc_io.h                # V6: mmap / 分块读取
│   └── calc_cycles.h            # V7: rdtsc/rdtscp 计时
├── src/
│   ├── main.c                   # asm-stat CLI（线程 + I/O + 报表）
│   ├── calc_scalar.c            # 标量 C 参考实现
│   ├── calc_dispatch.c          # CPUID 检测 + 引擎分发
│   ├── calc_engine.c            # 聚合结果合并 / 统一引擎入口
│   ├── calc_threads.c           # V5: pthread 线程池 + 64B 分块
│   ├── calc_io.c                # V6: mmap / chunked read 迭代器
│   ├── calc_cycles.c            # V7: TSC 频率校准
│   ├── calc_rdtsc.asm           # V7: lfence/rdtsc + rdtscp/lfence
│   ├── calc_sse2.asm            # SSE2：一次 2 个 double
│   ├── calc_avx2.asm            # AVX2：一次 4 个 double
│   └── calc_avx512.asm          # AVX-512：一次 8 个 double
├── tools/
│   └── asm-gen.c                # 快速生成 1 亿+ 条 double 测试数据
├── benchmarks/
│   └── benchmark.c              # 单线程 sum + pthread 聚合 + cycles/elem
└── tests/
    └── test_calc.c              # SIMD / 线程 / I/O / cycles 对拍
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
# 合成数据（Python，100 万条）
python3 scripts/convert.py --synthetic 1000000 amounts.bin
./build/asm-stat --input amounts.bin --engine auto

# 大文件（C 生成器，磁盘速度，适合 1 亿～10 亿条）
./build/asm-gen --out big.bin --count 100000000
./build/asm-stat --input big.bin --threads 16 --io mmap --repeat 3

# 真实 NYC 数据
./scripts/download_data.sh 2024-01
./build/asm-stat --input yellow_tripdata_2024-01.bin --engine auto
```

`asm-stat` 选项：

```text
--engine auto|scalar|sse2|avx2|avx512   计算内核（默认 auto，CPU 不支持时自动降级）
--threads N                            线程数，0 = 全部在线 CPU（默认 0）
--io auto|mmap|read                    I/O 方式（默认 auto：优先 mmap，失败回退 read）
--chunk-mb N                           分块大小 MiB（默认 64）
--repeat R                             重复扫描 R 次取最快（默认 1）
```

## V5 多线程分块计算（pthread）

`calc_aggregate_mt`（`src/calc_threads.c`）把输入切成 8 个 double（64 字节）对齐的块，每个线程独立计算自己的 `SUM/MIN/MAX/COUNT`，最后由调用线程按块顺序合并：

- 每个线程的结果放在独立的 64 字节 cache line 中，避免 false sharing。
- 调用线程也是 worker 0，N 个线程就真的用 N 个执行流。
- 惰性创建、跨调用复用的线程池：处理 1 亿条数据时文件会被分成很多块，避免每块都重新 `pthread_create`。
- 每线程至少 8192 个 double（64 KiB）才并行；小输入直接在当前线程完成。
- 线程池创建失败时退化为按调用创建线程，再失败则串行执行。
- 合并顺序固定，因此同一线程数下浮点结果可复现。

单元测试对 1/2/3/4/8/16 线程、0～100000 条数据与标量参考实现逐项对拍。

## V6 mmap / 分块 I/O 处理上亿条数据

`src/calc_io.c` 提供前向迭代器 `calc_file_next()`：每次返回一段完整的 double，调用方可以用**固定内存**扫描任意大小的文件。

- `--io auto`：优先 `mmap`，映射失败（非普通文件、地址空间不足等）自动回退到 `read`。
- `--io mmap`：整文件只读映射，零拷贝；用 `MADV_SEQUENTIAL` 提示内核顺序预读。
- `--io read`：复用一块 64 MiB（可调）页对齐缓冲区，内存占用恒定，适合内存小于数据集的机器。
- 文件大小必须是 8 的倍数，否则在 `open` 阶段直接报错；短读/文件截断会返回明确错误。
- 支持 `calc_file_rewind()`，`--repeat` 不必重新映射或重新打开文件。

`tools/asm-gen.c` 用 C 以磁盘速度生成测试文件（`./build/asm-gen --out big.bin --count 100000000`），避免 Python 生成 8 GB 数据过慢。

1 亿条记录（762.94 MB，16 线程，mmap）的实测输出示例（数值取决于机器）：

```text
== asm-data-engine report ==
input        : big.bin (762.94 MB)
records      : 100000000
engine       : avx512
threads      : 16 (16 online)
io           : mmap (chunk 64.0 MiB, 12 chunk(s))
best time    : 0.043184 s (3 scans)
throughput   : 18.53 GB/s, 2315.66 M elem/s
tsc          : 1.64 cycles/elem @ 3.79 GHz (wall, includes I/O)
SUM          : 12599523370.5600
AVG          : 125.9952
MIN          : 2.0000
MAX          : 249.9900
```

`throughput` 按有效数据（8 B/条）计算；`SUM/MIN/MAX` 实际扫描三遍，因此内存带宽约为该数字的 3 倍。10 亿条（8 GB）时使用 `--io read --chunk-mb 64` 可把常驻内存控制在 64 MiB 左右。

## V7 perf / CPU cycles 细粒度测量

- `src/calc_rdtsc.asm` 实现串行化计时：`calc_cycles_begin()` = `lfence; rdtsc`，`calc_cycles_end()` = `rdtscp; lfence`，避免乱序执行把测量区间前后指令混进来。
- `calc_cycle_hz()` 用 `CLOCK_MONOTONIC` 在约 20 ms 窗口内校准 invariant TSC 频率，不假设 CPU 标称频率。
- `asm-stat` 输出 wall time、GB/s、M elem/s 和 `cycles/elem`（含 I/O 的墙钟周期）。
- `asm-bench` 对每个内核同时报告 best time、GB/s、cycles/elem、ns/elem；pthread 聚合行使用与单线程聚合相同的 `SUM+MIN+MAX` 三遍扫描以便公平对比。

用 `perf` 查看缓存命中率、IPC、分支预测等整进程指标：

```bash
# Ubuntu: 安装与当前内核匹配的 perf
sudo apt-get install -y linux-tools-common linux-tools-$(uname -r)

sudo ./scripts/perf.sh ./build/asm-stat big.bin --threads 16 --io mmap
# 可选：用 perf record 采集调用图
PERF_RECORD=1 sudo ./scripts/perf.sh ./build/asm-stat big.bin --threads 16 --io mmap
```

`perf_event_paranoid` 较高时普通用户无法读取硬件计数器，脚本会给出提示；`cycles/elem` 始终可用，不依赖 `perf`。

## 性能对比

```bash
./build/asm-bench --count 2000000 --repeat 10 --threads 8
```

输出示例（AMD Ryzen 7 8845H / WSL2，数值取决于机器）：

```text
== asm-data-engine benchmark ==
elements: 2000000 (~15.3 MB)   repeats per kernel: 10
cpu: sse2=1 avx2=1 avx512f=1   cpu threads=16   tsc=3.79 GHz

kernel                          best time     throughput  cycles/elem    ns/elem  checksum
-------------------------------------------------------------------------------------------------------
C scalar -O3 (sum)             0.001240 s      12.91 GB/s        2.35       0.62  1049819112.8010
ASM SSE2 (sum)                 0.000630 s      25.42 GB/s        1.19       0.31  1049819112.8010
ASM AVX2 (sum)                 0.000345 s      46.35 GB/s        0.65       0.17  1049819112.8010
ASM AVX-512 (sum)              0.000410 s      38.99 GB/s        0.78       0.21  1049819112.8010
ASM avx512 aggregate x1        0.001584 s      30.29 GB/s        3.01       0.79  1049819112.8010
ASM avx512 aggregate x8        0.000469 s     102.44 GB/s        0.89       0.23  1049819112.8010
```

- `(sum)` 行是一次 SIMD 扫描；`aggregate` 行执行 `SUM+MIN+MAX` 三遍扫描，GB/s 按三遍总字节数计算，因此可以直接比较。
- 数据能放进 L3 时多线程接近线性扩展；上亿条、超出 cache 时通常先撞到内存带宽上限。
- 想复现 AVX-512 的差异，请在支持 AVX-512 的机器上运行；否则对应行自动跳过。

路线图：

- [x] V1 SSE2 基础
- [x] V3 AVX2 一次处理 4 个 double
- [x] V4 AVX-512 一次处理 8 个 double（运行时 CPUID 检测）
- [x] V5 多线程分块计算（pthread）
- [x] V6 mmap / 分块 I/O 处理上亿条数据
- [x] V7 perf / CPU cycles 细粒度测量

## CI

GitHub Actions（`.github/workflows/ci.yml`）在 `ubuntu-latest` 上执行：安装 NASM → CMake Release 构建 → 单元测试（ctest，覆盖 SIMD、pthread、mmap/read、TSC）→ 合成数据冒烟测试（标量/SIMD、mmap/read、多线程、各种分块大小）→ 基准测试冒烟。AVX-512 在 CI 机器不支持时测试会自动跳过对应内核，不会导致失败；`perf` 步骤为尽力而为，没有权限时不会使 CI 失败。


可以，而且非常适合用来学习 x86-64 汇编、CPU 性能优化、SIMD 向量计算和大数据处理。

建议做一个真实项目：

下载公开数据集 → 使用 ASM 读取数据 → 统计计算 → 输出报表 → 与 C 语言比较性能。

可以分为三个难度：

| 难度 | 数据规模 | ASM 任务 | 学习重点 |
|---|---|---|---|
| 初级 | 10 万条 | SUM、AVG、MAX、MIN | 寄存器、循环、内存访问 |
| 中级 | 100 万～1000 万条 | 分组统计、过滤、排序 | 文件 I/O、内存管理 |
| 高级 | 1 亿条以上 | SIMD 聚合、多线程统计 | AVX2、Cache、CPU 性能优化 |

我建议从真实的出租车订单数据入手，统计订单数量、行程距离、费用和平均车费。这些都是实际数据分析系统中的典型操作。

## 一、找到一个真实的公开数据集

### 纽约市出租车订单数据（NYC TLC）

官方数据下载：

[纽约市出租车公开数据集](https://www.nyc.gov/site/tlc/about/tlc-trip-record-data.page?utm_source=chatgpt.com)

这是纽约市政府发布的数据，包含大量真实出租车行程记录，按月份提供 Parquet 文件。[NYC.gov](https://www.nyc.gov/site/tlc/about/tlc-trip-record-data.page?source=post_page-----2eb2a27523a3--------------------------------\&utm_source=chatgpt.com)

数据包含：

| 字段 | 类型 | 含义 |
|---|---|---|
| trip_distance | double | 行驶距离 |
| fare_amount | double | 车费 |
| tip_amount | double | 小费 |
| total_amount | double | 总金额 |
| passenger_count | 数值 | 乘客数量 |
| tpep_pickup_datetime | 时间 | 上车时间 |

字段定义见 [官方数据字典](https://www.nyc.gov/assets/tlc/downloads/pdf/data_dictionary_trip_records_yellow.pdf?utm_source=chatgpt.com)。

我们可以实现以下 SQL 对应的计算：

```sql
SELECT
    COUNT(*) AS total_trips,
    SUM(total_amount) AS total_revenue,
    AVG(total_amount) AS avg_revenue,
    MAX(total_amount) AS max_revenue
FROM taxi_trips
WHERE total_amount > 0;
```

但不使用 MySQL 执行计算，而是让自己编写的 ASM 程序直接处理数据。

## 二、实现方案

原始数据是 Parquet 格式，包含压缩、编码和列式存储结构。

第一版不建议直接用 ASM 解析 Parquet，而是：

```text
NYC 官方 Parquet 文件
          |
          v
   Python / C 转换
          |
          v
   amounts.bin（二进制）
          |
          v
     x86-64 ASM
          |
          v
    CPU 浮点运算
          |
          v
    统计结果 / 报表
```

这里 Python/C 只负责格式转换，统计计算完全交给 ASM。

### 第一步：下载真实数据

选择 2024 年 1 月的黄色出租车订单：

```bash
wget https://d37ci6vzurychx.cloudfront.net/trip-data/yellow_tripdata_2024-01.parquet
```

### 第二步：转换为二进制

安装：

```bash
pip install pyarrow
```

编写 `convert.py`：

```python
import pyarrow.parquet as pq
import struct

table = pq.read_table(
    "yellow_tripdata_2024-01.parquet",
    columns=["total_amount"]
)

values = table.column("total_amount").to_pylist()

with open("amounts.bin", "wb") as f:
    for value in values:
        if value is not None and value > 0:
            f.write(struct.pack("<d", value))
```

转换后，每条数据占 8 字节。

例如：

```text
amounts.bin

15.50
22.75
31.20
18.00
...
```

注意：实际文件存储的是 IEEE 754 双精度浮点数的二进制表示，而不是文本。

## 三、使用 ASM 计算真实数据

下面是 Linux x86-64 + NASM 示例。

功能：

- 打开二进制文件
- 分块读取
- 使用 CPU 浮点指令计算总金额
- 统计有效记录数量
- 计算平均金额

文件 `stats.asm`：

```asm
default rel

section .data
    filename db "amounts.bin", 0

section .bss
    buffer resb 65536

section .text
    global _start

_start:
    ; open
    mov rax, 2
    lea rdi, [filename]
    xor rsi, rsi
    xor rdx, rdx
    syscall

    test rax, rax
    js error

    mov r12, rax

    ; sum = 0
    xorpd xmm0, xmm0

    ; count = 0
    xor r13, r13

read_loop:
    ; read(fd, buffer, 65536)
    xor rax, rax
    mov rdi, r12
    lea rsi, [buffer]
    mov rdx, 65536
    syscall

    test rax, rax
    js error_close
    jz finished

    ; 数据必须是完整的 double
    test rax, 7
    jnz error_close

    mov rcx, rax
    shr rcx, 3

    lea rbx, [buffer]

sum_loop:
    movsd xmm1, [rbx]

    addsd xmm0, xmm1

    add rbx, 8
    inc r13

    dec rcx
    jnz sum_loop

    jmp read_loop

finished:
    ; xmm0 = SUM
    ; r13 = COUNT

    test r13, r13
    jz close_file

    ; count 转换成 double
    cvtsi2sd xmm1, r13

    ; AVG = SUM / COUNT
    divsd xmm0, xmm1

    ; xmm0 = AVG

close_file:
    mov rax, 3
    mov rdi, r12
    syscall

    xor rdi, rdi
    jmp exit

error_close:
    mov rax, 3
    mov rdi, r12
    syscall

error:
    mov rdi, 1

exit:
    mov rax, 60
    syscall
```

编译：

```bash
nasm -f elf64 stats.asm -o stats.o

ld stats.o -o stats

./stats
```

这段程序是真正执行 CPU 浮点计算的汇编程序。

注意：这是最小计算版本，计算结果保存在寄存器中，尚未实现打印报表；假设输入文件完整，且每次读取均为 8 字节的整数倍。 正式版本还需要处理不完整读取、数值异常和输出格式。

## 四、后续升级为高性能计算引擎

第一版完成后，可以逐步加入：

| 版本 | 功能 | CPU 技术 |
|---|---|---|
| V1 | SUM、AVG | SSE2 |
| V2 | MAX、MIN、COUNT | SSE2 |
| V3 | 一次处理 4 个 double | AVX2 |
| V4 | 一次处理 8 个 double | AVX-512 |
| V5 | 多线程分块计算 | pthread |
| V6 | 处理上亿条数据 | mmap、分块 I/O |
| V7 | 性能测试 | perf、CPU cycles |

最终对比：

```text
数据规模：1000 万条 double

实现方式             执行时间
--------------------------------
C -O0                待测试
C -O3                待测试
ASM SSE2             待测试
ASM AVX2             待测试
ASM AVX-512          待测试
```

这样可以真实测量 ASM 相比 C 编译器自动优化究竟有多大优势，而不是假设 ASM 一定更快。

## 五、我的建议

可以将这个项目命名为 asm-data-engine，采用 NASM + CMake，在 Linux x86-64 上开发。

不过，有一个关键选择：

A. 纯 ASM 方案：文件读取、数据计算、报表输出全部用汇编实现，重点研究操作系统调用、寄存器和 CPU。

B. C + ASM 混合方案：C 负责文件读取和报表输出，ASM 负责 SUM、AVG、MAX、MIN、SIMD，重点研究 CPU 科学计算性能。

我更推荐 B，因为它更接近真实数据库和高性能计算软件的工程实践。

备注：中级维度 main分支 高级维度其他git分支

是的，王爽《汇编语言》第4版主要讲的是 Intel 8086 CPU 的 16 位汇编语言，使用 MASM 风格的汇编语法。

我前面给你的 `asm-data-engine` 项目使用的是 x86-64（64 位）汇编，采用 NASM 语法。

两者属于同一个 x86 指令体系的发展脉络，但在 CPU 工作模式、寄存器、内存寻址、程序结构和操作系统接口方面存在明显区别。

## 一、王爽 MASM 与现代 NASM 的主要区别

| 比较项目 | 王爽《汇编语言》第4版 | asm-data-engine 项目 |
|---|---|---|
| CPU 架构 | Intel 8086 | x86-64 |
| CPU 工作模式 | 16 位实模式 | 64 位长模式 |
| 汇编器 | MASM 风格 | NASM |
| 寄存器 | AX、BX、CX、DX | RAX、RBX、RCX、RDX |
| 寄存器宽度 | 16 位 | 64 位 |
| 内存地址 | 段地址 + 偏移地址 | 64 位虚拟地址 |
| 段定义 | `segment` / `ends` | `section` |
| 系统调用 | `int 21h`（DOS） | `syscall`（Linux） |
| 浮点计算 | 原始 8086 无集成 FPU | SSE2、AVX2、AVX-512 等 |
| 程序运行环境 | DOS / DOSBox | Linux x86-64 |
| 典型用途 | 学习 CPU 和汇编原理 | 高性能计算、数据处理 |

需要特别注意：MASM 和 NASM 是汇编器，而 16 位和 64 位是 CPU 执行模式。

MASM 并不只能编写 16 位程序。现代 MASM 也支持 32 位和 64 位汇编。

同样，NASM 可以编写 16 位、32 位和 64 位程序。

所以：

- MASM ≠ 16 位
- NASM ≠ 64 位
- 王爽的教学代码主要是 16 位
- 我们的计算引擎选择了 NASM + x86-64

---

## 二、代码结构有什么区别？

### 1. 王爽书中的 MASM 风格

你之前学习过类似代码：

```asm
assume cs:codesg

codesg segment

    mov ax, 0123h
    mov bx, 0456h

    add ax, bx

    mov ax, 4c00h
    int 21h

codesg ends

end
```

这里：

`codesg segment`

定义一个名为 `codesg` 的段。

`assume cs:codesg`

告诉汇编器，将 `CS` 段寄存器与 `codesg` 关联起来进行汇编。

注意，`assume` 本身不会生成修改 `CS` 的机器指令。

`codesg ends`

表示段定义结束。

`end`

表示源程序结束。

这些主要是 MASM 的汇编伪指令，而不是 CPU 机器指令。

### 2. 我之前给你的 NASM 风格

```asm
section .data
    num1 dq 100
    num2 dq 200

section .text
    global _start

_start:
    mov rax, [num1]
    add rax, [num2]

    mov rax, 60
    xor rdi, rdi
    syscall
```

这里：

`section .data`

定义已初始化数据所在的节。

`section .text`

定义代码所在的节。

`global _start`

将 `_start` 符号导出给链接器。

`_start:`

定义程序入口标签。

`syscall`

执行 Linux x86-64 系统调用。

两者虽然写法不同，但都是将汇编代码转换为 CPU 可以执行的机器指令。

## 三、为什么王爽要讲“段”，而现代汇编主要讲“节”？

这是两者最重要的区别之一。

### 8086：段地址 + 偏移地址

8086 使用如下方式计算物理地址：

```text
物理地址 = 段地址 × 16 + 偏移地址
```

例如：

```asm
mov ax, 2000h
mov ds, ax

mov bx, 0010h
mov al, [bx]
```

CPU 访问的物理地址为：

```text
DS = 2000h
BX = 0010h

物理地址 = 20000h + 0010h
         = 20010h
```

8086 的地址总线为 20 位，能够形成约 1MB 的物理地址空间。

单个段通过 16 位偏移量最多可以覆盖 64KB。

### x86-64：现代虚拟内存

64 位 Linux 程序通常不需要手动计算 `DS × 16 + 偏移地址`。

例如：

```asm
section .data
    number dq 100

section .text
    global _start

_start:
    mov rax, [rel number]
```

CPU 使用虚拟地址访问数据，操作系统负责建立虚拟内存映射，处理器的内存管理单元负责地址转换。

现代 x86-64 仍然保留段寄存器，但 64 位模式下传统的 CS、DS、ES、SS 分段机制已大幅简化，FS、GS 仍有特殊用途。

另外，NASM 的 `section .data` 和 MASM 的 `segment` 都属于汇编器及目标文件组织机制，但不应简单认为它们完全等价。

---

## 四、王爽书中的知识还能用于现代科学计算吗？

不仅能用，而且是基础。

| 王爽书中的知识 | 现代 x86-64 对应知识 | 科学计算用途 |
|---|---|---|
| AX、BX、CX | RAX、RBX、RCX | 整数计算 |
| SI、DI | RSI、RDI | 数组访问 |
| `MOV` | `MOV`、`VMOVUPD` | 数据加载 |
| `ADD` | `ADD`、`VADDPD` | 数值求和 |
| `MUL` | `IMUL`、`VMULPD` | 乘法计算 |
| `CMP` | `CMP`、向量比较 | 数据过滤 |
| `LOOP` | `DEC/JNZ` 等 | 数组遍历 |
| `CALL/RET` | `CALL/RET` | 计算函数 |
| `PUSH/POP` | `PUSH/POP` | 函数栈管理 |

不过，王爽的书并不以现代 SIMD 科学计算为主要内容。它的核心价值是帮助理解 CPU 如何执行程序。

## 五、应该继续学 MASM，还是直接学 NASM？

结合你准备开发的 `asm-data-engine` 项目，我建议：

先完成王爽《汇编语言》第4版，再转向 x86-64 NASM。

推荐学习顺序：

```text
王爽《汇编语言》第4版
        |
        v
8086 CPU / 16位汇编
        |
        v
寄存器、寻址、栈、CALL、RET
        |
        v
x86-64 架构
        |
        v
NASM / Linux ABI / 系统调用
        |
        v
SSE2 / AVX2 / AVX-512
        |
        v
CPU Cache / SIMD / 多线程
        |
        v
asm-data-engine
        |
        v
真实金融数据 / 出租车数据
        |
        v
千万至十亿条记录统计
```

你也可以参考 [清华大学出版社《汇编语言》第4版官方页面](https://www.tup.tsinghua.edu.cn/bookscenter/book_07970801.html?utm_source=chatgpt.com)，其中提供教材介绍及配套资源。

一个重要建议： 不必把王爽书中的 16 位程序逐条改写成 64 位程序。先掌握寄存器、寻址、栈和程序执行流程，再系统学习 64 位 ABI、虚拟内存及 SIMD，效率更高。

对于你的目标，最终要掌握的不是某一种汇编器，而是 现代 x86-64 CPU 的指令集与微架构性能优化技术。
