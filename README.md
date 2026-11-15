# asm-data-engine

High-performance data analytics engine using x86-64 Assembly (NASM), SIMD (SSE2 / AVX2 / AVX-512), and C, targeting Linux.

拿到一份真实的公开数据集（纽约出租车订单），把金额列转成二进制 double 流，然后用 手写汇编 在 CPU 上完成 `SUM / AVG / MIN / MAX / COUNT` 统计，并与 C 语言实现做性能对比。

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

路线图（中级改进 main 分支，高级改进其他分支）：

- [x] V1 SSE2 基础
- [x] V3 AVX2 一次处理 4 个 double
- [x] V4 AVX-512 一次处理 8 个 double（运行时 CPUID 检测）
- [ ] V5 多线程分块计算（pthread）
- [ ] V6 mmap / 分块 I/O 处理上亿条数据
- [ ] V7 perf / CPU cycles 细粒度测量

## CI

GitHub Actions（`.github/workflows/ci.yml`）在 `ubuntu-latest` 上执行：安装 NASM → CMake Release 构建 → 单元测试（ctest）→ 合成数据冒烟测试 → 基准测试冒烟。AVX-512 在 CI 机器不支持时测试会自动跳过对应内核，不会导致失败。
推荐 GitHub 仓库名称：

`asm-data-engine`

- 仓库名：`asm-data-engine`
- 项目描述：High-performance data analytics engine using x86-64 Assembly, SIMD (AVX2/AVX-512), and C.
- 主要技术：NASM、C、CMake、SSE2、AVX2、AVX-512
- 项目目标：使用真实公开数据集，实现 SUM、AVG、MAX、MIN、COUNT 等统计计算，并与 C 语言进行性能对比。

建议仓库结构：

```text
asm-data-engine/
├── README.md
├── CMakeLists.txt
├── data/
├── scripts/
│   └── convert.py
├── include/
│   └── calc.h
├── src/
│   ├── main.c
│   ├── calc_scalar.c
│   ├── calc_sse2.asm
│   ├── calc_avx2.asm
│   └── calc_avx512.asm
├── benchmarks/
│   └── benchmark.c
└── tests/
```


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
