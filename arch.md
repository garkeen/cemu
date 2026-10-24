# cemu 架构与路线图（arch.md）

本文是原 任务与计划.md 的架构与计划部分。当前状态与债务见 progress.md；
开发铁律见 AGENTS.md。

## 一、目标

1. **cemu**：纯 C、纯 Win32 API 的解释型全系统模拟器。多 ISA：riscv64 先行、
   riscv32 已接入，之后 x86 早期、arm 早期、mips32。加载 .bin/.elf/.img，模拟真实机器契约，
   运行真实软件。
2. **cesdk**：AM 式构建系统，C 源码 → 自含 .bin/.elf/.img，产物在真 QEMU 上
   可运行。
3. 顺序：cemu 先行；x86 保护模式与分页是最后一大块
   指令集语义，提前收掉，其后进入纯设备/IO 与真实 OS（xv6 → Linux）；
   cesdk 放后——真实软件跑通之后其 API 才有设计依据，cemu 的测试阶梯
   也不依赖 cesdk。

## 二、通用性靠结构

- core 不 include 任何 isa 的头文件；windows.h 只允许出现在 host/；
  设备不感知 CPU 型号；机器定义与 ISA 正交。
- 不造协议：机器按 spike 与 QEMU virt 的真实契约；退出走 HTIF 或
  sifive_test。
- 运行时选 ISA：ELF 读 e_machine + EI_CLASS 自动分派（isa/registry.c 的 kIsaTable，
  NULL 结尾注册表，未来 ISA 在此追加；riscv32/riscv64 共用 e_machine=243，
  由 isa_ops.elf_class 区分），.bin 由命令行指定。一个 exe 内
  所有 ISA 同时在场。x86 接入日即通用性审计日。
- 中断注入范式：机器把 int_ack/timer_read/irq sink 等钩子装在 CpuState 上，
  加载器之后才选 ISA——接线先于 ISA 存在。

## 三、目录结构

目录按被模拟机器的部件划分；各目录之间允许/禁止的 include 边由
`cmake --build build --target check` 机械检查，规则表见 AGENTS.md 第七节。

```
cemu/
  AGENTS.md        开发铁律（agent 必读）
  arch.md          本文档
  progress.md      进度与决策记录
  reference.md     参考项目与用法
  tools/           depcheck.sh  依赖边检查
                   clang.cmake  clang/mingw-w64 工具链文件（AGENTS.md 第七节）
  tools/front/     cemugui：gdb RSP 图形调试前端（宿主侧工具，非机器部件）
                   front.c  rsp.h/.c
  src/
    main.c         入口：参数解析 + 装配
    cpu/           处理器
      cpu.h        CpuState：寄存器 bank + 提交点 pc + 主板钩子
      step.h/.c    步进契约：frame / insn_rec / isa_ops / raise_
      isa/         各 ISA 解释器
        isa.h        ISA 侧汇聚头
        registry.c   k_isa_table：有哪些型号
        riscv64/     riscv.h  exec.h  platform.h  exec.c  step.c
                     csr.c  mmu.c  fp.c  trigger.c  isa_riscv64.c
        riscv32/     同形状的 RV32 后端：riscv32.h  exec.h  platform.h  exec.c  step.c
                     csr.c  mmu.c(Sv32)  fp.c  trigger.c  isa_riscv32.c
        x86/         x86.h  exec.h  exec.c  step.c  isa_x86.c
    bus/           bus.h/.c     地址空间与译码
    mem/           ram.h/.c     内存
    device/        外设，按类分
      char/         uart16550  oberon_pclink（Oberon 机的 PCLink1 文件通道）
      timer/        i8254  clint
      intc/         i8259  lapic  ioapic  plic
      input/        i8042（PS/2 键盘控制器） ps2kbd  ps2mouse  msmouse
      storage/      ide（PIIX ATA/ATAPI） fdc  oberon_sd（Oberon 机的 SD 卡）
      video/        vga（VGA 家族核，CGA/EGA 是其兼容模式；+ display.h 显示后端
                    契约） ramfb（RAM 内单色帧缓冲视图）
      misc/         htif  sifive_test  debug_exit  i440fx  piix3  pci
                    cmos  port92  fwcfg  debugcon  oberon_io（Oberon 机外设块）
                    oberon_clipboard（Oberon 机的宿主剪贴板通道）
      board/         主板：设备布局 + 复位态 + 接线 + 装载 + 运行循环
      board.h/.c    Board 契约 + 工厂
      run.c         运行循环 + 销毁
      loader.h/.c   装载（按 e_machine + EI_CLASS 选 ISA）
      elf.h/.c      ELF 解析
      spike_min.c  virt.c(+virt_dtb.h)  x86_min.c  oberon.c(+oberon_rom.h)
    debug/         debug.h/.c  CEMU_DEBUG 观测中枢；gdbstub.h/.c  RSP 服务端
    host/          host.h  console_win.c  file_win.c  time_win.c
                   display_win.c  input_win.c  sock_win.c
    util/          log.h/.c  table.h/.c  type.h
  test/            run.sh  riscv64/  riscv32/  x86/
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
- **共享语义后端**：不进 switch 的语义抽成函数——riscv64 的 `csr.c`（CSR/特权/
  陷阱/xRET）、`mmu.c`（Sv39 + PMP）、`fp.c`（IEEE-754，宿主浮点模拟，见简化
  登记 E1）；riscv32 目录是同形状的 RV32 副本（Sv32、RV32 CSR 布局、32 位本地
  寄存器 bank）；x86 的 `exec.c` 内 helper（do_int/IVT/串操作）。
- **中断注入**：主板只调 `CpuState.set_irq`（型号在自己的 init 里安装），不调
  型号函数。因此 `spike_min.c`、`x86_min.c` 完全不含 ISA 头；只有平台板
  `virt.c` 引 `cpu/isa/riscv64/platform.h` 取中断线号常量。
- **Raise 通道**：raise_ → setjmp/longjmp 落地 → trap 消费方设 dnpc → 单点提交。
  RMW 的 store 先于 flags 落地，故障时指令整体回滚。
- **规范出处注释**：csrrw 恒写、RV64 shamt 6 位、jalr rd==rs1 先读后写、
  SDM 各组编码位、popf/iret 的 RF|VM 屏蔽等，全部注来源。

## 五、三块主板

主板只是一张设备表加复位状态：spike 同时跑 riscv64 与 riscv32，virt 的复位
ROM 是 RV64（riscv32 的 virt32 尚未建），x86_min 装第三种 ISA。类型是 `Board`，
CLI 仍是 `--machine`（QEMU 惯例）。

| 主板 | 布局 | 用途 |
|---|---|---|
| spike_min | RAM 0x80000000 + HTIF | 阶段 1 起跑 riscv-tests（riscv64 与 riscv32 共用）。tohost 从 ELF 符号表读，spike 同款约定 |
| virt | DRAM 0x80000000、mask ROM 0x1000（QEMU RV64 复位向量逐字）、CLINT 0x02000000、PLIC 0x0c000000、UART16550 0x10000000、test 0x100000 | 阶段 2 起 OpenSBI（RV64）。权威来源 QEMU hw/riscv/virt.c + dumpdtb/pmemsave 实测；riscv32 需要单独的 virt32 复位 ROM/DTB |
| oberon | RAM 1MB@0（定死）、帧缓冲窗 0xE7F00（1024×768 1bpp，RAM 内，ramfb 视图）、ROM 0xFFFFF800（bootloader 512 字逐字，来自 oberon-riscv-emu）、IO 块 0xFFFFFFC0（ms 计时器/LED/开关 `--switches`/SPI-SD/PCLink 串口/PS/2 set-2 FIFO/绝对鼠标字/宿主剪贴板/插桩通道），复位 0xFFFFF800，`-dsk` 挂 SD 卡镜像 | Project Oberon（RV 移植）按 oberon-riscv-emu 逐契约复刻；CPU 即现成 riscv32 后端。验收 = 与参考模拟器对拍（fbdump PPM vs 截屏、key=@N vs 手按），镜像引导不进测试套件（用户裁决） |
| x86_min | 32MB RAM@0（`--mem` 可调，上限 0xFEE00000）、PIC 0x20/0xA0、PIT 0x40、COM1 0x3F8→IRQ4、debug-exit 0xF4、debugcon 0x402、fw_cfg 0x510、i8042 0x60/0x64、port92 0x92、CMOS 0x70/0x71、LAPIC 0xFEE00000、IOAPIC 0xFEC00000、CGA 0xB8000+0x3D0 → VGA 家族核 0xA0000-0xBFFFF+0x3B0-0x3DF（option ROM 走 `-vgabios`）、i440FX+PIIX3+IDE（`--hda/--hdb/--cdrom`）、ROM 窗口 0xC0000-0xFFFFF（`-bios`，复位 F000:FFF0） | 阶段 1.5 起的 x86 平台；PC 芯片组在阶段 4 片 1–14 补齐。引导扇区契约：加载 0x7C00、CS:IP=0000:7C00、DL=0x80 |

对照真机：riscv `qemu-system-riscv64 -M spike -bios none -kernel rv64ui-p-add.elf`；
x86 `qemu-system-i386 -device isa-debug-exit,iobase=0xf4,iosize=0x4`。

## 六、阶段计划

### 阶段 0：工具链与素材 ✅
xpack riscv gcc（guest riscv）、nasm（D:/nasm，2.16.03 在 D:/SSDOWN/tools）、
LLVM 23.1（D:/LLVM，cemu 本体与 seabios、x86 构建都用它：2026-09-13 起编译器是 clang，
`tools/clang.cmake`，mingw-w64 只当头文件与运行时 sysroot）、
qemu-system-i386（D:/qemu）。seabios bios.bin 已从源码构建成功（256KiB，
复位向量逐字节验证，为阶段 4 SeaBIOS 路线备料）——**在库外**，路径
`D:\code\c\EMU\seabios\out\bios.bin`（seabios 是与 cemu 平级的参考/素材目录，跑
`-bios` 时指过去即可；x86 的软盘/光盘引导都靠它的 INT 13h/10h/16h）。

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

### 阶段 3：x86 保护模式与分页 ✅（原阶段 4 的语义核心）
最后一大块指令集语义，收掉之后进入纯设备/IO 阶段。固件已备：x86 SeaBIOS
（bios.bin 阶段 0 已构建）、riscv OpenSBI（fw_jump 阶段 2 已引导）。
- 32 位保护模式：GDT/LDT/IDT 装载、描述符缓存、特权级检查
  （CPL/RPL/DPL）、门（call/int/ret/iret、经 TSS SS0 的栈切换）、PM 异常
  error code 语义。
- 386 分页：两级页走（形状参照 riscv 侧 mmu.c）、CR0.PG/CR3、page fault
  error code、A/D 位。
- 指令增量（tiny386 主 oracle，按需销账 D13 指令侧）：LGDT/LIDT/SGDT/
  SIDT、LLDT/LTR/STR、ARPL、LAR/LSL、VERR/VERW、MOV CR/DR、INVLPG、
  LMSW/SMSW/CLTS、CMPXCHG/CMPXCHG8B 等。
- 验收：kvm-unit-tests 32 位保护模式用例 + multiboot 平段内核开分页冒烟。
- riscv 侧无新增语义（S 态/Sv39 阶段 2 已落地）。

### 阶段 3.5：调试器与 GUI ✅（x86 硬件观察点已接；riscv 侧与"只读"编码按 D6 在册；VGA 图形模式与 ramfb 按 D17 挪入阶段 4）
三片独立可交付，顺序可调；显示通道是阶段 4 图形 OS 的硬前置。
- 前置任务：exec.c 逐 case 填 insn_rec.mnemonic（名字级，SDM/手册指令名；
  语法级不做）。CEMU_DEBUG 事件表的 MNEMONIC 列在 case 入口即有值，
  故障指令同样可读；state 基线不受影响。
- 显示通道：host/ 加 Win32 GDI 窗口与显示后端，device/ 加显示设备。
  x86 CGA 文本模式先行（realmode 即可点亮验收），VGA 图形模式、
  riscv ramfb 随后——阶段 4 设备清单中的 CGA 在此提前消化。
- 调试器内核：gdb 远程串行协议（RSP）over TCP（host/ 加 winsock），
  断点/单步/读写现场复用 raise 通道与 CEMU_DEBUG 基建。软件断点不依赖
  D6；硬件断点已接：`hbreak`（Z1）与普通断点共用**按线性 PC 匹配**的断点表，
  不往客机内存 patch 0xCC —— 为此 `isa_ops` 加了 `gdb_pc` 接缝（x86 交
  CS base + EIP，riscv 留 NULL），否则实模式给 gdb 的是段内偏移，`x/i $pc`
  反汇编取错字节、按 gdb 说得出的地址下的断点永不触发。
  **`watch`/`awatch`（Z2/Z4）已接**：走 `isa_ops` 的三个新接缝
  （`gdb_set_watch`/`gdb_clear_watch`/`gdb_watch_hits`）落到 CPU 自己的 DR0-DR7，
  装一次只有一份匹配器；命中**报给 stub、不投递给客机**（`x86_state.gdb_watch_mask`
  记下这四个槽里哪些是调试器装的）——否则一个没有 #DB 处理程序的裸引导扇区会把
  这个陷阱变成三重故障和复位环，gdb 还没看到值客机就重启了（实测过一次）。
  **`rwatch`（Z3）回空包**：DR7 的 R/W 字段没有"只读"编码（SDM vol.3 表 17-2），
  本机不把它偷偷放宽成"读写"，gdb 因此退回自己的软件观察点；riscv 一侧三个接缝
  留 NULL，trigger 模块与 RSP 之间那条通道记在 D6。`vCont?` 现在如实报
  `vCont;c;s`（解析分支只认这两个动作，所以不 advertise `t`/`r`）。
- 图形调试前端：依赖前两片，纯 RSP 客户端（寄存器/内存/断点/单步控制），
  不内置反汇编（反汇编一律外部工具——gdb/lldb
  attach 同一 stub 客户端侧反汇编，或 llvm-objdump 离线看镜像）。cemu
  本体保持纯 C 零新依赖。
- 验收：真 gdb attach 设断点/单步/看现场；CGA 窗口点亮 realmode hello；
  图形 OS 显示前置就绪，衔接阶段 4。

### 阶段 4：设备全集与真实 OS（此后为纯设备/IO 阶段；进行中）
- PC 设备模型全集（VGA 家族核、PS/2、PIC、PIT、UART、IDE/ATAPI、软盘 FDC+8237、LAPIC、
  IOAPIC、**PIIX4 PM function（ACPI PM timer）**）✅
  —— 片 1–14 逐项落地，片 26 补软盘通路，片 27 补显示家族，片 35 补 PM function（固件的
  微秒时基因此切到 PM timer，见库外 `../seabios/firmware.md`）。片 30–36 重做把显示形状压平（渲染基线、
  PCI BAR、光栅/片选、线性 PC 的 trace 与 gdb），此后 D17/D19 落地：VBE/DISPI 与链式
  256 色渲染、EDID、行比较/分屏/pel 平移、ATA LBA-48 与 multi-sector、PM1_STS/EN/CNT
  与 SCI IRQ9 及 S5 关机、IOAPIC 的 NMI 交付。余下的规格缺口（IOAPIC 的 SMI/INIT 交付、
  PIRQ 消费者、XBCS、SMBus 主机控制器、PM 的 S3/S4 唤醒路径；显示侧的 CRTC R17 bit5
  地址回绕、过扫描边框、CGA 复合 NTSC 伪色）按 AGENTS.md §X 台账在册。ACPI 表（DSDT/FADT）
  由 SeaBIOS 生成，不属于芯片组模型。
- bin 路线：multiboot 等价的入口契约，xv6 去掉 bootasm.S/bootmain.c
  编为 .bin；SeaBIOS 路线：官方 bios.bin 映射内存顶端，复位 F000:FFF0
  （bios.bin 已备料；跑通 SeaBIOS 本体属本阶段设备集成，不前置）。
  实际达成方式：入口契约由 **multiboot** 提供，而它随 ELF 一起交付 ——
  `board/loader.c` 认 `\x7fELF` 头，`board/elf.c` 把每个 PT_LOAD 段按
  p_vaddr 拷进物理内存、取 e_entry；`x86_init` 再扫镜像头 8 KiB 找
  `0x1BADB002`，命中即按 multiboot 契约进扁平保护模式（EAX=0x2BADB002、
  EBX=multiboot info、CS=0x08/DS=0x10），否则当 BIOS 引导扇区（DL=0x80）。
  所以 kvm-unit-tests 的 `.elf` 不需要"编成 .bin"这一步；xv6 验收走的是
  SeaBIOS 路线，bin 路线未被使用。
- 验收阶梯：xv6-x86 → Linux（参照 v86/tests/full 清单）；riscv 侧
  xv6-riscv（OpenSBI fw_jump 引导）并行验收。
- D13 剩余（x87 FPU）在 Linux 用户态销账。x87 ESC 已按"无 FPU
  处理器"语义落地（CR0.TS/EM 先 #NM，否则当 NOP），内核启动期 FPU 探测因此通过；
  仍缺 x87 算术与 RDTSC（0F 31 仍 #UD），前提是 CPUID 特性位如实为 0（EDX=0）。
  D14（VM86）已在片 18 按 SDM vol.3 17.3 实现并销账（IRET 返回 VM86、
  任务切换从 TSS 装载 VM、`seg_synth` 段合成、VM86 帧投递、TSS I/O 许可位图）。
- **现状（2026-10-21）**：验收 1（xv6-x86）✅ SeaBIOS → IDE 盘 → xv6 到 shell，
  与 QEMU 基线逐项一致。验收 2（Linux）✅ —— 片 15 把卡点钉到指令级：用户态访问
  只查 U/S 不查 R/W（SDM vol.3 §4.6 表 4-2），写时复制失效，`/linuxrc` 死在
  `_stdio_openlist_del_lock` 的 futex 上；补上用户态写保护后引导越过
  `VFS: Mounted root (ext2 filesystem)`，控制台出提示符。片 16–20 随后补测试覆盖
  （access 的 32 位移植：12 位适用空间的穷举 2305 条，含 largepage PDE 保留位轴；
  BT 位串寻址探针）、
  收紧判据（宿主失败码 2 与客机通过码撞码、kvm 用例
  输出不再丢弃）、修 `taskswitch2`（SLDT/STR 内存目标恒 16 位）与 EFLAGS 保留位
  不变式。验收 3（xv6-riscv）未开始：素材
  （xv6-riscv 镜像与 OpenSBI fw_jump）尚未取，串口输入通道已就绪。
  **验收 4（Windows 1.01）✅** —— 此前记的"显示驱动没有请求图形模式"是误判。真因是
  `modrm()` 的 16 位寻址两处结构错误（`[bp+si]`/`[bp+di]` 丢掉 SI/DI、16 位有效
  地址加段基址前没有 `& 0xffff` 回绕）：KERNEL 的路径解析器正是用它把 `"FONTS.FON"`
  截断后写进栈上的名字缓冲，五个字符全落到 64K 之外，模块查找拿空名逐个比较全部
  失败，长期不返回，最终触发堆守卫 FatalExit。补齐后进入桌面，
  帧缓冲与 QEMU 基线逐像素相同；连带修的是 GR8 按逐像素掩码展开（此前文字画成
  彩色横条）、CGA mode 6 的 bank 地址旋转（此前启动画面三份 logo）、8042 逐字节
  投递 IRQ12 边沿（此前 PS/2 指针不动；同一份自建固件在 QEMU 里指针同样不动的
  那半，根因在仓外 SeaBIOS 自身的 clang 8 位 farvar 误编译）。16 位寻址由
  `addr16_matrix`/`addr16_seg` 两格自足探针钉住（QEMU 同 transcript）。
  取证链见 progress.md 2026-10-21 各节。
  回归：riscv64 136/0、riscv32 104/0、x86 36/0、依赖边 ok。
- **验收 4（Windows 1.01）与显示实现形状**（2026-09-24 用户裁决；2026-09-29 落地）：验收件
  `build/windows/windows101.img`（1.44MB、MS-DOS 3.3 引导软盘，卷标 WIN101），QEMU
  基线是 **EGA 640×350** 下的 MS-DOS Executive 桌面（7 秒时截屏）。它给出两条硬前置：
  **软盘引导链**（8272 FDC + 8237 DMA 通道 2 + CMOS 软驱字节 + 引导 DL=0x00；片 26 交付）
  与**显示**。显示不按"CGA 图形 → EGA → VGA"三段各自成卡，而是**一个 VGA 家族核 +
  CGA/EGA 兼容模式**（片 27 交付）：核的显存 16MB（平面按 dword 交织存放，
  0xA0000-BFFFF 孔径最宽的形状达 512KB；BAR0 与 DISPI `VIDEO_MEMORY_64K` 同源），
  0x3C0-0x3DF 四组寄存器
  （CRTC/Sequencer/Graphics/Attribute）、平面/奇偶/链式寻址、256 色 DAC、CRTC 驱动的
  光栅状态；CGA 与 EGA 是它的兼容模式层 —— EGA 640×350 正是验收 4 要的那一层，CGA 的
  320×200/640×200 落在同一套寻址里。`cga.c` 已并入核（随卡删除，不留分支）。
  **机器的 int10 由显卡的 option ROM 提供**：SeaBIOS 按 `CONFIG_NO_VGABIOS=0` 重建
  （SeaVGABIOS，38400 字节），主板用 `-vgabios` 把它作为 fw_cfg 文件 `vgaroms/vgabios.bin`
  交给固件，固件自己影子到 C0000。显卡另有 PCI function（`00:02.0` = `1234:1111`，
  显示控制器类，片 30 之后补上）供固件与客人按 PCI 枚举它、并给线性帧缓冲提供 BAR0；
  Region 2 是 4KB MMIO（0x000-0x3FF 给 EDID：BAR2 偏移 0 返回 256 字节 = QEMU 默认
  1280×800 监视器的基础块 + DTA 扩展实测副本，0x400-0x41F 1:1 映射
  0x3C0-0x3DF 端口，按 QEMU 标准 VGA 规范），但它**不带扩展 ROM**（0x30 的 ROM
  地址寄存器读回 0），ROM 仍走上面这条 fw_cfg 通路。验收见
  `test/x86/probe/vga_probe.asm`（int10 切 mode 3/0x10/0x04 后核对寄存器与平面寻址，
  QEMU `-vga std` 同一镜像同样通过）。剩余显示缺口见 AGENTS.md D17（CRTC R17 bit5
  的 64K 地址回绕 AW、过扫描边框、CGA 复合 NTSC 伪色）。渲染结果的对拍通道是
  `CEMU_DEBUG=fbdump=FILE`：落二进制 P6 PPM，与 QEMU `screendump` 同格式可逐字节
  diff（win101 桌面帧与 QEMU 基线即以此核对）。

### 阶段 5：cesdk（放后：真实 OS 跑通后再做 SDK）
- 交付：crun 运行时（_start、putch→UART、halt→sifive_test）、klib、
  链接脚本、构建前端（先批处理形态）。
- 验收：hello 的 .bin 与 .elf 在 cemu 与真 QEMU 上行为一致。
- 蓝本：AM 源码逐文件对照移植。D11（HTIF syscall）在此销账。
- 工具链：构建前端统一 LLVM 系列（clang riscv64 交叉；riscv 客户机由
  xpack gcc 切换，x86 侧已在用 D:/LLVM 23.1）。

### 阶段 6：arm 与 mips
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
| abstract-machine | trm 模式、linker.ld、klib | 阶段 5 cesdk 的蓝本 |

## 八、测试阶梯

0. 冒烟：三条命令编五条指令的 .bin，HTIF 退出。
1. riscv-tests isa 套件全绿。
1.5. x86_min：0x7C00 bin 双跑对拍（cemu vs qemu-system-i386）→
    kvm-unit-tests 386 级子集全绿。
2. OpenSBI 起动出 banner。
3. xv6 引导跑通（x86 bin 路线；riscv 侧 OpenSBI 引导并行）。
4. Linux 引导。
5. cesdk hello。

每一级都是真实软件，无 cemu 定制成分。
