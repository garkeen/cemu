# cemu 架构与路线图（arch.md）

本文是原 任务与计划.md 的架构与计划部分。当前状态与债务见 progress.md；
开发铁律见 AGENTS.md。

## 一、目标

1. **cemu**：纯 C、纯 Win32 API 的解释型全系统模拟器。多 ISA：riscv64 先行，
   之后 x86 早期、arm 早期、mips32。加载 .bin/.elf/.img，模拟真实机器契约，
   运行真实软件。
2. **cesdk**：AM 式构建系统，C 源码 → 自含 .bin/.elf/.img，产物在真 QEMU 上
   可运行。
3. 顺序：cemu 先行，cesdk 第三阶段。理由：cemu 的测试阶梯不依赖 cesdk；
   cesdk 只能靠在 cemu 上跑输出来测；cesdk 的 API 要等真实软件跑过才有
   设计依据。

## 二、通用性靠结构

- core 不 include 任何 isa 的头文件；windows.h 只允许出现在 host/；
  设备不感知 CPU 型号；机器定义与 ISA 正交。
- 不造协议：机器按 spike 与 QEMU virt 的真实契约；退出走 HTIF 或
  sifive_test。
- 运行时选 ISA：ELF 读 e_machine 自动分派（isa/registry.c 的 kIsaTable，
  NULL 结尾注册表，未来 ISA 在此追加），.bin 由命令行指定。一个 exe 内
  所有 ISA 同时在场。x86 接入日即通用性审计日。
- 中断注入范式：机器把 int_ack/timer_read/irq sink 等钩子装在 CpuState 上，
  加载器之后才选 ISA——接线先于 ISA 存在。

## 三、目录结构

目录按被模拟机器的部件划分；各目录之间允许/禁止的 include 边由
`mingw32-make check` 机械检查，规则表见 AGENTS.md 第七节。

```
cemu/
  AGENTS.md        开发铁律（agent 必读）
  arch.md          本文档
  progress.md      进度与决策记录
  reference.md     参考项目与用法
  tools/           depcheck.sh  依赖边检查
  src/
    main.c         入口：参数解析 + 装配
    cpu/           处理器
      cpu.h        CpuState：寄存器 bank + 提交点 pc + 主板钩子
      step.h/.c    步进契约：frame / insn_rec / isa_ops / raise_
      isa/         各 ISA 解释器
        isa.h        ISA 侧汇聚头
        registry.c   k_isa_table：有哪些型号
        riscv64/     riscv.h  exec.h  platform.h  exec.c  step.c
                     csr.c  mmu.c  fp.c  isa_riscv64.c
        x86/         x86.h  exec.h  exec.c  step.c  isa_x86.c
    bus/           bus.h/.c     地址空间与译码
    mem/           ram.h/.c     内存
    device/        外设，按类分
      char/         uart16550
      timer/        i8254  clint
      intc/         i8259  plic
      misc/         htif  sifive_test  debug_exit
    board/         主板：设备布局 + 复位态 + 接线 + 装载 + 运行循环
      board.h/.c    Board 契约 + 工厂
      run.c         运行循环 + 销毁
      loader.h/.c   装载（按 e_machine 选 ISA）
      elf.h/.c      ELF 解析
      spike_min.c  virt.c(+virt_dtb.h)  x86_min.c
    debug/         debug.h/.c   CEMU_DEBUG 观测中枢
    host/          console_win.c  file_win.c  time_win.c
    util/          log.c  table.c  type.h
  test/            run.sh  riscv64/  x86/
```

CpuState（cpu.h）对所有模块可见；CPU 型号的私有态只存在于 `cpu/isa/<name>/`
内部。主板通过 CpuState 上的钩子（`timer_read`、`int_ack`、`set_irq`）接线，
接线发生在装载器选出 ISA 之前。

## 四、V3：巨型 switch 直接解释器（统一执行抽象）

V2 曾尝试 μIR 方案（解码产出建筑级微操作、共享执行器、表驱动分发），经三轮设计
后被用户否定：微操作把"一条指令"拆成多片，层间粘合累计 21 个失败，且可读性
未达标。当前 V3 用**巨型 switch 直接解释器**取代 μIR（旧 `src/ir/` 已删除，旧各
ISA 一份的 `execute.c` 大循环重写为 `exec.c`），遵循"代码简单清晰 + 小步操作
语义"两条根本目的。架构铁律见 AGENTS.md 第八节。

- **无中间表示**：主板与解释器之间只有 `src/cpu/step.h` 一个接缝——
  `frame`（每步世界：cpu/priv/isa/raise 跳板/rec/trap）、`insn_rec`（观测面：
  pc/dnpc/raw/mnemonic）、`isa_ops`（注册表：init/step/dump_regs + 调试钩子）。
  `raise_()` 走 setjmp/longjmp 做故障原子性；**唯一提交点**是 `cpu->pc`
  （step 尾部单点提交）。
- **数据结构**：`CpuState`（`cpu/cpu.h`：共享 gpr[32]/fpr[32] bank + pc + 主板
  钩子 timer_read/int_ack/set_irq）+ ISA 私有态挂 `priv`（`RiscvState` /
  `x86_state`）。指令执行 = 状态转移。
- **x86 寄存器模型**：union cell 做 EAX/AX/AL/AH 分层别名；EFLAGS 位域左值
  （`cf = 1`）；段基址走描述符缓存（`base[6]`），实模式及 CR0.PE=0 后缓存存续
  即 big real mode。
- **巨型 switch（指令集）**：`cpu/isa/riscv64/exec.c`（opcode→funct3→funct7
  嵌套 switch，含 compressed 象限展开）、`cpu/isa/x86/exec.c`（modrm/SIB→按
  opcode 序直执）。宽度写进名字（rm8/rd32/push32），无宽度形参；C 运算符做算术，
  helper 只补 C 没有的（标志/栈/段/MMU/FP/陷阱）。
- **步进协议**：各 ISA 的 `step.c` —— 中断投递 + 取指（x86 还有前缀消费）+
  setjmp/raise 落地 + 单点提交 + 计数器。与 `exec.c` 共享的每步视图放在私有头
  `exec.h`（必须最后 include：它定义 `x`/`f`/`eax` 这类短宏）。
- **共享语义后端**：不进 switch 的语义抽成函数——riscv 的 `csr.c`（CSR/特权/
  陷阱/xRET）、`mmu.c`（Sv39 + PMP）、`fp.c`（IEEE-754，宿主浮点模拟，见简化
  登记 E1）；x86 的 `exec.c` 内 helper（do_int/IVT/串操作）。
- **中断注入**：主板只调 `CpuState.set_irq`（型号在自己的 init 里安装），不调
  型号函数。因此 `spike_min.c`、`x86_min.c` 完全不含 ISA 头；只有平台板
  `virt.c` 引 `cpu/isa/riscv64/platform.h` 取中断线号常量。
- **Raise 通道**：raise_ → setjmp/longjmp 落地 → trap 消费方设 dnpc → 单点提交。
  RMW 的 store 先于 flags 落地，故障时指令整体回滚。
- **规范出处注释**：csrrw 恒写、RV64 shamt 6 位、jalr rd==rs1 先读后写、
  SDM 各组编码位、popf/iret 的 RF|VM 屏蔽等，全部注来源。

## 五、三块主板

主板只是一张设备表加复位状态：三块主板覆盖三种接线（spike/virt 共用 riscv64，
x86_min 装第三种 ISA）。类型是 `Board`，CLI 仍是 `--machine`（QEMU 惯例）。

| 主板 | 布局 | 用途 |
|---|---|---|
| spike_min | RAM 0x80000000 + HTIF | 阶段 1 跑 riscv-tests。tohost 从 ELF 符号表读，spike 同款约定 |
| virt | DRAM 0x80000000、mask ROM 0x1000（QEMU 复位向量逐字）、CLINT 0x02000000、PLIC 0x0c000000、UART16550 0x10000000、test 0x100000 | 阶段 2 起 OpenSBI。权威来源 QEMU hw/riscv/virt.c + dumpdtb/pmemsave 实测 |
| x86_min | 1MB RAM@0、PIC 0x20/0xA0、PIT 0x40、COM1 0x3F8、debug-exit 0xF4 | 阶段 1.5 起的 x86 平台。引导扇区契约：加载 0x7C00、CS:IP=0000:7C00、DL=0x80 |

对照真机：riscv `qemu-system-riscv64 -M spike -bios none -kernel rv64ui-p-add.elf`；
x86 `qemu-system-i386 -device isa-debug-exit,iobase=0xf4,iosize=0x4`。

## 六、阶段计划

### 阶段 0：工具链与素材 ✅
xpack riscv gcc（guest riscv）、nasm（D:/nasm，2.16.03 在 D:/SSDOWN/tools）、
LLVM 23.1（D:/LLVM，seabios 与 x86 构建用）、mingw gcc 8.1（cemu 本体）、
qemu-system-i386（D:/qemu）。seabios bios.bin 已从源码构建成功（256KiB，
复位向量逐字节验证，为阶段 4 SeaBIOS 路线备料）。

### 阶段 1：ISA 一致性 ✅
RV64IMAFDC + Zicsr + Zifencei + M 态 CSR。riscv-tests 127 项全绿曾达成。
已知坑已裁决：F 寄存器 NaN boxing、mcause/mtval 精确值、计数器抑制
（见 AGENTS.md 附录）。

### 阶段 1.5：x86 最小证明 ✅
实模式子集 + kIsaX86（EM_386=3）进 kIsaTable；core/device/host 零改动
编译通过 = 结构证明。含 PM 平段入口（multiboot 契约：EAX=0x2BADB002、
内置平 GDT、CR0.PE）、段描述符缓存（翻译恒用缓存 base——PE=0 后缓存存续
即 big real mode）、open-bus 内存语义、串操作 + REP、IVT 异常分发。
realmode（kvm-unit-tests 官方实模式套件）曾达 122 PASS/0 FAIL。

### 阶段 2：真实固件 ✅
- riscv 侧：virt 机器 + CLINT/PLIC/sifive_test + OpenSBI fw_jump 引导。
- x86 侧：PC 平台（i8259 + i8254，hlt 由 IRQ0 唤醒），realmode 全绿过。
- M 态完整化：misa/medeleg/mideleg/PMP/Sv39（mmu.c）全落地。

### 阶段 3：cesdk
- 交付：crun 运行时（_start、putch→UART、halt→sifive_test）、klib、
  链接脚本、构建前端（先批处理形态）。
- 验收：hello 的 .bin 与 .elf 在 cemu 与真 QEMU 上行为一致。
- 蓝本：AM 源码逐文件对照移植。D11（HTIF syscall）在此销账。

### 阶段 4：x86 全集
- 32 位保护模式（GDT/描述符/特权级/门）、386 分页、PC 设备模型全集
  （CGA、PS/2、PIC、PIT、UART、IDE、LAPIC、IOAPIC）。
- bin 路线：multiboot 等价的入口契约，xv6 去掉 bootasm.S/bootmain.c
  编为 .bin；SeaBIOS 路线：官方 bios.bin 映射内存顶端，复位 F000:FFF0
  （bios.bin 已备料）。
- 验收阶梯：xv6-x86 → Linux（参照 v86/tests/full 清单）。
- D13（FPU/全集指令）、D14（VM86）在此销账。

### 阶段 5：arm 与 mips
先引 LLVM 做交叉（本地无解释型参考，语义来源为官方手册）。

## 七、复用映射

| 来源 | 拿什么 | 备注 |
|---|---|---|
| nemu | cpu-exec 循环骨架、IOMap 回调思想 | 框架可抄，AM 的设备地址表丢弃 |
| nemu/src/isa/x86 | x86 实模式语义第二参照 | riscv64 是空壳符号链接，实文件在 riscv32 目录 |
| xiangshanNEMU | riscv 语义 oracle（priv.c/timer.c/trigger.c/mmu.c） | 逐条翻译 |
| tiny386/i386.c + i386ins.def | x86 386 主语义 oracle | 纯 C，5250 行 |
| v86/tests/kvm-unit-tests | x86 验收套件 | in-tree，386 级子集已入库 |
| dearchap-tinyemu | 映射树结构、iomem is_ram、uart16550/clint/plic、htif 应答语义 | MIT 许可；x86_cpu.c 是 96 行残桩，勿用 |
| abstract-machine | trm 模式、linker.ld、klib | 阶段 3 cesdk 的蓝本 |

## 八、测试阶梯

0. 冒烟：三条命令编五条指令的 .bin，HTIF 退出。
1. riscv-tests isa 套件全绿。
1.5. x86_min：0x7C00 bin 双跑对拍（cemu vs qemu-system-i386）→
    kvm-unit-tests 386 级子集全绿。
2. OpenSBI 起动出 banner。
3. Linux 引导、cesdk hello。

每一级都是真实软件，无 cemu 定制成分。
