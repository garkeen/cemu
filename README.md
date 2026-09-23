# cemu

纯 C、纯 Win32 API 的解释型全系统模拟器。多 ISA（riscv64 先行，x86 已接入，
之后 arm / mips），加载 `.bin` / `.elf` / `.img`，按真实机器契约运行真实软件。

## 目标

- **不做翻译、不做中间表示**：巨型 switch 直接解释一条指令（取指→译码→执行
  在同一 C 作用域内），代码对照手册逐行可读，pc 是唯一提交点。
- **通用性靠结构**：core 不认 CPU 型号，设备不感知 ISA，机器定义与 ISA 正交；
  运行时按 ELF 的 `e_machine` 选 ISA，一个 exe 内所有 ISA 同时在场。
- **阶梯上全是真实软件**：riscv-tests → OpenSBI → xv6 → Linux，无 cemu 定制成分。
- cesdk（AM 式构建系统）排在真实 OS 跑通之后，其 API 才有设计依据。

## 现状

阶段 0–3.5 完成（ISA 一致性、真实固件、x86 保护模式与分页、调试器与 GUI）；
**阶段 4（设备全集与真实 OS）进行中**：验收 1 xv6-x86 已到 shell；验收 2 Linux
引导链已到 `VFS: Mounted root (ext2 filesystem)`，卡在其后一段（未定案）；
验收 3 xv6-riscv 未开始。逐轮记录见 [progress.md](progress.md)。

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
# spike 主板：riscv-tests，HTIF 退出
build/cemu.exe test/riscv64/rv64ui-p-add.elf

# virt 主板：QEMU virt 布局，OpenSBI 引导
build/cemu.exe --machine virt -bios fw_jump.bin

# PC 主板：固件 ROM 引导 + 硬盘 / 光驱
build/cemu.exe --machine x86 -bios bios.bin -hda xv6.img
build/cemu.exe --machine x86 -display win32 -cdrom linux.iso
```

`.bin` 需要 `--isa NAME`（ELF 自报 ISA）。常用：`--mem MB`、`--max-inst N`、
`--skip-idle`、`-hdb`、`-gdb tcp::PORT` / `-s` / `-S`（gdb RSP stub，配套图形
前端 `cemugui`）。

## 测试

```sh
bash test/run.sh                            # 全部套件（分层：test/<isa>/run.sh）
cmake --build build --target check          # 目录依赖边（tools/depcheck.sh）
cmake --build build --target check-riscv64  # riscv64 套件
cmake --build build --target check-x86      # x86 套件
```

测试镜像已入库，不需要客户机工具链；x86 的三个探针 `.bin` 由 nasm 现生成
（没装 nasm 则这三格 SKIP）。所有运行都限时（脚本内 `timeout` 包裹）。

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
