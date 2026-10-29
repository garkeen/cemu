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
- 每轮改动结束前限时跑回归：`bash test/run.sh`（分层入口：riscv64
  `bash test/riscv64/run.sh`、riscv32 `bash test/riscv32/run.sh`，x86
  `bash test/x86/run.sh`）；依赖边用
  `cmake --build build --target check`。脚本是 bash，调用方不必是：CMake 在配置时
  解析出 Git 的 bash（打印 `shell for check targets:`），`check` / `check-x86` /
  `check-riscv64` / `check-riscv32` 四个目标用登录 shell 跑脚本，cmd/PowerShell 下入口一致；直跑脚本
  要用 Git Bash（非登录 shell 的 PATH 无 /usr/bin）。
- 运行限时：**所有**运行一律限时——run.sh 用 `timeout` 包裹 cemu 调用；
  会话内的临时手跑（调试观测、state 对拍、探针复现）同样必须前置
  `timeout <秒>`，防止被测程序死循环导致不退出的挂起（realmode 套件
  尾部 fninit #UD 死循环即由 timeout+输出判据容纳）。
- **测试只能跑探针，不许跑 bios/img/iso**（用户裁决）：套件里任何一格都不得
  引导固件镜像、软盘镜像或光盘镜像。理由是它们慢（一个 SeaBIOS 自举到 DOS 就要
  千万条指令、一分多钟），且依赖库外的产物（本仓的 `build/` 与 `../seabios/out/`
  都不在版本控制里，换台机器那几格只能 SKIP，计数也随之漂移）。设备语义一律用
  **自足探针**钉：探针自己编程那台设备（`test/*/probe/`，multiboot ELF 或引导扇区，
  从 `-bios`/`-fda`/`-cdrom` 起来的都不算），`timeout` 内在秒级跑完。会话内的临时手跑
  同样受此约束。**违反此条的格直接删除，不改写成探针**（用户裁决）：片 26/27 引入的
  `floppy` 与 `vga` 两格（`-bios ../seabios/out/bios.bin -vgabios ... -fda`）已从
  `test/x86/run.sh` 删除，覆盖改由两个自足探针承担（`vga_core` 直接写 0x3C4/0x3CE/
  0x3C0 与孔径；`fdc_dma` 自己下 8272A/8237 命令与 DMA 编程，都不借 SeaBIOS 的手），
  x86 套件 36 格（28 单格 + kvm 7 镜像 + realmode 1）。
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
  bus[:ADDR|:NAME]          MMIO / IO 端口命中（**不含指令取指**：固件在 ROM 窗口
                           执行时取指行会淹没一切）。`bus:ADDR` 只留该端口/MMIO 地址，
                           `bus:NAME` 只留该设备（`DeviceOps.name`，如 `bus:i8042`）——
                           对应 QEMU `-trace` 的事件模式匹配。不带过滤时一次引导的前
                           几万行全是 ROM 影子拷贝，5MB 输出硬顶会把要看的窗口吃掉；
                           被滤掉的行不占 budget，也不占 skip 窗口
  pmp                       PMP 拒绝事件（每次拒绝一行）：
                           `pmp-deny addr=<物理地址> len=4 w S entry=0 cfg=18 base=... size=...`
                           —— 被拒绝的访问不会到达总线，`bus` 一行都不出；这一行给出
                           最低号命中条目的 cfg 与解码区间（`entry=none` = 无规则命中）。
                           riscv 的负载/存储/取指与页表走查都过 PMP，32/64 两个后端同款
  regs=N                    每 N 条指令打一次全寄存器表；halt/exit 必打
  watch=ADDR:SIZE[:r|w|rw]  地址观测（可重复逗号分隔；数字按 C 字面量解析，
                            **十六进制必须写 0x**，否则静默丢弃——现已改为报错）。
                            **地址按物理地址匹配**：x86 开分页后要看内核某个符号
                            必须先减去映射偏移（内核 .data 的 0xc02c0180 是物理
                            0x2c0180），写线性地址会一行都不出
  vga-io                   客人对 VGA 卡寄存器文件的一次端口访问一行，读写各一行：
                            `vga-io rd|wr port=0x3d5 val=0x28`。对应 QEMU 给自家测试用的
                            vga_std_read_io / vga_std_write_io trace 事件——**两个模型之间
                            唯一能把"客人的整段对话"逐访问对齐的通道**。寄存器的写流说明
                            不到客人何时改了主意（那是读决定的），而渲染结果与寄存器文件
                            都只能给出终点；这条给出的是过程。同 budget/skip。
  dump=ADDR:SIZE:FILE       会话结束时按**物理地址**取回一段客机内存并落盘
                            （可重复，上限 4 段、单段 5MB）。watch= 只说某地址
                            被碰过，说不了客机留在"没人再读的结构"里的东西——
                            停住的客机的 log buffer、页表、task struct 只能靠它。
                            要求会话自行结束（--max-inst 或客机停机）：宿主信号
                            会跳过会话收尾，什么也不写。例：取 Linux 的 __log_buf
                            `dump=0x30a000:0x18000:build/logbuf.bin`
  fbdump=FILE                会话结束时把**显示卡的渲染缓冲**落成二进制 PPM
                            （P6，与 QEMU `screendump` 同格式，两张图可逐字节
                            diff；可重复，上限 2 段，超 5MB 拒写不截断）。
                            dump= 只能取回内存里的东西，而画面根本不在内存里——
                            它是卡自己的 XRGB 缓冲（display.h 的 vtable），由主板
                            交给中枢（D17 里缺的就是这个：无头核对渲染的唯一手段）。
                            同样要求会话自行结束。例：`fbdump=build/fb.ppm`
  pcshot=ADDR[:COUNT]       在**线性 PC** 触发的规范 state 行（可重复，上限 8 条；
                            COUNT 默认 1）。命中点是"下一条指令的线性 PC 等于 ADDR"，
                            也就是 CPU 即将执行 ADDR 时的状态——与 QEMU gdb 在 ADDR
                            设断点停住的边界相同，两个模型的寄存器/段/内存可以逐字段
                            对照。存在的理由：`state`/`trace` 按退休指令数开窗，REP
                            循环会吃掉预算、定时器 I/O 又会让指令数在两次运行间漂移；
                            gdb 断点虽按线性 PC 停，但从复位跑完整启动要过每步回调，
                            慢数倍。这条把匹配放在普通快路径里，30 秒级即可拿到指定
                            PC 的现场。可移动段的基址在两次运行间可能不同，定点应优先
                            选固定段（如 KERNEL 0xDF9）；可移动段用当前运行的实际基址。
                            标记行带 `inst=N`（本步提交后的退休指令数）：可移动段在崩溃
                            时可能已被卸载/搬走，`--max-inst=N` 重跑到同一刻再 `dump=`
                            才能取回当时仍驻留的段。例：`pcshot=0x1f7ab,pcshot=0x103b9:3`
  budget=N                  单类别事件上限，超出抑制并计数，退出时汇总
                           （会话另有 5MB 输出硬顶：固件 ROM 影子拷贝这类"每字
                            节两次事件"的热点会把它吃满；定位引导期设备流量要用
                            skip= 把窗口挪过去）
  skip=N                    类别静默窗口：该类别的前 N 个单位不打印。**trace 类目按
                            指令计**，其余类目按自己的事件计（trace 不为每条退休指令
                            都发射事件，按事件计会在没人要的指令号上开窗）
  hist[=N]                  环形缓冲里最后 N 条已退休指令（默认 32，上限 64）：
                            `<线性 PC> <助记符> <机器码>`。**任何无帧事件行触发时**
                            从旧到新整段打印（mark/watch/bus/trap/pmp/virtio）：
                            88 列的 DETAIL 装不下调用链，而调用链正是"客人自己干的，
                            还是中断插进了别的事"的分界（对照物：Intel PT、Bochs 指令
                            trace、NEMU ftrace）。**没有自己的 skip 窗口**：环只跟着
                            已放行的行打印，给它一个 skip 会让它永远攒不满（行被抑制，
                            dump 也就永不发生）。记的是**已提交**的指令，所以第 k 条
                            指令里发生的事件显示到 k-1 为止，当条指令就看行里的 `pc=`
  mark                      无帧事件行：板级/设备级/解释器级观测，`a` 十进制、
                            `b` 十六进制（位掩码与地址要能直接读）。现有 name：
                            isa(中断线) intr(CPU INTR 线) inta(INTA 向量)
                            pic0/pic1-imr|eoi|base|icw1 ioapic lapic-irr|ack|eoi
                            lidt/lgdt gate(门描述符) hostkey kbd-byte kbd-irq
                            aux-byte aux-irq(IRQ12) hostmouse mouse-inj key-inj
                            gatecall int=<向量> ax= bx= cx= dx= ds= es= at=<cs:ip>
                            gateret ax= bx= cx= dx= cf= at=<cs:ip> —— 软件 INT n
                            的两半。参数不止在 AX 里：AH=48h 的容量在 BX、AH=3Dh
                            的文件名在 DS:DX、AH=0Ah 的缓冲在 ES:DX，只报 AX 的
                            一行能说出"内存不够"却说不出客人要了多少；返回值也不
                            止 AX：int 10h AH=0Fh 的页号在 BH，DOS AH=45h 的重复
                            句柄在 AX，而 BIOS/DOS 一律用 CF 报败。两半按调用者
                            CS:IP 配对，不靠顺序，所以夹在中间的硬件中断挪不动
                            配对。只对软件中断发（外部中断与 CPU 抛的异常里 AX 与
                            返回值无关）。QEMU 侧无对照物：它的 -d int 整块在
                            `if (cr0 & PE)` 里且只在入口触发，实模式一条不出、
                            也没有返回值（见 progress.md 片 33）
                            reset-req(设备请求机器复位)
                            fdc-cmd(原始命令字/解码值) fdc-res(结果相位) fdc-irq
                            fdc-rw fdc-xfer(实际搬运字节) dma-xfer(计数寄存器/
                            mode&lt;&lt;8|道号&lt;&lt;4|masked) dma-reg
                            hostserial hostbyte inputopen inputpoll
                            debugcon-tx(客人写调试口 0x402 的字节) uart-tx(客人写 COM1
                            的字节) —— 客人对宿主的两个输出字节流各一行；客人打出的
                            每个字节都能追到引发它的指令
                            vga-mode(宽,高) vga-shapes(AR10图形位|chain4<<1|
                            odd_even<<2|双扫描<<3|移位<<4|平面<<6, 起始偏移)
                            vga-timing(R9|R18<<8|R7<<16, R13|GR6<<8|GR5<<16)
                            —— 显示卡只在**画面变化**时发这三行。客人自己编程
                            卡时（本项目的 Windows 1.01 显示驱动就是不经 int10
                            硬编码寄存器）靠它把整场启动的模式序列读出来
  mouse=DX:DY:BUTTONS[:WHEEL]@N
  key=SCAN[:EXT[:UP]]@N
                            合成宿主输入（探针作为客人按不了鼠标、也按不了键，
                            宿主事件路径只有这一条自动化通道；QEMU 侧的对应物
                            是监视器的 mouse_move/mouse_button/sendkey）。每 N
                            条指令往板级的 sink 送一次：mouse= 给的是位移/滚轮
                            （有符号 C 字面量，十六进制要写 0x）与按键位（bit0
                            左、bit1 右、bit2 中）；key= 给的是宿主报的 set-1
                            扫描码 + EXT（0xe0 前缀）+ UP（释放），也就是窗口
                            键盘交给 sink 的那个三元组。@N 必填：缺省的节奏
                            等于替调用方做决定
  screen                    无帧文本行：客机控制台镜像，**整行输出**（控制台一行
                            80 列，套进事件表只剩列框，call trace 就读不出来了）。
                            一行与上一次渲染相同（已稳定）才发；向上滚动只发进入
                            视口的新行。字形按 CP437 解码，`utf8` 下转成 UTF-8，
                            否则降为空格
  utf8                      UTF-8 边框与字形（默认 ASCII，Windows 代码页安全）
```

- 输出走 stderr，`2>file` 留档；热路径零成本（发射点一个位测试，
  类别掩码 init 时解析一次）。
- 事件表统一列 `KIND │ PC │ RAW │ MNEMONIC │ DETAIL │ FLAGS`（DETAIL 宽 88：
  一行 BIOS 调用要装下 `int/ax/bx/cx/dx/ds/es/at`，宽度不够就等于为它加宽了列
  还是把它截了），KIND：
  I 指令 / L 读 / S 写 / T 陷阱 / B 设备 / W 观测点 / O IR 操作转储 / K 标记
  （K 与 screen 都是无帧行：K 走事件表，screen 走整行文本）。T 行的 DETAIL 在
  访问故障时带 `paddr=`（被访问的物理地址；页表走查本身故障时是 PTE 地址）：
  tval 只给虚拟地址，PMP 拒绝与未映射设备在它上面分不开。
- **PC 列打的是取指地址（线性地址），`->` 用同一条线性约定**：x86 实模式的
  `cpu->pc` 是 EIP；解释器在取指时把 CS base + EIP 记进 `rec.pc_lin`，提交后把
  新 CS base + dnpc 记进 `rec.dnpc_lin`，所以一行里的 `->` 就是下一行的 `pc=`
  （riscv 的提交 pc 本来就是线性的，两个字段等于 pc/dnpc）。`state` 流仍是 ISA
  原始 pc（x86 为段内偏移）+ 段寄存器的自洽基线，不与事件表的线性 PC 混用。
  gdb 的断点/`$pc` 走 `isa_ops.gdb_pc` 同一个线性口径。
- **无帧行（K）的 PC 列打的是已退休指令数**，不是地址：mark/watch/gate 这类行没有
  指令帧可依赖，此前它们不带任何时刻，两行之间无法排序、也无法在下一轮复现。板级
  把 `inst_count` 交给中枢（`DebugSetClock`）后，看到的一行 `inst=N` 就能用
  `--max-inst=N` 重跑到同一刻，会话收尾再按 `dump=` 把客机状态取回来。
- **这些行的地址在 DETAIL 的 `pc=` 里**：设备/中断/输入行都没有指令帧（设备 vtable
  不带 CPU，见 §七），所以解释器每步把当前 `frame*` 交给中枢
  （`DebugSetFrame`，与 QEMU 的 `first_cpu` + `cpu_get_pc()` 同形），无帧行于是能
  说出是**哪条指令**引发了它；配 `hist` 的环形缓冲就得到"谁调的、从哪条指令来"。
  `pc=` 是线性地址，与 `pcshot=` 同一口径。
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
# 设备/端口里出现怪东西，要问"是哪条指令干的"：bus 按端口或设备名收窄，
# 每行自带线性 PC 与本条指令；再接 hist 就得到它之前的整段来路
CEMU_DEBUG="mark,bus:0x402,hist=16" ./cemu.exe ... img 2> d.txt

# 客人对宿主的输出字节（调试口 0x402 / COM1）逐字节归因
CEMU_DEBUG="mark,hist=8,budget=40" ./cemu.exe ... img 2> c.txt   # 看 debugcon-tx/uart-tx 行

# 访问故障（load/store access fault）：trap 行先给 paddr；若总线里明明有
# 这个设备，就是 PMP 拒绝，再用 pmp 看命中的是哪个条目、解码成了什么区间
CEMU_DEBUG="trap,pmp" ./cemu.exe ... img 2> p.txt

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

- **两模型同一条字节流的抓手是固件的 debugcon 口（0x402），不是 COM1。** SeaBIOS 的
  `printf_debug` 同时写 0x402 与 COM1（`seabios/src/output.c` 的 `debug_putc`），
  cemu 侧的 0x402 是 `src/device/misc/debugcon.c` 的 debugcon 设备（读回 0xE9，
  固件靠它探测）。DOS 与 BIOS 打的任何消息两边都能逐字节比。
  QEMU 侧的接法是 `-chardev file,id=dbg,path=X` 加
  `-device isa-debugcon,iobase=0x402,chardev=dbg`；它的 `-debugcon file:X` **单用
  录不到东西**（pc 机器自带的 `isa-debugcon` 是空 chardev）。另有两个坑：
  `-monitor stdio` 的 stdin 管道一到 EOF 就让 QEMU 直接退出（什么都没引导），
  `-icount sleep=off` 会把虚拟时钟在 `hlt` 上冻住、DOS 的空闲循环自锁。
  **这属于仓外的对拍工具，不进仓**：套件里任何一格都不许引导固件/软盘/光盘镜像
  （§七），所以别把它做成测试或提交进 `test/`、`tools/`。
  连带一件对拍的前提：**VGA option ROM 也在这条通道里**，而两个模型默认跑的不是
  同一个 ROM —— QEMU 加载它自己的 `share/vgabios-stdvga.bin`，与
  `seabios/out/vgabios.bin` 不是同一个构建（版本串里写着，`VBE DISPI` 那些字符串
  一个有没有就是判据）。`-fw_cfg name=opt/isa-vga/rom` 不生效、
  `-global VGA.romfile=` 会把 ROM 顶掉到根本不跑；可行的接法是给 cemu 的
  `out/vgabios.bin` 加上 PCI ROM 头（固件侧 `CONFIG_VGA_PCI=y`）再放进 QEMU 的
  `share/vgabios-stdvga.bin`。**对拍之前先核这一条**，否则两边跑的不是同一个 ROM。
  ⚠ **但 ROM 不同不等于原因**：2026-10-02 实测过，把 cemu 自己的 ROM 装进 QEMU
  之后 QEMU 照样 `set VGA mode 10` 进 640×350 桌面，而 cemu 用同一个 ROM 退回
  mode 3 —— 所以 win101 的分歧**在机器上，不在固件构建上**。见 progress.md 片 33。

- 交互式调试器/REPL、反汇编器、GUI 输出立项为阶段 3.5（见 arch.md 阶段计划：
  gdb RSP stub / Win32 显示通道 / 图形调试前端；前端不内置反汇编，反汇编一律
  走外部工具）。在 3.5 落地前 CEMU_DEBUG 仍是唯一调试入口（mnemonic 由译码表给出）。

## X、简化登记（在册的规格缺口台账）

规格行为缺失按阶段恢复。**销账 = 整行删除**：不写"已完成 / 已销账"，不留日期、片号、
过程或解释（过程只进 progress.md）；行内只写还缺什么。只有新功能才加行。

| 编号 | 位置 | 缺失 | 参考出处 | 恢复阶段 | 状态 |
|---|---|---|---|---|---|
| D6 | gdbstub.c 硬件观察点 | **`Z3`（只读观察点）不实现，回空包**：x86 的 DR7 R/W 字段只有 写 / I/O / 读写 三种编码（SDM vol.3 表 17-2），没有"只读"，本机不把它偷偷放宽成"读写"（那会在客人从没要求过的存储上停住）；gdb 因此退回自己的软件观察点。riscv 侧 `gdb_set_watch/gdb_clear_watch/gdb_watch_hits` 三个接缝为 NULL，trigger 模块与 RSP 之间还没有那条通道。另外复位会清掉 DR7 与归属掩码（SDM vol.3 17.2），所以 guest 重启后 gdb 之前装的观察点全部失效，而 stub 的表不会主动收回；客人写 DR0-DR3/DR7 也会收回对应归属（同一份机器状态，寄存器被改写后 stub 的观察点即失效，`z` 不再清客人的槽）；protected-mode 的 `G` 包写 CS 只存可见选择子、不重走描述符，PC 按缓存基址换算（实模式按选择子合成基址；VM86 的 flags 写入不载入 VM，D14） | RSP 规范 `Z/z` 包；SDM vol.3 17.2（DR7 的 R/W 编码）与 17.3.1（数据断点在指令完成后以 #DB 报告）；匹配走 CPU 自己的 DR0-DR7，见 `src/cpu/isa/x86/exec.c` 的 `x86_gdb_set_watch` | riscv 一侧把三个接缝接到 `cpu/isa/riscv64/trigger.c` 的 mcontrol 上；x86 一侧的"只读"没有恢复计划（编码里就没有） | 登记中 |
| D7 | LR/SC 单核预留集 | 无多核冲突语义 | spike 单核同款；规格允许 SC 假失败 | 多核引入时 | 登记中 |
| D11 | htif.c HTIF syscall（dev0/cmd0）报错退出 | 无 fesvr syscall 设备 | fesvr htif_t::handle_syscall | 阶段 5 cesdk | 登记中 |
| D17 | vga.c 渲染与显示通道 | DISPI/VBE（含 BANK、GETCAPS、模式寄存器联动）、链式 256 色（mode 13h 与 VBE 8/15/16/24/32bpp）、EDID（256 字节基础块 + DTA 扩展，QEMU 默认值的实测副本）、行比较/分屏、AR13 水平 pel 平移、按 CRTC 场周期推出的闪烁/光标相位均已实现。剩下：**CRTC R17 bit5 的 64K 地址回绕（AW）未建模**（QEMU 的 vga_draw_graphic 同样不读）；**过扫描边框不渲染**（显示通道发布活动画面，QEMU 也不把它画进 framebuffer）；**CGA 复合 NTSC 伪色未做**（没有宿主可见的监视器类型开关，QEMU 不建模）；光笔/开关感应/外部时钟在本卡上没有对应输入，输入状态 0 只报复位值 | IBM VGA Technical Reference；FreeVGA/VGAREGS；QEMU hw/display/vga.c（vbe_fixup_regs/vbe_update_vgaregs、line_compare、hpel、bank_offset）；seabios vgasrc/bochsvga.{h,c}；EDID 1.4（qemu_edid_generate 的默认 1280x800 两块的实测副本，test/x86/probe/edid_dump.asm 与 vbe_mode/PM 探针） | AW 在需要字模式回绕的客户机出现时；边框在需要模拟过扫描的显示通道出现时；NTSC 在出现宿主可选的复合监视器模式且色表可溯源时 | 登记中 |
| D19 | PIIX3/IDE/IOAPIC/PM | ATA 侧 LBA-48（READ/WRITE SECTORS/DMA EXT，48 位任务文件高字节先写）、READ/WRITE MULTIPLE、SET MULTIPLE 已实现；PM1a 状态/使能/控制（含 GBL_RLS、W1C、SCI_EN）、PM 定时器溢出事件、SCI IRQ9、S5 关机已实现；IOAPIC NMI 交付已实现，ExtINT 由同一条 ISA 线上的 8259 通路承担；BMDMA 已实现。剩下：**IOAPIC 的 SMI/INIT 交付不投递**（本机无 SMM、无第二个 APIC）；**PIRQ 路由字节 0x60-0x63 无消费者**（没有 PCI 设备拉 INTx，IDE 跑兼容模式）；**XBCS 未实现**；**SMBus 主机控制器未实现**；PM 的 S3/S4 睡眠没有唤醒路径；**port 0xF0 的 IGNNE#/FERR 锁存未建模**（CPU 的 FERR# 已接 8259 IRQ13，FNCLEX/FNINIT 清状态即撤销）。ACPI 表（DSDT/FADT）由 SeaBIOS 生成，不属于芯片组模型 | ATA/ATAPI-7 §6.2.2（48 位任务文件序列）、§7.10.4、§8.10；ACPI 4.8（PM1_STS/PM1_EN/PM1_CNT 位段）；PIIX4 datasheet（PMBASE/PMREGMISC）；82093AA datasheet §3.2.4；QEMU hw/acpi/piix4.c、hw/intc/ioapic.c、hw/ide/core.c；test/x86/probe/ata_multi.asm、pm_sci.asm、nmi_ioapic.asm 及对应 calibration | SMI/INIT 在需要 SMM 或多 APIC 的客户机出现时；PIRQ 在出现拉 INTx 的 PCI 设备时；XBCS/SMBus 在出现依赖它们的客户机时；S3/S4 在需要睡眠/唤醒时 | 登记中 |
| D21 | i8042.c 软驱数据线 | 8042 侧的软驱数据线（键盘控制器输出端口上那条给软驱控制器的数据线）没有建模；本机的软驱路径由 8272A + 8237 通道 2 承担，固件不经过这条线 | PC/AT Technical Reference（8042 输出端口）；IBM 8042 数据表 | 需要复现"经 8042 传盘"的硬件时 | 登记中 |
| D32 | uart16550.c 接收侧 FIFO 语义 | `PushRx` 无视 FCR.FE 一直缓存 16 字节：实机 16550 在 FIFO 关闭时只保留 1 字节（RBR），后续字节按溢出丢弃并置 LSR.OE，本机不丢。后果：不开 FIFO 的客户机驱动在本机也能收全突发（sermouse 探针原先正是靠这处偏差通过的），在实机/QEMU 上会丢字节 | 16550D datasheet（FCR.FE、RBR 单字节与 OE）；QEMU hw/char/serial.c `serial_receive1` | 需要精确接收溢出语义的客户机出现时（sermouse 探针已改为开 FIFO，销账无阻塞） | 登记中 |
| D34 | mmu.c Sv39 PTE 高位 | **Svnapot 与 Svpbmt 未实现**：叶 PTE 的 bit63:54（N 与 PBMT）任一位置起即按保留位报 page fault，本机不解释 NAPOT 大页也不解释 PBMT 内存类型。后果要说清楚：`rv64ssvnapot-p-napot` 那格走的是**验收件自带的 skip 分支**（件源 `riscv-tests/isa/rv64ssvnapot/napot.S` 的 `mtvec_handler` 见 store page fault + `mepc=napot_store` + `mtval=MY_VA` 就跳 `pass`），所以它在套件里是绿的，但**没钉任何东西**——64KB NAPOT 区间与 PBMT 属性在本机无验证。别和已实现的那半混淆：PMP 的 NAPOT 区间编码是实现了的（mmu.c 的 PMP 段） | 特权规范 4.3.2（叶 PTE 保留位与 N/PBMT 字段）；Svnapot v1.0、Svpbmt v1.0；QEMU target/riscv 的 RV_PTE_NAPOT 与 pbmt 解码；件源 riscv-tests isa/rv64ssvnapot/napot.S | 出现依赖 NAPOT 大页或 PBMT 的客户机时（当前 Linux 0.11/2.6.34 都不用；riscv 侧 xv6-riscv 验收时若 OpenSBI 要求再核） | 登记中 |
| D35 | riscv64/csr.c、riscv32/csr.c 的 misa 写路径 | `csrw misa` 的掩码 `(val & kMisaSupported) \| kMisaI` 只挡"未实现的位"，不挡"只读位"：客机写 0 能把 S 与 U 位清掉，而清掉之后本机照样进 U 模式、照样按 PTE.U 判权限（状态与自述脱钩）。这两位在规范里是否强制只读**缺手册 §3.1.1 的原文**，未据裁决（先例：DAS 的 18 处分歧按实机、计数器抑制只实现有测试覆盖的一侧——这里两侧都无证据，故不猜） | 特权规范 3.1.1 misa 的 WARL 措辞（原文待核）；dearchap-tinyemu riscv_cpu.c:1306 只组装 misa、无 csrw misa 路径可参照 | 核到手册原文后：若 S/U 强制只读，则把这两位从可改掩码里摘出并补一个自足探针（写 misa 后读回） | 登记中 |
| D36 | riscv64/csr.c、riscv32/csr.c 的 MHPM 计数器 | **MHPM 计数器只存储不自增**：`mhpmcounter3..18` 在 cemu 里没有事件源，写进去的值能读回，但从不增长；`mhpmevent3..18` 同样只存储。mcycle/minstret 不受影响（step.c 每步自增）。OpenSBI 按 CSR 存在探测计数器数量，拿到的是"能读但不动"的计数器 | QEMU target/riscv/csr.c 的 pmu 计数（Zihpm 允许实现定义事件）；特权规范 3.1.11 | 出现依赖性能计数器/剖析的客户机或工具时 | 登记中 |
| D38 | x86 窗口指针（PS/2） | **PS/2 鼠标只有相对计数，没有绝对包**，所以 x86 窗口把宿主指针的位移按相对增量投给 PS/2（与 QEMU 对 PS/2 的做法一致），不做板级绝对换算——回零/追赶式换算假设客人 1:1 应用计数并自己钳边，客人驱动一缩放或 warp 光标就被放大成光标乱窜（Windows 1.01 实测）。缺的是**指针抓取/隐藏**：光标顶到窗口边缘后宿主指针还能继续外移，两者幅度会脱节；客人光标是否与宿主指针重合取决于它自己的驱动。绝对定位只留给原生绝对设备（virtio-tablet、Oberon） | PS/2 鼠标协议（相对计数，9 位二补码）；QEMU 对 PS/2 用相对 + 指针 grab，绝对输入另走 USB tablet/vmmouse；真实绝对需要客人驱动 | 需要窗口指针与客人光标严格重合时（实现指针抓取/隐藏，或引入支持绝对包的输入设备） | 登记中 |
| D39 | x86 后端（exec.c / fpu.c / x86.h） | 已实现 AC 对齐检查（#AC 在页走查之后、只查 2/4/8 字节 CPL-3 访问）、PSE-36、SYSENTER/SYSEXIT、MMX/SSE/SSE2、FXSAVE/FXRSTOR、MTRR/PAT、MCE 寄存器组、架构性能计数器（0x3C/0xC0）、RDTSC/RDTSCP、CR4 WARL 与 CPUID 广告（EDX 含 FPU/MMX/FXSR/SSE/SSE2/APIC/PSE36/PAT/MTRR/MCE/SEP/TSC，leaf 0x0B 报单线程/单核拓扑）。剩下：**VME/PVI 的软件中断重定向位图未实现**（CLI/STI/PUSHF/POPF/IRET 的 VIF/VIP 与硬件中断门控已在；未广告 CPUID.VME）；**性能计数器只计数 0x3C/0xC0，其余 event select、PEBS/PMI 未实现**；长模式另立项 | SDM vol.2/vol.3（20.3 VME、21 PVI、19 PMU）；QEMU target/i386；v86 cpu.rs/instructions_0f.rs；`test/x86/probe/sys686.asm`、`sse.asm`、`ac.asm`、`pse36.asm` | VME/PVI 位图在出现依赖它的 V86 客户机时；PMU 其余事件在出现剖析工具时 | 登记中 |
| E1 | fp.c 用宿主 float/double/long double 模拟 IEEE | 偏离参考：QEMU/spike 用 Berkeley softfloat；宿主 long double 有 x87→float 双舍入长尾风险 | QEMU fpu/softfloat.c（BSD） | Linux 阶段出现浮点偏差时移植 softfloat | 登记中 |

## 十一、显示窗口指针契约（所有主板）

窗口的指针按设备**原生语义**投递，不做"宿主绝对位置"的强行换算：

- 原生绝对设备（virtio-tablet、Oberon 鼠标寄存器）：指针在画面矩形内时把客户区坐标
  映射成**源画面像素**（X 向右、Y 向下）直接发出；画面外的黑边钳到最近的边缘，绝不
  发画面外坐标；指针离开客户区完全停发；按住键拖出客户区时移动停发，但**释放事件用
  最后一次有效画面位置发出**，避免客人卡住按键。
- 相对设备（x86 PS/2、`-mouse serial`）：把宿主指针的**位移**按相对增量投递，不做
  回零/追赶式绝对定位。PS/2 协议里没有绝对包，换算模型会被客人驱动的缩放与钳位放大
  （Windows 1.01 上表现为光标乱窜）；错位边界见 D38。
- 画面矩形按 Blit 的 fit/letterbox 规则**每次事件重算**，窗口缩放/模式变化都不缓存几何
  （原"先固定窗口大小"的待做项直接实现，不留待做）。
- 每个移动/按键/滚轮包以 `EV_SYN`/`SYN_REPORT` 结束（virtio-input 路径）。
- 设备层：virtio-tablet 与 Oberon 鼠标寄存器原生绝对（`src/board/virt.c`、
  `src/device/misc/oberon_io.c`）；x86 PS/2 是相对设备。
- `CEMU_DEBUG=mouse=...` 的合成事件走相对 sink（`Board.mouse_in`）；窗口事件按设备走
  绝对 sink（`Board.mouse_abs`）或相对 sink（`Board.mouse_in`）。

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
  `kbus_load/kbus_store/krd*/kwr*/kpush*`，由 `lin_load/lin_store` 的 user
  参数分派（kbus_* 固定 user=0；`page_translate_as` 是慢路径的走查入口）。

- **PMP 零活动规则放行**：从未配置任何 PMP 条目（全部 A=OFF）时，S/U 访问
  放行（QEMU pmp.c `pmp_hart_has_privs` 的零规则早退；gem5 pmp.cc 与手册
  3.7 字面"实现过 PMP 但全 OFF 则 S/U 访问失败"分歧，按 QEMU）。实机依据：
  裸机 loader（HelenOS 0.14.1 riscv64）不配 PMP 直接 mret 进 S 态，
  首次取指与页表走查即按 S 态过 PMP，在 QEMU -M spike（cemu spike 板的
  机器参照）上正常引导。规则已配置但无一命中时仍拒 S/U 不变。

- **非叶 PTE 的 A/D/U/G 不拒**：手册 4.3.2"非叶 PTE 的 D、A、U 位必须
  清零"约束的是建表软件，不是走查硬件——QEMU ptw 忽略这些位。实机 OS
  就这么建表：HelenOS 0.14.1 riscv64 的指针 PTE 实测 `root[256] =
  0x101038d1`（V|U|A|D），取证走 pcshot 取 satp + 确定性回放 dump 根表。
  xiangshanNEMU 此处拒——其客户 OpenSBI/Linux 不置这些位，该检查未经
  校准，按 QEMU 放宽（riscv64/riscv32 两个后端同步）。

- **x86 A/D 在 TLB 填充时置位**：命中不再回写页表（QEMU softmmu 同款）。读填充
  置 A，写填充置 A|D；修改 PTE 后必须 INVLPG 或重载 CR3 才能让新权限/新映射
  生效，这与 SDM 对软件的要求一致。验收：pm_smoke t17（4KB）、t25（4MB）、
  KUT access（2305 组合）与 paging32 t3（PAE 4KB）；TLB 的读/写槽分开，否则
  一次读填充会让后续写永远不置 D。

- **x86 巨大页的 TLB 失效按线性范围**：PAE 2MB 与非 PAE 4MB 叶都按 4KB 槽缓存，
  但每项记录 `lin_base/page_shift`；INVLPG 任一子页要清掉整张巨大叶派生的所有
  槽。验收：test/x86/probe/paging32.asm 的 t2b（重映射 PDE[4] 后 INVLPG 一个
  4KB 子页，另一个子页的旧缓存也必须消失）。

- **x86 慢路径优先**：`CEMU_DEBUG=mem|bus|watch=` 任一开启时 `g_mem_slow=1`，
  `lin_load/lin_store` 退回逐次走查 + `phys_load/phys_store` 事件路径，保证
  观测流与 TLB 引入前一致；`trace/state` 不改变访存路径，只打开 `g_record_raw`
  与 `DebugInsn` 门控。

- **x87 CR0.NE=0 的 FERR#**：未屏蔽 x87 异常在 NE=0 时拉高 FERR#，板级接到
  8259 的 IRQ13；FNCLEX/FNINIT 清异常状态即撤销（电平由状态字导出，不另设
  锁存）。CR0.NE=1 仍走 #MF。port 0xF0/IGNNE# 见 D19。
  验收：`test/x86/probe/x87.asm` 的 IRQ13 项。

- **APIC CPUID**：LAPIC ID 为 0（BSP），leaf 1 EDX.APIC=1、EBX=0x00010800
  （APIC ID 0、1 逻辑处理器、64B CLFLUSH），leaf 0x0B 依次返回
  SMT(type 1, level 0)、core(type 2, level 1)、终止子叶；与
  `device/intc/lapic.c` 的 ID 寄存器一致。
  验收：`test/x86/probe/sys686.asm`。

- **16 位 LGDT/LIDT/SGDT/SIDT 只带 24 位基址**：操作数为 16 位时，基址按
  SDM vol.2 各指令的 Operation 截到低 24 位（32 位操作数不截）。SeaBIOS 的
  int 15h AH=87h 把调用者 GDT 的 1 号数据描述符当伪描述符用——该 6 字节的
  第 5 字节是访问权 0x93，装 32 位会把 GDTR 基址变成 0x93076c34，切换保护
  模式后的 `ljmp $0x20` 随即 #GP（DOS7 的 IO.SYS 一启动就走这条服务）。
  v86 的 `instr16_0F01_*` 用 0xffffff 掩码。验收：`test/x86/probe/lgdt16.asm`
  （双向各测 16/32 位，QEMU 同 transcript 同 status）。
