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
**阶段 4（设备全集与真实 OS）进行中**：PC 设备模型全集到位——VGA 家族核含
VBE/DISPI、链式 256 色与 EDID，PS/2 键盘鼠标与串口鼠标，8259/8254，8272 FDC +
8237 DMA，IDE/ATAPI 含 LBA-48 与 multi-sector，LAPIC/IOAPIC 含 NMI 交付，PIIX4 PM
含 SCI 与 S5 关机；余下缺口按 AGENTS.md §X 的 D17/D19 在册。
验收阶梯：**xv6-x86 ✅**（SeaBIOS → IDE 盘 → shell）、**Linux ✅**（引导越过
`VFS: Mounted root (ext2 filesystem)`，控制台出提示符）、**Windows 1.01 ✅**（EGA
640×350 的 MS-DOS Executive 桌面，`fbdump` 帧与 QEMU 基线逐像素相同，PS/2 指针可
移动）、**xv6-riscv 未开始**（镜像与 OpenSBI fw_jump 素材未取）。
回归：x86 36/0、riscv64 136/0、riscv32 104/0、依赖边 ok。
逐轮记录见 [progress.md](progress.md)。
x86 固件（SeaBIOS + 显卡 option ROM）来自库外的一棵源码树，其现状、重建步骤与配置
快照见库外 `../seabios/firmware.md`（不随本仓库跟踪）。

## 构建

依赖只有 clang 与 mingw-w64 sysroot（无第三方库）：

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

不带参数运行会打印全部开关。三块主板按 QEMU 惯例用 `--machine` 选：

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
```

`.bin` 需要 `--isa NAME`（ELF 自报 ISA）。常用：`--mem MB`、`--max-inst N`、
`--skip-idle`、`-hdb`、`-fda`、`-vgabios`、`-mouse ps2|serial`、`-gdb tcp::PORT`
/ `-s` / `-S`（gdb RSP stub，配套图形前端 `cemugui`）。

窗口鼠标统一按绝对位置映射：指针在画面内直接对应客人画面的像素位置，画面外的黑边
钳到最近边缘，离开窗口停发；缩放窗口时画面矩形动态重算。virt/Oberon 是原生绝对设备，
x86 的 PS/2 由板级换算（相对协议的近似与漂移边界见 AGENTS.md D38）。

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

观测统一走常驻设施 `CEMU_DEBUG`（唯一调试入口，语法见 AGENTS.md 第十节）：

```sh
CEMU_DEBUG="trace:table,state,mem,budget=200" \
  build/cemu.exe --machine x86 --isa x86 test/x86/realmode/realmode.elf
```

## 文档

| 文件 | 内容 |
|---|---|
| [AGENTS.md](AGENTS.md) | 开发铁律（改代码前必读） |
| [arch.md](arch.md) | 架构与阶段路线图 |
| [progress.md](progress.md) | 现状与决策记录（最近的在最上） |
| [reference.md](reference.md) | 参考项目索引与语义裁决顺序 |
