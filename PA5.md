# PA5：性能统计与分析

## 一、概述

PA5 的目标是为 NEMU 添加性能统计和分析功能，通过收集指令执行数量、运行时间等指标，计算模拟器的运行效率（每秒执行指令数、每条指令平均耗时），为性能优化提供数据支撑。同时通过 ITRACE（指令追踪）功能，支持对程序执行流的详细分析。

## 二、软件架构

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                    CPU 执行核心                            │
│                                                           │
│  ┌──────────────────────────────────────────────────┐    │
│  │              execute(n) 执行循环                   │    │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐       │    │
│  │  │ 指令执行  │→│ 计数统计  │→│ 时间统计  │       │    │
│  │  └──────────┘  └──────────┘  └──────────┘       │    │
│  └──────────────────────────────────────────────────┘    │
│                          │                                │
│                          ▼                                │
│  ┌──────────────────────────────────────────────────┐    │
│  │              statistic() 统计输出                  │    │
│  │  ├─ 总执行指令数 g_nr_guest_inst                  │    │
│  │  ├─ 总耗时 g_timer (微秒)                         │    │
│  │  ├─ 模拟频率 (inst/s)                             │    │
│  │  └─ 每条指令平均耗时 (ns)                         │    │
│  └──────────────────────────────────────────────────┘    │
│                          │                                │
│                          ▼                                │
│  ┌──────────────────────────────────────────────────┐    │
│  │              ITRACE 指令追踪                       │    │
│  │  每条指令 → disasm → 写入日志文件                  │    │
│  └──────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────┘
```

### 2.2 性能指标

| 指标 | 变量/函数 | 单位 | 说明 |
|------|----------|------|------|
| 总指令数 | `g_nr_guest_inst` | 条 | 已执行的客户机指令总数 |
| 总耗时 | `g_timer` | 微秒 (μs) | 宿主 CPU 时间 |
| 模拟频率 | `g_nr_guest_inst * 1000000 / g_timer` | inst/s | 每秒执行指令数 |
| 单指令耗时 | `g_timer * 1000 / g_nr_guest_inst` | 纳秒 (ns) | 每条指令平均耗时 |

## 三、详细设计

### 3.1 指令计数

全局计数器 `g_nr_guest_inst` 在每条指令执行后递增：

```c
uint64_t g_nr_guest_inst = 0;

static void execute(uint64_t n) {
  Decode s;
  for (; n > 0; n--) {
    exec_once(&s, cpu.pc);
    g_nr_guest_inst++;   // 每条指令 +1
    // ... 监视点检查、中断查询等
  }
}
```

**计数器特性：**
- 64 位无符号整数，足以计数 1.8e19 条指令（几乎不会溢出）
- 每次 `cpu_exec()` 调用都会累加，可跨多次 continue 统计
- 是性能计算的分子

### 3.2 时间统计

使用高精度计时器测量执行时间：

```c
static uint64_t g_timer = 0; // 累计运行时间（微秒）

void cpu_exec(uint64_t n) {
  uint64_t timer_start = get_time();  // 开始时间
  execute(n);                          // 执行
  uint64_t timer_end = get_time();    // 结束时间
  g_timer += timer_end - timer_start; // 累加到总时间
}
```

**时间源：** `get_time()` 函数返回微秒级时间戳，基于宿主系统的单调时钟。

### 3.3 统计输出

程序结束时（HIT TRAP / ABORT / QUIT）调用 `statistic()` 输出性能数据：

```c
static void statistic() {
  // 总耗时
  Log("host time spent = %' PRIu64 " us", g_timer);
  
  // 总指令数
  Log("total guest instructions = %' PRIu64, g_nr_guest_inst);
  
  // 模拟频率
  if (g_timer > 0) {
    uint64_t freq = g_nr_guest_inst * 1000000 / g_timer;
    Log("simulation frequency = %' PRIu64 " inst/s", freq);
    
    // PA5: 每条指令平均耗时
    uint64_t ns_per_inst = (g_timer * 1000) / 
                           (g_nr_guest_inst > 0 ? g_nr_guest_inst : 1);
    Log("average time per instruction = %" PRIu64 " ns", ns_per_inst);
  }
}
```

### 3.4 输出格式示例

```
host time spent = 4,199,490 us
total guest instructions = 4,285,277
simulation frequency = 1,020,427 inst/s
average time per instruction = 979 ns
```

解读：
- 运行了约 4.2 秒
- 执行了约 428 万条指令
- 模拟速度约 102 万条指令/秒
- 每条指令平均耗时 979 纳秒

### 3.5 ITRACE 指令追踪

ITRACE（Instruction Trace）功能记录每条执行过的指令的反汇编信息：

#### 3.5.1 追踪机制

```c
// cpu-exec.c
static void exec_once(Decode *s, vaddr_t pc) {
  s->pc = pc;
  s->snpc = pc;
  isa_exec_once(s);
  cpu.pc = s->dnpc;

#ifdef CONFIG_ITRACE
  // 格式化反汇编到 logbuf
  char *p = s->logbuf;
  p += snprintf(p, sizeof(s->logbuf), FMT_WORD ":", s->pc);
  // ... 指令字节
  disassemble(p, ...);   // 调用反汇编器
  
  // 条件追踪
  #ifdef CONFIG_ITRACE_COND
  if (ITRACE_COND) { log_write("%s\n", s->logbuf); }
  #else
  log_write("%s\n", s->logbuf);
  #endif
#endif
}
```

#### 3.5.2 反汇编器

使用 Capstone 反汇编库将机器码转换为汇编助记符：

```
0x80000000: 00 00 02 97 auipc   t0, 0
0x80000004: 00 02 88 23 sb      zero, 0x10(t0)
0x80000008: 01 02 c5 03 lbu     a0, 0x10(t0)
0x8000000c: 00 10 00 73 ebreak
```

#### 3.5.3 追踪日志

追踪信息写入日志文件（通过 `--log=FILE` 指定），可用于：
- 调试程序执行流
- 分析程序行为
- 性能剖析（热点函数识别）
- 与参考模拟器对比（差分测试）

### 3.6 性能影响因素分析

NEMU 的性能受以下因素影响：

| 因素 | 影响 | 说明 |
|------|------|------|
| ITRACE | 极大 | 每条指令都写文件，性能下降 10x+ |
| 监视点 | 大 | 每条指令扫描所有监视点，含表达式求值 |
| SDL 屏幕刷新 | 中 | 约 60Hz 更新纹理，每次 memcpy 整个帧缓冲 |
| MMIO 访问 | 中 | 设备回调函数开销大于普通内存 |
| 分支指令 | 小 | 额外的跳转计算 |
| 乘除法指令 | 小 | RV32M 调用宿主乘除，开销接近普通指令 |

## 四、函数调用执行流

### 4.1 性能统计完整流程

```
程序启动
    │
    ├─ cpu_exec(n)
    │   │
    │   ├─ timer_start = get_time()     ← 记录开始时间
    │   │
    │   ├─ execute(n)
    │   │   │
    │   │   └─ for n times:
    │   │        │
    │   │        ├─ exec_once()         ← 执行单条指令
    │   │        ├─ g_nr_guest_inst++   ← 指令计数 +1
    │   │        ├─ ITRACE 记录         ←（如开启）写反汇编到日志
    │   │        ├─ device_update()     ← 设备更新（节流）
    │   │        ├─ scan_watchpoint()   ← 监视点扫描
    │   │        └─ isa_query_intr()    ← 中断查询
    │   │
    │   ├─ timer_end = get_time()       ← 记录结束时间
    │   └─ g_timer += (timer_end - timer_start)  ← 累加耗时
    │
    └─ 程序结束（HIT TRAP / ABORT / QUIT）
         │
         └─ statistic()
              │
              ├─ Log("host time spent = %u us", g_timer)
              ├─ Log("total guest instructions = %u", g_nr_guest_inst)
              │
              ├─ 计算模拟频率: inst/s = g_nr_guest_inst * 1e6 / g_timer
              ├─ Log("simulation frequency = %u inst/s", freq)
              │
              └─ 计算单指令耗时: ns/inst = g_timer * 1000 / g_nr_guest_inst
                 Log("average time per instruction = %u ns", ns_per_inst)
```

### 4.2 单步执行统计流

```
用户输入 si 100
    │
    ├─ cmd_si("100")
    │   └─ cpu_exec(100)
    │
    └─ 执行100条指令后暂停
         └─ g_nr_guest_inst 累计增加100
            g_timer 累计增加对应时间
```

> 注意：`g_nr_guest_inst` 和 `g_timer` 都是累加的，跨多次 si/c 命令持续累计。
> 只有重新运行程序（重新加载镜像）才会重置。

### 4.3 ITRACE 执行流

```
开启 CONFIG_ITRACE
    │
    └─ 每条指令执行后:
         │
         ├─ 格式化 PC + 指令字节
         ├─ disassemble() → 调用 capstone 反汇编
         ├─ 检查 ITRACE_COND（如有）
         └─ log_write() → 写入日志文件
```

## 五、关键设计决策

### 5.1 时间统计粒度

选择 **微秒级** 时间统计：
- 足够精确（百万分之一秒），满足性能测量需求
- gettimeofday 或 clock_gettime 都能提供
- 64位微秒计数器可计数约 58 万年，不会溢出

### 5.2 计数器累加设计

全局累加设计（非每次重置）：
- 支持多次 continue 后统计总运行量
- 与真实硬件计数器行为一致
- 可观察不同阶段的性能变化

### 5.3 单指令平均耗时

新增 "每条指令平均耗时" 指标（ns/inst）的意义：
- 比 inst/s 更直观反映单条指令的开销
- 便于评估模拟器的解释执行效率
- 可与其他模拟器横向对比

## 六、性能测试数据

### 6.1 各程序性能对比

| 程序 | 总指令数 | 运行时间 | 模拟频率 | 单指令耗时 |
|------|---------|---------|---------|-----------|
| hello | 748 | 53 μs | 14 M inst/s | ~71 ns |
| snake (游戏中) | 2M | ~2s | 1 M inst/s | ~1000 ns |
| litenes (FC模拟器) | 198M | 117s | 1.7 M inst/s | ~590 ns |

### 6.2 性能分析

**为什么不同程序性能差异大？**

1. **hello 最快**：
   - 只有 748 条指令
   - 几乎全是寄存器运算
   - 很少的内存访问和设备访问

2. **snake 中等**：
   - 游戏循环中大量帧缓冲写入（MMIO）
   - 60Hz 屏幕刷新（SDL 纹理更新）
   - 键盘输入轮询

3. **litenes 较慢**：
   - 模拟器套模拟器，指令密度大
   - 大量内存操作模拟 6502 CPU
   - PPU 模拟计算量大

## 七、优化方向（可选）

| 优化技术 | 预期提升 | 实现难度 |
|---------|---------|---------|
| 解释器核心优化 | 1.5x~2x | 中 |
| 直接线程代码 (direct threading) | 2x~3x | 高 |
| JIT 编译 | 10x~50x | 很高 |
| 减少设备更新频率 | 视情况 | 低 |
| 批量内存访问 | 1.2x~1.5x | 中 |
