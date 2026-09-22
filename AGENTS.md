# cemu 开发准则（agent 必读）

违反任何一条即为 bug，发现即修复或登记到本文"简化登记"节，不允许先留着。

本文是原 开发准则.md、重构要求.md、调试方案.md 三份文档的合并版。

## 一、禁止硬编码

每个常量必须可溯源：RISC-V 手册章节、参考项目源文件、或本仓库的命名常量。
魔数直接出现在逻辑表达式里即违规。跨文件复用的位段、地址、掩码一律在 riscv.h
或对应设备头文件里定义命名常量。

## 二、禁止简化实现

规格定义的行为不许悄悄省略。任何简化必须在本文"简化登记"节登记，写明：
位置、缺什么、参考实现出处、恢复计划在哪个阶段。未登记的简化视同 bug。

## 三、禁止特判

禁止以"让某个程序跑通"为动机添加行为分支。分支条件只允许来自规格定义：
指令编码字段、特权模式、CSR 位段语义、协议状态机。
每写一个 if 先回答：规格哪一条让我在这里分支。回答不出来就是特判。

## 四、禁止死代码

无调用者的函数、无读取的字段、无引用的枚举/宏/形参不进源码。
设备或机器代码在接线（被 machine 实例化）之前不进构建。

## 五、参考优先

实现任何机制前先读参考实现，禁止闭门造车：

| 层面 | 参考出处 |
|---|---|
| 指令语义 | xiangshanNEMU src/isa/riscv64/instr/ 与 system/priv.c |
| CSR/特权机制 | xiangshanNEMU system/priv.c、timer.c、trigger.c、mmu.c |
| 机器与设备映射 | dearchap-tinyemu riscv_machine.c（CLINT/UART/内存树）、iomem.c |
| x86 386 语义 | tiny386/i386.c + i386ins.def（主 oracle）；nemu/src/isa/x86（第二参照） |
| x86 验收套件 | v86/tests/kvm-unit-tests（realmode 已入库 test/x86/realmode） |
| 行为裁决 | riscv-tests 期望值、QEMU、gem5 src/arch/riscv/ |
| 最终裁决 | RISC-V 非特权/特权手册；x86 以 Intel SDM 裁决 |

参考项目之间有分歧时以手册裁决；SDM 与实机分歧时按实机模型重写并经全表
对拍（先例：DAS 18 处分歧按 QEMU/v86 真值表修复）。

实现完成时在关键分支的注释里注明参考出处。

## 六、反案例驱动

官方套件（riscv-tests 等）只是验收器。测试失败时先修语义；
禁止为让某条测试变绿而添加规格外行为；禁止在模拟器里识别测试程序的特征。
验收件只能是官方测试套件或真实开源软件，手写 asm 只能当冒烟探针
（test/*/probe/ 性质），不算验收件。

## 七、构建纪律与目录依赖

### 目录 = 机器部件

src/ 按被模拟机器的部件分目录：

| 目录 | 负责什么 |
|---|---|
| `cpu/` | 处理器。`cpu.h` = CpuState（寄存器 bank + 提交点 pc + 主板钩子）；`step.h` = 步进契约 frame/isa_ops/raise_；`cpu/isa/<name>/` = 各 ISA 解释器 |
| `bus/` | 互连：地址空间与译码 |
| `mem/` | 内存子系统：RAM |
| `device/` | 外设，按类分：`char/` `timer/` `intc/` `misc/` `video/` |
| `board/` | 主板：设备布局、复位态、接线、装载（ELF）、运行循环 |
| `debug/` | CEMU_DEBUG 观测中枢 |
| `host/` | 宿主 OS 层，唯一可 include windows.h |
| `util/` | log / table / type |
| `src/main.c` | 入口：参数解析 + 装配 |

### 禁止的依赖边

`cmake --build build --target check`（tools/depcheck.sh）机械检查，命中即失败：

- `bus/`、`mem/`、`device/`、`debug/` 不得 include `cpu/isa/`：总线、内存、
  外设不认 CPU 型号，只见 `cpu/cpu.h`。
- `cpu/cpu.h`、`cpu/step.h`、`cpu/step.c` 不得 include `cpu/isa/`。
- `cpu/isa/**` 不得 include `device/`、`board/`：解释器不认外设与主板。
- `src/host/` 之外任何文件不得 include `windows.h`。

主板可以 include CPU 型号头，但仅限各 ISA 的 `cpu/isa/<name>/platform.h`
（中断线号常量）。中断注入走 `CpuState.set_irq` 钩子，由 CPU 型号在 init 里
安装；主板只调钩子，不调型号函数。`spike_min.c` 与 `x86_min.c` 不含任何
ISA 头，`virt.c` 仅引 `platform.h`。

- 命名：Google C 风格。文件与变量 snake_case，类型与函数 PascalCase，
  常量与枚举值 k 前缀，宏全大写。include guard 按路径（`CEMU_BUS_BUS_H`）。
- 构建（LLVM/clang）：CMake（`CMakeLists.txt`）+
  Ninja 生成器，编译器 clang（`tools/clang.cmake` 工具链文件，mingw-w64 目标
  ——D:/mingw64 只当头文件与运行时 sysroot，不再当编译器），`-Wall -Wextra`
  零告警。源文件用 `file(GLOB_RECURSE src/*.c)`（`CONFIGURE_DEPENDS`，新增/
  删除源文件自动重扫），是旧 Makefile 四级显式通配（`src/*.c` … `src/*/*/*/*.c`）
  的超集，不依赖 glob 尾部斜杠行为。头文件依赖由 CMake/Ninja 自动追踪。
  流程：`cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=tools/clang.cmake &&
  cmake --build build`（换编译器或首次配置后从空 build 目录起；ninja 不在
  PATH 时前置 `PATH=/d/ninja:$PATH`）。clang 与 gcc 的差异注意：
  -fno-common（暂定定义重复即错，exec.c 的 cpu 共享指针已显式化）、
  clang 的 mingw 驱动预定义 UNICODE（uxtheme 的 SetWindowTheme 映射 W 版）。
- 每轮改动结束前限时跑回归：`bash test/run.sh`（分层入口：riscv
  `bash test/riscv64/run.sh`，x86 `bash test/x86/run.sh`）；依赖边用
  `cmake --build build --target check`。脚本是 bash，调用方不必是：CMake 在配置时
  解析出 Git 的 bash（打印 `shell for check targets:`），`check` / `check-x86` /
  `check-riscv64` 三个目标用登录 shell 跑脚本，cmd/PowerShell 下入口一致；直跑脚本
  要用 Git Bash（非登录 shell 的 PATH 无 /usr/bin）。
- 运行限时：**所有**运行一律限时——run.sh 用 `timeout` 包裹 cemu 调用；
  会话内的临时手跑（调试观测、state 对拍、探针复现）同样必须前置
  `timeout <秒>`，防止被测程序死循环导致不退出的挂起（realmode 套件
  尾部 fninit #UD 死循环即由 timeout+输出判据容纳）。
- 落盘体积：**任何**写入文件不得超过 5MB——测试对拍数据/捕获文件、
  CEMU_DEBUG 观测落盘（`2>file` 的 trace/state/mem/regs 等）一律算；
  超出即视为违规，须改用采样、截断或 CEMU_DEBUG `budget=` 抑制等
  手段收敛体积。

## 八、统一执行抽象的架构铁律（V3 巨型 switch 直执）

V2 曾尝试 μIR：解码产出"建筑级微操作"、共享执行器、表驱动分发。三轮设计后被
用户否定——微操作把"一条指令"切成多片（表行/构造函数/共享动词/标志钩子），层间
粘合累计 21 个失败，且可读性未达标。当前 V3 采用**巨型 switch 直接解释器**，
遵循两条根本目的：**代码简单清晰（SDM 级）** + **纯 Win32 API**；执行模型是
**小步操作语义（small-step operational semantics）**：一条指令 = 一个 step，
step 是纯函数式的状态转移（给定当前 CPU 状态，产出下一状态），pc 是唯一的提交点。

1. **统一执行风格**：x86 与 riscv 用同一套抽象与执行范式（`cpu/step.h` 的
   frame / isa_ops 接缝），但每个 ISA 各自一份巨型 switch 直执。研究参照仍是
   TQemu/gem5 的执行模型，NEMU 只作参考不限其形。
2. **无中间表示**：不生成 IR / 微操作。取指→译码→执行在同一 C 作用域内完成，
   像手册逐页读（riscv case = opcode→funct3→funct7；x86 case = opcode 序）。
   宽度写进名字（rm8/rd32/push32），无宽度形参；C 运算符做算术，helper 只补
   C 没有的（标志/栈/段/MMU/FP/陷阱）。
3. **永远纯解释执行，不做翻译/JIT**——一切先以代码清晰可读为准，再追求速度。
4. **面向未来扩展设计**：x86 → i686 → x86_64 的指令增补、修 bug 的可维护性，
   是数据结构设计的第一约束。新增指令 = 新增 case，不改动共享层。
5. **CPU 的本质**：CPU = 一个状态数据结构；指令执行 = 状态转移。共享
   GPR/FPR bank（CpuState），pc 是唯一提交点（step 尾部单点提交 cpu->pc）。
6. **故障原子性**：Raise 走 setjmp/longjmp（raise_ → 设 trap → step 单点提交
   dnpc）；RMW 的 store 先于 flags 落地，故障时指令整体回滚。
7. **小步语义映射**：step(cpu) 即操作语义的"一步"；观测（CEMU_DEBUG 的
   state/trace）在每步提交后采集，对拍基准即逐指令状态流。寄存器即 union 别名
   （eax/ax/al/ah）、flags 位域左值（cf = 1），x86 case 对照 SDM Operation 节
   逐行可读，riscv 对照手册伪代码。
8. **ISA 目录形状统一**：每个 `cpu/isa/<name>/` 是同一套角色——`<name>.h`
   （私有态与常量）、`exec.c`（巨型 switch：指令集）、`step.c`（步进协议：
   中断投递 + 取指 + 单点提交）、`exec.h`（exec.c 与 step.c 共享的私有头：
   每步视图 + 寄存器短名宏 + 跨文件入口）、`<后端>.c`（csr/mmu/fp…）、
   `isa_<name>.c`（isa_ops 注册）、`platform.h`（仅当主板需要中断线号时才建，
   x86 就没有）。加新 ISA = 照形状填，不动共享层。
   坑：`exec.h` 定义了 `x`/`f`/`eax` 这类单双字母宏，**必须放在 include 列表
   最后**，否则会把它后面头文件的形参名（`frame *f`）替换掉。

## 九、调试与修复纪律（重构要求第二轮 + 调试方案）

1. **禁止临时探针**：getenv 一次性打印（CEMU_DBG/[step]/[trap2]/ 等）不许
   再出现。所有调试观测用常驻设施 `CEMU_DEBUG`（语法见第 X 节）。唯一例外：
   语义本身在执行器里算错属于代码 bug 而非观测缺口，但发现它的路径也是
   state/trace 而非探针。若修 bug 时仍手写了探针，说明设施有观测缺口，必须
   回来补设施并登记。
2. **禁止手动计算/手工模拟**：手动推演永远是错的。不许在脑内反汇编指令、
   不许纸上演算状态；用 CEMU_DEBUG 的 ops/watch/state 对拍定位，到源码里
   找差异。
3. **大重构错很多是正常的**：不案例驱动地逐个补丁。
4. **禁止 if 特判式修复**：真正的 CPU 不做特判，都是固定的、简单的几个
   RTL 微指令。修复必须落在统一的 op 语义 / 表定义 / 构造函数这一层；
   如果一个 bug 看起来需要"对某指令加个 if"，说明 op 语义或表行写错了，
   修那里，而不是加分支。
5. **修复不对照旧实现**：旧解释器与对拍通道已删除。`state` 流的对拍对象是
   自己留档的基线文件。
6. **对拍流程**：改动前 `CEMU_DEBUG=state ... 2> base.state`，改后
   `2> new.state`，diff 第一个分叉行即第一语义偏差。
7. **不要求全部通过**：把最基础的过了就行，剩下的按登记排期。
8. **子代理纪律**：任何 subagent **硬上限 3 分钟**，超时
   即停（TaskStop），要它**直接输出报告**，不许自己一路翻拍文档、不许扩大阅读
   范围；只读式"代码审查"没有产出价值，别为它花时间。GUI 类改动的验证是
   **构建 + 启动 + 用户目验**，不是脑内推理，也不是子代理评审。

## 十、CEMU_DEBUG 语法（常驻调试设施，唯一调试入口）

```
CEMU_DEBUG = item[,item...]

  trace[:line|:table][=N]   逐指令事件行（line=单行式，table=全列式）
  ops                       每条指令的 IR 操作转储（译码错 vs 执行错的分水岭：
                            op 列表对而状态错 → 执行器/ISA verb bug；
                            op 列表本身错 → 译码表/构造函数 bug）
  state                     规范状态流（对拍基准；格式由 bank/cell 枚举序生成，
                            跨重构恒定；x86 打 g0..g7/s0..s5/x，riscv 打
                            g0..g31/f0..f31/x）
  mem[:ld|:st][=N]          访存事件
  trap                      异常/中断/陷入事件
  bus                       MMIO / IO 端口命中（**不含指令取指**：固件在 ROM 窗口
                           执行时取指行会淹没一切）
  regs=N                    每 N 条指令打一次全寄存器表；halt/exit 必打
  watch=ADDR:SIZE[:r|w|rw]  地址观测（可重复逗号分隔；数字按 C 字面量解析，
                            **十六进制必须写 0x**，否则静默丢弃——现已改为报错）。
                            **地址按物理地址匹配**：x86 开分页后要看内核某个符号
                            必须先减去映射偏移（内核 .data 的 0xc02c0180 是物理
                            0x2c0180），写线性地址会一行都不出
  dump=ADDR:SIZE:FILE       会话结束时按**物理地址**取回一段客机内存并落盘
                            （可重复，上限 4 段、单段 5MB）。watch= 只说某地址
                            被碰过，说不了客机留在"没人再读的结构"里的东西——
                            停住的客机的 log buffer、页表、task struct 只能靠它。
                            要求会话自行结束（--max-inst 或客机停机）：宿主信号
                            会跳过会话收尾，什么也不写。例：取 Linux 的 __log_buf
                            `dump=0x30a000:0x18000:build/logbuf.bin`
  budget=N                  单类别事件上限，超出抑制并计数，退出时汇总
                           （会话另有 5MB 输出硬顶：固件 ROM 影子拷贝这类"每字
                            节两次事件"的热点会把它吃满；定位引导期设备流量要用
                            skip= 把窗口挪过去）
  skip=N                    类别静默窗口：该类别的前 N 个单位不打印。**trace 类目按
                            指令计**，其余类目按自己的事件计（trace 不为每条退休指令
                            都发射事件，按事件计会在没人要的指令号上开窗）
  mark                      无帧事件行：板级/设备级/解释器级观测，`a` 十进制、
                            `b` 十六进制（位掩码与地址要能直接读）。现有 name：
                            isa(中断线) intr(CPU INTR 线) inta(INTA 向量)
                            pic0/pic1-imr|eoi|base ioapic lapic-irr|ack|eoi
                            lidt/lgdt gate(门描述符) hostkey reset-req(设备请求
                            机器复位)
  screen                    无帧文本行：客机控制台镜像，**整行输出**（控制台一行
                            80 列，事件表 DETAIL 只有 41，截断后读不了 call trace）
  utf8                      UTF-8 边框（默认 ASCII，Windows 代码页安全）
```

- 输出走 stderr，`2>file` 留档；热路径零成本（发射点一个位测试，
  类别掩码 init 时解析一次）。
- 事件表统一列 `KIND │ PC │ RAW │ MNEMONIC │ DETAIL │ FLAGS`，KIND：
  I 指令 / L 读 / S 写 / T 陷阱 / B 设备 / W 观测点 / O IR 操作转储 / K 标记
  （K 与 screen 都是无帧行：K 走事件表，screen 走整行文本）。
- 症状 → 命令配方速查：

```bash
# 语义对拍基线（重构/改 op 后）
CEMU_DEBUG=state ./cemu.exe ... img >/dev/null 2> base.state   # 改动前
CEMU_DEBUG=state ./cemu.exe ... img >/dev/null 2> new.state    # 改动后
diff base.state new.state | head

# 某条指令行为可疑：看它的逐指令行为 + 上下文（前 200 条后停）
CEMU_DEBUG="trace:table,state,mem,budget=200" ./cemu.exe ... img 2> t.txt

# 某地址的写入不知来自哪条指令（watch 行自带 PC/RAW/MNEMONIC）
CEMU_DEBUG="trace:line,watch=c0e4:4:w" ./cemu.exe ... img 2> w.txt

# 异常去向不明 / 设备寄存器被谁读写
CEMU_DEBUG="trap,trace:line" ./cemu.exe ... img 2> t.txt
CEMU_DEBUG="bus,trace:line" ./cemu.exe ... img 2> b.txt

# 卡死：哪两类事件在打转（退出汇总的 suppressed 计数暴露循环构成）
CEMU_DEBUG="trace:line,mem,budget=50000" ./cemu.exe ... img 2> t.txt

# 定期全寄存器现场
CEMU_DEBUG="regs=100000" ./cemu.exe ... img 2> r.txt

# 中断"送不到"：先看线（isa=设备侧到控制器，intr=控制器到 CPU），再看 ack/eoi
# （三者都不出 = 控制器没present；intr 有而 inta 无 = CPU 没取；inta 有而无 eoi
#  = 处理程序没跑完，多半交付落到了错的桩）
CEMU_DEBUG="mark" ./cemu.exe ... img 2> m.txt

# 交付到了哪个桩：gate 给出该向量门描述符里的 handler 偏移（无帧标记，十六进制）
# 门内容可疑时再按物理地址 watch 整个 IDT（x86 内核 .data 要减 0xc0000000）
CEMU_DEBUG="mark" ./cemu.exe ... img 2> m.txt
CEMU_DEBUG="watch=0x2c0000:0x800:w" ./cemu.exe ... img 2> idt.txt

# 客机只往显存打（内核早期 console=tty0）：screen 给整行控制台镜像
CEMU_DEBUG="screen" ./cemu.exe ... img 2> c.txt
```

- 交互式调试器/REPL、反汇编器、GUI 输出立项为阶段 3.5（见 arch.md 阶段计划：
  gdb RSP stub / Win32 显示通道 / 图形调试前端；前端不内置反汇编，反汇编一律
  走外部工具）。在 3.5 落地前 CEMU_DEBUG 仍是唯一调试入口（mnemonic 由译码表给出）。

## X、简化登记（在册的规格缺口台账）

规格行为缺失按阶段恢复。**销账 = 整行删除**：不写"已完成 / 已销账"，不留日期、片号、
过程或解释（过程只进 progress.md）；行内只写还缺什么。只有新功能才加行。

| 编号 | 位置 | 缺失 | 参考出处 | 恢复阶段 | 状态 |
|---|---|---|---|---|---|
| D7 | LR/SC 单核预留集 | 无多核冲突语义 | spike 单核同款；规格允许 SC 假失败 | 多核引入时 | 登记中 |
| D11 | htif.c HTIF syscall（dev0/cmd0）报错退出 | 无 fesvr syscall 设备 | fesvr htif_t::handle_syscall | 阶段 5 cesdk | 登记中 |
| D13 | x86 x87 FPU 与 RDTSC | **x87 FPU 未实现**：本机 CPUID 报 EDX.FPU = 0（无 FPU），因此 ESC 指令（D8-DF）按"无 FPU 处理器"语义执行——CR0.TS/EM 置位时先 #NM，否则解码 modrm 后当 NOP（tiny386 ESC() 同款；386SX/486SX 上 Linux 就是这么引导的）。**RDTSC 仍为显式 ud()**（0F 31） | intel SDM vol.2/vol.3（ESC 与 CR0.EM/TS 语义）；tiny386 i386.c 的 ESC() 宏；Linux arch/x86/boot/setup.S 的 FPU 探测（靠 ESC 为 NOP 判定"无 FPU"） | 真 FPU（含 RDTSC 与 CPUID 特性位）在需要 x87 算术的客户机出现时（Linux 用户态/浮点程序） | 登记中 |
| D14 | x86 iret/popf 载入屏蔽 VM(bit17) | VM86 位不装载（iret/popf/task-switch 进入 VM86 无实现） | intel SDM vol.3 17.3.1 | DOS/BIOS 兼容路线需要 VM86 时评估（Linux 不需要） | 登记中 |
| D15 | x86 16 位 TSS 任务切换（类型 1/3）Fatal | 任务切换只支持 32 位 TSS（类型 9/B）；门/任务门对 16 位 TSS 拒绝进入 | v86 do_task_switch（assert 32 位）；tiny386（assert 9/11） | 有验收件需要 286 任务时 | 登记中 |
| D17 | cga.c 渲染与光栅时序缺口 | CGA 图形模式（0x3D8 bit1）不渲染（黑屏）；过扫描边框不渲染（视频禁止时填黑）；0x3DA 回扫状态为宿主时钟近似（262 行×63.6µs 帧模型，行内只分活跃/消隐两相，非逐像素光栅）；属性/光标闪烁取固定场倍数周期，不跟随场相位 | IBM CGA Technical Reference；FreeVGA；QEMU vga 行为旁证 | 阶段 4 VGA 图形切片（图形模式随 VGA 一起做）；需要精确光栅时序的软件（raster 技巧 demo）出现时再校准 | 登记中 |
| D18 | CMOS RTC 的周期/闹钟中断 | RTC 的周期中断与闹钟中断（IRQ8，经从片 PIC 的线 0）不投递，status C 恒读 0 —— 读日期时间够用，按周期取中断的驱动（Linux rtc 驱动、周期性 timekeeping）不够 | MC146818 datasheet；QEMU hw/timer/mc146818rtc.c | 有客户机按周期用 RTC 中断时（现在 Linux 用 PIT 走时） | 登记中 |
| D19 | PIIX3 芯片组、IDE 与 IOAPIC | IOAPIC 只投递 fixed 与 lowest-priority，**NMI/SMI/INIT/ExtINT 四种交付模式不投递**；PIIX3 ISA 桥的 PIRQ 路由字节 0x60-0x63 无消费者（IDE 跑兼容模式，没有 PCI 设备拉 INTx）；XBCS/PM/DMA 块未做；ATA 侧无 LBA-48 与 multi-sector（未实现的命令按规格 ABRT） | ATA/ATAPI-7 §6.3（未实现命令的应答就是 ABRT）；QEMU hw/isa/piix3.c、seabios src/fw/pciinit.c piix_isa_bridge_setup；IOAPIC 交付模式：82093AA datasheet §3.2.4；NMI 输入：SDM vol.3 §6.7 | NMI/SMI 交付模式要先给 CPU 加 NMI 输入（本机无 SMM、单 APIC、无 PIC→IOAPIC 通路）；PIRQ 需要真拉 INTx 的 PCI 设备；PM/ACPI 在需要挂起/电源管理时；LBA-48 在需要 >128 GiB 或客户机强制时 | 登记中 |
| D21 | 软盘与 DMA 通路缺失 | 无 Intel 8272 软盘控制器、无 8237 DMA（通道 2 给软盘、通道 0 给内存刷新），CMOS 设备字节也不报软驱；再加上 8042 软驱数据线语义，整条"软盘引导 + PC 兼容传软盘"的路径都不存在。后果：Linux 0.11/0.12 那类把引导码写死成 DL=0（且要求每道 15/18 扇区）的软盘引导镜像无法引导 —— 只能走硬盘/光盘引导的镜像 | PC/AT Technical Reference（FDC 命令集）；Intel 8237A datasheet；QEMU hw/block/fdc.c + hw/dma/i8257.c；SeaBIOS src/hw/floppy.c（INT 13h 路径） | 需要软盘引导的镜像（如 oldlinux 的 0.11/0.12 套件）作为验收件时；Linux 阶梯本身不需要（ISO 路线） | 登记中 |
| D20 | i8042/PS/2 键盘 | 键盘设备只**应答**命令（每条 0xFA；0xFF→ACK+0xAA、0xF2→ACK+0xAB 0x83、0xEE→0xEE），但不真正执行：0xF0/0xED/0xF3 的参数字节只回 ACK、不切换扫描码集/LED/typematic；扫描码一律按 set 1 发（命令字节翻译位只存不译）；无鼠标（AUX）；输出队列满时丢字节（无 overrun 位） | PC/AT Technical Reference；QEMU `ps2.c`/`pckbd.c`（tiny386/i8042.c 同源）：ACK 逐命令、IRQ1 门控 `mode & KBD_INT && !(mode & DISABLE_KBD)` | 需要 set 2 键盘、鼠标或真正走 PS/2 设备命令的客户机时 | 登记中 |
| D25 | `test/x86/realmode/probe_ah.elf`（15016 B，已入 git） | **中间产物入库 + 配方缺失**：§七 规定 test/ 只入库源与脚本，这份 kvm-harness 探针的 ELF 产品却在库里；更要紧的是生成它的命令从未入库 —— `test/x86/realmode/build.sh` 只构建 realmode.elf，probe_ah.c / probe_harness.c 的手工编译没有配方，同目录 .o / .exe / .gen.s / .flat.s 都被 .gitignore 覆盖且已清出，只有这份 ELF 例外 ⇒ 删掉即不可再生 | 配方可照抄：test/x86/realmode/build.sh（realmode.elf 的 gcc -m32 管线）、test/x86/build_kut.sh（clang i386 管线）；探针源 probe_ah.c、probe_harness.c 已在库 | 想清理 test/ 里的中间产物时：先在 build.sh 补 probe_ah 目标，再 `git rm --cached` 该 ELF 并让它走 ignore | 登记中 |
| D26 | `test/riscv64/rv64ssvnapot-p-napot.elf`（3 147 616 B，全库最大跟踪文件） | **体积构成的 99.95％ 是填充**：全文件只有 1517 个非零字节。上游 napot.S 的两个 `.align 20` 把 .data 顶成 1 MiB 对齐 ⇒ 该 PT_LOAD 的 p_align = 2**20，lld 为满足 file offset ≡ vaddr (mod 2^20) 把段放在文件偏移 0x100000，于是 1 MiB 空洞 + filesz 0x200010 的段内容。非规格缺口，也不违反 §七 的 5 MB 落盘上限（3.0 MB < 5 MB）—— git 实存仅 ≈ 4.5 KB（zlib 实测），成本只在工作树与拷贝 | riscv-tests rv64ssvnapot 上游的 `.align 20`（test/riscv64/build_si_extras.sh 头注释已记）；实测：llvm-objdump -p（p_align 2**20、off 0x100000）、非零字节计数、deflate 估计 | 仓库瘦身提上日程时；或所有跑回归的机器都有 clang riscv64 交叉工具链后，改为测前由 build_si_extras.sh 现生成、镜像不入库（93aee5b 当初入库镜像正是为了免这个依赖）。单纯 `--max-page-size` 未必压得下来（节对齐会把 p_align 顶回去，未实测） | 登记中 |
| E1 | fp.c 用宿主 float/double/long double 模拟 IEEE | 偏离参考：QEMU/spike 用 Berkeley softfloat；宿主 long double 有 x87→float 双舍入长尾风险 | QEMU fpu/softfloat.c（BSD） | Linux 阶段出现浮点偏差时移植 softfloat | 登记中 |

## 附：关键行为裁决存档（修复时的先例依据）

- **计数器抑制**：写哪个计数器，抑制哪个自身的本步自增，无跨计数器连累
  （写 minstret 后下一条读到写入值；写 mcycle 不冻结 minstret）。QEMU 在
  mcycle 侧与规格分歧，只实现有测试覆盖的一侧，不模仿。
  探针证据：test/riscv64/probe/probe_counters.S。
- **isa-debug-exit**：status = (value<<1)|1，不是 value+1（实测校准）。
- **DAS**：SDM 伪代码与实机 1024 组真值表有 18 处分歧（SDM 第二步用减 6 后
  的 AL，实机用原始 AL），按实机（QEMU/v86）模型实现，全表对拍 0 分歧。
- **HTIF**：单通道语义——组装写出的字节到影子寄存器，非零即处理并清零
  （等价 fesvr 轮询）；fromhost 应答 `(dev<<56)|(cmd<<48)`。
- **x86 DR7 字段布局**：R/Wi@17:16+4i、LENi@19:18+4i（4 位间隔，SDM vol.3
  figure 17-3，模式无关——64 位无重排，检索证实）；LEN 编码取 QEMU/KVM 的
  {1,2,4,8} 字节（32 位下 LEN=11 属 SDM 未定义域，kvm debug 测试在 32 位
  移植中按 8 字节对拍双绿，故从 QEMU 真值）。
- **x86 DR6 写语义**：B0-B3 任意写清除、BD/BS/BT 按写入值、保留位强制读 1
  ——由 kvm debug 测试 set_dr6(0x4002) 的读回值锚定（W1C 假设被
  single-step 用例证伪）。注意 gem5 此处取"按写入值"模型，与 KVM 实机分
  歧（gem5 从不发射 B 位、断点使能即 panic，该路径无实机对拍）——按
  规则"参考分歧以手册/实机裁决"，从实机。v86 则原样存（同样无投递层，
  写 0 会丢保留位 1，不可对拍）。
- **x86 DR 存取层三方佐证**：复位值 DR6=0xffff0ff0、
  DR7=0x400 三方一致（gem5 isa.cc:136、v86 cpu.rs:4610、SDM）；DR4/5 在
  CR4.DE=0 时别名 DR6/7、DE=1 时 #UD 三方一致（v86 `dreg_index += 2`、
  gem5 fallthrough、cemu 同款）；DR7 BitUnion 位布局（gem5
  rw0@17:16/len0@19:18…）与 SDM figure 17-3、Wikipedia 表三方一致。
- **kvm debug 测试 32 位移植**：上游 64 位专用（Makefile.i386 注释掉；
  asm 用 %rax/%rip）。32 位移植件 test/x86/kut_debug.c 的期望地址按 clang
  实际编码重推导（AND 累加器格式 5 字节 vs 原版 81/4 假设 6 字节；
  `lea (%%rip)` 改 call/pop 锚点 +3），语义全部模式无关（SDM ch.17 单处
  定义），QEMU TCG 32 位与 cemu 双绿 8/8——批准作为 A 档
  验收件。

- **x86 处理器内部访问 = 超级权限**：描述符表（GDT/IDT/
  LDT）、TSS，以及换栈后压入的投递帧，都是**处理器自己的访问**，忽略 U/S 位；
  只有程序自己的访问才按 CPL 检查（SDM vol.3 4.6）。硬性判据：ring 3 的段加载
  要能读 U=0 的 GDT，ring 3 的中断要能写 U=0 的内核栈 —— 任何 OS 都依赖这条。
  先例：xv6 首个用户态 iret 里 `seg_commit(cs)` 已把 CPL 切成 3，紧接着的描述符
  A 位写回被当成"用户写 GDT"（U=0）→ #PF 错误码 7；随后 #PF 的投递读 IDT 又被
  当成"用户读" → #PF 错误码 5 → #DF → triple fault（三次陷阱都报在 iret 上，
  因为 CS 已提交而 eip 还没写）。实现：程序类 `bus_load/bus_store` 对处理器类
  `kbus_load/kbus_store/krd*/kwr*/kpush*`，由
  `page_translate_as(lin, write, user)` 的 user 参数分派。
