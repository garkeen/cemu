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

## 构建

依赖只有 clang 与 mingw-w64 sysroot；第三方代码只有树内 vendored 的 Berkeley
SoftFloat 3e（BSD-3，x86 x87/SSE 与 riscv F/D 共用，`src/cpu/softfloat/`），不引入外部库：

```sh
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=tools/clang.cmake
cmake --build build
```

`tools/clang.cmake` 把编译器指向 `D:/LLVM/bin/clang.exe`、目标
`x86_64-w64-mingw32`；mingw-w64 的 sysroot 由 clang 依 PATH 上的
`x86_64-w64-mingw32-gcc` 定位。**换机器改这个文件里的两行即可**（ninja 不在
PATH 时前置 `PATH=/d/ninja:$PATH`）。要求 `-Wall -Wextra` 零告警。

x86 固件（SeaBIOS + 显卡 option ROM）来自库外的一棵源码树，其重建步骤与配置
快照见库外 `../seabios/firmware.md`（不随本仓库跟踪）。

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
`-audio win32`（宿主 PCM 输出，默认关闭）、
`-gdb tcp::PORT` / `-s` / `-S`（gdb RSP stub，配套图形前端 `cemugui`）。

宿主声音默认关闭：不加 `-audio win32` 时 SB16 仍按采样率消费 DMA、照常发 IRQ，
只是从不打开 waveOut，所以探针与回归套件不会发出任何声音。

窗口指针按**设备原生语义**投递（契约见 AGENTS.md §七）：virt 的 virtio-tablet
与 Oberon 鼠标寄存器是原生绝对设备，客户区坐标直接映射到源画面像素；x86 的
PS/2 与 `-mouse serial` 只有相对协议，投的是宿主指针的位移，不做绝对定位。

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

GitHub Actions（`.github/workflows/ci.yml`）在每次 push/PR 跑构建、依赖边与
三个套件（riscv64 / riscv32 / x86）。xv6、SeaBIOS、Win95 这类真实镜像验收
不在 CI：它们依赖仓外的 `seabios/out` 与 `xv6-build` 产物，而且套件按
AGENTS.md §二 不引导固件/软盘/光盘镜像，只在开发机手跑。

## 调试

观测统一走常驻设施 `CEMU_DEBUG`（唯一调试入口，语法见 AGENTS.md §五）：

```sh
CEMU_DEBUG="trace:table,state,mem,budget=200" \
  build/cemu.exe --machine x86 --isa x86 test/x86/realmode/realmode.elf
```

阶段 3.5 的三片（CEMU_DEBUG 观测中枢、gdb RSP stub、cemugui）已完成；
gdb 断点/单步/硬件观察点走 `-gdb tcp::PORT` / `-s` / `-S`，图形前端是
`cemugui.exe`。x86 硬件观察点走 DR0-DR7，riscv32/64 走 trigger 模块的内部
mcontrol 槽位；x86 的 `rwatch`（Z3）因 DR7 没有只读编码而回空包，riscv
的写/读/访问三种 kind 都支持。

## 文档

| 文件 | 内容 |
|---|---|
| [AGENTS.md](AGENTS.md) | 开发铁律（改代码前必读） |
| [arch.md](arch.md) | 架构与阶段路线图 |
| [progress.log](progress.log) | 开发日志：过程、证据与裁决（按时间倒序） |
| [reference.md](reference.md) | 参考项目索引与语义裁决顺序 |
