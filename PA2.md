# PA2：RISC-V 指令集实现

## 一、概述

PA2 的目标是在 NEMU 模拟器中实现完整的 RISC-V RV32I 基础整数指令集（39条）以及 RV32M 乘除法扩展（8条），共计 47 条指令。指令解码采用模式匹配（INSTPAT）的方式，按指令类型（R/I/S/B/U/J）分类处理操作数和立即数。

## 二、软件架构

### 2.1 整体架构

```
┌────────────────────────────────────────────────────┐
│                 CPU 执行核心                          │
│                                                      │
│  cpu-exec.c                 isa/riscv32/inst.c       │
│  ┌─────────────┐          ┌──────────────────────┐   │
│  │ cpu_exec(n) │──┐       │  isa_exec_once()     │   │
│  │ execute()   │  └──────▶│    inst_fetch()      │   │
│  └─────────────┘          │    decode_exec()     │   │
│                           │      ├─ 模式匹配       │   │
│                           │      ├─ 操作数解码     │   │
│                           │      └─ 执行体         │   │
│                           └──────────────────────┘   │
│                                                      │
│  寄存器文件 gpr[32]      内存系统 paddr/vaddr       │
└────────────────────────────────────────────────────┘
```

### 2.2 指令类型分类

RISC-V RV32I 指令按格式分为 6 种类型：

| 类型 | 格式 | 典型指令 | 立即数位域 |
|------|------|---------|-----------|
| R 型 | `opcode rd funct3 rs1 rs2 funct7` | add, sub, sll | 无立即数 |
| I 型 | `opcode rd funct3 rs1 imm[11:0]` | addi, lb, jalr | imm[11:0] (12位) |
| S 型 | `opcode imm[11:5] funct3 rs1 rs2 imm[4:0]` | sb, sw | imm[11:0] (12位) |
| B 型 | `opcode imm[12|10:5] funct3 rs1 rs2 imm[4:1|11]` | beq, bne | imm[12:1] (13位，左移1位) |
| U 型 | `opcode rd imm[31:12]` | lui, auipc | imm[31:12] (20位，左移12位) |
| J 型 | `opcode rd imm[20|10:1|11|19:12]` | jal | imm[20:1] (21位，左移1位) |

## 三、详细设计

### 3.1 指令模式匹配系统

采用 **INSTPAT 宏** 实现声明式指令匹配，每条指令由模式串、名称、类型和执行体组成：

```c
INSTPAT("??????? ????? ????? 000 ????? 00100 11", addi, I, R(rd) = src1 + imm);
```

**匹配规则：**
- `0`/`1`：必须匹配对应位
- `?`：任意位（通配符）
- 按从左到右顺序匹配，先匹配的优先

### 3.2 操作数解码

根据指令类型自动解码源操作数、目的寄存器和立即数：

```c
static void decode_operand(Decode *s, int *rd, word_t *src1, word_t *src2, word_t *imm, int type) {
  uint32_t i = s->isa.inst;
  int rs1 = BITS(i, 19, 15);
  int rs2 = BITS(i, 24, 20);
  *rd     = BITS(i, 11, 7);
  switch (type) {
    case TYPE_I: src1R();          immI(); break;
    case TYPE_U:                   immU(); break;
    case TYPE_S: src1R(); src2R(); immS(); break;
    case TYPE_R: src1R(); src2R();         break;
    case TYPE_B: src1R(); src2R(); immB(); break;
    case TYPE_J:                   immJ(); break;
  }
}
```

### 3.3 立即数生成宏

每种类型的立即数通过宏从指令位域中提取并符号扩展：

```c
#define immI()  do { *imm = SEXT(BITS(i, 31, 20), 12); } while(0)
#define immU()  do { *imm = SEXT(BITS(i, 31, 12), 20) << 12; } while(0)
#define immS()  do { *imm = (SEXT(BITS(i, 31, 25), 7) << 5) | BITS(i, 11, 7); } while(0)
#define immB()  do { *imm = (SEXT(BITS(i, 31, 31), 1) << 12) | (BITS(i, 7, 7) << 11) | (BITS(i, 30, 25) << 5) | (BITS(i, 11, 8) << 1); } while(0)
#define immJ()  do { *imm = (SEXT(BITS(i, 31, 31), 1) << 20) | (BITS(i, 19, 12) << 12) | (BITS(i, 20, 20) << 11) | (BITS(i, 30, 21) << 1); } while(0)
```

**关键特性：**
- 所有立即数都进行**符号扩展**（SEXT）
- B 型和 J 型立即数自动左移 1 位（最低位恒为 0）
- U 型立即数左移 12 位，对应位 [31:12]

### 3.4 RV32I 指令全集（39条）

#### 3.4.1 U 型指令（2条）

| 指令 | 操作码 | 功能 | 伪代码 |
|------|--------|------|--------|
| `lui` | 0110111 | 加载立即数到高位 | `rd = imm << 12` |
| `auipc` | 0010111 | PC加立即数到高位 | `rd = pc + (imm << 12)` |

#### 3.4.2 J 型指令（1条）

| 指令 | 操作码 | 功能 | 伪代码 |
|------|--------|------|--------|
| `jal` | 1101111 | 跳转并链接 | `rd = pc+4; pc = pc + imm` |

#### 3.4.3 I 型指令（17条）

**跳转：**
| 指令 | funct3 | 功能 | 伪代码 |
|------|--------|------|--------|
| `jalr` | 000 | 间接跳转并链接 | `rd = pc+4; pc = (rs1 + imm) & ~1` |

**加载：**
| 指令 | funct3 | 功能 | 伪代码 |
|------|--------|------|--------|
| `lb` | 000 | 加载字节（有符号） | `rd = SEXT(Mem[rs1+imm][7:0], 8)` |
| `lh` | 001 | 加载半字（有符号） | `rd = SEXT(Mem[rs1+imm][15:0], 16)` |
| `lw` | 010 | 加载字 | `rd = Mem[rs1+imm][31:0]` |
| `lbu` | 100 | 加载字节（无符号） | `rd = Mem[rs1+imm][7:0]` |
| `lhu` | 101 | 加载半字（无符号） | `rd = Mem[rs1+imm][15:0]` |

**立即数运算：**
| 指令 | funct3 | 功能 | 伪代码 |
|------|--------|------|--------|
| `addi` | 000 | 加立即数 | `rd = rs1 + imm` |
| `slti` | 010 | 小于立即数（有符号） | `rd = (signed)rs1 < (signed)imm` |
| `sltiu` | 011 | 小于立即数（无符号） | `rd = rs1 < (unsigned)imm` |
| `xori` | 100 | 异或立即数 | `rd = rs1 ^ imm` |
| `ori` | 110 | 或立即数 | `rd = rs1 \| imm` |
| `andi` | 111 | 与立即数 | `rd = rs1 & imm` |
| `slli` | 001 | 逻辑左移立即数 | `rd = rs1 << imm[4:0]` |
| `srli` | 101 | 逻辑右移立即数 | `rd = rs1 >> imm[4:0]` (逻辑) |
| `srai` | 101 | 算术右移立即数 | `rd = (signed)rs1 >> imm[4:0]` (算术) |

#### 3.4.4 S 型指令（3条）

| 指令 | funct3 | 功能 | 伪代码 |
|------|--------|------|--------|
| `sb` | 000 | 存储字节 | `Mem[rs1+imm][7:0] = rs2[7:0]` |
| `sh` | 001 | 存储半字 | `Mem[rs1+imm][15:0] = rs2[15:0]` |
| `sw` | 010 | 存储字 | `Mem[rs1+imm][31:0] = rs2[31:0]` |

#### 3.4.5 B 型指令（6条）

| 指令 | funct3 | 功能 | 伪代码 |
|------|--------|------|--------|
| `beq` | 000 | 等于则跳转 | `if (rs1 == rs2) pc = pc + imm` |
| `bne` | 001 | 不等则跳转 | `if (rs1 != rs2) pc = pc + imm` |
| `blt` | 100 | 小于则跳转（有符号） | `if (signed rs1 < signed rs2) pc = pc + imm` |
| `bge` | 101 | 大于等于则跳转（有符号） | `if (signed rs1 >= signed rs2) pc = pc + imm` |
| `bltu` | 110 | 小于则跳转（无符号） | `if (rs1 < rs2) pc = pc + imm` |
| `bgeu` | 111 | 大于等于则跳转（无符号） | `if (rs1 >= rs2) pc = pc + imm` |

#### 3.4.6 R 型指令（10条）

| 指令 | funct3 | funct7 | 功能 | 伪代码 |
|------|--------|--------|------|--------|
| `add` | 000 | 0000000 | 加 | `rd = rs1 + rs2` |
| `sub` | 000 | 0100000 | 减 | `rd = rs1 - rs2` |
| `sll` | 001 | 0000000 | 逻辑左移 | `rd = rs1 << rs2[4:0]` |
| `slt` | 010 | 0000000 | 小于（有符号） | `rd = (signed)rs1 < (signed)rs2` |
| `sltu` | 011 | 0000000 | 小于（无符号） | `rd = rs1 < rs2` |
| `xor` | 100 | 0000000 | 异或 | `rd = rs1 ^ rs2` |
| `srl` | 101 | 0000000 | 逻辑右移 | `rd = rs1 >> rs2[4:0]` (逻辑) |
| `sra` | 101 | 0100000 | 算术右移 | `rd = (signed)rs1 >> rs2[4:0]` (算术) |
| `or` | 110 | 0000000 | 或 | `rd = rs1 \| rs2` |
| `and` | 111 | 0000000 | 与 | `rd = rs1 & rs2` |

### 3.5 RV32M 乘除法扩展（8条）

RV32M 扩展的指令与 R 型指令共享操作码 0110011，通过 funct7 = 0000001 区分。

| 指令 | funct3 | funct7 | 功能 | 伪代码 |
|------|--------|--------|------|--------|
| `mul` | 000 | 0000001 | 乘法（低32位） | `rd = (rs1 * rs2)[31:0]` |
| `mulh` | 001 | 0000001 | 有符号乘法（高32位） | `rd = (signed64 rs1 * signed64 rs2)[63:32]` |
| `mulhsu` | 010 | 0000001 | 有符号×无符号乘法（高32位） | `rd = (signed64 rs1 * unsigned64 rs2)[63:32]` |
| `mulhu` | 011 | 0000001 | 无符号乘法（高32位） | `rd = (unsigned64 rs1 * unsigned64 rs2)[63:32]` |
| `div` | 100 | 0000001 | 有符号除法 | `rd = (signed)rs1 / (signed)rs2`（除以0返回-1） |
| `divu` | 101 | 0000001 | 无符号除法 | `rd = rs1 / rs2`（除以0返回-1） |
| `rem` | 110 | 0000001 | 有符号取余 | `rd = (signed)rs1 % (signed)rs2`（除以0返回rs1） |
| `remu` | 111 | 0000001 | 无符号取余 | `rd = rs1 % rs2`（除以0返回rs1） |

**乘除法特殊处理：**
- 乘法高32位：将32位操作数扩展为64位后相乘，取高32位
- 除以零：根据RISC-V规范，返回全1（-1），不触发异常
- 取余除以零：返回被除数，不触发异常

### 3.6 系统指令

| 指令 | 编码 | 功能 |
|------|------|------|
| `ecall` | 0x00000073 | 环境调用（触发trap） |
| `ebreak` | 0x00100073 | 断点（触发trap） |
| `mret` | 0x30200073 | M模式异常返回 |
| `nop` | 0x00000013 | 空操作（addi x0, x0, 0） |

**mret 伪代码：**
```
pc = mepc
mstatus.MIE = mstatus.MPIE
mstatus.MPIE = 1
```

### 3.7 CSR 指令（6条）

CSR指令属于 I 型格式，立即数字段为CSR地址。

| 指令 | funct3 | 功能 | 伪代码 |
|------|--------|------|--------|
| `csrrw` | 001 | 读写CSR | `t = csr; csr = rs1; rd = t` |
| `csrrs` | 010 | 读置位CSR | `t = csr; csr = csr \| rs1; rd = t` |
| `csrrc` | 011 | 读清零CSR | `t = csr; csr = csr & ~rs1; rd = t` |
| `csrrwi` | 101 | 立即数读写CSR | `t = csr; csr = zimm; rd = t` |
| `csrrsi` | 110 | 立即数读置位CSR | `t = csr; csr = csr \| zimm; rd = t` |
| `csrrci` | 111 | 立即数读清零CSR | `t = csr; csr = csr & ~zimm; rd = t` |

**支持的CSR寄存器：**
- `mstatus` (0x300) - M模式状态寄存器
- `mtvec` (0x305) - M模式异常向量基址
- `mscratch` (0x340) - M模式暂存寄存器
- `mepc` (0x341) - M模式异常PC
- `mcause` (0x342) - M模式异常原因
- `mie` (0x304) - M模式中断使能
- `mip` (0x344) - M模式中断挂起（软件只读）

## 四、函数调用执行流

### 4.1 指令执行主流程

```
cpu_exec(n)
    │
    └─ execute(n)
         │
         └─ for i = 1..n:
              │
              ├─ exec_once(&s, cpu.pc)
              │   │
              │   ├─ isa_exec_once(s)
              │   │   │
              │   │   ├─ inst_fetch(&s->snpc, 4)
              │   │   │   └─ vaddr_read(pc, 4)
              │   │   │       └─ 从内存取指 (32位)
              │   │   │
              │   │   └─ decode_exec(s)
              │   │       │
              │   │       ├─ s->dnpc = s->snpc  // 默认顺序执行
              │   │       │
              │   │       ├─ INSTPAT_START()
              │   │       │   ├─ lui? → U型 → R(rd) = imm
              │   │       │   ├─ auipc? → U型 → R(rd) = pc + imm
              │   │       │   ├─ jal? → J型 → 保存返回地址+跳转
              │   │       │   ├─ jalr? → I型 → 间接跳转
              │   │       │   ├─ lb/lh/lw/lbu/lhu? → I型 → 内存读
              │   │       │   ├─ sb/sh/sw? → S型 → 内存写
              │   │       │   ├─ addi/slti/... → I型 → 立即数运算
              │   │       │   ├─ add/sub/sll/... → R型 → 寄存器运算
              │   │       │   ├─ beq/bne/... → B型 → 条件分支
              │   │       │   ├─ mul/div/... → R型 → 乘除运算
              │   │       │   ├─ ecall/ebreak → N型 → 触发trap
              │   │       │   ├─ mret → N型 → 异常返回
              │   │       │   ├─ csrrw/csrrs/... → I型 → CSR操作
              │   │       │   └─ inv → 非法指令
              │   │       │
              │   │       ├─ R(0) = 0  // 零寄存器恒为0
              │   │       │
              │   │       └─ return 0
              │   │
              │   └─ cpu.pc = s->dnpc
              │
              ├─ g_nr_guest_inst++
              │
              ├─ device_update()    // 设备更新
              ├─ scan_watchpoint()  // 监视点扫描
              └─ isa_query_intr()   // 中断查询
```

### 4.2 指令取指流程

```
inst_fetch(snpc, len)
    │
    ├─ uint32_t inst = vaddr_read(pc, len)
    │
    ├─ *snpc = pc + len  // 下一条指令地址
    │
    └─ return inst
```

### 4.3 跳转指令执行流（以 jalr 为例）

```
INSTPAT 匹配 jalr (funct3=000, opcode=1100111)
    │
    ├─ decode_operand: TYPE_I
    │   ├─ src1 = gpr[rs1]     // 基址寄存器
    │   ├─ rd = rd             // 返回地址寄存器
    │   └─ imm = SEXT(imm12)   // 偏移（符号扩展）
    │
    ├─ 执行体:
    │   ├─ R(rd) = s->snpc     // 保存返回地址 (pc+4)
    │   └─ s->dnpc = (src1 + imm) & ~1  // 目标地址（对齐）
    │
    └─ R(0) = 0  // 零寄存器复位
```

### 4.4 内存访问流程（以 lw 为例）

```
INSTPAT 匹配 lw (funct3=010, opcode=0000011)
    │
    ├─ decode_operand: TYPE_I
    │   ├─ src1 = gpr[rs1]  // 基址
    │   └─ imm = SEXT(imm12) // 偏移
    │
    ├─ 执行体:
    │   └─ R(rd) = Mr(src1 + imm, 4)
    │       │
    │       └─ vaddr_read(addr, 4)
    │           │
    │           └─ paddr_read(addr, 4)
    │               │
    │               ├─ 是MMIO? → mmio_read()
    │               └─ 是内存? → *(uint32_t *)(guest_to_host(addr))
    │
    └─ R(0) = 0
```

### 4.5 CSR 指令执行流（以 csrrw 为例）

```
INSTPAT 匹配 csrrw (funct3=001, opcode=1110011)
    │
    ├─ decode_operand: TYPE_I
    │   ├─ src1 = gpr[rs1]      // 新值来源
    │   └─ imm 不直接用（csr地址在 imm 字段）
    │
    ├─ 执行体:
    │   ├─ csr_addr = BITS(inst, 31, 20)  // 提取CSR地址
    │   │
    │   ├─ switch(csr_addr):
    │   │   ├─ CSR_MSTATUS:
    │   │   │   old = cpu.mstatus
    │   │   │   cpu.mstatus = src1
    │   │   │
    │   │   ├─ CSR_MTVEC:
    │   │   │   old = cpu.mtvec
    │   │   │   cpu.mtvec = src1
    │   │   │
    │   │   ├─ CSR_MSCRATCH: ...
    │   │   ├─ CSR_MEPC: ...
    │   │   ├─ CSR_MCAUSE: ...
    │   │   ├─ CSR_MIE: ...
    │   │   ├─ CSR_MIP: old = cpu.mip (只读，不写)
    │   │   └─ default: old = 0
    │   │
    │   └─ R(rd) = old  // 旧值写入rd
    │
    └─ R(0) = 0
```

## 五、关键设计决策

### 5.1 模式匹配 vs 解码树

选择 **INSTPAT 模式匹配** 而非传统的 opcode/funct3 解码树，原因：
- 声明式定义，指令与模式一一对应，可读性强
- 易于添加新指令，只需一行 INSTPAT 宏
- 匹配顺序可控制（优先级从前到后）
- 与官方RISC-V指令编码直观对应

### 5.2 零寄存器恒零机制

在 `decode_exec()` 末尾统一设置 `R(0) = 0`：
- 保证任何指令写入 x0 都无效
- 只需一处处理，无需每条指令单独判断
- 符合 RISC-V ISA 规范

### 5.3 乘除法实现策略

利用宿主64位运算实现RV32M：
- 乘法高32位：32位操作数 → 扩展为64位 → 相乘 → 取高32位
- 除法/取余：直接使用宿主的 `/` 和 `%` 运算符
- 除零特殊处理：返回规范定义的值（不触发异常）

## 六、测试与验证

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| hello 程序 | 基础指令验证 | ✅ 748条指令，正常输出 |
| snake 游戏 | 综合指令验证 | ✅ 200万+指令，游戏正常运行 |
| demo 程序 | 8个demo全部可用 | ✅ 菜单正常显示 |
| litenes FC模拟器 | 最复杂测试 | ✅ 1.98亿条指令，模拟器正常运行 |
| RV32I 全覆盖 | 39条指令全部实现 | ✅ 通过 |
| RV32M 乘除法 | 8条乘除指令 | ✅ 通过 |
| 异常指令 | 非法指令触发BAD TRAP | ✅ 通过 |
