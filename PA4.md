# PA4：中断与异常

## 一、概述

PA4 的目标是实现 RISC-V 的异常与中断系统，包括控制状态寄存器（CSR）、异常处理流程、中断控制器以及定时器中断。这使得 NEMU 能够支持操作系统的抢占式调度、设备中断等高级功能，是从裸机程序到多任务系统的关键一步。

## 二、软件架构

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────────────┐
│                       CPU 核心                                │
│                                                               │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐   │
│  │  通用寄存器   │    │   PC 寄存器  │    │  CSR 寄存器  │   │
│  │  gpr[32]     │    │              │    │  mstatus     │   │
│  │              │    │              │    │  mtvec       │   │
│  │              │    │              │    │  mepc        │   │
│  │              │    │              │    │  mcause      │   │
│  │              │    │              │    │  mie / mip   │   │
│  │              │    │              │    │  mscratch    │   │
│  └──────────────┘    └──────────────┘    └──────────────┘   │
│         │                     │                  │           │
│         ▼                     ▼                  ▼           │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                执行循环 (cpu-exec.c)                 │    │
│  │  取指 → 译码 → 执行 → 异常检查 → 中断查询 → 下一条   │    │
│  └─────────────────────────────────────────────────────┘    │
└──────────────────────────────────┬───────────────────────────┘
                                   │ 中断信号 (dev_raise_intr)
┌──────────────────────────────────▼───────────────────────────┐
│                       设备层                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐     │
│  │ 定时器   │  │  键盘    │  │  VGA     │  │  串口    │     │
│  │ (RTC)    │  │ (i8042)  │  │          │  │          │     │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘     │
└──────────────────────────────────────────────────────────────┘
```

### 2.2 RISC-V 中断/异常分类

| 类型 | 触发方式 | 例子 | mcause 值 |
|------|---------|------|-----------|
| **异常** | 指令执行同步触发 | ecall, ebreak, 非法指令, 访存错误 | 0~ (正/负) |
| **中断** | 设备异步触发 | 定时器中断, 外部中断 | 最高位为1 + 中断号 |

**mcause 编码：**
- 最高位 (bit31) = 0 → 异常
- 最高位 (bit31) = 1 → 中断

## 三、详细设计

### 3.1 CSR 寄存器组

在 CPU 状态结构中添加 CSR 寄存器：

```c
typedef struct {
  word_t gpr[32];       // 通用寄存器
  vaddr_t pc;           // 程序计数器
  // CSR 寄存器
  word_t mstatus;       // M 模式状态寄存器
  word_t mtvec;         // M 模式异常向量基址
  word_t mscratch;      // M 模式暂存寄存器
  word_t mepc;          // M 模式异常 PC
  word_t mcause;        // M 模式异常原因
  word_t mie;           // M 模式中断使能
  word_t mip;           // M 模式中断挂起
} riscv32_CPU_state;
```

#### 3.1.1 mstatus (M 模式状态寄存器, 0x300)

| 位 | 名称 | 功能 |
|----|------|------|
| 3  | MIE  | M 模式全局中断使能 |
| 7  | MPIE | M 模式前一次中断使能（进入异常时保存 MIE，返回时恢复） |

**中断使能位操作：**
- 进入异常：`MPIE = MIE; MIE = 0`
- 退出异常（mret）：`MIE = MPIE; MPIE = 1`

#### 3.1.2 mtvec (M 模式异常向量基址, 0x305)

```
31                            2 1      0
┌───────────────────────────────┬───────┐
│         BASE (4字节对齐)      │ MODE  │
└───────────────────────────────┴───────┘
```

- MODE = 0：直接模式，所有异常都跳转到 BASE
- MODE = 1：向量模式，异常号 × 4 + BASE

> 本实现使用直接模式（MODE=0）。

#### 3.1.3 mepc (M 模式异常 PC, 0x341)

保存触发异常的指令地址（或中断时的下一条指令地址），供 mret 返回时使用。

#### 3.1.4 mcause (M 模式异常原因, 0x342)

| 值（32位） | 含义 |
|-----------|------|
| 0x00000000 | 指令地址不对齐 |
| 0x00000002 | 非法指令 |
| 0x00000003 | 断点（ebreak） |
| 0x00000007 | 环境调用（来自M模式的ecall） |
| 0x80000007 | **机器定时器中断**（最高位=1表示中断） |

#### 3.1.5 mie / mip (中断使能/挂起, 0x304/0x344)

| 位 | 名称 | 功能 |
|----|------|------|
| 7  | MTIE / MTIP | 机器定时器中断使能 / 挂起 |

**中断判定公式：**
```
中断触发 = MIE (全局使能) && (mip & mie) (对应位同时为1)
```

### 3.2 CSR 指令实现

6 条 CSR 指令，均为 I 型格式，funct3 区分操作类型：

| 指令 | funct3 | 操作 | 伪代码 |
|------|--------|------|--------|
| `csrrw rd, csr, rs1` | 001 | 读写 | `t = csr; csr = rs1; rd = t` |
| `csrrs rd, csr, rs1` | 010 | 读置位 | `t = csr; csr = csr \| rs1; rd = t` |
| `csrrc rd, csr, rs1` | 011 | 读清零 | `t = csr; csr = csr & ~rs1; rd = t` |
| `csrrwi rd, csr, zimm` | 101 | 立即数读写 | `t = csr; csr = zimm; rd = t` |
| `csrrsi rd, csr, zimm` | 110 | 立即数读置位 | `t = csr; csr = csr \| zimm; rd = t` |
| `csrrci rd, csr, zimm` | 111 | 立即数读清零 | `t = csr; csr = csr & ~zimm; rd = t` |

**实现要点：**
- 所有 CSR 指令都会读取旧值写入 rd
- rs1 = x0 或 zimm = 0 时，不执行写操作（只读取）
- mip 寄存器对软件只读（只可通过设备中断设置）

### 3.3 异常处理

#### 3.3.1 异常触发

异常通过 `NEMUTRAP(pc, cause)` 宏触发，在以下情况发生：

| 异常源 | 触发位置 | mcause |
|--------|---------|--------|
| ecall 指令 | 指令执行 | 11 (M模式环境调用) |
| ebreak 指令 | 指令执行 | 3 (断点) |
| 非法指令 | 指令译码 | 2 (非法指令) |

**异常处理函数 `isa_raise_intr()`：**

```c
word_t isa_raise_intr(word_t NO, vaddr_t epc) {
  cpu.mepc = epc;                           // 保存当前 PC
  cpu.mcause = NO;                          // 保存异常原因
  
  // 保存中断使能状态，关闭中断
  cpu.mstatus = (cpu.mstatus & ~MSTATUS_MIE) | 
                ((cpu.mstatus & MSTATUS_MIE) ? MSTATUS_MPIE : 0);
  
  return cpu.mtvec;                         // 返回异常处理入口地址
}
```

#### 3.3.2 异常返回 (mret)

```c
// mret 指令执行：
s->dnpc = cpu.mepc;    // 恢复 PC
cpu.mstatus = (cpu.mstatus & ~MSTATUS_MPIE) | 
              ((cpu.mstatus & MSTATUS_MPIE) ? MSTATUS_MIE : 0);
// MIE = MPIE, MPIE = 1
```

### 3.4 中断系统

#### 3.4.1 中断触发流程

中断是**异步**的，由设备在需要服务时触发：

```
设备需要服务
    │
    ▼
dev_raise_intr()       [device/intr.c]
    │
    ▼
cpu.mip |= MIP_MTIP    // 设置中断挂起位
    │
    ▼
（等待 CPU 检查中断）
```

`dev_raise_intr()` 函数：
```c
void dev_raise_intr() {
  cpu.mip |= MIP_MTIP;  // 设置定时器中断挂起位
}
```

#### 3.4.2 中断查询

CPU 在每条指令执行后检查是否有中断：

```c
word_t isa_query_intr() {
  if ((cpu.mstatus & MSTATUS_MIE) &&     // 全局中断使能
      (cpu.mip & cpu.mie & MIP_MTIP)) {  // 对应中断挂起且使能
    return 7;  // 机器定时器中断号
  }
  return INTR_EMPTY;
}
```

**中断触发条件：**
1. mstatus.MIE = 1（全局中断使能打开）
2. mip.MTIP = 1（有定时器中断挂起）
3. mie.MTIE = 1（定时器中断使能打开）

三者缺一不可。

#### 3.4.3 中断处理

中断被触发后，处理流程与异常类似：

```
cpu_exec 循环中
    │
    ├─ 执行一条指令
    │
    ├─ isa_query_intr() → 返回中断号
    │
    ├─ isa_raise_intr(中断号 | (1<<31), next_pc)
    │   ├─ mepc = 下一条指令地址
    │   ├─ mcause = 中断号 | 0x80000000 (最高位=1表示中断)
    │   ├─ mstatus.MPIE = mstatus.MIE
    │   └─ mstatus.MIE = 0 (关闭中断)
    │
    └─ pc = mtvec (跳转到中断处理程序)
```

> **中断 vs 异常的 mepc 区别：**
> - 异常：mepc = 触发异常的指令地址（因为异常指令未执行成功）
> - 中断：mepc = 下一条指令地址（因为当前指令已执行完成）

### 3.5 AM 层的异常/中断支持

#### 3.5.1 上下文切换 (CTE)

AM 的 `cte.c` 提供异常/中断的上下文保存与恢复：

```
异常/中断发生
    │
    ▼
trap.S (汇编)
    │
    ├─ 保存所有寄存器到栈上
    │
    ├─ 调用 __am_irq_handle(Context *c)
    │   │
    │   ├─ 判断是异常还是中断
    │   ├─ 异常 → _halt()
    │   └─ 中断 → 调用设备中断处理函数
    │       └─ 定时器中断 → 增加时间戳
    │
    └─ 恢复所有寄存器
        │
        ▼
     mret → 返回
```

#### 3.5.2 向量模式

`cte_init()` 设置异常向量基址：
```c
void cte_init(Context *(*handler)(Context *ev)) {
  // 设置 mtvec = __am_vectors (直接模式)
  asm volatile("csrw mtvec, %0" : : "r"(__am_vectors));
  
  // 打开全局中断使能和定时器中断使能
  asm volatile("csrs mstatus, %0" : : "r"(0x8));  // MIE
  asm volatile("csrs mie, %0" : : "r"(0x80));     // MTIE
}
```

## 四、函数调用执行流

### 4.1 异常处理执行流（以 ecall 为例）

```
CPU 执行 ecall 指令
    │
    ├─ NEMUTRAP(s->pc, R(10))  // a0 = 系统调用号
    │   │
    │   └─ isa_raise_intr(11, s->pc)
    │       │
    │       ├─ cpu.mepc = s->pc           // 保存触发地址
    │       ├─ cpu.mcause = 11             // 环境调用
    │       │
    │       ├─ // 保存中断使能状态
    │       ├─ mstatus.MPIE = mstatus.MIE
    │       └─ mstatus.MIE = 0
    │
    ├─ s->dnpc = cpu.mtvec                 // 跳转到异常向量
    │
    └─ 继续执行 → 进入异常处理程序
         │
         └─ __am_irq_handle(Context*)
              │
              ├─ 检查 mcause → 是异常
              │
              └─ 异常处理：
                  ├─ 系统调用号在 a0
                  ├─ 调用对应的系统调用
                  └─ 设置返回值到 a0
```

### 4.2 中断处理执行流（以定时器中断为例）

```
定时器产生中断 → dev_raise_intr()
    │
    └─ cpu.mip |= MIP_MTIP  // 设置挂起位
         │
         └─（等待CPU检查）

CPU 执行循环中（每条指令后）
    │
    ├─ 执行完当前指令
    │
    ├─ isa_query_intr()
    │   │
    │   ├─ 检查 mstatus.MIE → 1？
    │   ├─ 检查 mip.MTIP → 1？
    │   └─ 检查 mie.MTIE → 1？
    │
    └─ 三者都满足 → 返回中断号 7
         │
         └─ isa_raise_intr(7 | 0x80000000, s->dnpc)
             │
             ├─ cpu.mepc = s->dnpc        // 保存下一条指令地址
             ├─ cpu.mcause = 0x80000007    // 中断号 + 最高位
             │
             ├─ mstatus.MPIE = mstatus.MIE
             └─ mstatus.MIE = 0 (关闭中断)
                  │
                  └─ pc = mtvec → 中断处理程序
                       │
                       └─ __am_irq_handle(Context*)
                            │
                            ├─ 检查 mcause 最高位 → 是中断
                            │
                            ├─ 定时器中断？
                            │   └─ 更新系统时间
                            │
                            └─ （可选）任务调度
                                 │
                                 └─ 上下文切换到另一个线程
```

### 4.3 mret 指令执行流

```
中断/异常处理程序末尾 → mret 指令
    │
    └─ decode_exec 匹配 mret
         │
         ├─ s->dnpc = cpu.mepc    // 恢复 PC 到进入异常时的地址
         │
         ├─ // 恢复中断使能
         ├─ mstatus.MIE = mstatus.MPIE
         └─ mstatus.MPIE = 1
              │
              └─ CPU 继续执行被中断的程序
```

### 4.4 CSR 指令执行流（以 csrrw 为例）

```
CPU 执行 csrrw rd, mstatus, rs1
    │
    └─ INSTPAT 匹配 csrrw (funct3=001)
         │
         ├─ decode_operand: TYPE_I
         │   ├─ src1 = gpr[rs1]   // 新值来自 rs1
         │   └─ rd = rd           // 旧值存到 rd
         │
         ├─ csr_addr = BITS(inst, 31, 20)  // 从imm字段提取CSR地址
         │
         ├─ switch(csr_addr):
         │   ├─ case CSR_MSTATUS:
         │   │   old = cpu.mstatus        // 读旧值
         │   │   cpu.mstatus = src1       // 写新值
         │   │
         │   ├─ case CSR_MTVEC: ...
         │   ├─ case CSR_MSCRATCH: ...
         │   ├─ case CSR_MEPC: ...
         │   ├─ case CSR_MCAUSE: ...
         │   ├─ case CSR_MIE: ...
         │   └─ case CSR_MIP:
         │       old = cpu.mip            // 只读，只返回旧值
         │       // 不执行写入
         │
         └─ R(rd) = old  // 旧值写入目标寄存器
```

## 五、关键设计决策

### 5.1 中断查询位置

在 CPU 执行循环中**每条指令后**查询中断：
- 实现简单，逻辑清晰
- 中断延迟最多一条指令
- 对性能影响极小（一次位运算比较）

### 5.2 mstatus 位设计

只实现必要的 MIE 和 MPIE 位：
- 对于教学目的，只需要理解中断使能的保存/恢复机制
- 完整 mstatus 有大量其他位（MPP, MPRV, MXR, SUM 等），但基础OS用不到
- 避免过度设计

### 5.3 中断号选择

使用机器定时器中断（中断号 7）作为主要演示：
- 定时器是最常见的中断源
- 实现抢占式调度的基础
- 与 RTC 设备天然关联

## 六、测试与验证

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| CSR 读写 | csrrw/csrrs/csrrc 读写各CSR | ✅ 通过 |
| CSR 立即数 | csrrwi/csrrsi/csrrci 立即数操作 | ✅ 通过 |
| mip 只读 | 软件写 mip 是否被忽略 | ✅ 只读（部分实现） |
| ecall 异常 | 系统调用触发异常并返回 | ✅ 通过 |
| ebreak 异常 | 断点指令触发BAD TRAP | ✅ 通过 |
| 非法指令 | 未定义指令触发异常 | ✅ 通过 |
| 中断使能 | MIE=0 时中断被屏蔽 | ✅ 通过 |
| 定时器中断 | RTC中断正常触发 | ✅ 通过 |
| mret 返回 | 异常返回恢复PC和中断状态 | ✅ 通过 |
| thread-os | 抢占式多线程OS | ✅ 正常调度 |
