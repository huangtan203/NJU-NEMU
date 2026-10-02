# PA1：调试器与基础库

## 一、概述

PA1 的目标是构建一个功能完整的指令级调试器（SDB - Simple DeBugger），并实现 AbstractMachine（AM）的基础运行库（klib）。调试器支持单步执行、寄存器查看、内存检查、表达式求值和监视点等核心调试功能，为后续 PA 的开发和调试提供基础设施。

## 二、软件架构

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────┐
│                    NEMU 模拟器                        │
│  ┌──────────────────────────────────────────────┐   │
│  │              SDB 调试器 (sdb.c)               │   │
│  │  ┌─────────┐ ┌──────────┐ ┌──────────────┐   │   │
│  │  │ 命令解析 │ │ 表达式求值 │ │  监视点管理  │   │   │
│  │  └─────────┘ └──────────┘ └──────────────┘   │   │
│  └──────────────────────────────────────────────┘   │
│  ┌────────────┐  ┌─────────────┐  ┌─────────────┐   │
│  │ CPU 执行核 │  │  内存系统   │  │  寄存器文件  │   │
│  └────────────┘  └─────────────┘  └─────────────┘   │
└─────────────────────────────────────────────────────┘
```

### 2.2 模块划分

| 模块 | 文件 | 职责 |
|------|------|------|
| 调试器主循环 | `nemu/src/monitor/sdb/sdb.c` | 命令解析、调度、交互 |
| 表达式求值 | `nemu/src/monitor/sdb/expr.c` | 词法分析、递归下降求值 |
| 监视点 | `nemu/src/monitor/sdb/watchpoint.c` | 监视点池管理、扫描检测 |
| 寄存器 | `nemu/src/isa/riscv32/reg.c` | 寄存器显示、名称查找 |
| CPU执行 | `nemu/src/cpu/cpu-exec.c` | 指令执行循环、监视点扫描 |
| 字符串库 | `abstract-machine/klib/src/string.c` | AM运行时字符串函数 |
| 格式化输出 | `abstract-machine/klib/src/stdio.c` | AM运行时printf系列函数 |

## 三、详细设计

### 3.1 调试器命令系统

采用命令表驱动的设计，每个命令由名称、描述和处理函数组成：

```c
static struct {
  const char *name;
  const char *description;
  int (*handler) (char *);
} cmd_table [] = {
  { "help", "Display information about all supported commands", cmd_help },
  { "c",    "Continue the execution of the program",          cmd_c },
  { "q",    "Exit NEMU",                                     cmd_q },
  { "si",   "Single step [N] instructions",                  cmd_si },
  { "info", "Print program status (r/w)",                    cmd_info },
  { "x",    "Examine memory: x N EXPR",                      cmd_x },
  { "p",    "Evaluate expression: p EXPR",                   cmd_p },
  { "w",    "Set watchpoint: w EXPR",                        cmd_w },
  { "d",    "Delete watchpoint: d N",                        cmd_d },
};
```

**命令列表：**

| 命令 | 格式 | 功能 |
|------|------|------|
| `help` | `help [cmd]` | 显示帮助信息 |
| `c` | `c` | 继续执行程序 |
| `q` | `q` | 退出 NEMU |
| `si` | `si [N]` | 单步执行 N 条指令（默认1条） |
| `info r` | `info r` | 显示所有寄存器值 |
| `info w` | `info w` | 显示所有监视点 |
| `x` | `x N EXPR` | 检查内存（N个4字节单元） |
| `p` | `p EXPR` | 求值表达式 |
| `w` | `w EXPR` | 设置监视点 |
| `d` | `d N` | 删除监视点 |

### 3.2 表达式求值器

#### 3.2.1 词法分析

使用 POSIX 正则表达式库（regex.h）进行词法分析，支持以下 token 类型：

| Token 类型 | 正则表达式 | 说明 |
|-----------|-----------|------|
| `TK_HEXNUM` | `0[xX][0-9a-fA-F]+` | 十六进制数 |
| `TK_NUM` | `[0-9]+` | 十进制数 |
| `TK_REG` | `\$[$]?[a-zA-Z0-9]+` | 寄存器（如 $ra, $0） |
| `TK_EQ` | `==` | 等于 |
| `TK_NEQ` | `!=` | 不等于 |
| `TK_AND` | `&&` | 逻辑与 |
| `TK_OR` | `\|\|` | 逻辑或 |
| 单字符 | `+ - * / ( )` | 运算符和括号 |

#### 3.2.2 语法分析（递归下降）

采用**找主运算符**的递归下降算法：

```
eval(p, q) = 
  1. 若 tokens[p..q] 被括号整体包裹 → eval(p+1, q-1)
  2. 若是一元运算符（- + *） → 一元求值
  3. 找到最外层优先级最低的运算符 op
  4. 递归计算左边 eval(p, op-1) 和右边 eval(op+1, q)
  5. 应用 op 运算
```

**运算符优先级（从高到低）：**

| 优先级 | 运算符 | 说明 |
|-------|--------|------|
| 5 | `*`, `/` | 乘除 / 解引用（一元） |
| 4 | `+`, `-` | 加减 / 正负（一元） |
| 3 | `==`, `!=` | 相等比较 |
| 2 | `&&` | 逻辑与 |
| 1 | `\|\|` | 逻辑或 |

**一元运算符支持：**
- 一元负号：`-expr`
- 一元正号：`+expr`
- 指针解引用：`*expr`（读取内存）

#### 3.2.3 主运算符查找算法

```c
static int find_main_op(int p, int q) {
  int cnt = 0, op = -1, op_prio = 100;
  for (int i = p; i <= q; i++) {
    if (tokens[i].type == '(') { cnt++; continue; }
    if (tokens[i].type == ')') { cnt--; continue; }
    if (cnt > 0) continue;  // 跳过括号内的运算符
    // 计算当前运算符优先级，取优先级最低（最右）的作为主运算符
    int prio = get_priority(tokens[i].type);
    if (prio <= op_prio) { op_prio = prio; op = i; }
  }
  return op;
}
```

### 3.3 监视点系统

#### 3.3.1 数据结构

采用**静态池 + 双向链表**管理监视点：

```c
#define NR_WP 32           // 最多32个监视点

typedef struct watchpoint {
  int NO;                  // 监视点编号
  struct watchpoint *next; // 链表指针
  char expr[32];           // 监视表达式
  word_t last_val;         // 上一次的值
} WP;

static WP wp_pool[NR_WP];  // 监视点池
static WP *head = NULL;    // 使用中的链表头
static WP *free_ = NULL;   // 空闲链表头
```

#### 3.3.2 核心操作

| 操作 | 函数 | 时间复杂度 |
|------|------|-----------|
| 设置监视点 | `set_watchpoint(expr)` | O(1) 从空闲链取 |
| 删除监视点 | `delete_watchpoint(no)` | O(n) 遍历查找 |
| 列出监视点 | `list_watchpoint()` | O(n) |
| 扫描变化 | `scan_watchpoint()` | O(n) 每条指令执行后调用 |

#### 3.3.3 扫描触发机制

监视点扫描集成在 CPU 执行循环中，**每条指令执行后**检查所有监视点的值是否变化：

```
cpu_exec(n):
  for i = 1..n:
    执行一条指令
    scan_watchpoint()      ← 检查所有监视点
    if 有监视点变化:
      暂停执行，打印新旧值
```

### 3.4 寄存器系统

#### 3.4.1 寄存器显示

`isa_reg_display()` 以表格形式打印所有 32 个通用寄存器 + PC：

```
name         value    hex
zero  0x00000000       0
  ra  0x80000040  2147483712
  sp  0x87000000  2264924160
  gp  0x00000000       0
  tp  0x00000000       0
 t0~t6 ...
 s0~s11 ...
 a0~a7 ...
  pc  0x80000000  2147483648
```

#### 3.4.2 寄存器名查找

`isa_reg_str2val(name, *success)` 将寄存器名称（如 `"ra"`, `"$ra"`, `"$1"`）转换为寄存器值，供表达式求值器使用。

### 3.5 AM 基础库 (klib)

#### 3.5.1 字符串函数 (`string.c`)

| 函数 | 功能 |
|------|------|
| `strlen` | 字符串长度 |
| `strcpy` | 字符串拷贝 |
| `strncpy` | 限定长度字符串拷贝 |
| `strcat` | 字符串拼接 |
| `strcmp` | 字符串比较 |
| `strncmp` | 限定长度字符串比较 |
| `memset` | 内存设置 |
| `memcpy` | 内存拷贝 |
| `memmove` | 内存移动（支持重叠） |
| `memcmp` | 内存比较 |

#### 3.5.2 格式化输出 (`stdio.c`)

实现 `printf`, `sprintf`, `snprintf`, `vprintf`, `vsprintf`, `vsnprintf` 系列函数，支持以下格式说明符：

| 格式符 | 说明 |
|--------|------|
| `%d` | 十进制有符号整数 |
| `%u` | 十进制无符号整数 |
| `%x`, `%X` | 十六进制整数 |
| `%s` | 字符串 |
| `%c` | 字符 |
| `%p` | 指针 |
| `%%` | 百分号本身 |
| `%0Nd` | 前导零填充 |

## 四、函数调用执行流

### 4.1 调试器主循环执行流

```
sdb_mainloop()
    │
    ├─ rl_gets()                    ← 读取用户输入
    │
    ├─ strtok() 解析命令和参数
    │
    ├─ sdl_clear_event_queue()      ← 清空SDL事件队列
    │
    ├─ 查找命令表 cmd_table[]
    │
    └─ 调用命令处理函数
         ├─ cmd_c()     → cpu_exec(-1)     持续执行
         ├─ cmd_si()    → cpu_exec(N)      单步N条
         ├─ cmd_info()  → isa_reg_display() 寄存器
         │             → list_watchpoint()  监视点
         ├─ cmd_x()     → expr() 求值地址
         │             → vaddr_read() 读内存
         ├─ cmd_p()     → expr() 求值表达式
         ├─ cmd_w()     → set_watchpoint() 设置监视点
         ├─ cmd_d()     → delete_watchpoint() 删除监视点
         └─ cmd_q()     → 返回 -1 退出
```

### 4.2 表达式求值调用链

```
expr("*0x80000000 + $ra == 100", &success)
    │
    ├─ make_token()           ← 词法分析，生成 token 数组
    │   └─ regexec()          ← 逐个正则匹配
    │
    └─ eval(0, n-1)           ← 递归下降求值
         │
         ├─ find_main_op()    ← 找到 == 作为主运算符（优先级最低）
         │
         ├─ eval(左)          ← 递归求 "*0x80000000 + $ra"
         │   ├─ find_main_op() → 找到 +
         │   ├─ eval(左左)    ← 一元 * 解引用 0x80000000
         │   │   └─ vaddr_read()
         │   └─ eval(左右)    ← $ra 寄存器
         │       └─ isa_reg_str2val()
         │
         └─ eval(右)          ← 数字 100
```

### 4.3 监视点触发执行流

```
cpu_exec(n)
    │
    └─ execute(n)
         │
         └─ for n times:
              │
              ├─ exec_once()           ← 执行一条指令
              ├─ g_nr_guest_inst++
              │
              ├─ scan_watchpoint()     ← 扫描所有监视点
              │   │
              │   └─ for each wp in head:
              │        ├─ expr(wp.expr) ← 重新求值
              │        └─ if val != last_val:
              │             ├─ 打印新旧值
              │             ├─ wp.last_val = val
              │             └─ return 1 (触发暂停)
              │
              └─ if 触发: nemu_state = STOP
```

### 4.4 内存地址访问流

```
cmd_x() / 表达式求值中的 * 运算符
    │
    └─ vaddr_read(addr, len)
         │
         └─ paddr_read(addr, len)    ← 物理内存读
              │
              ├─ 查找 MMIO 区域
              │   └─ mmio_read()     ← 设备 MMIO 读
              │
              └─ 内存区域
                   └─ guest_to_host() + *ptr
```

## 五、关键设计决策

### 5.1 表达式求值算法选择

选择**找主运算符的递归下降法**而非传统的递归下降文法（如 expr → term + expr），原因：
- 实现更简洁，无需为每个优先级写一个函数
- 括号处理通过括号深度计数器统一处理
- 一元运算符通过上下文判断（前一个token是运算符或左括号）

### 5.2 监视点池设计

采用**静态数组 + 链表**的内存池设计：
- 避免动态内存分配（嵌入式环境友好）
- 分配/回收 O(1) 时间复杂度
- 32个监视点完全满足调试需求

### 5.3 词法分析实现

使用 POSIX regex 库而非手写词法分析器：
- 正则表达式声明式定义token，可读性好
- 初始化时预编译所有正则表达式，运行效率高
- 扩展性好，添加新token只需增加一条规则

## 六、测试与验证

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| hello 程序 | 运行 hello 输出 "Hello, AbstractMachine!" | ✅ 通过 |
| 单步执行 | si 10 单步10条指令 | ✅ 通过 |
| 寄存器查看 | info r 显示所有寄存器 | ✅ 通过 |
| 内存检查 | x 10 0x80000000 读取内存 | ✅ 通过 |
| 表达式求值 | p $ra + 4 等各种表达式 | ✅ 通过 |
| 监视点 | 设置监视点，值变化时暂停 | ✅ 通过 |
| 字符串库 | 各种字符串函数 | ✅ 通过 |
| 格式化输出 | printf 各种格式符 | ✅ 通过 |
