# PA3：设备与总线

## 一、概述

PA3 的目标是实现 NEMU 中的输入输出（IO）设备模型，以及完善 AbstractMachine（AM）层的设备驱动，使上层应用程序能够通过统一的 AM IO 接口访问图形、定时器、键盘等外设。核心内容包括 VGA 图形设备、定时器（RTC）、键盘输入以及 MMIO 总线机制。

## 二、软件架构

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        应用程序层 (am-kernels)                   │
│               snake / litenes / demo / typing-game               │
└─────────────────────────────┬───────────────────────────────────┘
                              │ AM IO 接口 (am.h, amdev.h)
┌─────────────────────────────▼───────────────────────────────────┐
│              AbstractMachine (AM) 设备驱动层                      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐            │
│  │ gpu.c    │ │ timer.c  │ │ input.c  │ │ audio.c  │            │
│  │ (图形)   │ │ (定时器) │ │ (键盘)   │ │ (音频)   │            │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘            │
└─────────────────────────────┬───────────────────────────────────┘
                              │ MMIO 读写 (inl/outl)
┌─────────────────────────────▼───────────────────────────────────┐
│                    NEMU 设备模型层                                │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐            │
│  │ vga.c    │ │ timer.c  │ │ keyboard │ │ serial.c │            │
│  │ VGA图形  │ │ RTC时钟  │ │ i8042键盘│ │ 串口输出  │            │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘            │
│  ┌─────────────────────────────────────────────────┐             │
│  │              MMIO 映射系统 (mmio.c)              │             │
│  │  地址解码 → 查找设备 → 调用设备回调 → 返回数据    │             │
│  └─────────────────────────────────────────────────┘             │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 MMIO 内存映射

| 设备 | 基地址 | 大小 | 说明 |
|------|--------|------|------|
| 串口 (serial) | 0xa00003f8 | 8字节 | 16550 UART 兼容 |
| RTC 时钟 | 0xa0000048 | 8字节 | 64位微秒计数器 |
| VGA 控制 | 0xa0000100 | 8字节 | 宽高寄存器 + sync寄存器 |
| VGA 帧缓冲 | 0xa1000000 | 480KB | 400×300 × 4字节 (ARGB8888) |
| 键盘 (i8042) | 0xa0000060 | 4字节 | PS/2 键盘数据端口 |
| 音频控制 | 0xa0000200 | 24字节 | AC97 兼容 |
| 音频缓冲 | 0xa1200000 | 64KB | DMA 流缓冲 |

## 三、详细设计

### 3.1 MMIO 总线机制

#### 3.1.1 MMIO 映射表

NEMU 使用 MMIO（Memory-Mapped I/O）机制将设备寄存器映射到物理地址空间。所有 MMIO 区域通过 `add_mmio_map()` 注册：

```c
void add_mmio_map(const char *name, paddr_t addr, void *space, 
                  uint32_t len, io_callback_t callback);
```

**参数说明：**
- `name`: 设备名称（调试用）
- `addr`: 物理地址基址
- `space`: 宿主内存中对应的数据空间
- `len`: 映射区域长度（字节）
- `callback`: 读写回调函数（NULL 表示直接内存读写）

#### 3.1.2 地址解码流程

当 CPU 执行内存读写指令访问 MMIO 区域时：

```
vaddr_read/write(addr, len)
    │
    └─ paddr_read/write(addr, len)
         │
         ├─ 在 MMIO 映射表中查找 addr 所属区域
         │
         ├─ 找到 → 调用该区域的 callback(offset, len, is_write)
         │   │
         │   ├─ 读操作: 从 space 读取数据返回
         │   └─ 写操作: 写入数据到 space，触发设备动作
         │
         └─ 未找到 → 正常内存读写
```

### 3.2 VGA 图形设备

#### 3.2.1 NEMU 侧 VGA 模型 (`vga.c`)

**寄存器定义：**

| 偏移 | 寄存器 | 类型 | 说明 |
|------|--------|------|------|
| 0x00 | VGACTL | 只读 | 高16位=宽度，低16位=高度 |
| 0x04 | SYNC | 只写 | 写入1触发屏幕刷新 |

**核心函数：**

```c
void init_vga() {
  // 分配 VGA 控制寄存器空间 (8字节)
  vgactl_port_base = new_space(8);
  vgactl_port_base[0] = (width << 16) | height;  // 宽高只读
  
  // 注册 VGACTL MMIO 映射（带回调）
  add_mmio_map("vgactl", CONFIG_VGA_CTL_MMIO, vgactl_port_base, 8, vgactl_io_handler);
  
  // 分配帧缓冲 (width * height * 4字节)
  vmem = new_space(screen_size());
  
  // 注册帧缓冲 MMIO 映射（直接内存读写，无回调）
  add_mmio_map("vmem", CONFIG_FB_ADDR, vmem, screen_size(), NULL);
  
  // 初始化 SDL 窗口（如果开启了 VGA_SHOW_SCREEN）
  init_screen();
}
```

**SYNC 寄存器回调：**

```c
static void vgactl_io_handler(uint32_t offset, int len, bool is_write) {
  // 写入 sync 寄存器时触发屏幕更新
  if (is_write && offset == 4) {
    vga_update_screen();
  }
}

void vga_update_screen() {
  if (vgactl_port_base[1] != 0) {
    update_screen();      // SDL 更新纹理 + 渲染
    vgactl_port_base[1] = 0;  // 清除 sync 标志
  }
}
```

#### 3.2.2 AM 侧 GPU 驱动 (`gpu.c`)

**AM GPU 接口：**

| 接口 | 功能 |
|------|------|
| `AM_GPU_CONFIG` | 获取 GPU 配置（分辨率、显存大小） |
| `AM_GPU_FBDRAW` | 帧缓冲绘制（指定位置+像素数据+同步） |
| `AM_GPU_STATUS` | 查询 GPU 状态（是否就绪） |

**GPU 配置读取：**

```c
void __am_gpu_config(AM_GPU_CONFIG_T *cfg) {
  uint32_t vgactl = inl(VGACTL_ADDR);    // 读 VGACTL 寄存器
  w = (vgactl >> 16) & 0xffff;          // 高16位 = 宽度
  h = vgactl & 0xffff;                  // 低16位 = 高度
  cfg->present = true;
  cfg->width = w;
  cfg->height = h;
  cfg->vmemsz = w * h * sizeof(uint32_t); // 显存大小
}
```

**帧缓冲绘制：**

```c
void __am_gpu_fbdraw(AM_GPU_FBDRAW_T *ctl) {
  int x = ctl->x, y = ctl->y;
  uint32_t *pixels = (uint32_t *)ctl->pixels;
  // 逐像素写入帧缓冲
  for (int j = 0; j < ctl->h; j++) {
    for (int i = 0; i < ctl->w; i++) {
      uint32_t px = pixels[j * ctl->w + i];
      outl(FB_ADDR + ((y + j) * w + (x + i)) * 4, px);
    }
  }
  // 如果 sync=true，写入 sync 寄存器触发刷新
  if (ctl->sync) {
    outl(SYNC_ADDR, 1);
  }
}
```

#### 3.2.3 SDL 屏幕渲染

当开启 `CONFIG_VGA_SHOW_SCREEN` 时：

```
帧缓冲 (vmem)
    │ SDL_UpdateTexture
    ▼
SDL_Texture (GPU纹理)
    │ SDL_RenderCopy
    ▼
SDL_Renderer (渲染器)
    │ SDL_RenderPresent
    ▼
SDL_Window (窗口)
```

**缩放策略：**
- 400×300 分辨率 → 窗口放大 2 倍显示（800×600 像素）
- 800×600 分辨率 → 1:1 显示

### 3.3 定时器设备

#### 3.3.1 NEMU 侧 RTC 设备 (`timer.c`)

提供 64 位微秒级实时计数器：

| 偏移 | 寄存器 | 类型 | 说明 |
|------|--------|------|------|
| 0x00 | LO | 只读 | 低 32 位微秒数 |
| 0x04 | HI | 只读 | 高 32 位微秒数 |

计数器从宿主机系统时间获取，确保与真实时间同步。

#### 3.3.2 AM 侧定时器驱动 (`timer.c`)

**AM 定时器接口：**

| 接口 | 功能 |
|------|------|
| `AM_TIMER_UPTIME` | 获取自启动以来的微秒数（64位） |
| `AM_TIMER_RTC` | 获取实时时钟（年月日时分秒） |

**64位时间读取：**

```c
void __am_timer_uptime(AM_TIMER_UPTIME_T *uptime) {
  uint32_t lo = inl(RTC_ADDR + 0);   // 读低32位
  uint32_t hi = inl(RTC_ADDR + 4);   // 读高32位
  uptime->us = ((uint64_t)hi << 32) | lo;  // 组合为64位
}
```

> **注意**：由于分两次读取32位寄存器，存在高位进位的竞态可能。严谨实现应多次读取验证一致性。

### 3.4 键盘输入设备

#### 3.4.1 NEMU 侧 i8042 键盘控制器 (`keyboard.c`)

模拟经典的 8042 PS/2 键盘控制器：

| 端口地址 | 类型 | 说明 |
|---------|------|------|
| KBD_DATA | 读 | 读取按键扫描码（带 KEYDOWN_MASK） |

**按键编码格式：**
```
31           15 14              0
┌──────────────┬─────────────────┐
│   保留(0)    │  KEYDOWN_MASK   │  keycode
└──────────────┴─────────────────┘
bit15 = 1 表示按下，0 表示抬起
bit14..0 = AM 键盘扫描码
```

**键盘事件队列：**

```c
#define KEY_QUEUE_LEN 1024
static int key_queue[KEY_QUEUE_LEN];
static int key_f = 0, key_r = 0;  // 队首/队尾指针

// SDL 事件 → 按键入队
void send_key(uint8_t scancode, bool is_keydown) {
  if (nemu_state.state == NEMU_RUNNING && keymap[scancode] != NEMU_KEY_NONE) {
    uint32_t am_scancode = keymap[scancode] | (is_keydown ? KEYDOWN_MASK : 0);
    key_enqueue(am_scancode);
  }
}

// 读端口 → 出队
static void i8042_data_io_handler(uint32_t offset, int len, bool is_write) {
  assert(!is_write);
  assert(offset == 0);
  i8042_data_port_base[0] = key_dequeue();
}
```

**SDL 扫描码 → AM 扫描码映射：**

NEMU 内部维护一个 256 项的 keymap 数组，将 SDL 的 SDL_SCANCODE_* 映射为 AM_KEY_* 编码。

#### 3.4.2 AM 侧键盘驱动 (`input.c`)

```c
void __am_input_keybrd(AM_INPUT_KEYBRD_T *kbd) {
  uint32_t code = inl(KBD_ADDR);           // 读键盘数据端口
  kbd->keydown = (code & KEYDOWN_MASK) != 0; // 提取按下/抬起标志
  kbd->keycode = code & ~KEYDOWN_MASK;     // 提取键码
}
```

### 3.5 串口设备 (`serial.c`)

模拟 16550 UART 串口，用于文本输出：
- 写入 THR（发送保持寄存器）→ 输出到 stderr
- 是 AM 程序 printf 的最终输出通道

### 3.6 设备更新机制

`device_update()` 函数在 CPU 执行循环中被周期性调用（由定时器节流，默认约 60Hz）：

```c
void device_update() {
  static uint64_t last = 0;
  uint64_t now = get_time();
  if (now - last < 1000000 / TIMER_HZ) return;  // 节流
  last = now;

  vga_update_screen();    // 检查并更新 VGA 屏幕

  // 处理 SDL 事件队列
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT:      nemu_state.state = NEMU_QUIT; break;
      case SDL_KEYDOWN:
      case SDL_KEYUP:     send_key(scancode, is_keydown); break;
      default: break;
    }
  }
}
```

## 四、函数调用执行流

### 4.1 VGA 帧缓冲绘制执行流

```
应用程序调用 io_write(AM_GPU_FBDRAW, ...)
    │
    └─ __am_gpu_fbdraw(ctl)            [AM 侧: gpu.c]
         │
         ├─ for 每个像素:
         │   └─ outl(FB_ADDR + offset, pixel)
         │       └─ 写内存指令 (sw)     [RISC-V 指令]
         │           │
         │           └─ vaddr_write()
         │               └─ paddr_write()
         │                   │
         │                   └─ MMIO 查找 → vmem 区域
         │                       └─ 直接写入帧缓冲内存
         │
         └─ if (ctl->sync):
              └─ outl(SYNC_ADDR, 1)
                  └─ 写内存指令 (sw)
                      └─ vaddr_write()
                          └─ paddr_write()
                              │
                              └─ MMIO 查找 → vgactl 区域
                                  └─ vgactl_io_handler(offset=4, is_write=true)
                                      │
                                      └─ vga_update_screen()
                                          │
                                          └─ update_screen()     [SDL]
                                              ├─ SDL_UpdateTexture
                                              ├─ SDL_RenderCopy
                                              └─ SDL_RenderPresent
```

### 4.2 定时器读取执行流

```
应用程序调用 io_read(AM_TIMER_UPTIME)
    │
    └─ __am_timer_uptime(uptime)        [AM 侧: timer.c]
         │
         ├─ inl(RTC_ADDR + 0)           // 读低32位
         │   └─ 读内存指令 (lw)
         │       └─ vaddr_read()
         │           └─ paddr_read()
         │               └─ MMIO 查找 → rtc 区域
         │                   └─ rtc_io_handler → 返回低32位时间
         │
         ├─ inl(RTC_ADDR + 4)           // 读高32位
         │   └─ 同上
         │
         └─ uptime->us = (hi << 32) | lo
```

### 4.3 键盘输入执行流

```
用户按下键盘
    │
    └─ SDL 产生 SDL_KEYDOWN 事件
         │
         └─ device_update() 轮询 SDL 事件
              │
              ├─ send_key(scancode, is_keydown=true)
              │   │
              │   ├─ keymap[scancode] → AM_KEY_* 码
              │   └─ key_enqueue(am_scancode | KEYDOWN_MASK)
              │
              └─ （事件入队，等待程序读取）

应用程序调用 io_read(AM_INPUT_KEYBRD)
    │
    └─ __am_input_keybrd(kbd)           [AM 侧: input.c]
         │
         └─ inl(KBD_ADDR)
             └─ 读内存指令 (lw)
                 └─ vaddr_read()
                     └─ paddr_read()
                         │
                         └─ MMIO 查找 → keyboard 区域
                             └─ i8042_data_io_handler()
                                 └─ key_dequeue() → 返回按键码
```

### 4.4 GPU 配置读取执行流

```
应用程序 io_read(AM_GPU_CONFIG)
    │
    └─ __am_gpu_config(cfg)             [AM 侧: gpu.c]
         │
         └─ inl(VGACTL_ADDR)
             └─ 读内存指令 (lw)
                 └─ paddr_read(vgactl_addr, 4)
                     │
                     └─ MMIO 查找 → vgactl 区域
                         └─ 直接读取 vgactl_port_base[0]
                            = (width << 16) | height
```

## 五、关键设计决策

### 5.1 MMIO vs PIO

选择 **MMIO（内存映射IO）** 作为主要的设备访问方式：
- RISC-V 架构没有专门的 IO 指令（如 x86 的 in/out）
- 所有设备访问统一为内存读写指令（lw/sw），简化了 CPU 实现
- 地址空间充足（32位可寻址4GB），MMIO 区域放在高地址

### 5.2 帧缓冲设计

采用 **线性帧缓冲（Linear Framebuffer）** 模式：
- 每个像素 4 字节（ARGB8888 格式）
- 像素按行连续存储，地址 = FB_BASE + (y * width + x) * 4
- 与 SDL 纹理格式一致，可直接 memcpy 更新
- 简单直接，易于理解和使用

### 5.3 SYNC 寄存器机制

使用 **显式 sync 寄存器** 而非自动刷新：
- 程序主动写入 sync 寄存器触发屏幕更新
- 避免每帧像素写入都触发 SDL 渲染（性能影响大）
- 程序控制刷新时机，实现双缓冲效果

### 5.4 键盘事件队列

采用 **环形缓冲区** 存储键盘事件：
- 解耦 SDL 事件产生与程序读取
- 防止快速按键丢失
- 1024 项深度完全满足需求

## 六、测试与验证

| 测试项 | 测试内容 | 结果 |
|--------|---------|------|
| hello 输出 | 串口输出 "Hello, AbstractMachine!" | ✅ 通过 |
| VGA 配置读取 | AM 读取 GPU 宽高为 400×300 | ✅ 通过 |
| 帧缓冲写入 | 写入像素 → SDL 窗口显示 | ✅ 通过 |
| SYNC 刷新 | 写入 sync 寄存器 → 屏幕更新 | ✅ 通过 |
| 定时器读取 | io_read(AM_TIMER_UPTIME) 返回递增时间 | ✅ 通过 |
| 键盘输入 | 按键 → AM_INPUT_KEYBRD 返回正确键码 | ✅ 通过 |
| snake 游戏 | 贪吃蛇图形正常，键盘控制移动 | ✅ 通过 |
| demo 程序 | 8个demo图形效果正常 | ✅ 通过 |
| litenes FC模拟器 | 马里奥游戏画面正常渲染 | ✅ 通过 |
