# cemu 参考资料索引（reference.md）

调研全文在 TinyEMU/项目调研.md（各项目逐个分析：执行模型、译码结构、
覆盖面、POSIX 依赖、许可证、参考建议）。本文是**工作索引**：修 bug、
写设备、加指令时按此表直达文件，并守住两条纪律——

1. **参考优先，防止造轮子**（AGENTS.md 第五节）：动手前先查下表对应
   实现；cemu 现有设备（uart16550/clint/plic/i8259/i8254/htif）当初都
   是照参考逐条校准的，先看 cemu 已有什么。
2. **参考优先，防止无意义调试**：模拟器 bug 的真相往往在参考实现里
   已有答案（先例：DAS 的 SDM/实机 18 处分歧按 QEMU/v86 真值表修复）。
   定位不了时先读参考同位置代码，再回 CEMU_DEBUG 验证假设。

## 一、参考代码树（D:/code/c/TinyEMU/，cemu 的邻居）

| 项目 | 位置 | 拿什么 | 许可 |
|---|---|---|---|
| nemu | nemu/ | 循环骨架、IOMap 回调思想；x86 实模式第二参照 src/isa/x86/inst.c（riscv64/ 是 riscv32/ 的符号链接，Windows checkout 上注意） | Mulan PSL v2 |
| xiangshanNEMU | xiangshanNEMU/ | riscv 语义 oracle：src/isa/riscv64/instr/{rvi,rvm,rva,rvf,rvd,rvc,priv}/exec.h 逐条 def_EHelper；system/{priv,timer,trigger,mmu}.c | Mulan PSL v2 |
| tiny386 | tiny386/ | x86 386 主语义 oracle：i386.c（5250 行）+ i386ins.def；PC 外设集 i8259.c/i8254.c/i8042.c/vga.c/ide.c | **All rights reserved，只能读不能抄代码** |
| dearchap-tinyemu | dearchap-tinyemu/ | riscv_machine.c（CLINT/UART/内存树）、iomem.c、riscv_cpu_template.h（单文件 riscv 解释模板）；**x86_cpu.c 是 96 行残桩，勿用** | MIT |
| byang1217-TQemu | byang1217-TQemu/ | 可移植哲学（OS 无关、单 mainloop、libc/libm only）；本 checkout 无 target 后端不可跑 | GPL-2 |
| gem5 | gem5/ | 行为裁决第三票；ARM 阶段的行为规范 src/arch/arm/；riscv 侧 src/arch/riscv/ | BSD-3 |
| v86 | v86/ | x86 设备/启动行为；tests/kvm-unit-tests（realmode 验收套件已入库 test/x86/realmode）、tests/nasm、tests/qemu 双跑方法论、tests/full（真实 OS 清单，阶段 4 用） | MIT |
| abstract-machine | abstract-machine/ | 阶段 3 cesdk 蓝本：am/src/platform/nemu/ 的 trm.c、include/nemu.h 设备图、scripts/linker.ld | Mulan PSL v2 |
| opensbi | opensbi/ | 阶段 2 验收固件源码（fw_dynamic.h 等结构定义） | BSD-2 |
| riscv-tests / riscv-test-env | riscv-tests/ riscv-test-env/ | riscv 验收套件；env/p/riscv_test.h 是 tohost/fromhost 退出契约 | MIT/BSD 系 |
| seabios | seabios/ | 阶段 4 SeaBIOS 路线：bios.bin 已从源码构建成功（256KiB，复位向量逐字节验证） | LGPL 等 |

QEMU 源码不在树中：QEMU 行为经 D:/qemu 的二进制实测校准
（dumpdtb/pmemsave/-device help/双跑对拍），cemu 的 virt 机器即由此而来。

## 二、语义裁决顺序（分歧时从上往下）

1. 官方手册：RISC-V 非特权/特权手册；x86 用 Intel SDM。
2. 官方测试期望值（riscv-tests、kvm-unit-tests）。
3. QEMU 实测行为（D:/qemu 二进制；QEMU 与规格分歧且有测试锚定时**不
   模仿 QEMU**，先例：计数器抑制）。
4. gem5 / v86（交叉验证票；SDM 与实机分歧时按实机，先例：DAS）。

## 三、可用工具（约束：除此之外不得依赖）

| 工具 | 位置 | 用途 |
|---|---|---|
| clang / lld / llvm-objcopy / llvm-objdump | D:/LLVM/bin（LLVM 23.1） | seabios/x86 多目标交叉构建、ELF 检视（比 GNU objdump 格式友好） |
| gcc / mingw32-make / objcopy / objdump | D:/mingw64/bin（注意：cemu 本体构建用 D:/mingw64/bin/gcc.exe，路径中无 8.1 版本号，与旧文档"mingw64 gcc 8.1"的描述以实际为准） | 编 cemu 本体（mingw32-make），平二进制 objcopy |
| ninja | D:/ninja/ninja.exe | seabios 等已带 build.ninja 的构建 |
| qemu-system-i386 / riscv64 / riscv32 / arm | D:/qemu | 双跑对拍、机器契约实测（dumpdtb、pmemsave、-device help） |
| nasm | D:/nasm/nasm.exe（2.16.03 亦在 D:/SSDOWN/tools/nasm-2.16.03/） | 16/32 位 x86 测试镜像（-f bin 引导扇区） |

- QEMU 是裁决仪器不是依赖：对拍用 `-device isa-debug-exit,iobase=0xf4,iosize=0x4`，
  exit status = (value<<1)|1（实测校准，非 value+1）。
- riscv 交叉 gcc（xpack）不在当前工具清单内：阶段 1 时曾用，现状可用
  LLVM/clang 按目标三元组补位，缺什么再登记。

## 四、cemu 内已有资产的对照表（别重造）

| 机制 | cemu 已有 | 当初照谁写的 |
|---|---|---|
| riscv 指令语义 | cpu/isa/riscv64/exec.c（巨型 switch 直执） | xiangshanNEMU def_EHelper 逐条翻译 |
| riscv CSR/特权 | cpu/isa/riscv64/csr.c + mmu.c（PMP/Sv39） | xiangshanNEMU system/*.c |
| riscv 步进协议 | cpu/isa/riscv64/step.c（wfi + 取指 + 单点提交 + 计数器） | priv spec + spike 单核语义 |
| x86 指令语义 | cpu/isa/x86/exec.c（modrm/SIB→opcode 序直执） | tiny386 i386.c + SDM |
| x86 步进协议 | cpu/isa/x86/step.c（INTR 采样 + 前缀消费 + 单点提交） | SDM 6.x/20.x + QEMU 行为 |
| HTIF 退出 | device/misc/htif.c（单通道 + fromhost 应答） | fesvr/dearchap riscv_machine.c |
| virt 主板布局 | board/virt.c + virt_dtb.h | QEMU hw/riscv/virt.c + dumpdtb/pmemsave 实测 |
| uart16550 | device/char/uart16550.c | dearchap SerialState 语义（TX-only） |
| CLINT / PLIC | device/timer/clint.c / device/intc/plic.c | dearchap + DTB |
| 8259 PIC / 8254 PIT | device/intc/i8259.c / device/timer/i8254.c | QEMU i8259/i8254 语义 |
| spike 主板 | board/spike_min.c | spike 契约（tohost 从 ELF 符号表） |
| 引导扇区契约 | board/x86_min.c（0x7C00、DL=0x80） | QEMU seabios 交接契约实测 |
| 目录依赖检查 | tools/depcheck.sh（`mingw32-make check`） | 自订；规则见 AGENTS.md 第七节 |

新设备/新机器动手前，先按"当初照谁写的"列找到参考原型读一遍。
