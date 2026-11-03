# cemu

纯 C、纯 Win32 API 的解释型全系统模拟器。多 ISA（riscv64 先行、riscv32 已接入，
x86 已接入，之后 arm / mips），加载 `.bin` / `.elf` / `.img`，按真实机器契约运行真实软件。

## 目标

- **不做翻译、不做中间表示**：巨型 switch 直接解释一条指令（取指→译码→执行
  在同一 C 作用域内），代码对照手册逐行可读，pc 是唯一提交点。
- **通用性靠结构**：core 不认 CPU 型号，设备不感知 ISA，机器定义与 ISA 正交；
  运行时按 ELF 的 `e_machine` + `EI_CLASS` 选 ISA（riscv32/riscv64 共用
  `e_machine=243`），一个 exe 内所有 ISA 同时在场。
- **阶梯上全是真实软件**：riscv-tests → OpenSBI → xv6 → Linux，无 cemu 定制成分。
- cesdk（AM 式构建系统）排在真实 OS 跑通之后，其 API 才有设计依据。

## 现状

阶段 0–3.5 完成（ISA 一致性、真实固件、x86 保护模式与分页、调试器与 GUI；
gdb 的硬件观察点走 CPU 自己的 DR0-DR7，只读观察点因 DR7 没有该编码而拒接、
riscv 侧尚未接 trigger 模块，两处记在 D6）。

**阶段 4（设备全集与真实 OS）进行中。** x86 的 PC 设备模型全集到位：VGA 家族核
（VBE/DISPI、链式 256 色、EDID）、PS/2 键鼠与串口鼠标、8259/8254、8272 FDC +
8237 DMA、IDE/ATAPI（LBA-48、multi-sector）、LAPIC/IOAPIC（含 NMI 交付）、
PIIX4 PM（SCI、S5）、SMBus 主机 + SPD EEPROM、XBCS、IGNNE#；riscv 侧 virt 板带
virtio-mmio（blk/console/keyboard/tablet），oberon 板按参考模拟器逐契约复刻。
余下规格缺口按 AGENTS.md §六 的 D17/D19 在册。

验收阶梯（全部是真实软件，无 cemu 定制成分）：

- **xv6-x86 ✅**：SeaBIOS → IDE 盘 → shell。
- **Linux ✅**：引导越过 `VFS: Mounted root (ext2 filesystem)`，控制台出提示符。
- **Windows 1.01 ✅**：EGA 640×350 的 MS-DOS Executive 桌面，`fbdump` 帧与
  QEMU 基线逐像素相同，PS/2 指针可移动。
- **Windows 2.0 ✅**：过 logo（此前卡死是 vgabios 的 int10 用客人栈写穿客人代码），
  帧与 QEMU 逐色相同，详见 progress.log。
- **Windows 3.1 ✅**：1024×768 桌面（此前的红花屏是 vgabios 用了"标准 VGA"构建、
  与 Bochs stdvga 卡不匹配；重建为 Bochs VBE 后对上）。
- **HelenOS 0.14.1 riscv64 ✅**：内核全量引导零异常，止于上游未实现的 S→U 交接
  （QEMU 执行同一镜像停在同一处 `j .`）。
- **SPDOS/DOS7 软盘 ✅**：经 SeaBIOS 进 DOS；WPS 卡死经 QEMU 对拍判为 CHLIB
  依赖真机 BIOS 约定（0x80 的几何、段寄存器跨调用保持），不是 cemu 缺陷。
- **Windows 95 进行中**：旧结论（VMCB `0x120` 对 `0`、根因未定）已推翻——那是
  VMCB 位每几千条指令翻转一次的采样相位差。真根因是 **POPF 在特权检查前就推进了
  ESP**：V86/IOPL<3 的 #GP 是 fault，不许改 ESP，cemu 却先弹后查，V86 监视器按
  保存的 SP 读到的不是 flags 而是 `call` 压入的返回地址 `0x0FB7`（TF=1），于是
  把客户置成单步模式，在零页 `0x3C87` 上一条指令一次 #DB，最后报
  `Windows protection error`。已修（`exec.c` case 0x9d：先 peek，检查完再提交
  ESP/flags）。同脚本实测 traps 13621 → 374，保护错误消失，安全模式可进桌面；
  普通模式切到 1024×768 之后仍会蓝屏（显示驱动/VGA 侧，下一步）。见 progress.log 片 43。
- **xv6-riscv 未开始**：镜像与 OpenSBI fw_jump 素材未取；riscv 侧的真实 OS 验收
  目前由 HelenOS 承担。

CPU 面：x86 到 i686（x87 走树内 Berkeley SoftFloat 3e、MMX/SSE/SSE2、RDTSC(P)、
SYSENTER/SYSEXIT、MTRR/PAT、MCE 寄存器组、性能计数器、AC、PSE-36、FXSAVE/FXRSTOR）；
APIC 的 CPUID/leaf 0x0B 拓扑已广告，CR0.NE=0 的 FERR# 接到 8259 IRQ13；余下
VME/PVI 重定向与 PMU 细节记在 D39，port 0xF0/IGNNE# 记在 D19。riscv 侧
RV64IMAFDC + Sv39/Sv48 + PMP，RV32 是独立后端（RV32IMAFDC + Sv32）。

回归：**riscv64 136 格、riscv32 104 格、x86 59 格，全绿**（x86 = 51 个自足探针 +
7 个 kvm-unit-tests 镜像 + realmode 1 格，127 PASS/0 FAIL；realmode 偶见对 PIT
IRQ0 到达时机的敏感，见 progress.log）。依赖边 `dep: ok`。逐轮记录见
[progress.log](progress.log)。
x86 固件（SeaBIOS + 显卡 option ROM）来自库外的一棵源码树，其现状、重建步骤与配置
快照见库外 `../seabios/firmware.md`（不随本仓库跟踪）。

## 构建

依赖只有 clang 与 mingw-w64 sysroot；第三方代码只有树内 vendored 的 Berkeley
SoftFloat 3e（BSD-3，x87 用，`src/cpu/isa/x86/softfloat/`），不引入外部库：

```sh
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=tools/clang.cmake
cmake --build build
```

`tools/clang.cmake` 把编译器指向 `D:/LLVM/bin/clang.exe`、目标
`x86_64-w64-mingw32`；mingw-w64 的 sysroot 由 clang 依 PATH 上的
`x86_64-w64-mingw32-gcc` 定位。**换机器改这个文件里的两行即可**（ninja 不在
PATH 时前置 `PATH=/d/ninja:$PATH`）。要求 `-Wall -Wextra` 零告警。

## 使用

```
build/cemu.exe [options] image.bin|image.elf
```

不带参数运行会打印全部开关。四块主板按 QEMU 惯例用 `--machine` 选：

```sh
# spike 主板：riscv-tests，HTIF 退出（ELF 的 EI_CLASS 决定 32/64 位）
build/cemu.exe test/riscv64/rv64ui-p-add.elf
build/cemu.exe test/riscv32/rv32ui-p-add.elf

# virt 主板：QEMU virt 布局，OpenSBI 引导
build/cemu.exe --machine virt -bios fw_jump.bin

# PC 主板：固件 ROM 引导 + 硬盘 / 光驱
build/cemu.exe --machine x86 -bios bios.bin -hda xv6.img
build/cemu.exe --machine x86 -display win32 -cdrom linux.iso

# PC 主板 + 显示：显卡的 option ROM 提供 int10 视频服务
# （vgaroms/vgabios.bin 一般是 seabios 树按 CONFIG_NO_VGABIOS=0 构建的产物）
build/cemu.exe --machine x86 -display win32 -bios bios.bin \
  -vgabios vgabios.bin -fda win101.img -mouse ps2

# oberon 主板：Project Oberon（RV32）从 SD 卡启动，帧缓冲在窗口里
build/cemu.exe --machine oberon -dsk disk.img -display win32
```

`.bin` 需要 `--isa NAME`（ELF 自报 ISA）。常用：`--mem MB`、`--max-inst N`、
`--skip-idle`、`-icount shift=N`（每条指令记 1<<N ns 的客机时间，定时器边缘
因此只由指令流决定，复现性用）、`-kernel`/`-append`（virt）、`-hdb`、`-fda`、
`-dsk`、`--switches N`（oberon）、`-vgabios`、`-mouse ps2|serial`、
`-gdb tcp::PORT` / `-s` / `-S`（gdb RSP stub，配套图形前端 `cemugui`）。

窗口指针按**设备原生语义**投递（契约见 AGENTS.md §七）：virt 的 virtio-tablet
与 Oberon 鼠标寄存器是原生绝对设备，客户区坐标直接映射到源画面像素；x86 的
PS/2 与 `-mouse serial` 只有相对协议，投的是宿主指针的位移（漂移边界见 D38）。

聚焦窗口内首次移动即抓取指针并隐藏宿主光标；绝对设备同样抓取但不回中，坐标
映射不受影响。**Ctrl+Alt+G** 释放抓取、宿主光标回来，点击窗口重新抓取。
宿主侧的 Ctrl+Alt+Del 由 Windows 自己收走（winlogon 的 SAS），窗口收不到，
用 **Ctrl+Alt+End**（RDP 写法）或 **Ctrl+Alt+Insert**（VMware 写法）代发
Delete；这三条手势对所有主板一致。

## 测试

```sh
bash test/run.sh                            # 全部套件（分层：test/<isa>/run.sh）
cmake --build build --target check          # 目录依赖边（tools/depcheck.sh）
cmake --build build --target check-riscv64  # riscv64 套件
cmake --build build --target check-riscv32  # riscv32 套件
cmake --build build --target check-x86      # x86 套件
```

测试镜像已入库，不需要客户机工具链；x86 探针的 `.bin` 由 nasm 现生成
（没装 nasm 则对应那格 SKIP）。所有运行都限时（脚本内 `timeout` 包裹）。

## 调试

观测统一走常驻设施 `CEMU_DEBUG`（唯一调试入口，语法见 AGENTS.md §五）：

```sh
CEMU_DEBUG="trace:table,state,mem,budget=200" \
  build/cemu.exe --machine x86 --isa x86 test/x86/realmode/realmode.elf
```

## 文档

| 文件 | 内容 |
|---|---|
| [AGENTS.md](AGENTS.md) | 开发铁律（改代码前必读） |
| [arch.md](arch.md) | 架构与阶段路线图 |
| [progress.log](progress.log) | 开发日志：过程、证据与裁决（按时间倒序） |
| [reference.md](reference.md) | 参考项目索引与语义裁决顺序 |
