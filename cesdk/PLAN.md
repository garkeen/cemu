# cesdk 计划（cemu 阶段 5：自举链）

本文是 5 的方案与排期。ABI 的权威定义是 `include/cemu_boot.h`，本文只解释它。

## 1. 是什么

一条**自举链**：固件 → 引导契约 → 内核 → 用户态/安装器，全部是本项目产物；
从一块空块设备开始安装，重启后由自家固件把自家内核装起来。

抽象只有两件东西：

1. **`cemu_boot_info`**：固件交给内核的一份版本化、纯数据的机器描述
   （内存图、控制台、块设备、帧缓冲、中断控制器、PCI、DTB/ACPI、根分区、墙钟）。
2. **两层后端**：`arch/<isa>/` 只放 ISA 规定的东西（入口、陷阱、页表、上下文、
   原子、时钟）；`machine/<machine>/` 只放机器的东西（设备在哪、怎么初始化）。

固件、内核、用户态是三份独立程序；共享的是各自内部的 core，不是链接进同一个镜像。

## 2. 怎么做

### 2.1 契约

- 固件填 `cemu_boot_info`，内核入口只做：校验 `magic/version/size` → 拷贝记录 →
  切到自己的栈 → 用记录里的内存图建分配器。
- 数组一律 `物理地址 + count + stride`；未知的 `kind` 跳过，不猜。
- 交接后记录不可变；固件占用内存在内存图里标 `CEMU_MEM_RESERVED`。
- 没有跨模式回调。riscv 的 M 模式服务（定时器、IPI、RFENCE）走标准 SBI ecall，
  是 ISA 级接口，不是指向固件逻辑的函数指针。
- 入口寄存器状态写在 `include/cemu_boot.h` 顶部，两边按同一份实现。

### 2.2 所有权移交

固件在跳转前：关中断、停 DMA、清设备中断状态、把设备留在静止态。
内核在初始化自己的驱动时重新编程设备，不假设固件留下的状态。

### 2.3 固件做什么

1. **POST**：内存探测、CPU 特性、控制台、RTC/CMOS（x86）、设备枚举
   （x86 PCI / riscv DTB）、块设备识别、帧缓冲。
2. **启动策略**：启动顺序 → 读 LBA0 → 校验 `0x55AA` → MBR/GPT → 找 FAT32 分区
   → 读 `/boot/cemu.elf` 与配置。
3. **装载**：ELF32/ELF64（按 ISA 校验 ELFCLASS）→ PT_LOAD + 清 bss。
4. **交接**：构造 `cemu_boot_info` → 按入口状态跳转。

### 2.4 内核做什么

早期：控制台、内存图、页表、陷阱。之后：块设备/VFS/FAT32、调度、用户态。
`arch/` 之外不出现 ISA 判断；驱动只依赖 `cemu_boot_info` 里的描述符。

### 2.5 riscv 的 M 模式服务（SBI 子集）

固件交接后驻留 M 模式，只提供三组标准 SBI：`TIME`（stimecmp）、`IPI`（msip）、
`RFENCE`（SMP 后）。x86 没有对应物，固件交接即退场。

## 3. 基线清单（“任何固件/OS 都会做的基础事情”）

下面两份清单是 5 的验收基线，不是可选项。括号里是交付它的里程碑。

### 3.1 固件基线

- [ ] **CPU 进入已知状态**：关中断、设栈、装 GDT/IDT（x86）或 mtvec（riscv），
      清 bss，建立可预测的入口环境。（5.1 / 5.2）
- [ ] **内存发现**：探测可用 RAM、区分 RAM/MMIO/保留区，生成内存图并保留固件
      自身、SBI、帧缓冲、ACPI/DTB 区。（5.1 / 5.2）
- [ ] **POST 自检**：内存写读图案、CPU 特性（CPUID/misa）、关键设备存在性；
      结果打印成表，失败给出可读错误并尝试下一启动项。（5.1 / 5.2）
- [ ] **早期控制台**：串口（x86 COM1 / riscv UART）从第一条 POST 消息起可用；
      x86 另有 VGA 文本/线性帧缓冲。（5.1 / 5.2）
- [ ] **时钟与 RTC**：单调时钟（PIT/CLINT 或 DTB timebase）与墙钟
      （x86 CMOS/RTC；riscv 无 RTC 时传 0 并标记）。（5.1 / 5.2）
- [ ] **设备枚举**：x86 走 PCI 配置空间（CF8/CFC，ECAM 可选）；riscv 解析 DTB。
      产出块设备、显示、输入、中断控制器的描述符。（5.1 / 5.2）
- [ ] **中断控制器初始化**：PIC 重映射/屏蔽、IOAPIC 编程、PLIC 使能与阈值；
      交接前全部静默。（5.1 / 5.2）
- [ ] **存储栈**：ATA PIO（x86）与 virtio-mmio（riscv）识别、容量读取、扇区读。
      （5.1 / 5.2）
- [ ] **分区与文件系统**：MBR 与 GPT 解析、活动分区选择、FAT32 只读挂载。
      （5.1 / 5.2）
- [ ] **启动管理**：启动顺序（可配置、持久化到 CMOS 保留字节或磁盘固件区）、
      启动菜单与超时默认项、失败回退到下一项。（5.2；riscv 先固定顺序）
- [ ] **装载器**：ELF32/ELF64 解析、PT_LOAD、清 bss、按 ISA 校验 ELFCLASS 与
      入口地址、越界检查。（5.1 / 5.2）
- [ ] **交接**：填 `cemu_boot_info`、内存保留、设备静默、按约定寄存器状态跳转。
      （5.1 / 5.2）
- [ ] **固件自身服务**：复位与关机（x86 port 0x92/0xCF9，riscv sifive_test）、
      看门狗/超时、POST 错误码。（5.1 / 5.2）

### 3.2 OS 基线

- [ ] **启动**：入口校验并拷贝 `cemu_boot_info`、早期控制台、建立物理页分配器
      与内核堆。（5.3）
- [ ] **虚拟内存**：内核页表、用户地址空间、缺页处理、地址空间切换、保护
      （x86 32 位分页 / riscv Sv39）。（5.3）
- [ ] **中断与异常**：自己的 IDT/stvec、IRQ 分发与 EOΙ、定时器 tick、系统调用
      入口、用户态陷阱返回。（5.3）
- [ ] **并发**：原子操作、自旋锁、关中断临界区、上下文切换、睡眠/唤醒。
      （5.3）
- [ ] **调度与进程**：PCB/TCB、时间片或协作调度、`fork`/`exec`/`wait`/`exit`
      （或等价的 spawn/join）。（5.3）
- [ ] **用户态**：ring3/U-mode、`copyin`/`copyout` 参数校验、系统调用表。
      （5.3）
- [ ] **设备驱动**：控制台、块设备（virtio-blk/ATA）、定时器、中断控制器、
      帧缓冲；驱动只依赖 `cemu_boot_info` 描述符。（5.3）
- [ ] **文件系统**：VFS 抽象 + FAT32 读写、根分区挂载、目录与文件操作。
      （5.3，写路径 5.4）
- [ ] **时间**：单调时钟、墙钟、睡眠与超时。（5.3）
- [ ] **用户程序**：init、shell、`ls`/`cat`/`echo` 等基本工具。（5.3）
- [ ] **错误处理**：panic、断言、内核栈溢出检测、非法访问与缺页的可读报告。
      （5.3）
- [ ] **安装器**：作为用户程序运行，写 GPT + FAT32、复制内核与用户态、设置
      启动项；失败可重试。（5.4）
- [ ] **可观测性**：内核日志环形缓冲（可 `dmesg` 读取）、启动参数。（5.3）

基线之外、后续阶段再加：SMP、抢占式调度、网络、GPU 加速、电源管理策略。

## 4. 目录

```
cesdk/
  include/cemu_boot.h    固件→内核契约（版本化，权威定义）
  include/cesdk.h        crun/klib 公共头
  crun/                  _start（每 ISA）、halt、klib
  arch/riscv64/          入口、陷阱、Sv39、上下文、原子
  arch/x86/              GDT/IDT、分页、上下文、原子
  machine/virt/          DTB、UART/CLINT/PLIC、virtio-mmio
  machine/pc/            PCI、ATA、PIT/PIC、COM1、CMOS/RTC、VGA
  firmware/core/         POST、设备表、ELF 装载、boot_info、交接
  firmware/boot/         启动顺序、MBR/GPT、FAT32 读、菜单
  kernel/core/           内存、调度、VFS、FAT32、shell
  kernel/drivers/        virtio-blk、ATA、UART、定时器
  user/                  init、shell、installer
  tools/mkdisk.py        宿主侧 GPT+FAT32 镜像生成（自举期；之后由安装器取代）
```

`cesdk/` 是仓内独立 CMake 工程，不进 cemu 的 `src/` 构建，也不参与依赖边检查。

## 5. 构建与运行

```sh
cmake -G Ninja -B build \
  -DCESDK_CLANG=D:/LLVM/bin/clang.exe -DCESDK_LD=D:/LLVM/bin/ld.lld.exe
cmake --build build

../build/cemu.exe --machine virt --isa riscv64 -bios build/hello-riscv64.elf
../build/cemu.exe --machine x86 --isa x86 build/hello-x86.elf
```

5.1 起增加 `firmware-<isa>-<machine>.elf`、`kernel-<isa>-<machine>.elf` 与
`test-disk.img`（`tools/mkdisk.py` 生成）。

## 6. 里程碑与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| 5.0 | crun + hello | 两 ISA 在 cemu 打印并正常退出 ✅ |
| 5.1 | `cemu_boot_info` + riscv 固件（POST/DTB/GPT+FAT32/ELF/交接 + SBI） | 固件打印 POST 表；从测试盘装载 hello 内核；内核用自己的 UART 驱动打印 |
| 5.2 | x86 固件（POST/CMOS/PCI/ATA/GPT+FAT32/ELF/交接） | 同一份 hello 内核在 x86 上跑通 |
| 5.3 | 内核（内存/页表/陷阱/定时/块设备/FAT32/shell） | 两 ISA 挂根文件系统、进 shell、`ls`/`cat` |
| 5.4 | 安装器（GPT+FAT32 写） | 空盘 → 安装 → 重启 → 进 shell；宿主侧能验出 GPT/FAT32 |
| 5.2b（可选） | 实模式前端 + 最小 INT 13h/10h | 真执行 0x7C00 的 MBR 链 |

## 7. 取舍

- x86 先 32 位、非 PAE；PAE/长模式归阶段 6。
- FAT32 先读后写；写用预分配簇链，不做碎片整理。
- ELF 固定基址、非 PIE；固件只做 PT_LOAD + 清 bss。
- 先单核、先协作调度；SMP 与抢占在内核稳定后加。
- 引导期的第一块盘由 `tools/mkdisk.py` 造；它同时是安装器的对拍基准。

## 8. 为什么这样抽象

- 一份内核源码编两次：内核只见数据，ISA 差异全在 `arch/`。
- 可测：宿主能合成 `cemu_boot_info`，内核不经过固件就能跑。
- 可替换：固件和内核各自演进，契约不变即可互换。
- 故障面小：没有跨模式回调，就没有“回调时栈是谁的、中断开没开”。
- 可演化：版本 + size + stride，只追加字段，旧内核仍能启动。
