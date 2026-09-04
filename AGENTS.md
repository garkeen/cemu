# cemu 开发准则（agent 必读）

违反任何一条即为 bug，发现即修复或登记到本文"简化登记"节，不允许先留着。

本文是原 开发准则.md、重构要求.md、调试方案.md、准则审查.md 四份文档的合并版
（2026-09-03 重组；准则审查的历史登记项已按当前代码逐条核实销账，仅存活的
登记项迁入本文第 X 节）。

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
| `device/` | 外设，按类分：`char/` `timer/` `intc/` `misc/` |
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
- 构建：CMake（`CMakeLists.txt`）+ Ninja 生成器，MinGW gcc，`-Wall -Wextra`
  零告警。源文件用 `file(GLOB_RECURSE src/*.c)`（`CONFIGURE_DEPENDS`，新增/
  删除源文件自动重扫），是旧 Makefile 四级显式通配（`src/*.c` … `src/*/*/*/*.c`）
  的超集，不依赖 glob 尾部斜杠行为。头文件依赖由 CMake/Ninja 自动追踪。
  流程：`cmake -G Ninja -B build && cmake --build build`（ninja 不在 PATH 时
  前置 `PATH=/d/ninja:$PATH`）。
- 每轮改动结束前限时跑回归：`bash test/run.sh`（分层入口：riscv
  `bash test/riscv64/run.sh`，x86 `bash test/x86/run.sh`）；依赖边用
  `cmake --build build --target check`。
- 测试超时：任何测试套件/单测都必须有 time limit（run.sh 用 `timeout`
  包裹 cemu 调用），防止被测程序死循环导致不退出的挂起。
- 对比数据体积：凡需落地对比数据的测试（如导出万步状态流做对拍），其
  数据/捕获文件不得超过 5MB；超出即视为测试设计违规，须改用采样、截断或
  CEMU_DEBUG `budget=` 抑制等手段收敛体积。

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
  bus                       MMIO / IO 端口命中
  regs=N                    每 N 条指令打一次全寄存器表；halt/exit 必打
  watch=ADDR:SIZE[:r|w|rw]  地址观测（可重复逗号分隔）
  budget=N                  单类别事件上限，超出抑制并计数，退出时汇总
  utf8                      UTF-8 边框（默认 ASCII，Windows 代码页安全）
```

- 输出走 stderr，`2>file` 留档；热路径零成本（发射点一个位测试，
  类别掩码 init 时解析一次）。
- 事件表统一列 `KIND │ PC │ RAW │ MNEMONIC │ DETAIL │ FLAGS`，KIND：
  I 指令 / L 读 / S 写 / T 陷阱 / B 设备 / W 观测点 / O IR 操作转储。
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
```

- 交互式调试器/REPL、反汇编器、GUI 输出明确排除（mnemonic 由译码表给出，
  够用；单独立项再说）。

## X、简化登记（原准则审查 D/E 表，2026-09-03 按代码逐条核实后仅存存活项）

规格行为缺失按阶段恢复；销账时在代码落地并在本表勾掉。

| 编号 | 位置 | 缺失 | 参考出处 | 恢复阶段 | 状态 |
|---|---|---|---|---|---|
| D6 | csr.c tdata1/2/3 WARL 存储无触发匹配 | debug trigger（gem5 tselect 写 val+1 报告存在 trigger） | xiangshanNEMU trigger.c | gdb stub 时 | 登记中 |
| D7 | LR/SC 单核预留集 | 无多核冲突语义 | spike 单核同款；规格允许 SC 假失败 | 多核引入时 | 登记中 |
| D11 | htif.c HTIF syscall（dev0/cmd0）报错退出 | 无 fesvr syscall 设备 | fesvr htif_t::handle_syscall | 阶段 3 cesdk | 登记中 |
| D13 | x86 LMSW/INVLPG/RDTSC/CMPXCHG/CMPXCHG8B/x87 FPU 判非法 #UD | 386 子集外指令与 FPU 未实现 | intel SDM vol.2；QEMU translate.c | 阶段 4 全 x86 | 登记中。现实表现：realmode 尾 test_fninit #UD→垃圾 IVT[6]→死循环，run.sh 以 timeout+输出判据容纳 |
| D14 | x86 iret/popf 载入屏蔽 RF(bit16)/VM(bit17) | RF 瞬态建模（真机不可观测为 1，等价）；VM86 不进入 | intel SDM EFLAGS | 阶段 4（VM86 时） | 登记中 |
| E1 | fp.c 用宿主 float/double/long double 模拟 IEEE | 偏离参考：QEMU/spike 用 Berkeley softfloat；宿主 long double 有 x87→float 双舍入长尾风险 | QEMU fpu/softfloat.c（BSD） | Linux 阶段出现浮点偏差时移植 softfloat | 登记中（先加 softfloat 测试向量回归对照） |

已销账（历史存档，无需再管）：D1-D5（S 态机制/sret/wfi/sfence/time 真实
计时源/PMP 全部随阶段 2 virt+OpenSBI 落地）、D8（中断优先级表 csr.c:77）、
D9（ELF32，elf.c 已双支持 ei_class 1/2）、D10（HTIF fromhost 应答语义）、
D12（PIC+PIT+hlt 唤醒，realmode 122 PASS 达成过）、E2/E3（CSR 表驱动化与
mepc WIRI 随重构落地）。

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
