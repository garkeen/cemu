# cemu 当前状态（progress.md）

本文是原 任务与计划.md 的状态部分，按轮次记录。架构与路线图见 arch.md；
开发铁律见 AGENTS.md。最近的记录在最上。

## 阶段 4 片 19：access 的 pde.bit13 轴 —— largepage PDE 保留位（D31 销账）（2026-09-23）

起点是审 D31 那条台账（"flag 空间 27 位收到 11 位、2^27 不可跑"）。审下来发现两件事。

**1. 那条台账的论证基础不成立**

实测 access 全套 **0.24 秒**（466,981 条指令 / 1537 条用例 ≈ 304 条每例）。而"全空间不可跑"
的依据是片 16 定边界时那 70,735,171 条指令 —— 那个数字来自 769 个假失败（片 17 修掉的
空指针 UB），修掉后同一套降了 **151 倍**：收窄的运行时间前提，在定下它的第二天就没了。

更要紧的是 16 个被删的 bit 分两类，台账把它们混成了一类：

- **14 个本机不存在的轴**（NX×3、bit51×2、PKU×3、SMEP、PKE、access_twice）：32 位非 PAE
  机器没有这些硬件，删掉是对的，不是"为省时间抽样"。说明移进 `kut_access.c` 的头注释
  （源码即范围声明），不再挂账。
- **pde.bit13**：这个轴**适用**本机 —— 4MB 页的 PDE 把帧放在 bits 31:22，bits 21:13 是保留
  位，置位即 #PF（SDM vol.3 fig 4-3 把 bit 21 标 "RSvd"、20:13 标 "Reserved"，§4.3 要求
  为 0；本机 CPUID 无 PSE-36，不会把 20:13 提升成地址位）。上游 access.c 的注释写明
  "pde.bit13 checks handling of reserved bits in largepage PDEs"。而 cemu 的 4MB 叶分支
  **根本没有保留位检查** —— 注释承认了这件事却没做检查，且移植时这个轴被静默删掉、没进
  台账（按 §二 属未登记简化）。

**2. 补上检查与轴**

- `exec.c` 的 4MB 叶分支在权限检查**之前**加保留位检查（`kPde4mReserved`，定义在 x86.h，
  出处 fig 4-3/§4.3）。386 类错误码没有 RSVD 位，故障就是普通的 present 故障（`code | 1`）
  —— 这正是上游模型里 `PFERR_RESERVED_MASK` 在本机落不下去的原因。
- `kut_access.c` 恢复 `AC_PDE_BIT13_BIT`：枚举、mask、`ac_test_legal`（无 PSE 时该轴无意义，
  上游原话）、`ac_test_setup_pte` 置 bit13、`ac_emulate_access` 的 `pde_valid` 折入
  `!F(AC_PDE_BIT13)`（present 时 P 位保持），并进 flag 名表。

**验证**：access **2305 tests / 0 failures**（+768 条，0.42 s）。反证：抽掉 cemu 的检查后
同一镜像 **256 failures**（直方图全部带 pde.present）⇒ 轴有牙。

**3. 4MB 叶的 A/D 更新补覆盖（pm t25）**

审 A/D 那条时发现替代理由只对一半：pm t17 覆盖 4KB 走表（PTE.A/D + PDE.A），而
`check_large_pte_dirty_for_nowp` 只测"WP=0 写只读 4MB 叶不故障" —— `ac_test_setup_pte`
无条件预置 A|D（331/342 行），所以 access 里 A/D **更新**从不发生（值本来就等于期望值）。
4MB 叶的 `new_pde = pde | A | D` 写回因此零覆盖。新增 pm t25：CR4.PSE 置位、PDE[0] 换成
A=D=0 的 4MB 叶，读一次要求 A=1/D=0，再清 A|D 写一次要求 D=1。失败分支跳自己的 `t25_end`
（跳 `pg_done` 会反向重入 t25 死循环 —— 第一版踩到过）。反证：把 4MB 叶的 A/D 写回改成
`new_pde = pde` 后 t25 BAD。

**QEMU 标定**：`test/x86/pm/pm_qemu3.txt` —— 25 格里 23 格与 cemu 逐字节一致，两处分歧仍是
t7/t20（QEMU TCG 不执行数据段限长检查，见 run.sh 注释）；t25 两侧都过。run.sh 的
`expected_pm` 24 → 25。

**顺带**：重建 kvm 镜像时发现 taskswitch/taskswitch2/cmpxchg8b/memory/debug 五份是 09-19
入库的，二进制里嵌着**旧路径** `D:/code/c/TinyEMU/...` 的 `__FILE__` 串（片 16 把 KVM_UT
改到 EMU/ 之后没重建）。一并重建，六份镜像现在路径一致。

**回归**：riscv64 136/0、x86 14/0、depcheck ok、零告警。

**台账**：D31 整行删除。

## 阶段 4 片 18：taskswitch2 转绿 —— SLDT/STR 的 32 位内存写 + VM86（D14 销账）（2026-09-23）

起点是片 17 留下的唯一红格：`taskswitch2` 的 `FAIL: PF exeption` 与它撞上的
`[fatal] VM86 not implemented (D14)`。

**1. `FAIL: PF exeption` —— SLDT/STR 把内存目标写成 32 位（已修）**

用 `CEMU_DEBUG=watch=0x44a9cc:4:rw` 盯现场那个错误码槽位（地址取自实测的
`pt_regs`），31 条命中里最后两条说明一切：

```
| W | 0x401495 | 0f 00 4d fa | str | w 44a9ca:4 <- 20 |   ← print_current_tss_info 的 str
| W | 0x4002e7 | ff 36       | push| r 44a9cc:4 -> 2  |   ← do_pf_tss 读错误码
```

`0f 00 4d fa` 是 `str -0x6(%ebp)`，目标是 `u16`，本该只写 2 字节（0x44a9ca/0x44a9cb），
却写了 **4 字节**，把上方两字节（0x44a9cc/0x44a9cd，正是任务门投递压在中断栈上的
#PF 错误码槽）清零 ⇒ 紧随的 `cmpl $0x2,(%esi)` 读到 0 ⇒ `test_count++` 不执行 ⇒ FAIL。
（`printf` 时读到 2、`cmp` 时读到 0 的矛盾，就是这么来的；中间只隔了一个
`print_current_tss_info`。）

SDM vol.2 STR 原文："When the destination operand is a memory location, the segment
selector is written to memory as a **16-bit quantity, regardless of operand size**"；
SLDT 同款。32 位寄存器目标才零扩展。cemu 原来按 `d.w32` 分派，内存目标走了
`set_rm32`。修：新增 `set_sys_sel`（内存恒 16 位；32 位寄存器零扩展）。顺带补齐同组
的模式限制（SDM vol.2 各页）：SLDT/STR/VERR/VERW/LAR/LSL 在实模式与 VM86 下 #UD、
LTR 在实模式 #UD 且 CPL≠0 时 #GP(0)（LTR 原来既无 PE 检查也无 CPL 检查）。

**2. D14：VM86（已实现，台账该行删除）**

按 SDM vol.3 17.3 与 vol.2 各指令页实现，参考 QEMU `do_interrupt_protected` 与
tiny386 `set_seg`：

- **进入**：IRET 在 CPL 0 且栈上 EFLAGS 映像 VM=1 时走 `RETURN-TO-VIRTUAL-8086-MODE`
  （弹 ESP/SS/ES/DS/FS/GS 整套 VM86 帧，CPL→3）；任务切换从 TSS 映像装载 VM
  （VM86 任务的 CS/SS 是实模式选择子，不查描述符）。
- **段语义**：`seg_synth` —— 实模式与 VM86 都由选择子合成缓存（base = sel<<4、64K 限、
  16 位、DPL 3；VM86 的 CPL 3 就从这里被 `cpl()` 读到），且不写描述符的 A 位。
  段装载（load_data/load_ss/load_cs）、far 控制流（pm_far / RETF）在 VM86 走实模式那条路。
- **投递**：从 VM86 进 ring 0 处理器时按 SDM 17.3.3 压 VM86 帧（GS FS DS ES 在
  SS/ESP 之上，EFLAGS 映像保 VM=1），活动 EFLAGS 清 VM。
- **VM86 内的 IRET**：IOPL=3 时只弹 EIP/CS/EFLAGS（IOPL/VM 不从映像装载），IOPL<3
  时 #GP(0) 陷阱到监视器。
- **POPF/PUSHF**：按 SDM vol.2 POPF/PUSHF Operation 的整张模式表实现（CPL 与 IOPL 决定
  哪些位可改；VM86 无 VME 时 POPF 需 IOPL=3 否则 #GP，PUSHF 同；映像不携带 VM/RF）。
- **I/O 特权**：`io_allowed` —— 实模式恒允许；CPL≤IOPL 允许；其余（含 VM86，该模式
  忽略 IOPL）查 TSS 的 I/O 许可位图（SDM vol.2 IN/OUT Operation）。CLI/STI 的 IOPL
  检查同批补上。

**3. 回归打回的两格暴露了一个潜伏 bug（已修）**

POPF 按 SDM 改成"保留位不受映像影响"后，`realmode` 的 `DAS` 与 `sahf` 转红 —— 它们用
`pushw <flags>; popfw` 装载初始标志，再比对结果标志的低字节。根因不是 POPF：是 cemu
的 EFLAGS **保留位（bit 3/5/15）会被历史镜像写成 1**（旧 POPF/IRET 把映像的保留位原样
装进寄存器），而硬件上它们恒读 0（SDM vol.1 3.4.3 表 3-1）。修法落在不变式上而非测试
上：`step.c` 的单点提交处把 bit 1 置 1、bit 3/5/15 清 0（`kEflagsOne`/`kEflagsZero`，
定义在 x86.h），任何指令装载的映像都无法破坏它。

**验证**：`taskswitch2` **11 PASS / 0 FAIL**（含 `PF exeption` 与 `VM86`）；
`test/run.sh`：riscv64 **136 / 0**、x86 **14 / 0**（x86 从 13 格增到 14 —— taskswitch2
由红转绿）；`cmake --build build --target check` 依赖边 ok。

## 阶段 4 片 17：测试判据收紧 + 一批被掩盖的真失败（2026-09-23）

起点是 `access` 首跑 769 个假失败（见片 16 末尾），顺着往下挖，暴露出**判据本身太弱**
这个更大的问题。

**1. access 的 769 假失败 —— 测试自己的 bug（已修）**

直方图显示失败全部落在 `pde.pse`（768/768），且 769 条 FAIL 报的都是同一个检查
（"pte 80cf0 expected 800067"），没有一条是 "unexpected fault/access"、"error code" 或
"pde"。根因：PSE 用例没有 PTE（叶就是 PDE），`at->ptep` 为 NULL，本该跳过 PTE 比较；但
上游把判据写成 `at->ptep && *at->ptep != at->expected_pte, "pte %x ...", *at->ptep`
——`*at->ptep` 作为可变参数被**无条件求值**，而空指针解引用是 UB，于是 GCC -O2 认定
ptep 非空、删掉 `&&` 保护并把 load 提前。每个 PSE 用例都去读**线性地址 0**（实模式
IVT），拿到常量 `0x80cf0`，与上一用例残留的 `expected_pte`（`0x800067`）比较 → 假失败。
修法：把读取放进显式守卫（`actual_pte = at->ptep ? *at->ptep : at->expected_pte`），
生成代码核对为 `cmovne`。修后 **1537 tests / 0 failures**，指令数 70,735,171 → 466,981。

**副产品**：769 条 FAIL 里没有一条是缺页判定、错误码或 PDE/PTE 更新 —— cemu 的
`page_translate_as` 在 1536 个合法组合上**全部正确**，片 15 的修复站得住。

**2. LAPIC 缺 TMR（原 D32，已实现，台账该行删除）**

`x86/ioapic.c` 的 4 个 TMR 用例读 `APIC_TMR`（0x180 区）。QEMU `hw/intc/apic.c` 是权威
参考：`apic_set_irq()` 在**投递时**按 trigger 位设置/清除 TMR（`apic_set_bit/reset_bit`），
`apic_eoi()` **只读**它（用来决定是否向 I/O APIC 广播 EOI），从不清除。据此实现：TMR 在
`LapicDeliver` 按投递的 trigger 位写，EOI 读它来决定是否广播 `IoapicEoi`；顺带把
ISR/TMR/IRR 三个向量集寄存器做成可读（0x100/0x180/0x200，8 个寄存器 × 16 字节步长，
每寄存器 4 字节）。触发模式从重定向项的 trigger 位一路 plumb 到 `LapicDeliver`
（`IoapicSetDeliverSink` 的 sink 多一个 trigger 形参）。

**3. LAPIC 优先级方向反了（已修）**

`HighestVector` 取的是**最低**置位向量，而 QEMU `get_highest_priority_int` 从最高字往下
扫、取该字最高位 —— 即**高 vector 优先**；`x86/ioapic.c` 的 `ioapic simultaneous edge
interrupts` 也要求 0x78 先于 0x66 被服务（`g_66_after_78`），两边一致。修后该用例通过。

**4. IOAPIC 仲裁寄存器（已修）**

索引 0x02 是 ID 的只读镜像（82093AA §3.2.2），原来恒返回 0。写路径本就忽略它，只补了读。

**5. 宿主侧失败退出码与「通过」撞码（已修）—— 这是本轮最重要的一条**

客机状态经 debug-exit 以 `(value << 1) | 1` 上报，**恒为奇数**；而 `Fatal` 与 main.c 的
宿主错误路径原来都 `return 1` —— 与「客机报 0 失败」的通过码**完全相同**。后果：
`taskswitch2` 撞上 `[fatal] VM86 not implemented (D14)` 后退出 1，被 `run.sh` 判成**通过**，
而它此前已经打出 `FAIL: PF exeption`。新增 `kExitHostFailure = 2`（`util/log.h`，偶数码
永不可能与客机状态相同），`Fatal` 与 main.c 全部宿主错误路径改用它。自检：缺文件现在
rc=2。

**6. `run.sh` 的 kvm 判据收紧（已修）**

原来只看 `rc -eq 1`。改为 `rc -eq 1` **且** 客机输出里没有 `FAIL`（`XFAIL` 除外 ——
`memory` 的 clflush/sfence/… 是套件自己的「预期失败」，其总结行是
"8 tests, 7 expected failures"）也没有 `[fatal]`。理由写在脚本注释里。

**7. `rmap_chain` 移出构建（已修）**

它从 `0xfffffa000`（64 位地址，32 位下截断成 0xffffa000）开始按 fw_cfg 的 RAM_SIZE 循环
装页，32 位机上前者回绕过 4GB、把正在构造的页表覆盖掉 → 三重故障。它在本机不可能有意义
地跑，故从 `build_kut.sh` 移除（`ioapic` 同时接进 `run.sh`）。

**回归（本轮实测）**：riscv64 **136 passed / 0 failed**；x86 **13 passed / 1 failed** ——
红的只有 `taskswitch2`，且原因由脚本直接打印出来。

**未完成：`taskswitch2` 的两处（本轮已定位，未修）**

- `FAIL: PF exeption`。用 `watch=` 盯 `test_count`（0x449820）证明：该用例把变量清零后、
  在 report 读它之前，**全程没有任何一次写命中**。而 `do_pf_tss` 的
  `printf("PF task is running %p %lx", error_code, *error_code)` 打印出的错误码是 `2`
  （正确），紧跟的 `cmpl $0x2,(%esi)` 却没走分支，于是 `incl 0x449820` 从未执行。即
  **printf 时该字是 2、cmp 时不是**，而中间只有 `print_current_tss_info()`。错误码的
  地址（handler 入口 esp）打印为 `0x44a9cc` —— 需要把 `do_int` 的任务门分支与
  `do_task_switch` 的压帧路径（`has_ec` 只压错误码，未见返回帧 EFLAGS/CS/EIP 的压入）
  对着 SDM vol.3 §7.2.1 走一遍才能定案。
- VM86（D14）：用例 `test_vm86_switch` 靠 NT 置位 iret 做任务切换到 VM86 任务，断言只是
  `report("VM86", 1)`（只要切过去再回来即算过），但进入 VM86 被显式 `Fatal` 拦住。
  实现 VM86 模式是 D14 那个已决策的取舍（"Linux 不需要"），本轮没动。

## 阶段 4 片 16：补 x86 页权限测试覆盖（access 的 32 位移植）（2026-09-22）

片 15 的 bug 能漏掉，根因是**测试覆盖缺口**，而且缺得很具体：`test/x86/pm` 的 t16 测
ring-0 写只读页（CR0.WP=1 → #PF(3)）、t18 测 ring-3 写 **U=0** 页（#PF(7)），**唯独没有
"ring-3 写 U=1、W=0 的页"这一格**；5 个 kvm 用例也都没有。上游 kvm-unit-tests 里覆盖这
一格的是 `x86/access.c`（页权限表驱动测试，oracle 的 `kwritable = !CR0.WP && !user`
就是 SDM vol.3 §4.6 表 4-2 的直译），但它**只给 x86_64 编**——`x86/Makefile.i386` 里
`access.flat` 被注释成 "These tests from Makefile.x86_64 don't compile"。

**`test/x86/kut_access.c`**：access.c 的 32 位移植（与 `kut_debug.c` 同一先例，登记为
AGENTS.md D31）。去掉本机不存在的轴（NX/bit51/PKU/SMEP）与 KVM-MMU 专属项，走表改两层
（PDE/PTE），访问蹦床改 32 位寄存器，枚举从 27 位收到 11 位（2048 组合）。oracle、逐例
判据、汇总行都是上游原码。**移植中自己抓到一处真 bug**：上游 `PT_INDEX` 是 4 级走表的
位移（level2 右移 21、掩 511），32 位非 PAE 的 PDE 覆盖 10 位，必须右移 22、掩 1023
——照抄会把 `0x40000000` 算成 PDE[0]，覆盖掉镜像自身的映射。已在反汇编上核对（位移 22
出现 14 处、掩 1023 出现 7 处、掩 511 为 0）。

**(c) i386 清单剩下 4 个用例的结论：只有 1 个能跑。**

- `rmap_chain` 编入（编译零错，fw_cfg 有 RAM_SIZE）。
- `ioapic` **不能跑**：中断注入走 kvm-unit-tests 的测试设备端口 `0x2000+line`，cemu 只有
  QEMU 的 `0xF4` debug-exit，没有该设备。要跑得先给机器加这个设备（属新设备，另议）。
- `apic` / `tscdeadline_latency` **不能跑**：都要 `rdtsc` + `MSR_IA32_TSCDEADLINE`
  (0x6E0)。cemu 是 i686 无 TSC 模型（D13，`rdtsc` 为 #UD），MSR 表只有
  0x1b / 0xc0000100-1 / 0x1a0。这是模型不是 bug，塞进套件只会是噪音。

**复现性发现（未处理）**：本机重跑 `build_kut.sh` 会改动 5 个已入库镜像的字节
（taskswitch 差 1049 B、memory 1504 B、debug 1648 B、cmpxchg8b 5539 B 且小 16 B、
taskswitch2 6650 B 且小 16 B）⇒ 该 harness 在本机**不是逐字节可复现的**（clang 版本或
v86 检出相对入库时已变动）。本轮把这 5 个还原，只留新增的 access.elf / rmap_chain.elf。

**验证（实跑）**：`access` 首跑 **1537 tests / 769 failures**，直方图显示失败**全部**落在
`pde.pse`（768/768），且 769 条 FAIL 报的都是同一个检查 —— "pte 80cf0 expected 800067"。

**根因是测试自己的 bug，不是 cemu 的**：PSE 用例没有 PTE（叶就是 PDE），`at->ptep` 为
NULL，本该跳过 PTE 比较。上游把判据写成

    at->ptep && *at->ptep != at->expected_pte, "pte %x ...", *at->ptep

但 `*at->ptep` 同时作为可变参数被**无条件求值**，而解引用空指针是 UB，于是优化器有权
认定 ptep 非空、把 `at->ptep &&` 这个保护删掉并把 load 提前。GCC -O2 正是这么做的：每个
PSE 用例都去读**线性地址 0**（实模式 IVT），拿到那里的常量 `0x80cf0`，与上一用例残留的
`expected_pte`（`0x800067`）比较 → 假失败。修法是把读取放进显式守卫（先算
`actual_pte = at->ptep ? *at->ptep : at->expected_pte` 再比较），已核对生成代码为
`cmovne` 守卫、不再无条件解引用。修复后 **1537 tests / 0 failures，rc=1**，指令数从
70,735,171 降到 466,981（769 次失败映射 dump 消失）。

**副产品结论**：769 条 FAIL 里没有一条是 "unexpected fault" / "unexpected access" /
"error code" / "pde"，即 cemu 的 `page_translate_as` 在 **1536 个合法组合上全部正确**
（缺页/不欠页、错误码、PDE/PTE 更新都对），片 15 的修复站得住。

**验证（其余）**：`build_kut.sh` 全绿（8 个镜像）；`kut_access.c` 单独编译 `-Wall
-Wextra` 零告警（唯一告警来自上游 `processor.h` 的 sign-compare）。`rmap_chain` 的
triple fault（发生在 `cr3=44d000 / cr4=10` 之后、第一条报告之前）未查。

**补：kvm-unit-tests 测试设备（`device/misc/testdev.c`）** —— 为了让 `ioapic` 能跑。
`x86/ioapic.c` 靠 `out` 到端口 `0x2000 + line` 注入 IRQ，cemu 原本只有 QEMU 的
`0xF4` debug-exit，没有这个窗口。新设备照参考实现 v86 `src/cpu.js`（"only for
kvm-unit-test"：`i` 从 0 到 0xF，写 `0x2000+i` 非 0 抬线、0 降线，三种宽度同一个处理
器）实现，接到主板既有的 ISA IRQ 扇出（`OnIsaIrq` → `PicSetIrq` + `IoapicSetPin`），
与 PIT/i8042/IDE 完全同构。编译零告警、`depcheck.sh` 通过。

**但 `ioapic` 仍不能进套件**：它的 4 个 TMR 用例读 `APIC_TMR`（`0x180` 区）的 `0x79`
位，而 cemu 的 LAPIC **没有 TMR**（`lapic.c` 只有 `irr`/`isr`，`0x180` 无处理）——
这是未登记的规格缺口，已按 §二 登记为 **D32**（SDM vol.3 §11.5.8：接受中断时置位、
EOI 时清零；需把重定向项的 trigger 位 plumb 到 `LapicDeliver`）。故 `ioapic` 暂不入
`run.sh`，避免把已知必红的用例塞进套件。

## 阶段 4 片 15：x86 用户态写保护缺失 —— 验收 2 卡点定案（2026-09-22）

**卡点定案**：片 13 取回的 `__log_buf` 停在 `VFS: Mounted root` 之后零输出。本轮用 gdb
stub 抓回整块 32MB RAM，走客机任务链 + 页表 + libc 符号表，把卡点钉到指令级：卡住的进程
是 `/linuxrc`（busybox init），睡在 `futex(FUTEX_WAIT_PRIVATE, 0xb784937c, 2, NULL)`；
`0xb784937c` = libuClibc 基址 `0xb7801000` + `0x4837c`，`readelf -sW` 给出符号名
`_stdio_openlist_del_lock`（12 字节），`GOT[0xe4]` 的 `R_386_GLOB_DAT` 重定位正指向它，
与 `exec.c` 里 `mov edx,[ebx+0xe4]` 逐条对上。（此前用 `objdump -T` 的窄正则搜不到
`static` 局部符号，那条"锁的身份"结论当时不可靠；改用符号表后坐实。）

**真因**：`_stdio_openlist_del_lock.__lock` 在进程启动时不是 0。第一次 `fopen` 的
`lock cmpxchg [lock],1`（期望 0）失败 ⇒ 走慢路径 `__lll_mutex_lock`（libc+0x8970）⇒
`xchg [lock],2` ⇒ `futex_wait(addr,2)`；单线程无人唤醒 ⇒ 永久死锁。init 因此不读
`/etc/inittab`、不 spawn getty，控制台与串口全静默 —— `VFS: Mounted root` 是症状，
不是卡点。

**锁字为什么非 0**：`page_translate_as()` 的用户态分支**只查 U/S 位，从不查 R/W 位**。
SDM vol.3 §4.6 表 4-2：CPL=3 写一个 R/W=0 的页一律 #PF，与 CR0.WP 无关（cemu 只在
内核态才看 R/W，且带 WP 条件）。后果是 Linux 的写时复制完全失效：内核把每个未触碰的
匿名页映射到全局只读零页、指望首次写触发缺页，cemu 直接放行 ⇒ 写落进共享帧，写它的
那个 VA 也拿不到私有副本。

物证：`0xb7849000 / 0xb784b000 / 0xb784c000` 三个 VA 的 PTE 逐字节相同（`0030c265`），
flags `PRUAD` —— **只读却带 D 位**，正是"写穿过只读 PTE"的指纹；全用户空间只有这 1 个
帧被 ≥2 个 VA 共享，它同时映在内核线性区 `0xc030c000`（2.6.34 的 `empty_zero_page`），
内容是用户写进去的 `INIT_LIST_HEAD` 自指指针表。旁证：整轮引导只有 65 条 #PF，正常光
`.bss` 的 COW 就该上千 —— 写保护不生效把这些缺页全消掉了，这也是此前查不出真因的原因。

**修**：4KB 与 4MB 两条叶子分支各补用户态写的 R/W 检查，注释按 §五 注明 SDM 出处。

**验证**：构建 clang/ninja 零告警（`-Wall -Wextra`）；linux.iso 实测引导越过
`VFS: Mounted root`，控制台出提示符，验收 2 通过。
**未做**：回归套件本轮未跑。

## 阶段 4 片 14：复位设施 + 观测/入口缺口（D28/D29/D23 销账，D18 收窄）（2026-09-22）

**D28 是误登记：断点在客户机虚拟地址上是好的。** xv6（分页开、cs base=0）实测：
`Z0,801046c2,4` 后 `c`，客机跑到该地址时 5.9 秒停下，`g` 读回 eip=0x801046c2；对照组
`Z0,80109999,4`（不在执行路径上）跑满 20 秒不停。片 11 那三个地址里 `0xc02ef6ea` 根本
不是指令边界，另两个只在启动的特定阶段执行，而那一轮跑到 `--max-inst` 就结束了——stub
关掉连接是运行结束，不是断点失效。台账删行。

**D29：trace 类目的 `skip=` 改按指令计**（新增 `AllowTrace(cat, inst)`，用
`cpu->inst_count` 判窗口；其余类目仍按自身事件计，`skip=0` 时输出逐字节同旧）。实测
`trace:line,skip=10000000,budget=5`：配 `--max-inst 10000000` → 0 行，`12000000` → 4 行。删行。

**回归脚本不再丢证据**：`test/x86/run.sh` 每个失败分支改为 `report_fail`（FAIL 行 + 客机
输出尾部，`FAIL_TAIL` 默认 12 行），kvm 用例的输出不再进 /dev/null；`test/riscv64/run.sh`
同样捕获输出并补 `timeout 60`。

**D23**：CMake 配置时解析 Git 的 bash，`check` / `check-x86` / `check-riscv64` 三个目标以
登录 shell 跑脚本（非登录 shell 的 PATH 里没有 /usr/bin，那样跑 depcheck 会零命中报 ok）；
`tools/depcheck.sh` 先检查 dirname/grep/sed 在位。PowerShell 下实测通过。删行。

**D18 复位设施**：`Board` 加 `entry` / `reset_pending` / `reset`，`BoardRunSteps` 在**指令
边界**执行复位（请求来自写寄存器那条指令内部，就地拆机会抽掉它正在用的状态）；三个触发源
接 sink——port 0x92 bit 0、PIIX3 复位控制寄存器 **0xCF9**（bit1 类型 / bit2 请求，SeaBIOS
`pci_reboot` 写 |2 再写 |6）、i8042 命令 **0xFE**；`X86Reset` 重置设备 + CPU 回 `entry`
（CMOS 保留电池内容、IDE 保留介质与芯片组配置）。四个自带堆状态的设备 Init 改为可重入。
新探针 `test/x86/probe/reset.asm`（生命计数记在 CMOS——低端 RAM 不行，两次复位之间有固件
在跑；三源各占一条命，第四条命打印 `reset ok`；`-DTRIGGER=1|2|3` 变体打印 `no reset` 用于
单独对拍）：**cemu 与 qemu-system-i386 双跑都是 rc=11 + `reset ok`**。D18 收窄到剩下的一半
（RTC 周期/闹钟中断 IRQ8 不投递）。

**未做**：x86 回归套件本轮没跑（等许可）；linux.iso 卡点未动。

## 阶段 4 片 13：内存取证设施 + D30/D22/D27/D19 落地（2026-09-22）

**新增常驻设施（AGENTS.md §十）**：`CEMU_DEBUG` 的 `dump=ADDR:SIZE:FILE` —— 会话结束时按
**物理地址**取回一段客机内存落盘（上限 4 段、单段 5MB）。watch= 只说某地址被碰过，说不了
客机留在"没人再读的结构"里的东西。实现：debug.c/debug.h + host `HostFileCreate`；读取走
`DebugSetMemReader`，由 board/run.c 用 BusRead 装配（debug/ 仍不碰地址空间）。**会话必须
自行结束**（--max-inst 或客机停机）：宿主信号会跳过收尾、不落盘。自检：ROM 0xffff0 的
16 字节与 bios.bin 尾部逐字节一致。

**验收 2 卡点的物证（内核自己的 log）**：`dump=0x30a000:0x18000:build/logbuf.bin`
（linux.iso 跑到 200M 条指令、2m26s，停 pc=0xc011664a、traps=65）取回 `__log_buf` 原文：
- 内核最后一行日志是 `[ 19.768583] VFS: Mounted root (ext2 filesystem) on device 1:0.`
- **之后再无任何输出，也没有一条 BUG/告警** —— 此前"卡在 RCU grace period / might_sleep
  告警风暴"的说法是手工反汇编的产物，按 §九.2 作废。
- 命令行：`BOOT_IMAGE=/bzImage root=/dev/sr0 initrd=/root.bin load_ramdisk=1
  prompt_ramdisk=0 loglevel=7`；日志另有 `no APIC, boot with the "lapic" boot parameter
  to force-enable it.`（确认 PIC-only 构建，LAPIC/IOAPIC 从未被访问）。
- ⇒ 卡点在 `mount_root()` 之后、`free_initmem()`（"Freeing unused kernel memory"）之前，
  即 `devtmpfs_mount` / MS_MOVE / chroot / `async_synchronize_full` 这一段。未定案。

**D22 COM1 接收路径**：uart16550 补全 16550D 接收侧（16 字节 FIFO、FCR 触发级 1/4/8/14、
4 字符时间的字符超时、LSR 的 OE/PE/FE/BI 与读清、IIR 优先级、MCR 回环、MSR 增量位、
DLAB 分频），新增 `Uart16550Receive/Poll`；x86 板接线 COM1→IRQ4（`OnCom1Irq`）；宿主输入
改为一条 stdin 两个去向（`HostInputOpen`：有串口的板把 stdin 交给串口收原始字节，窗口
键盘仍走 8042 —— QEMU `-serial stdio` 模型）。实测：Linux 认到 `serial8250: ttyS0 at I/O
0x3f8 (irq = 4) is a 16550A`（回环 autoprobe 生效）。

**D27 ATAPI 命令集**：补 MODE SENSE(6/10)、GET CONFIGURATION、READ TOC（格式 0/1/2）、
READ CD、SEEK、PREVENT/ALLOW MEDIUM REMOVAL，加 LUN 检查（ASC 0x25）、UNIT ATTENTION
门控与托盘模型（tray_open/tray_locked，START STOP UNIT 的 LoEj 是唯一介质变更事件）。
参考：tiny386/ide.c（QEMU hw/ide/core.c 的移植）。READ CD 的 2352 字节"读全部数据"按介质
能力拒绝（ISO 里没有原始扇区）。实测：CD 引导链完好（12.7s 到 `Decompressing Linux`）。

**D19 芯片组/IDE/IOAPIC**：
- IDE **bus-master DMA**：BAR4=0xc000|1（16 端口，每通道 8 字节：cmd@0/status@2/PRDT@4）、
  PRD 表遍历、方向位（bit3=1 = 引擎写内存 = 读盘，与 libata 的 ATA_DMA_WR 同向）、状态 W1C、
  ATA READ/WRITE DMA(0xC8/0xCA)、ATAPI 数据相位按"引擎是否已启动"选 DMA（两种写入顺序都覆盖）、
  IDENTIFY 补 word 49 bit8 / 53 bit1 / 63 / 64 / 65–68。接线 `IdeSetDmaBus(ide, &m->bus)`。
- ATA 命令集扩展：SET FEATURES(0xEF)、CHECK POWER MODE / IDLE / STANDBY / SLEEP、
  READ VERIFY(0x40)、INITIALIZE DEVICE PARAMETERS(0x91)；feature 寄存器开始保存。
- **ELCR + PIC 电平触发**：0x4D0/0x4D1（`PicRegisterElcr`，elcr_mask 主 0xf8/从 0xde）、
  ICW1 的 LTIM 位不再报错、按 ELCR|LTIM 选电平/边沿（QEMU pic_set_irq1 同款）。
- IOAPIC 增加 lowest-priority 投递（单 APIC 下等价 fixed）。
- **澄清**：0x3F7 不是 IDE 寄存器，是**软盘的 DIR**（seabios `PORT_FD_DIR`、v86 同名）——
  属 D21 的范围，IDE 侧无需改。
- **未做**：IOAPIC 的 NMI/SMI/INIT/ExtINT 四种交付模式需要本机没有的硬件输入（CPU 无 NMI
  引脚、无 SMM、只有一个 APIC、没有"PIC 经 IOAPIC"的通路）；PIRQ 路由字节存在但本机没有
  任何 PCI 设备拉 INTx（IDE 跑在兼容模式），无处可路由。

## 阶段 4 片 12：x86 BT/BTS/BTR/BTC 的位串寻址 —— 内核卡在 calibrate_delay 的真因（2026-09-21）

片 11 留下的卡点（内核在 0xc02ef710 自旋等 jiffies、2.5 亿条指令零推进）在本片定案。
**不是设备侧问题**：片 11 的 pit_irq 探针已经证明 PIT/PIC/CPU 通路可用（cemu 与 QEMU
各收 10 个 tick），本片实测 IRQ0 边沿一路打到运行结束（2464 次），master IMR=0xfa
（IRQ0 未屏蔽）。真因是 x86 解释器的一条规格偏差。

**症状链（全部由 CEMU_DEBUG 对拍 + QEMU 同 ISO 对照取得，不是推理）**：
1. `mark` 新增的 CPU 侧标记显示：内核全程只收到 **1 次** IRQ0 交付
   （`inta a=48` = 向量 0x30），之后 PIC 的 ISR bit0 永久置位，再无投递
   （最后一次 `pic0-eoi` 远早于此）。
2. 新增的 `gate` 标记给出那唯一一次交付读到的门：**handler = 0xc02ef560
   （`ignore_int`，早期默认桩）**。内核因此打印
   `Unknown interrupt or fault at: 00000246 00000060 c012ce00`（上下文正是
   `setup_default_timer_irq` 解屏蔽那一刻），既不算 tick 也不发 EOI。
3. QEMU 对照（同一 ISO、同一内核）：`-monitor` 读物理 0x2c0180 的门 = 0xc0102648
   （真 IRQ stub），IRQ0 交付 2412 次，客机停在 0xc0106130 的 `hlt` 空闲。
4. `watch=0x2c0000:0x800:w` 覆盖整个 IDT：向量 **0x20–0x3f 的门最后一次写入全部是
   `setup_idt` 写的 `ignore_int`**（PC 0xc02ef487，256 次循环），内核的 C 代码只写了
   异常向量与 0x80 —— 即 `init_IRQ()` 的 IRQ 门安装循环一个门都没装。
5. 反汇编运行镜像（QEMU `pmemsave` + `objdump -m i386`）定位到该循环 0xc02deff3：
   `bt %eax,0xc030e304` 判 `system_vectors` 位图，位清才装门。
   `watch=0x30e304:0x20:rw` 显示该双字被 `trap_init` 的 `bts` 置成 **0xffffffff**，
   而循环对 i=0x20..0xff 每次都读到同一个双字、取 `i & 31` 位 ⇒ **全部判成系统向量
   ⇒ 224 个 IRQ 门全被跳过**。

**根因（真 bug，SDM 明确）**：`BT/BTS/BTR/BTC` 的内存操作数是**位串**，
SDM vol.2 的 Operation 是 `BitBase ← BitOffset DIV OperandSize` —— 位偏移越过一个
操作数宽度时，有效地址要前进一个操作数（32 位操作数每 32 位进 4 字节，16 位每 16 位
进 2 字节）；寄存器操作数才取模。原实现只读基址那一个操作数、用 `bit & 31` 取位，
所以偏移 ≥ 32 时读错了字。

**修复**：`src/cpu/isa/x86/exec.c` 的 bt/bts/btr/btc 分支——先取位偏移，内存操作数按
`d.mlin += 4 * (bit >> 5)`（16 位 `2 * (bit >> 4)`）推进后再读写；写回走同一地址。

**实测（linux.iso 单跑，150M 条指令）**：`Calibrating delay loop... 4.40 BogoMIPS
(lpj=22016)`（定时器校准通过，时间戳 0.02s → 19.2s 正常走）→ RTC/8250/IDE-ATAPI
（认出 `CEMU VIRTUAL CD-ROM`）/i8042 → `RAMDISK: Loading 3883KiB ... done` →
**`VFS: Mounted root (ext2 filesystem) on device 1:0`**。PC 已进 `0xc014bf7b`
（真任务栈 esp=0xc13e1b1c），不再自旋。

**设施（本片为定位补的，已进 AGENTS.md §十）**：`screen` 镜像改整行输出（原来被事件表
41 列截断，读不了 call trace）；`DebugMark` 的 `b` 改十六进制；新增标记
`intr`/`inta`/`ioapic`/`lapic-irr|ack|eoi`/`lidt`/`lgdt`/`gate`；`i8259` 增加
`imr`/`eoi`/`base`。另记两个坑：`watch=` 匹配**物理地址**（内核 .data 要减映射偏移）；
QEMU 侧 `pmemsave` 必须给 Windows 路径。

**下一片的卡点**：本片未跑到用户态（`VFS: Mounted root` 之后的行未取），回归与探针
（`test/x86/probe/bt_bits.asm`，双跑对拍）待补。

## 阶段 4 片 11：验收 2 推进到内核 —— 五处 x86 语义缺口（rep 计数 0、BSR、x87 ESC、CMPXCHG、xadd 顺序）（2026-09-21）

片 10 之后的卡点（ldlinux 的 do_sysappend 在 guest 0x103906 的 `rep movsd` 上
打转）在本片定案，并一路推到 **cemu 进入内核**。

**方法（全部 ≤2 分钟的有界命令）**：QEMU 侧 `-gdb` 与 cemu 侧 `-gdb` 同协议，
同一地址逐次命中对拍寄存器；必要时用 `-monitor pmemsave` 取运行镜像、用
`llvm-objdump/objdump -m i8086` 离线反汇编；GNU/QEMU 的内存镜像互校以定基址。

**五处语义缺口（逐个修掉，均为真 bug 不是特判）**：
1. **`string_op()` 的 REP 计数为 0**：原实现"先执行一次再减 1"，`(E)CX = 0` 时
   算出 0xffffffff 并判定继续 ⇒ 40 亿次拷贝。SDM vol.2 REP 前缀的循环体在计数
   判断之后，(E)CX = 0 是**完全空操作**。触发点是 ldlinux memcpy 的
   `shr ecx,2; rep movsd`（尾长 2–3 字节时 ECX 恰为 0）。现场：cemu 逐次命中
   0x103906 为 `ecx=2→1→0→0xffffffff`，QEMU 为 `2→1→0→下一条指令`；用户看到的
   `ecx≈0xff6c40a4` = 0xffffffff 已减 9,686,363 次。
2. **BSR/BSF（0F BC/BD）未实现**：#UD ⇒ 内核 setup 的磁盘几何计算（`bsr eax,eax`）
   反复陷入（2 亿条指令 205 万次陷阱，零设备 I/O、零推进）。按 SDM 补实现。
3. **x87 ESC 一律 #UD**：内核启动期 FPU 探测（arch/x86/boot/setup.S：先把栈上两字
   填 0xffff，再 fninit/fnstsw/fnstcw，靠"内存未被写回"判定无 FPU）因此陷入
   3460 万次陷阱。改为**无 FPU 处理器语义**：CR0.TS/EM 置位先 #NM，否则解码
   modrm 当 NOP（tiny386 `ESC()` 同款；本机 CPUID 本就报 EDX.FPU=0）。
4. **CMPXCHG（0F B0/B1）未实现**：内核 0xc0105df2 处 #UD。按 SDM 补实现
   （ZF 与算术标志取自与累加器的比较）。
5. **XADD 的写回顺序**：SDM 是 `SRC ← DEST` 再 `DEST ← TEMP`，原实现反了；当
   目的与源同寄存器（realmode 的 `xaddl %eax,%eax`）结果错。

**验收进展（有界证据）**：isolinux → 内核 setup → `Decompressing Linux... Parsing
ELF... Booting the kernel.`（screen 读到）→ 内核以 **cs=0x60**、cr0=0x8005002f
（分页开）运行，PC 采样已过用户给的判据点 0xc0106130（150M 条时 eip=0xc0116b30，
280M 条时 0xc02ef716）✓。

**当前卡点（下一片第一件事）**：内核卡在 `calibrate_delay_converge()`
（0xc02ef710–0xc02ef718 自旋等 `jiffies`，初值 0xffff8ad0 = -30000 = INITIAL_JIFFIES
@HZ=100）。证据链：`watch=0x2cb818:4:w` 在 2.5 亿条指令内**一次写入都没有** ⇒ 定时器
ISR 从未运行；`bus` 日志显示内核已解除 IRQ0 屏蔽（master IMR = 0xb8，bit0=0）且
PIT 通道 0 有边沿（`mark` 的 `isa a=0 b=1/0`）；而**新探针 test/x86/probe/pit_irq.asm
（自建 IDT/PIC、编 100Hz、sti 后自旋不 hlt）在 cemu 与 QEMU 上都能收到 10 个 tick**
⇒ PIT/PIC/CPU 通路本身可用，问题在内核这一侧的投递配置（IOAPIC/PIRQ 或内核的
request_irq 路径，D19 的 PIRQ/ELCR 缺口是首要嫌疑）。

**回归**：x86 套件 11 格全绿（smoke / cga / rep-zero / pit-irq / pm 24 ok / kvm×5 /
realmode 126 PASS + 1 FAIL(fninit，无 FPU 模型的必然，判据已注释)）；riscv 侧未动。

**设施**：新增两个常驻探针 `test/x86/probe/rep_zero.asm`（REP 计数 0）与
`pit_irq.asm`（自旋下的 IRQ0 投递），均双跑校准并接入 run.sh；登记两个观测缺口
**D28**（gdb stub 在内核虚拟地址上的断点不触发）与 **D29**（`trace` 的 `skip=`
不是"跳过前 N 条指令"）。

## 阶段 4 片 10：x86 中断投递多压错误码（IRQ 被当成异常）——isolinux 的 iret #GP 根因（2026-09-19）

片 9 的卡点（El Torito 引导镜像跳转后，isolinux 的 32 位 PM 代码在 guest 0x8ccc 的
`iret` 上反复 #GP → triple fault）在这一片定案并修掉。

**症状回顾**：`trap` 行 cause=13、错误码是被弹入的伪选择子（随运行变化）；PC 恒为
0x8ccc（isolinux.bin 文件偏移 0x10cc = `popa; add esp,4; iret`）。

**定位（用调试设施，不是推理）**：
1. 同一地址在 cemu 与 QEMU 两边下断点（stub）：两边的指令流到 0x873c/0x8cc2 逐条
   一致、每条指令的 ESP 增量也一致，但 **ESP 绝对值差 4 字节** ⇒ 漂移发生在更早处；
2. 两侧栈镜像对齐后可见：ceme 的返回帧比 QEMU 的高一个双字，且那个槽里是 **0**
   ⇒ 有一次压栈多写了 4 字节的零；
3. `CEMU_DEBUG=watch=0x31ff80:0x80:w` 的 W 行（带 PC/RAW/助记符）直接给出写者：
   `| W | 0000000000008dd7 | intr | w 31ffb0:4 <- 0 |` —— **一次中断投递把错误码
   0 压进了栈**（帧 = [EC=0][EIP][CS][FLAGS]）。

**根因（真 bug，不是特判需求）**：`do_int` 用 `!soft` 判断"这个向量要不要压错误码"。
软件 `INT n` 已经排除（`!soft`），但 **step.c 的硬件 INTR 投递也传 `soft=0`**，于是被
当成异常：标准 PC 映射下 IRQ0..7 = 向量 8..15，其中 8/10/11/12/13/14/17 在我的
`vec_has_ec` 表里 ⇒ **每一次定时器中断都多压 4 字节** ⇒ 客人的 IRQ 处理程序按
"无错误码"的布局收尾（`popa; add esp,4; iret`）⇒ 栈漂移 ⇒ `iret` 弹出伪选择子。

**修复（按 SDM vol.2 INT Operation / vol.3 table 6-1 分三类来源）**：`x86.h` 新增
`kIntException / kIntExternal / kIntSoft`；`do_int(vec, ret_eip, origin, ec)` 只在
`kIntException` 时压该向量的错误码，门故障错误码的 EXT 位只对 `kIntExternal` 置，
门 DPL 检查仍只对 `kIntSoft` 生效。调用点按语境传参：step.c 的三处（异常桥
`kIntException`、INTR 投递 `kIntExternal`、单步 #DB `kIntException`），exec.c 四处
（INT3/INT n/INTO = `kIntSoft`，单步 #DB = `kIntException`）。

**实测**：triple fault 消失（150M 条指令干净跑完、`trap` 类目 25M 条内零异常）；
引导越过 isolinux 自举：SeaBIOS 的 El Torito 交接完成后，isolinux 经 INT 13h 把 /bzImage
读进内存并跳入其早期引导。**下一片的卡点（本轮已做到地址级定位，不再动手）**：PC 恒在
guest 0x103906（一段 `rep movsd` 拷贝例程）被反复重入；调用方 0x102604（`jz` /
`mov ecx,edi` / `call 0x1038e0` 进 memcpy 风格例程），表基 0x106ba0、12 字节表项、
`[ebp]!=0` 时回跳 0x1024af。三次采样（10M / 30M / 60M 条指令）寄存器完全相同
（cs=0x20 ss=0x28 esp=0x31ffb8 eip=0x103906 eax=0x0032000a ebx=edi=0x00320010 ecx=2
edx=0x000c8004 ebp=0x00106ba0 esi=0x00106fda eflags=0x203）⇒ 整段装载被从头重来
（不是原地死循环，是外层循环重入），而无异常、无控制台输出；该字节序列在 ISO 里找不到
⇒ 由运行时生成/解压，即已经在内核早期引导里。

**回归**：riscv64 136/0、x86 9/0、depcheck ok（CPU 语义改动没动摇既有基线）。

**设施记要（本轮踩到的两个坑，已顺手补一个）**：`watch=` 的地址/长度要写 `0x`
前缀，写成 `31ff80:80:w` 会被当十进制解析失败而**静默丢弃**（现已补错误日志）；
`bus`/`mem` 的 5MB 会话上限会被 BIOS 的 ROM 影子拷贝（每字节两次事件）吃满，
要定位引导期的设备流量得用 `skip=` 把窗口挪过去。

**文档收尾（本轮）**：reference.md 销账 D24 —— 两处陈旧路径修正（`TinyEMU/项目调研.md`
→ `EMU/项目调研.md`、参考树根 `D:/code/c/TinyEMU/` → `D:/code/c/EMU/`）、§一 参考清单补
`xv6-public`/`pintos`/`UcoreOS` 三行、§三 补 QEMU 侧裁决仪器（`-gdb tcp::N -S`、`-d cpu`、
`-trace 'ide_*'`，与 cemu 自身的 `-gdb` stub 同协议）、§四 补 PC 芯片组与固件接口/PIIX IDE
与 ATAPI/PS/2 键盘三行；AGENTS.md 销 D24 行、§十 补两条设施注记（`watch=` 的 `0x` 前缀、
会话 5MB 硬顶与 `skip=`）；arch.md 目录树按现状补全（device 增 input/storage/video，intc 增
lapic/ioapic，misc 增 i440fx/piix3/pci/cmos/port92/fwcfg/debugcon；debug 增 gdbstub；host 增
display/input/sock），阶段 3 与 3.5 补 ✅、阶段 4 标注"进行中"并写入当前验收状态。
## 阶段 4 片 9：ATAPI 落地 —— SeaBIOS 从光盘引导 isolinux（2026-09-19）

验收 2 主线的第一半：给 PIIX IDE 补上 packet（ATAPI）设备，让 SeaBIOS 的
El Torito 路径能读光盘。

**设备侧**（`device/storage/ide.{h,c}`）：新增介质种类（`kIdeMediaDisk` /
`kIdeMediaCd`）与 packet 设备的完整状态——复位后呈现 ATAPI 签名（SC=SN=1、
CL/CH=0x14/0xEB；选择该盘位时重新呈现，这是驱动在没读 IDENTIFY 前认出它的
唯一途径）、IDENTIFY PACKET DEVICE（0xA1，word 0 = 0x8580 = packet 设备 +
设备类型 5 = CD-ROM + 可换介质）、PACKET（0xA0）经数据口收 12 字节 CDB 并按
§9.6 解释。CDB 覆盖 TUR(0x00)/REQUEST SENSE(0x03，18 字节定长 sense)/
INQUIRY(0x12，36 字节)/START STOP UNIT(0x1B)/READ CAPACITY(0x25)/
READ(10)(0x28)/READ(12)(0xA8)；其余回 ILLEGAL REQUEST + ASC（错误寄存器
高位 nibble = sense key）。逻辑块 2048 字节。
**传输核心统一成"DRQ 轮次"**（round/used/left/limit/refill/packet 六个字段）：
ATA 仍是每轮 512 字节、只在命令末尾中断；ATAPI 每轮 2048 字节（受字节数上限
CL/CH 与块边界约束）、每轮中断、结束置"命令完成"中断理由。ATA 语义逐字保留
（回归 riscv64 136/0、x86 9/0 全绿，depcheck ok）。

**板级**：`-cdrom FILE`（QEMU 惯例；落在次通道主盘）——main.c / board.h /
x86_min.c。

**关键 bug（新探针逮住，不是推理）**：数据相没有把 `block_size` 从 CDB 阶段的
12 改回 2048 ⇒ 每轮只搬 12 字节、且按 `lba × 12` 读介质 ⇒ 引导记录卷描述符
（LBA 0x11）读出全零 ⇒ SeaBIOS 报 `Could not read from CDROM (code 0005)`。
先用 QEMU `-trace 'ide_*'` 取客人下发的 CDB 真值（`28 00 00 00 00 11 00 00 01
00 00 00`，字节数上限 0x0800），排除译码嫌疑后定位到数据面。

**观测设施补缺（§九.1）**：`bus` 类目此前把**指令取指**也记为设备命中——固件
在 ROM 窗口执行时，取指行会吃满会话 5MB 输出上限，IDE 端口流量完全看不见
（本轮为此白跑数轮）。现在取指不进 `bus`：x86 走 `phys_load(..., is_fetch)`
与 `bus_fetch`，riscv 用既有的 `acc_ifetch` 访问类；`mem` 类目不受影响。

**新增 smoke 探针** `test/x86/probe/cd_atapi.asm`（.bin 已 ignore）：直接驱动
次通道 ATAPI，打印签名 / IDENTIFY / READ(10) 块字节与结束状态，**cemu 与 QEMU
双跑对拍**。实测两侧转录逐字一致，且 `blk=` 与 ISO 第 17 扇区逐字节相同。

**验收进展**：SeaBIOS `Booting from DVD/CD...` → `Booting from 0000:7c00`
（El Torito 无仿真引导镜像被加载并跳转，与 QEMU 转录一致）✓。

**当前卡点（下一轮第一件事）**：isolinux 的 32 位保护模式代码在 guest 0x8ccc
的 `iret` 上反复 #GP（`trap` 行 cause=13，首个错误码是被弹入的伪选择子且随运行
变化）→ triple fault。已定位指令区间（6.0M 条时 PC=0x8c98，紧邻故障点）。
下一步查 cemu 的 PM iret 帧宽度与选择子校验（SDM vol.3 6.13/6.14）。

**登记**：D19 的 ATAPI 条目销账；未实现的 CDB 与 ATAPI DMA 立 D27。
## 阶段 4 片 8：验收 2 起步 —— 取镜像、补 VGA 文本回读设施、定位 Linux 0.11 引导码（2026-09-19）

**镜像**：`build/linux/` 下有两份（用户自取）：`linux.iso`（5.4MB，El Torito 光盘 ✓ 走 ISO
路线）与 `linux-0.11-devel-040329.zip`（oldlinux 的 0.11 开发套件；`bootimage-0.11-hd` +
`hdc-0.11.img` 根文件系统 + Bochs 配置 ✓）。

**新观测设施 `screen`**（AGENTS.md §十）：CGA 字符平面逐行 diff，变化的行以 `V` 行打出。
Linux 0.11 这类客机的控制台只写 VRAM（`console=` 是后来的东西），没有它就只能靠肉眼看
窗口。实现：`cga.c` 的 `ScreenMirror`（渲染路径上，80×25 字符面 + 属性字节跳过）+ 
`DebugText`。用它做的 A/B：xv6 全程 VRAM 全 0（一行 V 行都没有 ⇒ 这台机器上没人写过
VGA 文本；待查是否与用户那版 xv6 的控制台改法有关），而设施本身工作正常（自报
`screen-run` ✓ + watch 金丝雀命中 BDA ✓）。**结论不变：Linux 0.11 的输出一定能被它读到**
（0.11 只有 VGA 控制台）。

**Linux 0.11 引导失败的真实原因**（逐步用设施定案，不是推理）：
1. SeaBIOS 从 hda 引导 ✗ 只提示一次 "Booting from Hard Disk..." 便转去试**软驱**；
2. 用 `-hdb bootimage-0.11-hd`（补丁 DL=0x81）⇒ SeaBIOS 根本不试第二块盘；
3. 合成单盘（扇区 0 = 补丁引导码 + 合成分区表指向 LBA1024、LBA1-237 = 内核、
   LBA1024 起 = 根文件系统）⇒ 引导码**执行了**（watch 0x90000 看到它 `rep movsw` 自我
   重定位 512 字节到 0x90000）、内核也读进去了，但随后在 **0x78-0x8d 死循环**：
   它读 `int 13h AH=8` 报的每道扇区数，**只接受 15 或 18**（软盘几何），我这块盘是
   63 扇区/道 ⇒ 直接 `jmp 0x8d` 自旋。反汇编 `bootimage-0.11`/`-fd`/`-hd` 三个变体
   全都带这个检查；`bootimage-0.12-hd` 同样把 DL 写死为 0（软驱）。
4. ⇒ 这套 0.11/0.12 的"hd"名字指**根文件系统在硬盘**，引导码本身是**软盘引导**
   （Bochs 配置就是 `floppya=bootimage-0.11-hd`）。cemu 无 FDC、无 8237 DMA ⇒ 登记
   **D21**（软盘与 DMA 通路缺失），Linux 阶梯不走这条路。

**下一步（验收 2 主线）**：走 **ISO 路线** —— 给 PIIX IDE 补 **ATAPI**（0xA1 IDENTIFY、
0xA0 PACKET + CDB：READ(12)/TEST UNIT READY/REQUEST SENSE/READ CAPACITY/INQUIRY、
2048 字节块），SeaBIOS 的 El Torito 引导路径即可工作，随后用 `linux.iso` 打第一发；
Linux 的控制台输出由新的 `screen` 设施读回。这也顺带销掉 D19 里"无 ATAPI"那条。
## 阶段 4 片 6/7：宿主输入源 + mark 观测类目；键盘端到端打通（2026-09-19）

**片 6**：`src/host/input_win.c` —— stdin（控制台或管道）→ 美式布局 set-1 扫描码
（大写走 shift 组合）→ 机板 `key_in` 钩子；`HostKeyOpen`/`HostKeyPoll`，run.c 的步进
循环轮询（自带 2 ms 门限）。管道里的 `\r\n` 只当一次 Enter。

**片 7 观测设施**：新增 `mark` 类目（无帧事件行 `DebugMark()`，K 行，AGENTS.md §十），
打点：`I8042KeyByte`（kbd-byte）、`I8042SyncIrq` 的线跳变（kbd-irq）、机板 ISA 扇出
`OnIsaIrq`（isa）、`OnHostKey`（hostkey），外加输入源自报 keypoll/hostbyte。
§IX.1：这次卡住的就是"中断线与宿主输入完全看不见"，补设施而非继续推理。

**根因（数据定案，不是推理）**：
1. `mark` 下**一条 hostkey/kbd-byte 都没有** ⇒ 键没进 8042；
2. 自报移到 `HostKeyPoll` 顶部（原先被 `if (!g_sink) return` 挡住）⇒ 仍无 keypoll
   ⇒ `HostKeyPoll()` 根本没跑起来；
3. 读 main.c：**`HostKeyOpen` 被写在 `if (a.display_backend)` 块里**，而未传 `-display`
   的运行（你和我的都是）⇒ 既没有窗口、stdin 也没挂上 ⇒ 敲什么都没反应。
   此前"工具会话没有可见桌面、自己测不了"的判断是错的：那个进程根本没有窗口。

**修复**：`HostKeyOpen` 移出显示分支（stdin 恒接）；窗口按键仍由
`HostDisplaySetKeySink` 单独接。

**实测（无头、管道，可重复）**：启动 32 s 后写 `ls\r` ⇒ xv6 打出完整目录表
（kill / ln / ls / mkdir / rm / sh / stressfs / usertests / wc / zombie / console）
⇒ 宿主输入 → set-1 扫描码 → 8042 队列 → IRQ1 → xv6 kbd → 控制台，全链路通。

**用法**：`-display win32` 出窗口（点窗口敲键）；不传 `-display` 即无头，直接在启动
cemu 的终端里敲键（xv6 控制台同时写串口，回显就在同一终端）。
## 阶段 4 片 5：PS/2 键盘通路（i8042 输出队列 + IRQ1 + 宿主窗口按键）（2026-09-19）

- **i8042**（`src/device/input/i8042.c`）：输出队列（16 字节，OBF = 非空，队列即输出
  缓冲，QEMU pckbd 同深度）、键盘字节入队 `I8042KeyByte`、**IRQ1 电平** = 队列非空
  && 命令字节允许键盘中断 && 键盘未被禁用（PC/AT Technical Reference；QEMU
  pckbd.c kbd_update_irq 的同三条条件）。命令应答（0x20/0xD0/0xAA/0xAB）改装进同一
  队列，不再单用一个 outbuf。
- **宿主按键**：显示窗口 WM_KEYDOWN/WM_KEYUP（`host/display_win.c`）→
  `HostDisplaySetKeySink` → 机板 `key_in` 钩子（`board.h`，main.c 在开窗后接）→ PC
  机板 `OnHostKey` 把 lParam 里的 set-1 扫描码（bit16-23、bit24 = 0xE0 前缀、释放
  加 0x80）送进 8042 → IRQ1 走既有 ISA 扇出（8259 + IOAPIC pin 1，xv6 用 pin 1）。
- 登记 **D20**（键盘设备命令集只记录不回答、扫描码固定 set 1、无鼠标、队列满丢
  字节）。
- **验证状态**：设备路径完成、构建零告警、xv6 引导到 `$ ` 不受影响；但本会话的工具
  环境没有可见桌面（`MainWindowHandle=0`），宿主按键注入（SendKeys 与 PostMessage）
  都进不到窗口 —— 键盘的端到端只能**用户目验**（AGENTS.md 九.8）：启动 cemu 后点
  一下窗口敲 `ls`。自动化侧已确认：90M 指令 ≈ 27.6 s（3.3 MIPS），到 shell 约 23 s。
- 顺带发现：**COM1 的 RX 未接线**（`uart16550.c`：RBR 恒读 0），而 xv6 的控制台输入
  同时接受串口路径，riscv 侧 xv6-riscv 的 shell 也要它 —— 下一片候选。
## 阶段 4 片 4：xv6-x86 引导到 shell（验收 1 达成）（2026-09-19）

片 3 遗留的"首个用户态陷阱返回 triple fault"根因找到并修掉：xv6 从盘上引导到
`init: starting sh` + `$ ` 提示符，输出与 QEMU 基线逐字一致。

**根因：处理器内部访问被当成用户程序访问去查 U/S 权限。**

- 现场（`CEMU_DEBUG=trap`）：三行陷阱全落在 iret 自己（PC=0x8010585c），
  `cause=14 tval=7 → 5 → 5`。`tval` 对 #PF 是**错误码**（线性地址在 CR2，
  见 `pf_fault`）：7 = present + 写 + 用户，5 = present + 读 + 用户。
- 用 gdb 挂 RSP stub 在第 8 次 `iret` 处停下（判据：帧里 cs=0x1b 是用户段，
  前 7 次都是内核内的返回），读出完整合法的陷阱帧（eip=0 cs=001b efl=200
  esp=1000 ss=0023；内核 esp=0x8dffffec = 内核栈顶-20 = 帧的 5 个字）：
  帧没问题，问题在 iret 执行期间。
- 机制：`pm_iret` 先 `seg_commit(cs_i, 0x1b, …)`（`cpl()` 由缓存 AR 得到，
  此刻已是 3），而 `seg_commit` 里紧接着要把该描述符的 A 位写回 GDT ——
  那个写走 `bus_store`（页表翻译 + 按 CPL 做 U/S 检查），GDT 在 U=0 的内核页
  → **#PF 错误码 7**（第一行）。异常投递读 IDT（`bus_load(s->idtr+…)`）时
  CPL 仍是 3，IDT 页 U=0 → **#PF 错误码 5**（第二、三行）→ #DF → triple
  fault。三次陷阱都报在 iret 上，是因为 CPU 状态在 `seg_commit` 里已经改过
  （CS 已提交、eip 还没写）。
- 修复（`src/cpu/isa/x86/exec.c`）：访问层分两类 —— 程序访问
  `bus_load/bus_store`（U/S 按 CPL 检查）与**处理器访问**
  `kbus_load/kbus_store/krd*/kwr*/kpush*`（忽略 U/S，恒为超级权限）。裁决
  依据：SDM vol.3 4.6 的 U/S 检查对象是**程序**的 CPL；描述符表、TSS、换栈
  后压的投递帧都是处理器自己的状态 —— 硬性判据是任何 OS 都必须成立：ring 3
  的段加载要能读 U=0 的 GDT，ring 3 的中断要能写 U=0 的内核栈。
- 站点：`desc_parse`（GDT/LDT 描述符读）、`seg_commit`（A 位写回）、
  `tss_stack` 与 `do_task_switch`（TSS 全字段读写 + GDT 忙位写回）、
  `pm_far` 的 LDT/GDT 描述符读、`do_int` 的 IDT 门读与投递帧、`call_gate`
  的换栈入栈、任务切换的错误码入栈。`page_translate` 拆为
  `page_translate_as(lin, write, user)`，访问类由调用者给出。
- 实测：`init: starting sh` + `$ `（QEMU 同镜像基线一致）；回归 riscv64
  136/0、x86 9/0、depcheck ok。
- 遗留（验收 1 的可用性）：PS/2 键盘输入未接（i8042 只有 POST 骨架，无按键
  注入与 IRQ1 投递），所以 shell 起得来但还不能敲命令 —— 下一片。
## 阶段 4 片 3：IOAPIC + LAPIC 中断路径打通；xv6 卡在首个用户态陷阱（2026-09-19）

片 2 之后 xv6 停在"首次读 fs.img 等完成中断"。本片补齐中断链路，并修掉一个
IDE 建模错误：

- **LAPIC（重写）**：IRR/ISR/PPR 优先级仲裁、INTA（`LapicAcknowledge`：启用且
  有可投递请求时返回向量并置 ISR，否则交回机板让 8259 应答）、EOI（清最高 ISR
  位，并把该向量通知 IOAPIC 退 remote-IRR）、SVR 使能位、TPR、PPR 只读、
  TCCR/APIC 定时器（LVTT 周期/单次、TICR、TDCR 分频表，按宿主时钟以 100MHz
  总线时钟推进，`LapicPoll` 进机板 poll 循环）。复位态 = APIC 关闭、SVR=0xFF，
  所以固件仍跑在 8259 上。
- **IOAPIC（新）**：`src/device/intc/ioapic.{h,c}`，0xFEC00000（IOREGSEL 0x00 /
  IOWIN 0x10）、IOAPICID、IOVER（version 0x11，低字节 0x11 = 最大重定向项，
  xv6 的 `maxintr` 由此而来）、24 项重定向表（vector / delivery / dest mode /
  polarity / trigger / mask / destination，remote-IRR 由芯片维护）、边沿与电平
  两种触发（电平走 remote-IRR + EOI 重触发）、fixed 投递（D19 登记）。复位全
  mask。
- **机板接线（PC/AT 的真实拓扑）**：每条 ISA IRQ 线**同时**进 8259 与 IOAPIC
  的针（`IrqBus`），由客户机的编程决定谁投递；INTR 是两者电平的或
  （`UpdateIntr`）；INTA 先问 LAPIC（启用时），否则问 8259 —— 与 QEMU
  `cpu_get_pic_interrupt` 的分流一致。
- **IDE 建模修正（真 bug，被 xv6 逼出来）**：任务文件寄存器是**每通道一套**，
  不是每盘一套（ATA/ATAPI-7 §7.10：主机接口只有一套寄存器，drive/head 的
  bit4 只决定哪块盘执行下一条命令）。xv6、libata 的 `ata_tf_load`、SeaBIOS 的
  `send_cmd` 都是"先写计数与 LBA、最后写 device/head 选盘"，按每盘建模就会把
  命令交给从盘那份**空**寄存器 → 读到 LBA 0 → 全 0 数据（现象：超级块打印全 0）。
  改后 cemu 打印的超级块与真 QEMU 逐字节一致。
- **QEMU 对拍基线**：同一份 `xv6.img`/`fs.img` 在
  `qemu-system-i386 -m 512 -nographic` 下：`sb: size 1000 nblocks 941 ninodes
  200 nlog 30 logstart 2 inodestart 32 bmap start 58` → `init: starting sh` → `$ `。
- **当前遗留（下一轮第一件事）**：xv6 走到**首个用户态陷阱返回**时 triple fault：
  `trapret` 的 `iret`（0x8010585c）取指到 VA 5/7（initcode 的 `int $T_SYSCALL`
  及其返回点）→ #PF(14) → 投递再失败。已排除：磁盘（超级块正确）、IOAPIC/LAPIC
  投递（中断确实到达并唤醒了 iderw）、APIC 定时器（停掉后同样崩）。`pm_iret`
  的无特权变化路径与帧弹出顺序看了是对的。下一步：先用 `watch` 定位 `allocproc`
  写入的 `p->kstack` 值，再盯那页内核栈顶 76 字节的 trapframe，看 exec 改写
  `tf->eip` 与 `iret` 读取的实际值（或把帧内容补进 trap 观测设施后回来登记）。
- **回归**：`bash test/run.sh` → riscv64 136 passed / 0 failed、x86 9 passed /
  0 failed；`bash tools/depcheck.sh` → ok。
- **登记**：D19 增补"IOAPIC 只做 fixed 投递"。
## 阶段 4 片 2：IDE 落地，xv6 从盘上被引导、内核起跑到首个进程（2026-09-19）


片 1 之后 POST 卡在“没有可引导设备”。本片把机器补到**有盘**，并用用户自己的
xv6（`D:\xv6img\xv6.img` + `fs.img`，stock xv6-public）当验收件：

- **IDE(PIIX PATA)**：新增 `src/device/storage/ide.{h,c}`（新目录 `storage/`，
  外设按类分的第二类）。PIO 半套 ATA-4：IDENTIFY DEVICE(0xEC)、READ SECTORS
  (0x20)、WRITE SECTORS(0x30)、FLUSH CACHE(0xE7)，LBA-28 与 CHS 两种寻址，
  DRQ 逐扇区握手；其余命令（含 ATAPI 的 0xA1 与 LBA-48/multi-sector）按规格
  回 ABRT。中断线 IRQ14/15 走机板 sink 到 PIC（兼容模式，`intr pin = 0`）。
  BAR 做了尺寸解码（写全 1 读回给窗口大小，PCI §6.2.5.1），BAR4 读 0 = 无
  BMDMA（D19 登记）。
- **PIIX3 ISA 桥**：新增 `src/device/misc/piix3.{h,c}`（8086:7000，class 0x0601，
  header type bit7 = 多功能）。**它是 IDE 能不能被看见的前提**：固件按
  “function 0 不开多功能就跳下一个设备”遍历（seabios src/hw/pci.c pci_next），
  没有它 00:01.1 永远不会被探测。
- **机板**：`-hda/-hdb` 挂主通道主/从盘（`BoardOpts` 加两个字段），`--mem`
  在 x86 上也生效（xv6 的 PHYSTOP 是 224MB，必须给够；CMOS 内存量跟着走，
  e820 自然正确）。另加**全 4GiB open-bus 区**（未被任何译码器认领的地址读全 1、
  写丢弃）——xv6 的 `ioapicinit` 写 0xFEC00000，此前会 `[fatal] read of
  unmapped address`，而真实 PC 该处是 open bus。
- **解释器里抠出的两个真 bug（都由 xv6 逼出来，不是为跑通加分支）**：
  ① **GDT/LDT/IDT 表项是线性地址，必须过分页**（SDM vol.3 3.5.1）：原实现用
  `BusRead` 直读物理地址，xv6 的内核表在 0x8011xxxx（高于恒等映射），读到的是
  open bus 的全 1 → IDT 门选择子 0xFFFF → `#GP(0xfffc)` → 双重投递失败 →
  triple fault（`cpu0: starting 0` 之后立刻崩）。改为走 `bus_load/bus_store`
  （7 处：desc_parse、段装载的 A 位回写、LTR/LLDT/任务切换的忙位回写、CALL/JMP
  门的描述符读、IDT 门读）。
  ② **INTR 是电平，不是锁存**（SDM vol.3 6.3.2）：PIC 的 `PicUpdateIrq` 只置位
  从不清零、x86 的 `x86_set_intr` 忽略 level=0——POST 期间 IRQ0 置起的 INTR
  一直挂着，xv6 `picinit` 屏蔽全部线之后开 IF 仍会“收到”一个 PIC 已经没了的
  中断。现在两侧都按电平语义：PIC 每次重算可投递请求并相应拉高/拉低，
  CPU 只在线上有电平时置 `intr_pending`。
- **设备模型一修**：**空盘位的命令不产生任何应答**。原先对空盘也回 ABRT，
  于是状态寄存器变 0x41 非零，xv6 的 `havedisk1` 探测就以为 1 号盘存在
  （SeaBIOS 的 `ata_detect` 也是靠“状态为 0”判定空位）。现在空位状态恒 0、
  命令写忽略，只有任务文件寄存器照存（固件回读探针要过）。
- **实测证据**（`-bios bios.bin -hda xv6.img -hdb fs.img --mem 512`）：
  SeaBIOS 认盘 `ata0-0: CEMU VIRTUAL DISK ATA-3 Hard-Disk (4 MiBytes)` /
  `ata0-1: ... (0 MiBytes)`（从盘也认到了 → word 93 的 CBLID 位按 0 报，
  否则 SeaBIOS 会认定“device 0 在替 device 1 应答”而跳过从盘），
  `Booting from Hard Disk... Booting from 0000:7c00` → xv6 引导块自己从 0x1F0
  读内核（bootmain.c 的 PIO 路径）→ `xv6...` → `cpu0: starting 0`（内核 main
  全跑完 + mpmain）→ 进 scheduler 自旋、**0 个异常**，卡在 init 进程第一次读
  fs.img 等中断上。反证：去掉 `-hdb` 立刻得到 xv6 自己的
  `lapicid 0: panic: iderw: ide disk 1 not present`——说明它确实走到了那次读盘；
  装上 fs.img 就是等 IRQ14 的完成中断，而 xv6 把 IDE 中断交给 **IOAPIC**
  （0xFEC00000，本片还没有）投递 → 下一片。
- **回归**：`bash test/run.sh` → riscv64 136 passed / 0 failed、x86 9 passed /
  0 failed（门表寻址与 INTR 电平两处核心改动没有动摇任何基线）；
  `bash tools/depcheck.sh` → ok。
- **登记**：AGENTS.md 加 **D19**（PIIX3/IDE 简化清单：无 BMDMA、0x3F7 不解码、
  IDENTIFY 时序字留 0、ISA 桥只有身份 + PIRQ）。
- **下一片**：IOAPIC（+ LAPIC 定时器复核）→ xv6 起 shell；随后 PS/2 键盘
  (IRQ1) 让 shell 能输入。
## 阶段 4 片 1：PC 固件路线打通，SeaBIOS POST 完整跑完（2026-09-19）

x86 侧按用户指定走 SeaBIOS 固件路线（riscv 走 opensbi、xv6 用 D:\xv6img 的
i386 镜像验收），目标是让 xv6 在 cemu 里起来。本片先把 **POST 跑到底**：

- **板级**：`-bios FILE` 引导 —— ROM 窗口 0xC0000 起 256KiB（SeaBIOS 自身
  布局 BUILD_ROM_START..0xfffff，复位向量 F000:FFF0）+ 高别名 0xFFFC0000
  （它的 shadow 拷贝源，BIOS_SRC_OFFSET）；镜像可选，无镜像时板子自报
  `default_isa`。新设备 6 个：debugcon(0x402 POST 日志)、i8042(0x60/0x64 +
  A20)、port92(0x92)、cmos RTC(0x70/0x71 + 内存量寄存器 0x30/0x31/0x34/0x35)、
  pci 配置空间(0xcf8/0xcfc)、i440fx(8086:1237 + PAM 0x59-0x5F = ROM/RAM 切换)。
- **观测设施两处补齐**（§九：缺口要补设施而不是加探针）：x86 端口 I/O 此前
  不发 bus 事件（文档说 bus 覆盖“MMIO / IO 端口命中”）——补上；**异常在抛出
  时就发 T 事件**（原先只在投递成功后发，所以 triple fault 时毫无线索；补上后
  下面几个 bug 都是自己现形的）。
- **解释器修掉的真 bug**（全部由 SeaBIOS 逼出，不是为跑通加分支）：① 16 位段
  64K 回绕按“旧”CS 的 D 位判断，远跳进 32 位段被截断（`0xfc9e4`→`0xc9e4`）；
  ② INS/OUTS(0x6c-0x6f) 整族缺失（fw_cfg 用 `rep insb`）；③ **POP r/m 的内存
  目标地址必须在 ESP 自增之后计算**（SDM vol.2 POP）——修前 CPUID 检测序列
  `pushf;pop [esp+0x20];mov eax,[esp+0x20]` 读到栈上残留指针，把垃圾值 popf 进
  EFLAGS（TF=1）→ 单步 #DB → POST 早期 IDT limit=0 → triple fault；④ PCI 地址
  寄存器按字节合并（使能位曾被移出 32 位）；⑤ PCI 数据窗口偏移要取地址寄存器
  低字节（PAM 写曾落到 config[0..7]）；⑥ WBINVD/INVD(0f 08/09) 缺失 → #UD。
  另登记 D18（复位设施与 RTC 中断缺口）。
- **实测**（判据通道 = POST 日志/端口 0x402）：`RamSize: 0x02000000`、PMM
  重定位、`Found 1 PCI devices` + `PCI: init bdf=00:00.0 id=8086:1237`、
  PIR/MPTABLE/SMBIOS 拷贝、`Turning on vga text mode console`、e820 五项、
  `enter handle_19` → “No bootable device”（缺 IDE/磁盘，下一片）。
- **回归**：`bash test/run.sh` → riscv64 136 passed / 0 failed、x86 9 passed /
  0 failed（上述语义改动没动既有基线）；`bash tools/depcheck.sh` → ok。
  注：`cmake --build build --target check` 在 cmd/PowerShell 下会调到 PATH 上的
  WSL bash 而报 127（WSL 不认 `D:/...` 路径，是文档记过的坑），从 Git Bash 跑或
  直接 `bash tools/depcheck.sh` 即可。
- **下一片**：IDE(PIIX PATA) + `-hda/-hdb` 挂 xv6.img/fs.img → SeaBIOS int13h 读
  引导扇区 → xv6 kernel 输出到 CGA；随后 PS/2 键盘供 shell 使用。

## cemugui 回归最简形态：原生控件 + 自适应布局（2026-09-13，用户裁决）

暗色主题轮（2aa14f7）被用户否决（要白主题；自定义绘制层连出三层
事故），本轮回退并固化成最简形态：**全部控件原生系统绘制，零自绘**，
唯一保留的自定义是自适应布局与 Consolas 数据字体。过程中修掉的真实
缺陷，按层记录：

1. **WM_DRAWITEM 参数误用**：把 wParam（控件 ID）当 HDC 传给绘制
   函数——DC 无效、GDI 全部静默失败、按钮留在系统白底上（"白按钮
   看不见字"）。自绘层删除后此类问题整体消失。
2. **物理控件重叠**：底部提示静态的 x 锚在 MEMORY 面板左缘，而
   BREAKPOINTS 面板按 48% 比例展开会越过该位置——两个控件互相绘制
   （叠影"乱码"）。修：提示从两面板右缘较大者之后开始。
3. **初始窗口尺寸未随 DPI 缩放**：200% 显示器上窗口仍是 1160 物理
   像素，布局却按 2× 算——必然挤压重叠。修：WinMain 先取主屏
   LOGPIXELSX 再按 S() 建窗；最小尺寸同步缩放。
4. **父窗口自绘 chrome 与子控件局部重绘互踩**：WM_PAINT 画面板框/
   标题 + 静态控件独立重绘 = 状态栏叠影、提示双重绘制（部分失效
   区域裁剪的经典坑）。修：删除全部父窗口自绘，面板标题/提示/
   状态都是真正的 STATIC，各自正确重绘。
5. 曾试验 DeferWindowPos 原子重排与 WS_EX_COMPOSITED 合成，用户报
   常态闪烁——一并回退，最简 SetWindowPos 重排即够（教训：看不见
   的自绘层每层都是一个失败面；先有能用的基线再谈打磨）。

最终形态：原生控件 + 系统配色；自适应布局单一来源 ComputeLayout
（比例 + 钳制，Relayout 唯一消费者）；初始窗口/最小窗口随 DPI 缩放；
WM_DPICHANGED 实时重排；界面字体系统默认、数据区 Consolas；无反汇编
（外部工具）不变。用户测试通过后提交本轮。

## cemugui 暗色主题 + Per-Monitor DPI（2026-09-13，用户"现代一点"要求）

纯系统 API 的现代暗色：DWM 暗色标题栏（DWMWA_USE_IMMERSIVE_DARK_MODE，20/19
双探）+ Win11 圆角（DWMWA_WINDOW_CORNER_PREFERENCE）；uxtheme 的
DarkMode_Explorer 控件主题（暗滚动条/边框，序号 133/136/137 + SetWindowThemeW
全部 GetProcAddress 动态解析，pre-1809 自动回退亮色）；owner-draw 扁平按钮
（hover/按压态，SetWindowSubclass 跟踪，Connect 带主题蓝描边）；列表/编辑框/
列表框暗色（WM_CTLCOLOR* + LVM_SETBKCOLOR）；Per-Monitor V2 DPI 感知，
布局与字号全量随 DPI 缩放（S() 宏），WM_DPICHANGED 实时重排；Segoe UI/
Consolas 字体；面板框与说明文字由主窗口 WM_PAINT 绘制；状态行按态着色
（running 绿 / stopped 黄 / 失败红）。CMake 链接增 dwmapi/uxtheme。
逻辑层（RSP 会话、断点、跟随）零改动。

## 阶段 3.5 片 3：图形调试前端 cemugui（2026-09-13）

`tools/front/{front.c,rsp.c}`（新宿主侧工具，非机器部件；CMake 第二目标
`cemugui.exe`，链接 ws2_32/user32/gdi32/comctl32，复用 src/host/sock_win.c
并新增 HostSockConnect）。**纯 RSP 客户端**：qSupported / ? / g / m /
Z0,z0 / c,s / 0x03 异步中断 / D detach / qXfer target.xml——寄存器表
（名字/位宽/code_ptr/float 组）从目标描述解析，前端零 ISA 知识，x86 与
riscv64 同一套 UI。UI（Win32 单线程 + 100ms 定时器轮询，同显示窗口范式）：
寄存器列表、内存十六进制+ASCII 视图（256B/次，停机自动跟随 eip）、断点
增删（Z0/z0 pc 匹配表）、Run/Step/Interrupt/Detach（F5/F10 加速键）。
**不内置反汇编**（2026-09-12 决定）：窗口内注明用 llvm-objdump 或 gdb 看
同一 stub。

**过程中修复的模拟器侧真 bug**（GUI 联调暴露）：

1. **attach 冻结链**：BoardRunSteps 的 asleep-continue 路径从不调 stop_cb
   ——客户机 hlt 睡眠期间 stub 的 accept 轮询一次都不跑，前端连接永远躺
   在 backlog；配合 cga_hello 缺 EOI（PIC ISR 位不清，IRQ0 只发一次，
   park 空转不退役指令）= 前端阻塞读无限等。修：asleep 路径每轮跑
   stop_cb（对齐 i8254.h"睡眠期 poll 更勤"的既有约定）；hello 的 IRQ0
   存根补 EOI；GdbStubRun 的 attach 停止现在正确进入 Session（原代码
   `!stop_pending` 即 break，attach 到自由运行客户机会直接退出模拟）。
2. **x86 hlt 睡眠语义**（SDM vol.2 HLT；vol.3 halt state）：睡眠期间
   step 会投机执行 hlt 的下一条指令（副作用落地，jmp 每毫秒被执行）。
   修：wait 置位且无中断投递时 step 直接返回，不取指不执行；realmode
   122 全绿不受影响。GDB 事件：CEMU_DEBUG gdb 类别新增生命周期行
   （DebugGdbNote：accept/session/stop 原因），本轮全程靠它定位。
3. **前端健壮性**：socket 3s 收包超时（协议失配降级为断连提示，UI 永不
   冻结）；qXfer 属性解析限定在本元素内（原 strstr 越界把 fctrl 的
   group="float" 泄漏给全部寄存器 → 列表全空）；`m` 包无空格（RSP 帧
   无空格，带空格被 stub 判 E14）；UI 字符串全 ASCII（UTF-8 破折号在
   ANSI 窗口成乱码）。

**验收**：用户驱动 GUI 全流程（attach→寄存器 16 项→单步→Run/Interrupt
→断点 7c7b 命中→Remove→Detach）；内存视图对照引导扇区字节（eb fd 跳转
+ EOI 处理程序三指令 + "CGA display channel OK" 字符串逐字节正确）；
无头 RSP 探针（复用前端 rsp.c）验证握手/g 寄存器映像/m 读回/detach 后
cemu 存活全链路。**回归**：x86 9 passed / riscv 136 passed / depcheck ok、
零告警。阶段 3.5 三片全部落地（gdb stub、CGA 显示通道、图形前端），
arch.md 阶段 3.5 验收条款达成，衔接阶段 4。

## 阶段 3.5 片 2：CGA 显示通道（2026-09-13）

**设备侧** `device/video/`（新目录类）：`cga.c`——IBM CGA：0xB8000 16KB
VRAM（kRamOps 直接叠在板 RAM 上，总线小区优先规则接管译码；BusRamRange
同步改为最小匹配）、MC6845（R0-R17，索引口 0x3D0/2/4 三重镜像 = A2 未译码、
数据口 0x3D1/3/5 可读，QEMU/dearchap 惯例）、模式锁存 0x3D8 / 颜色锁存
0x3D9（写实只读，读 0xFF）、状态口 0x3DA（display-enable / vsync 位由宿主
时钟的帧模型驱动 + 光笔锁存位）、光笔 strobe 0x3DB/DC；端口按 ISA 字节设备
逐字节分解（`out 0x3D4,ax` 一条周期写 index+data 两通道）。渲染：文本模式
0-3（80/40 列、CRTC 起址/光标形状/逐属性前景背景、blink 位与背景亮度互斥、
扫描线垂直加倍、40 列水平加倍），字形 = seabios vgafont8（公有领域
fntcol16 集合，出处随码），调色板 = IBM CGA RGBI 十六色。复位态 = BIOS
POST 后的 mode 3 参数块。缺口语义登记 D17（图形模式黑屏、光栅近似、无过
扫描边框）。

**主机侧** `host/display_win.c`（win32 GDI：StretchDIBits + PeekMessage 泵，
同线程零锁）+ 接缝 `device/video/display.h` 的 DisplaySourceOps（设备发布
固定 XRGB 缓冲 + 版本计数，窗口按版本重绘——未来 VGA/ramfb 同缝）。接线：
Board.display_dev/display_ops（x86 板发布 kCgaDisplayOps）、BoardRunSteps
每步调 HostDisplayPump（内部 4ms 限频；用户关窗 = 模拟结束，exit 0）、
main.c `-display win32`（QEMU 惯例单横杠；默认无窗 = 回归全无头）。CMake
加 gdi32/user32。

**验收**：探针 `test/x86/cga/cga_probe.asm`（VRAM 写读回、MC6845 光标地址
程序化+读回、0x3DA vsync 边沿有界等待）cemu 与 qemu-system-i386 双绿
（同 rc=11 + "cga-probe ok"；QEMU 的 VGA 共享 6845 寄存器契约），入 run.sh
门禁；窗口视觉验收 `cga_hello.bin`（文本行/15 色属性条/闪烁属性/硬件光标
闪烁开灭两相均目验，截图与放大确认）——点 X 关窗即干净退出。**回归**：
x86 9 passed（smoke + cga + pm + kvm×5 + realmode）/ riscv 136/136 /
depcheck ok、零告警。

## 阶段 3.5 片 1d：Sdtrig 触发器 + x86 DR 断点（2026-09-13）

**riscv 侧（D6）**：新增 `cpu/isa/riscv64/trigger.c`——mcontrol6 匹配（execute
在 step.c 取指后按 pc、load/store 在 exec.c 漏斗按 vaddr 于访存前；EQ/GE/LT
写策略、chain、mode 门控、重入门），action=0 → breakpoint 异常（mtval=匹配
地址）、action=1（debug mode）Fatal。tdata1/2/3 改为 tselect 索引的每触发器
三元组（顺带修正旧代码 tselect 不索引的错位），tdata1 写策略 = checked_write
（type 固定、dmode 只许清、select WARL-0、hit/size/uncertain 清零），tdata3
mh 字段无 H 强制 0；新增 scontext CSR（0x5a8，M/S 由 priv 位自动门控）。探针
`test/riscv64/probe/probe_triggers.S` 六位全 1（执行/读/写/GE/tselect 钳制/
WARL）。riscv 套件 136/136 保持。

**x86 侧（D13 DR 部分 + D14 RF 部分）**：DR0-3 执行（fault，rip=断点指令，
RF=1 入映像）/写/读写/I-O 断点（trap，指令完成后统一投递）、DR6 写清除 B 位 +
BD/BS/BT + 保留位读 1、DR7 GD（MOV DR 触发 #DB+BD 且自清）/LE/GE、CR4.DE 对
DR4/5 别名与 #UD、TF 逐指令单步（BS）、RF 经 iret/popf/任务切换装载并在受保护
指令完成后清除、icebp(0xF1) #DB trap、TSS.T 任务切换 #DB（BT）。step.c 落地
landing-pad/提交点两处调试陷阱投递，数据断点检查挂 rm*/栈/串指令/xlat/moffs
漏斗（fetch/描述符/TSS/中断帧走原始通道不触发）。**顺带修正 rdmsr 编码错位**
（原接 0F 31 = RDTSC；SDM 规定 RDMSR = 0F 32，0F 31 现为显式 ud()）。

**验收**：kvm `debug` 测试 32 位移植件（上游 64 位专用）入库 run.sh 门禁，
cemu 与 QEMU TCG **双绿 8/8**（#BP/执行断点×2/单步/单步模拟指令/观察点×2/
icebp）；期望地址按 clang 实际编码重推导（AND 5 字节 vs 原版假设 6 字节、
call/pop 锚点），DR7 字段布局检索证实 4 位间隔且模式无关，DR6 写语义由测试
锚定（W1C 假设被证伪）——细节与用户裁决记录在 AGENTS.md 附录。DR 无第三方
oracle（tiny386/nemu-x86/TQemu 均未实现），SDM vol.3 ch.17 终裁。
**回归**：x86 **8 passed / 0 failed**（smoke + pm + realmode + kvm×5）、
riscv 136/136、depcheck ok；pm/realmode 状态流双跑确定性（状态行新增
dr6/dr7，复位值 ffff0ff0/400 可观测）。

## 阶段 3.5 A 档：kvm-unit-tests 32 位镜像入库，x86 平台补全（2026-09-12）

采纳 v86 checkout 的 kvm-unit-tests i386 flat 镜像（test/x86/build_kut.sh：
clang i386 + ld.lld + llvm-ar；三处构建适配记录在脚本头——IAS 的 %gs:6
movzwl 宽度、exception_table 符号数组化、stack.c 同名冲突改名）。四个镜像
入库：**taskswitch、taskswitch2、cmpxchg8b、memory**，rc=1（payload 0）
判据入 run.sh，x86 套件 3 → 7。

x86 机器平台补全（阶段 4 设备按需拉前，SDM/QEMU 文档出处随码）：

1. **CR4**（0f 20/22 reg 4）+ **PSE 4MB 页**（kPdePs × kCr4Pse，SDM vol.3
   4.3；A/D 更新在 PDE）——cstart 以 4MB 恒等映射开分页，硬前置。
2. **Local APIC 寄存器页** device/intc/lapic.c（0xFEE00000，SDM vol.3
   ch.11；寄存器存储语义，ID=0 单 BSP；中断投递仍属阶段 4）。cstart 的
   load_tss 读 APIC ID 决定 CPU 号，开放总线全 1 会使 LTR 越限 #GP。
3. **fw_cfg** device/misc/fwcfg.c（0x510 selector/0x511 data，QEMU
   docs/specs/fw_cfg.txt；signature/RAM_SIZE/NB_CPUS=1）。selector 是
   16 位写但 32 位代码段的 `out %ax` 生成 4 字节写——区域放宽到 4。
4. **WRMSR/RDMSR**（0f 30/31，CPL0；0x1B APIC base、0xc0000100/101 长模式
   FS/GS base 读写回；未知 MSR #GP）——套件 boot（setup_percpu_area）写
   长模式 GS base，QEMU 宽松语义有测试锚定。
5. **CMPXCHG8B**（0f c7 /1，mem-only；66 形式 CMPXCHG16B 不存在于 32 位
   ——判定用 !w32 而非 w32，初版写反被套件当场纠正）；LOCK 单核无操作。
6. **CMOVcc**（0f 40-4f，686+）——report 的 printf 路径使用。
7. **multiboot 引导栈 ESP=0x6f00**：multiboot 规范 ESP 未定义，QEMU
   -kernel 实测进入时栈在 mb info（0x7000）下方。旧 ESP=0 使早期 push 落
   入未映射被丢弃——真 bug 修复，pm/realmode state 流随之正当漂移，基线
   重采并双跑自洽。
8. x86 机器 RAM 1MB → 32MB（kvm 镜像链接在 4M+）。

**关键裁决（cmpxchg8b 的异常表扫描崩溃）**：exception_table_start/end 是
linker-script 同址符号对，clang 按 C 对象模型"不同对象地址不同"丢弃循环
入口检查（GCC 没做此假设），空表被扫过整个地址空间踩进未映射页。修复 =
声明为数组（linker-script 符号对的 canonical 写法），存档于脚本头。
另一件：本机 LLVM 无 ELF i386 compiler-rt，__udivdi3 族手写移位减法 +
宿主 / % 边界与随机对拍验证（校验程序自身曾因 INT64_MIN/-1 宿主除法溢出
假死，加边界保护）。

回归：x86 **7/7**（smoke+pm+4 kvm+realmode）、riscv **136/136**、depcheck
ok、零告警；state 漂移仅 g4(ESP) 及其下游 push 流（引导栈修复的正当结
果），pm/realmode 基线重采自洽。QEMU 对拍口径：esp/fw_cfg/APIC ID 均以
D:/qemu 二进制实测校准。

## riscv-tests 补全：rv64si/mzicbo/ssvnapot 入套件，三个语义 bug 修复（2026-09-12）

用户指出 riscv64 "没有完全通过"——实况：原 127/127 绿，但参照仓的
rv64si（7 个 S 态测试）、rv64mzicbo、rv64ssvnapot 从未构建入套件（阶段 1
只搬了 M 态批量）。clang riscv64 交叉补位构建（xpack gcc 已出工具清单，
reference.md 三预留的路）：两个 lld 适配——link.ld 的 SHF_* 换数值 FLAGS
(0x7)、测试源 `.global stvec/mtvec_handler` 行改 `.weak`（lld 拒绝
weak→global 升级，GNU ld 容忍；改后绑定不变）。配方入库
test/riscv64/build_si_extras.sh，9 个 elf 入 test/riscv64/。

三个真语义 bug（前两个由 rv64si-p-dirty / icache-alias 钉死）：

1. **kExStorePageFault = 14 → 15**（priv spec 表 1.2：14 保留，15 才是
   store page fault）。127 项旧套件没有校验 store PF cause 值的用例，
   OpenSBI 引导也未触发——套件缺口正是漏洞藏身处。
2. **Sv39 A/D 语义**：A 位无条件硬件置位；D 位仅 store/amo 需要，更新由
   menvcfg.ADUE 门控（ADUE=0 → store 故障交软件置 D）。原实现 A=0 也
   故障，dirty 的 handler 流程（手工置 D 后重试）走死。
3. **取指误要求 D 位**：`acc != acc_read` 惯用法把 ifetch 当写。取指永不
   置 D；icache-alias 的代码页 PTE 只有 V|X|A，一踩即炸。

修复后 rv64si 7/7、mzicbo 1/1、ssvnapot 1/1，套件 127 → **136/136**（参照
仓 rv64 全集）；x86 3/3、depcheck ok、state 零漂移（smoke/rv_csr，mmu 与
cause 改动后重验）。调试全程 CEMU_DEBUG（trap 行暴露 cause=14、gp 反解
TESTNUM、mem 行看 PTE 写入），无临时探针。

## 阶段 3.5 片 1（核心）：gdb RSP stub（2026-09-12）

- **结构**：`host/sock_win.c`（winsock 只进 host/，depcheck 规则同步扩
  winsock2/ws2tcpip）+ `debug/gdbstub.{h,c}`（RSP 会话：包帧/转义/校验和、
  qSupported、qXfer target.xml、g/G/m/M/Z0/z0/Z1/z1、c/s/vCont;c|s、?/D/k/
  H/qfThreadInfo/qAttached、Ctrl-C 0x03 异步中断）+ `BoardRunSteps`（从
  BoardRun 拆出的批次循环，stop 回调 = 每步提交后观测点）+ main.c
  `-s`/`-S`/`-gdb tcp::PORT`（QEMU 惯例）。
- **断点 = 模拟器侧 pc 匹配表**（QEMU/gem5 同款，不写客户内存；提交后
  检查 = 命中处指令未执行，可观测语义同硬件断点）。isa_ops 新增
  gdb_read_regs/gdb_write_regs/gdb_last_trap 三钩子（step.h）；两 ISA 私有态
  加 trap_seq/trap_signal（x86 INTR 投递不计——硬件中断不是调试事件）。
- **观测设施**：CEMU_DEBUG 新 `gdb` 类别（stub 协议包 tx/rx 事件行）——
  本轮 lldb 排查全程靠它定位，无临时探针。
- **裁决与修复**：
  1. gdb 的 i386 校验要求 core 特征内含 x87 组（st0-7 + fctrl..fop）且
     带 `<architecture>` 元素，"纯 16 寄存器"描述被拒（离线
     `set tdesc filename` 二分定位；期间一次 sed 模式未匹配导致四例假
     通过——教训：验证脚本自身要先验证）。形状照 QEMU 端上的 gdb 官方
     32bit-core.xml；本机无 FPU（D13），FPU 镜像全零读写并在注释登记。
  2. bus.c FindRegion 区间回绕 2^64 误判映射：lldb 探测读
     0xfffffffffffffe00+0x200 触发 Fatal。共享漏斗修复（addr+len 溢出
     预拒绝），guest M 态裸机对顶地址访问同边可达。
  3. mingw64 自带 gdb 不含 riscv 架构 → riscv 侧验收客户端用 lldb
     （LLVM 白名单内），x86 侧用 gdb。
- **单线程模型记录**：attach 即停（QEMU 的 io-thread 异步应答在单线程
  下不可得，登记为对 QEMU 的已知偏差）；`-s` 不带 `-S` 时 guest 先跑，
  连接后停。
- **验收**：gdb.exe 走 x86 realmode（attach/`break *0x8061`/continue 命中
  inst=1/寄存器 eip eax cs eflags/内存读 0f 01 15 48/kill 干净退出）；
  lldb 走 riscv（attach SIGTRAP/断点命中 0x80000008/寄存器读+写 pc 生效/
  内存读与镜像一致/detach 后 guest 跑完 exit=0）；lldb 客户端侧反汇编
  （csrwi/csrr）与 CEMU_DEBUG trace mnemonic 逐条一致。
- **回归**：riscv 127/127、x86 3/3、depcheck ok（含新 winsock 边）、零告
  警、state 零漂移（smoke/rv_csr 重采对拍）。
- **stub 层已知限制**（工具语义，非 CPU 语义）：stub 内存访问为物理地址
  （realmode/裸机 M 态 linear==物理；PM 虚拟视图需无故障 translate probe，
  挂后续）；p/P 单寄存器包回空（RSP 内回退到 g/G，非简化）。

## 阶段 3.5 前置任务：exec.c 名字级 mnemonic 填充（2026-09-12）

arch.md 阶段 3.5 新增前置任务后本轮完成：insn_rec.mnemonic 全链填到
SDM/手册指令名（名字级；语法级反汇编形式不做，见同日范围修订）。

- **riscv64**（exec.c + step.c）：主 switch 逐 case、csr_op（f3 名表）、
  system_op 六叶、amo_op（lr/sc + 两宽度内表 18 臂）、fp_op/fma_op/fp_ldst
  （.s/.d 后缀名表）、cbo、压缩三象限全臂；c.fld/c.fsd/c.fldsp/c.fsdsp 在
  fp_ldst 调用后覆写 c 前缀名。step.c 补每步 rec.mnemonic=NULL 复位
  （x86 本有；riscv 静态 frame 会把上一步名字串进下一步——填充过程中
  自查抓到）。
- **x86**（exec.c）：run_op/run_op2 case 入口逐一填（脚本对 129 个单标签
  case 做锚定插入 + 手改共享体/组/reg 子字段 ~66 处，一次性脚本用后即删）；
  grp1/2/3 按 reg 查名（kGrp1/kShift/kGrp3Names），jcc/setcc 两张 cc 序名表
  挂 cond() 旁；串操作按宽度 b/w/d 三态；0f 00/01 组、bt 族、lar/lsl、
  lss/lfs/lgs、movzx/movsx、xadd、bswap 全覆盖。x87 escape 显示 x87，
  FNINIT 接受后覆写 fninit。
- **顺带修复（规范裁决）**：OP-32（0x3b）f3=4/6/7 在 f7=0 处接受未定义编码
  xorw/orw/andw——clang riscv64 汇编器实证三助记符不存在（mulw/sllw/
  remw 均可编），该三臂改 illegal，仅保留 divw/remw/remuw。MULW 在
  f3=0/f7=1 曾自疑错位，llvm-objdump 反汇编 rv64um-p-mulw 证实 cemu
  解码与规范一致（教训：手册记忆不可靠时用工具实证，勿手算编码）。
- 填充位置一律 case 入口：指令中途故障（raise longjmp）时 trap 行同样带名。

回归：riscv 127/127、x86 3/3（smoke+pm+realmode）、depcheck ok，零告警；
state 零漂移——smoke/pm/realmode/rv_csr/probe_counters 五条 state 流与
改动前逐字节一致（pm/realmode 撞 debug 枢纽 5MB 硬顶，按规则收敛到
4.9MB 前缀后 diff）。trace 名列效果：`CEMU_DEBUG="trace:table"` 的
MNEMONIC 列由空 → 逐指令指令名。

## 阶段 3.5 范围修订：GUI 前端不内置反汇编（2026-09-12）

用户决定：图形调试前端降为纯 RSP 客户端（寄存器/内存/断点/单步），不含
反汇编视图；需要反汇编一律外部工具——gdb/lldb attach 同一 stub（客户端
侧反汇编，QEMU gdbstub 同款）或 llvm-objdump 离线看镜像。连带效果：无
LLVM 子进程选型问题（llvm-mc 缺失一事作废），cemu 本体与前端均零新依赖。
cemu 侧执行流观测仍由 CEMU_DEBUG trace 的译码表 mnemonic 覆盖。arch.md
阶段 3.5 与 AGENTS.md §十已同步修订。

## seabios clang 构建修复：完整 POST 跑通（2026-09-06）

用户约束续：compare/ 放了官方 1.17.0 源码+tarball+官方 bios.bin（未动），
外层树去掉全部 make/mingw 遗留后，继续修 clang 构建的引导挂死。

- **清理**：gen-ninja.py 源文件清单改为按 1.17.0 Makefile 原序钉死（include
  顺序进 tmp.c 影响段布局，承重）；删除 Makefile/Makefile.probe/probe6.c/
  scripts/kconfig/tarball.sh/test-build.sh 及六个 make 管线孤儿脚本
  （checkrom import 的 buildrom 保留）、kconfig 运行残留。清理后再生
  build.ninja/.includes 与拆前逐字节一致。树里只剩 ninja 链 8 脚本+纯源码。
- **依赖跟踪修复（关键）**：全程序编译规则原先不跟踪 src/*.c——clang
  `-MD` 的 depfile 按输入名落盘（ccode32flat.d 而非 ccode32flat.o.d），
  ninja 从未用过。补 `deps=gcc`+`-MF $out.d` 后增量构建才可信；此前两轮
  调试被陈旧对象污染。
- **挂死根因（分层取证）**：#UD 风暴（-d int）→ f000:4001 等地址
  （monitor 寄存器+栈对符号表）→ **`transition32_nmi_off` 是
  `.text.asm.transition32` 内偏移 +0x18 的局部标签，layoutrom 只为
  offset-0 段符号发 LDS 方程 → 标签在两个 LDS 里都没有方程 → 各 pass
  解析值互不一致（rom16 内部=段内偏移；xref 导出=线性绝对），且
  移植补丁的 `ljmpl $imm,$sym` 在 32 位汇编上下文被 clang 编成 o32
  `66 ea` 远跳、偏移字段装完整线性地址，实模式执行 EIP=0xfc9be 直接
  出轨**。
- **修复**：romlayout.S 给 `transition32_nmi_off` 独立成段
  （`DECLFUNC`，offset-0 → 两个 LDS 都出方程）；stacks.c 的
  __call32/call16 远跳改为**寄存器间接 o16 近跳**：`movl $sym,%edi /
  subl $BUILD_BIOS_ADDR,%edi / jmp *%edi`——线性减法用 asm 立即数做
  （C 算术会被 IAS/链接器符号算术搞坏：lea 带 eax 基址、addend 重定位
  丢失、R_386_16 越界三连）。16 位 pass 的 rom16 链接值本来就是段内
  偏移，__call32 不减。
- **结果**：自建 bios.bin 119 行 debugcon 日志 vs 官方 bios.bin-1.17.0
  120 行，除版本串/重定位地址/UMB 尺寸（代码生成差）与一行 floppy 错误
  打印（上游 main 漂移）外逐行一致，走完初始化搬运、ACPI、SeaVGABIOS
  option rom、pmm 往返、四轮引导尝试至 "No bootable device."。
- `.version` 文件（内容 1.17.0，抄官方 tarball 惯例）→ 横幅显示
  "SeaBIOS (version 1.17.0-20260906_152920-…)"。
- 调试方法沉淀：#UD 风暴用 `-d int` 限时采样（3.6MB/s 超 5MB 预算，
  必须 ≤1s 窗口）；挂死现场用 monitor `info registers`+栈对符号表；
  `-debugcon` 简写静默不挂接，必须显式 `-chardev`+`-device isa-debugcon`。

## seabios 构建链去 mingw 化：ninja + 纯 LLVM（2026-09-06）

用户约束：seabios 只许 clang/lld/ninja，不许 mingw（gcc、GNU binutils）、
WSL/MSYS2、MSVC。此前 build.ninja（scripts/gen-ninja.py 生成）仍有两处
mingw 残留：layoutrom/checkrom 消费的 dump 由 mingw64 GNU objdump 产出；
asm-offsets.h 靠 kconfig/make 附带产物。本轮收掉：

- **scripts/llvmdump.py**（新）：llvm-readobj --sections 产 GNU 形状
  Sections 块（关键缺列 Algn=2**n），llvm-objdump -thr 符号/重定位块透传
  （实测与 GNU 逐列一致）；跳过 SHT_REL/SHT_SYMTAB/SHT_STRTAB 与 GNU -h
  行为对齐。等价性以 layoutrom.parseObjDump 实证：code32seg
  115 段/142 符号/174 重定位、code32flat 1014/2088/5820，与 GNU dump
  解析结果全等。
- **asm-offsets.h 收进 ninja**：clang -S（F16 flags）+ gen-offsets.sh，
  再生输出与 gcc 时代逐字节一致（仅头注释里的调用路径不同）。
- **净室重建**：out/ 清空仅保留 autoconf.h，gen-ninja.py 重生输入 +
  ninja 17/17 全绿；bios.bin 262144 字节，复位向量 ljmp f000:e05b 与
  LAYOUT 报告一致；两次构建仅差 buildversion 时间戳秒数 2 字节（唯一非
  确定性源，修不了也不需要修）。
- **功能冒烟**：qemu-system-i386 引导自建 bios.bin，isa-debugcon 捕获
  （注意：本版 QEMU 的 `-debugcon` 简写**静默不挂接**，必须显式
  `-chardev stdio,id=dbg -device isa-debugcon,iobase=0x402,chardev=dbg`），
  打出 "SeaBIOS (version ?-20260906_124119-…)" 并走完 fw_cfg/e820/
  init 搬运（96560 字节重定位到 6fe8640，flat 布局正确性的硬证据）/
  PCI 全枚举。
- **不可再生项**：kconfig conf.exe 是宿主 C 程序，本机无 MSVC、clang 编
  Windows 宿主程序必须挂 mingw 头或 MSVC 头——两条路都违反约束。
  .config/out/autoconf.h 定位为配置输入（等价内核项目提交的 .config），
  ninja 构建不执行任何宿主编译。
- build.ninja 里已无 gcc/mingw/GNU binutils 引用；bash.exe（Git Bash）
  仅作命令壳，与 cemu 回归脚本同一约定。

## 阶段 3 项 5：LDT/LDTR 机制整体（2026-09-06）

exec.c 补齐 LDT/LDTR 全链（对照 v86 lookup_segment_selector/load_ldt、
tiny386 read_desc、SDM vol.3 2.4.4/3.5/5.3）：

- **LDTR 状态**：可见选择器 + 描述符缓存（base/limit），lldt null 清缓存。
- **TI=1 查找**：desc_parse 按选择器 TI 位选 LDT 缓存；表限检查按
  table_limit(sel) 双表化，落到各装载点自己的向量（data/CS→#GP、SS→#SS、
  任务切换→#TS）。null LDTR 缓存 limit 0，任何 TI=1 引用按表限规则失败
  ——tiny386/v86/QEMU 三方一致（一度按 #TS 实现后被 QEMU 双跑实测推翻，
  见下）。
- **指令组**：lldt（CPL0、GDT-only、type 2、#NP）、sldt、verr/verw（查
  找缺陷只清 ZF 不故障；verr=可读、verw=可写数据、conforming 跳过 DPL
  规则）、lar（hi dword & 00FxFF00——x nibble 未定义位清零，QEMU 实测校
  准；v86 用 00FFFF00 保留之，两读法皆 SDM 兼容）、lsl（G 展开限长）、
  arpl（0x63，PM-only #UD）。六条新指令 PM-only，与 SDM/v86 一致。
- **任务切换**：TSS +0x60 装载 LDTR（null 合法、TI=1/#TS、越限/#TS、
  type≠2/#TS、不present/#NP），且在段选择器装载**之前**——新任务的
  TI=1 选择器走它自己的 LDT（SDM 7.2.1 步序）；旧任务 LDTR 不写回
  （SDM 切出保存列表无 LDTR，v86 注释掉的 save 行印证）。
- **seg_commit 的 A 位写**改按选择器所在表（GDT 或 LDT）。
- **LAR/LSL** 补齐（0f 02/03）：类型有效表照 v86 LAR/LSL_INVALID_TYPE，
  系统类型一律 DPL≥max(CPL,RPL)，失败 ZF=0 且目的寄存器不变。

探针扩到 24 项：t19 建 LDT+lldt/sldt 回读+TI=1 数据装载、t20 段限
#GP(0)+null LDTR 查找 #GP(0x0c)、t21 verr/verw 矩阵、t22 lar/lsl 值与
失败（LAR 掩码 00FxFF00 的 x nibble 清零）、t23 arpl、t24 任务切换装载
LDT 后任务体自用。**QEMU 对拍 22/24**（pm_qemu2.txt）：t7 与 t20 检查 1
同根——此 dirty QEMU 的 TCG 不执行数据段限检查（LSL 证 limit 0x1ff、
store 照落），GDT/LDT 一视同仁，SDM 卷 3 5.2.1/5.3 强制，按 cemu 输出
判（run.sh 注释登记）。

**调试教训（九.2 三度亲证）**：t20 曾按"SDM 要求 #TS"实现 ldt_check，
QEMU 双跑 + 诊断字符（"00"=无 #TS 投递、"5"=限违例未抛）推翻了级联假说
——三个参考实现（tiny386/v86/QEMU）一致走表限规则，SDM 文本无从查证时
以实现共识落地并在 D16 销账行留痕。另有探针三处自伤（LDT 描述符 dword0
字节序、跨段恢复时 `mov ax` 毁掉 eax 读回值、`pop eax` 覆盖 ax 里的选择
器）全靠 CEMU_DEBUG trace/mem 定位。

回归：riscv 127/127、x86 smoke+pm+realmode 全绿、depcheck ok；smoke
state 流 194KB 全量一致，realmode state 流新旧二进制前 5MB 一致。

## 阶段 3 项 4：386 两级分页（2026-09-06）

exec.c 新增 page_translate（对照 tiny386 tlb_refill/translate_lpgno 与
SDM 卷 3 §4.3/4.6/4.7；v86 checkout 无可读分页核心，gem5 此版 walker 不写
A/D，行为裁决靠 QEMU 双跑）：

- **两级 4KB 页走**：CR3→PDE→PTE；R/W 与 U/S 两级组合（两级都允许才放
  行），权限检查一把出：user 需两级 U=1，写需组合 W=1，supervisor 仅当
  CR0.WP=1 才受 R/W 约束（WP 是 486 位，SDM 卷 3 4.6 定义了它，照实现）。
  缺页（任一级 P=0）error code 不带 P 位，其余违例 P=1；W/U 按访问与 CPL。
- **A/D 位**：成功翻译才置 A（PDE+PTE）、写成功才置 D——故障访问不动页
  表。RMW 走物理侧直达 RAM；页表自身的读取是机器态，不进 debug mem/bus
  事件。这版 dirty QEMU 实测会写 A/D 且与 cemu 时序吻合（t17 双跑一致）。
- **#PF 通道**：pf_fault 置 CR2 = 故障线性地址再 raise（error code 经既有
  vec_has_ec 压栈）；CR2 可经 MOV CR2 读写。
- **翻译挂钩点**：bus_load/bus_store 唯一漏斗先 page_translate 再碰总线，
  取指/数据/栈/串/系统表全被罩住（GDT/IDT/TSS 走线性地址、按 SDM 同样翻
  译）。debug mem/bus 事件改报**物理**地址（总线与设备所见的地址；调试内
  核页表/DMA 时才是有效观测面），realmode 线性==物理不受影响。
- **控制寄存器组补全**：MOV r,CR2/CR3、MOV CR0 的 PG 需 PE（否则 #GP(0)，
  SDM 卷 2）；0f 20-23 全组 CPL≠0 → #GP(0)；LMSW（PE 只能置不能清、PG 不
  动）、CLTS、INVLPG（无 TLB，无操作、不对未映射页故障）照 SDM 销账；
  LGDT/LIDT 补 CPL 检查。DR7.GD 调试支持并入 D13。
- 任务切换的 CR3 携带（项 3 已写）现接分页生效；无 TLB，CR3/CR0 写无需冲
  刷（探针里的 CR3 reload 是给 QEMU/真机缓存的，cemu 上是空操作）。

探针扩到 18 项（t14 恒等映射+PG|WP 开启走表、t15 缺页 #PF ec=2+CR2、
t16 只读页写 #PF ec=3（WP=1）、t17 A/D 位、t18 ring3 用户/监督页 ec=7；
OBS 槽位挪 0x2700 避开扩容后的 RES）。**QEMU 对拍 17/18**（pm_qemu2.txt），
唯一分歧仍是已登记的 t7 dirty-QEMU 段限缺陷；五个分页用例双跑逐字节一致。
回归：riscv 127/127、x86 smoke+pm+realmode 全绿、depcheck ok；smoke state
流与 9/5 基线逐字节一致，realmode state 流新旧二进制前 5MB 一致（debug
枢纽 kOutLimit 硬顶，与本次改动无关）。

## 阶段 3 项 3：调用门（参数拷贝）+ 任务切换（2026-09-05）

exec.c 新增 PM 门机制全链（对照 v86 far_jump/do_task_switch/call_interrupt_vector
与 tiny386 pmcall/task_switch，SDM 裁决）：

- `do_task_switch`（SDM vol.3 7.2.1 全序）：TSS 描述符检查（忙位三态——JMP 清旧
  设新/CALL 只设新/IRET 清旧保新，P、限长 ≥0x67）；旧态保存（EIP/EFLAGS/GPR/
  6 段选择器，段字段步距 4；IRET 保存时清 NT）；CALL 写 back-link、强制 NT；
  TR 先于新态提交（tiny386/SDM 序）；CR3 携带、LDT 仅接受空（D16）、
  FLAGS 全图载入、CS 以 #TS 检查提交（cpl=RPL）、GPR/数据段按普通向量载入、
  CR0.TS 置位；异常经任务门投递时 ec 压新栈。
- 调用门 `call_gate`（0xC/0x4）：门 DPL/P、内层 CS 检查、向内时 TSS 环栈
  （`tss_stack`，do_int 复用）+ 门 count 字段参数拷贝（SDM CALL 伪代码序：
  SS:ESP 最深、参数居中、CS:EIP 顶部）；JMP 过门不拷参数不换栈
  （非 conforming 且 DPL≠CPL → #GP）。
- `pm_far` 统一 0x9a/0xea/ff /2,3/ff /5 的 PM 分派；`pm_ret` 实现 PM
  RETF/RETF imm——**外层返回 imm 用两次**（先在本栈跳过门参数再读
  SS:ESP，提交后再清调用者栈参数；felixcloutier 转录的 SDM RET 伪代码
  裁决，QEMU 行为印证）。
- pm_iret 加 NT 嵌套任务返回（读 back-link，空 → #TS）；IRET/RETF 全部
  peek-then-commit——`raise_` 不回滚寄存器，故障必须发生在任何提交之前。
- 既有偏差修正：软件 `int n` 落在带 ec 向量上不再压 ec=0（SDM vol.2 INT，
  v86 传 None）；ltr 置 TSS 忙位（SDM 7.2.3）；LDT Fatal 站点改为按站点
  向量抛故障（D16）。

探针扩到 13 项（t9 同权调用门/t10 内层门+参数拷贝+RETF 8/t11 lcall 任务
往返/t12 ljmp 任务往返+忙 TSS #GP(0x38)/t13 int 任务门+#GP 经任务门 ec
传递+EIP 修复）。QEMU 对拍 12/13（t7 仍是 dirty-QEMU 段限缺陷，已登记），
x86 run.sh 3/3、riscv 127/127、depcheck ok；三套件新旧二进制 state 流
逐字节一致。调试教训再+1：TSS 段字段步距与 RETF 双 imm 都是脑内推演失败、
靠"QEMU 仲裁 + 内存 dump + 手册转录"定位的（九.2 再验证）。

## 阶段 3 项 1-2 验收：PM 冒烟探针 + 六处解释器修复（2026-09-05）

PM 冒烟探针（test/x86/pm，multiboot ELF 入口，nasm+ld.lld 构建）在平坦
保护模式下自建 GDT/IDT/TSS，走完项 1-2 的门语义八项：段装载+读写回、
ring0 越限 #GP(0)、同特权门（不换栈/清 IF/压栈 flags 位1）、IRET 入
ring3、ring3 过门 TSS 换栈（老 SS/ESP 帧）、门 DPL 违例 #GP(vec*8|2)、
ring3 越限 #GP(0)、门帧改写回 ring0。探针把解释器里六个从未被验收件
踩过的 bug 全部炸出并修复：

1. **门字段布局颠倒**（exec.c do_int）：SDM 卷3 图3-8 门布局是
   [off15:0][selector][保留][P DPL type][off31:16]，原实现把 bits0-15 当
   selector。PM 门从未被走过，一踩即 LDT Fatal。
2. **SIB 基址 ESP 缺 SS 默认段**（exec.c modrm）：只给了 EBP，漏了 ESP
   （SDM 卷1 2.1.2）；`add [esp],2` 走了 DS。实模式 seg_use 早退掩蔽。
3. **int imm8 返回地址差 1**（exec.c case 0xcd）：`do_int(imm8(),
   rec.pc+d.nxt, ...)` 踩 C 实参求值顺序未定义——imm8 先提出去。
4. **提交点缺 32 位截断**（step.c）：EIP 是 32 位，负 rel32 靠回绕；
   原来在 64 位 pc 上相加不回绕。实模式被 code16 掩蔽。
5. **A 位写破坏描述符**（exec.c seg_commit）：`(ar&0xf)|1` 把 P/DPL/S
   抹掉；应为 `ar|1`。
6. **故障投递中故障无限乒乓**（step.c）：新增 delivering_vec——投递中
   故障升级 #DF（SDM 卷3 6.9），#DF 自身投递再失败 = 三重故障 → 停机。
   之前 GP↔DF 无限 longjmp 循环。

QEMU 对拍（qemu-system-i386 -kernel，multiboot 同镜像）：**7/8 逐字节
一致**。唯一分歧 t7（ring3 数据段越限写）：本机 QEMU 是 dirty 开发版
（10.2.92, v11.0.0-rc2-12119-gaa7f0eb8d8-**dirty**）——LSL 证实其缓存的
limit=0x1ff，但越界写照样落地（monitor xp 证值落在 0x40000）——TCG 不
执行数据段限检查，与 SDM 卷3 5.2.1 相悖。按裁决序 SDM 为最终裁决
（DAS 先例只允许"实机真值表"推翻 SDM，dirty 构建仿真器不算实机），
cemu 保持 fault，run.sh 注释登记该分歧。待办：换干净 QEMU 构建重新
校准 t7。

回归：riscv 127/127、x86 smoke+pm+realmode 全绿（run.sh 新增 pm 判据，
timeout 30）、smoke state 基线与改动前逐字节一致（git stash 采基线）、
depcheck ok。教训两次亲证 AGENTS.md 第九节：TSS 描述符常量字节序手写
错了（0x89 应在 byte5）、门槽位 0x13 是十六进制 19——纸面推演全部
错过，靠 CEMU_DEBUG + gdb（-O0 临时构建）+ QEMU monitor/gdbstub 定位。

## 阶段 3 项 2：PM 异常与中断门（2026-09-05）

do_int 按 CR0.PE 分臂。PM 臂：IDT 门派发全序（表限 → 软中断 DPL → 门型 →
P，error code = vec*8|2|EXT）→ 目标 CS 检查（可执行/DPL<=CPL/P）→ 特权
变更时 TSS SS0:ESP0 换栈（缺陷全部 #TS，支持 32/16 位 TSS 偏移）→ 压栈
（门宽度决定 16/32 位，非操作数宽度）→ IF/TF/NT 处理（中断门清 IF）。配套：

- PM IRET：同特权/外层两臂；外层返回弹 ESP/SS（SS 检查全 #GP、按新 CPL
  校验 DPL/RPL）；IOPL/NT 仅外层或 CPL0 返回可载入；RF/VM 继续屏蔽（D14）。
- LTR/STR（0f 00 组）+ TR 描述符缓存（tr_base/limit/ar）；sldt/lldt/
  verr/verw 留项 5。任务门与 NT 任务返回显式 Fatal（等项 3，不装假语义）。
- eflags 命名位补 iopl/nt/rf/vm；do_int 签名加 soft/ec（软中断才查门 DPL；
  ec 按向量表 vec_has_ec 压栈，实模式不压）。
- 参考格局：tiny386 call_isr/pmret（结构主 oracle）+ v86 call_interrupt_vector
  （行为对照，kvm 驱动）+ gem5（其门派发在本版本下放微码 ROM，不可直读；
  价值=行为裁决，复位 TR 态已记档）。自查抓到门 EIP 位段掩码 bug
  （gate>>16 未截 16 位，会混入 type/limit 字节）。
- 回归：riscv 127/127、x86 smoke+realmode 122/122、state 基线零漂移。
  PM 门路径尚无验收件——下一步先搭 PM 冒烟探针（multiboot 平段 +
  GDT/IDT/门 + 换栈往返），再进项 3（调用门/任务切换）。

## 阶段 3 开工：项 1 段机制核心（2026-09-05）

保护模式语义第一片：描述符解析 + 特权/limit 检查（exec.c +218 行，回归
全绿、state 基线零漂移）。要点：

- **检查语义主 oracle 是 SDM 卷 3 §5.3**，不是 tiny386——实读发现 tiny386
  的 limit 检查整个 `#if 0`、特权检查只有半套（自认 TODO）。tiny386 只作
  描述符解析/缓存形状参照。
- 结构：`desc_parse`（实模式 sel<<4 / PM 解析表项）→ 各目标类检查
  （`load_data`/`load_ss`/`load_cs`，按 SDM 伪代码顺序）→ `seg_commit`
  （写缓存 + 置 A 位）；访问侧 `seg_use` 钩进 rm*/栈/串/moffs/xlat/fetch。
  CPL 从 CS 缓存的 DPL 派生，不设独立状态。
- 栈模型修正：B=0 栈只经由 SP——访存地址 16 位、ESP 高 32 位保留
  （SDM vol.3 3.4.5）。kvm realmode 的 push_pop_high_esp_bits 测试钉死
  该行为，第一版把寄存器更新和访存地址混用导致 1 FAIL，分开后过。
- 连带修复：STI 的 INTR inhibit 窗口原来是死的（步尾清零早于下一步采样），
  改为采样后消费；POP SS/MOV SS 补上同一 shadow（SDM vol.2）。
- 其他：MOV to CS 判 #UD；far call 先查描述符后压栈（故障不压栈）；
  xlat 补 2^addr_size 取模；GDT 表越界 #GP(sel)；LSS 走 SS 全套检查。
- 门/任务切换/分页/0F 00 组增量仍未动（项 2-5）；LGDT/LIDT/SMSW/MOV CR0
  原已有。D13 恢复阶段已按重排改挂阶段 3。

## 路线图重排（2026-09-05，用户指令）

x86 保护模式提前、cesdk 放后：**阶段 3 = x86 保护模式与分页**（原阶段 4
的语义核心拆出，最后一大块指令集语义，D13 指令侧按需销账）；阶段 3.5 =
调试器与 GUI（不变）；**阶段 4 = 设备全集与真实 OS**（xv6-x86 bin 路线 →
Linux，riscv 侧 xv6-riscv/OpenSBI 并行；SeaBIOS 本体跑通属设备集成、不
前置）；**阶段 5 = cesdk**（真实 OS 跑通后再做 SDK，D11 销账）；阶段 6 =
arm/mips。理由：收掉 PM/分页后不再专研指令集语义，其后全是设备与 IO；
固件已备（x86 SeaBIOS bios.bin、riscv OpenSBI fw_jump）。D7（LR/SC 多核）
继续挂账。

## 阶段 3.5 立项：调试器与 GUI（2026-09-05）

用户立项调试器 + GUI，原 §十「明确排除」条款解除，落位阶段 3.5（cesdk 与
x86 全集之间，见 arch.md 阶段计划）。三片：显示通道（Win32 GDI 窗口 +
CGA/VGA 设备，CGA 文本 realmode 即可点亮）、gdb RSP stub（软件断点先行，
硬件断点销账 D6）、图形前端 + LLVM 反汇编（llvm-objdump/llvm-mc 子进程
起步，cemu 本体零新依赖）。工具链统一 LLVM 系列：x86 侧已在用 D:/LLVM
23.1，cesdk 构建前端（阶段 3）切 clang riscv64 交叉。D7（LR/SC 多核）按
用户指示继续挂账不动。

## riscv64 浮点/RVC 修复（2026-09-05）

回归基线从 114/13 → **127/127 全绿**。三个独立根因，全部在 exec.c 的译码/
写回层，fp.c 的 IEEE 语义（E1 登记的宿主浮点）一个没碰：

1. **fmv.x.w 把整个 NaN-box cell 拷进 rd**。规格（volume I FMV.X.S）：RV64
   下结果 = 低 32 位**符号扩展**进 rd（上 32 位随位 31），测试用 `lw` 符号
   扩展的期望值配对，正负结果都要对。第一版修复误用零扩展（负数用例反败），
   反汇编 test_3 对照寄存器现场后纠正为符号扩展。
2. **fcvt 符号性字段映射错**：`to_signed = rs2 < 2` 把 rs2=1 的 .wu 当有
   符号（rs2=3 的 .lu 碰巧对）。编码语义 rs2 偶数=有符号、奇数=无符号
   （0:w 1:wu 2:l 3:lu），修为 `(rs2 & 1) == 0`。fcvt.w.s/wu.s/l/lu 与
   I2F 两侧同修。
3. **fcvt→整数写回了浮点组**：case 0x60/0x61 算出整数后 `break`，掉进
   fp_op 尾部公共写回 `f[rd] = r`——-1 写进了 f10，a0 纹丝不动。state 流
   直接暴露（g10 不变、f10=0xffff_ffff_ffff_ffff）。修为显式 `x[rd] = ...;
   return`，与比较类/fmv.x 类一致。E2 类教训：新增返回整数的 case 忘了
   绕开公共浮点写回。
4. **c.j 目标滑 2 字节**：Q1 case 5 写 `pc += imm; break`，掉进 RVC 公共
   出口 `return pc + 2`——目标 = 指令地址 + imm + 2。三个 c.j 链（rvc.S
   用例 30）被逐个滑进 `j fail`。修为直接 `return pc + imm`，与
   c.beqz/c.bnez、c.jr/c.jalr 的显式 return 风格一致。

方法论记录：失败用例号从 RVTEST_FAIL 的 `(gp<<1)|1` 反解（exit dump 的
gp）；位段类 bug 用 llvm-objdump 对照 cemu trace 的 raw/dnpc 即可裁决，
无需手解码。E1 登记项保持不变（宿主浮点的长尾风险独立于本次修复）。

## grp2 立即数移位修复（2026-09-05）

- 症状：realmode 118/122，FAIL 为 DAS/lahf/movsx ah/movzx ah，疑似 AH 访问
  共性 bug（后证为误判）。
- 定位：探针 test/x86/realmode/probe_ah.c（kvm harness 最小复刻 + 寄存器
  转储 + DAS 前 32 真值表例）复现四例后，发现各测试的比较表达式都在客户机
  内执行 `x >> 8/16/24` 且结果恒等于未移位的 x——C1（grp2 imm8 计数形式）
  的 imm8() 作为调用实参在 grp2 内部 modrm() **之前**求值：modrm 字节被当作
  移位计数、立即数字节被当作 modrm 译码。D1/D3 无立即数字节故幸免；gas 把
  `shr $1` 优化成 D1 编码，解释了 count=1 侥幸正确、变量移位（D3）一直正确。
- 修复：modrm() 从 grp2 内部移到六个调用点，调用序 = 编码序（opcode →
  modrm → count），与 tiny386 计数访问器的求值时机一致。审计同模式调用点
  （grp1 80-83、grp3 F6/F7、C6/C7、69/6B），仅 grp2 一处违反。
- 结果：x86 smoke PASS + realmode **122/122 判据达成**（套件尾部 fninit #UD
  死循环由 D13 登记容纳）；riscv64 114/13 不变（E1）；探针上移位 7 形式、
  lahf/movsx/movzx 全 OK，DAS 前 32 真值表例零分叉。
- 教训：套件 FAIL 的"第一嫌疑"（AH 访问）是测试比较式自己的移位坏了——
  失败模式要看测试的判定路径，不能只看被测指令。

## V3 重写裁决（2026-09-03，用户指令）

μIR 方案（原 5cca035 提交，已按用户指令从分支历史移除、代码保留为
工作区未提交状态）被否决。三轮设计的结论：

- V1（59c1d6e 旧 switch 解释器，realmode 122/122）：骨架对（直执、
  modrm 分层、raise 通道），丑在词汇缺失——RegRead(opcode&7,opsz)
  式操作数访问、size 参数贯穿、位戏法压缩 case（opcode&2 拆方向）。
- V2（μIR）：修了宽度问题，代价是把一条指令切成四片（表行/构造函数/
  共享动词/标志钩子），"一条指令"概念本身碎了。当前 21 个失败
  （x86 14 + riscv fp 7）全部住在层间粘合上。
- **V3（已批准并落地：代码在 working tree，未提交）**：巨型 switch 直接执行 + 手册记号词汇层。
  排版契约三条：全小写 snake_case（全树，含 core/device/machine 改名）；
  宽度在名字里（rm8/rd16/push32，无宽度形参）；C 运算符直接用，
  helper 只写 C 没有的东西（标志/栈/访存/段/异常）。寄存器即 union
  别名（eax/ax/al/ah），flags 位域左值（cf = 1），x86 case 对照 SDM
  Operation 节逐行读得通，riscv 对照手册伪代码，双臂 if(w32) 直译
  IF/FI 不折叠。死：src/ir/ 整目录、aux 位段、IsaOps 七个钩子内化。
  活：CpuState bank（cell union）、单点提交、raise 通道、CEMU_DEBUG
  全类（漏斗改接新词汇函数；ops 类随 IR 死亡）、机器/设备/加载层
  零改动。验收三条写进 AGENTS.md 第八节。
- 迁移顺序：state 基线（185 个通过用例）→ x86 重写（14 例顺手钉死）
  → riscv 重写（7 例 fp 钉死）→ 删 ir/、收紧 IsaOps、全树小写化 →
  全程 state 对拍。
- 铁律重申：**参考实现永远只对齐语义，不照抄风格**（所有参考实现的
  风格都不合格）；两个根本目的：SDM 级简单清晰 + 纯 Win32 API。

## 阶段 2 完成记录（2026-09-02）

- **riscv 侧**：virt 机器 + CLINT/PLIC/sifive_test + OpenSBI fw_jump
  引导（01d9ecc）。布局逐字校准 QEMU：reset 向量来自 mask ROM dump、
  FDT 打内存补丁、Sv39/PMP/medeleg/mideleg 全落地。
- **x86 侧**：PC 平台（device/i8259.c 8259A 主从级联、device/i8254.c
  8253 六模式 ch0→IRQ0、宿主时钟懒锚定），isa_ops.raise_irq +
  CpuState.int_ack 落地，INTR 采样/SDM inhibit 窗口/hlt→wait。
  realmode 套件 **122 PASS/0 FAIL**（旧解释器时代），QEMU 参考的 110 行
  逐字节一致。
- 连带修掉的真实解释器 bug（历史存档）：SIB index×scale 丢失、0x9A 远调用
  缺失、DoInt 软中断误压 fault 地址、16/32 位 MUL/DIV 高半误写整寄存器、
  DAS 按 SDM 实现但与实机真值表 18 处分歧（按实机重写）。

## 阶段 1 / 1.5 完成记录（2026-08-30/31）

- 阶段 1：RV64IMAFDC riscv-tests 127 项全绿（ui 54/um 13/ua 19/uc 1/
  uf 11/ud 12/mi 17），准则审查第一轮完毕，构建零告警。
- 阶段 1.5：spike 机器拆到 machine/spike_min.c（core 零改动 = 结构证明）；
  x86 实模式解释器 + ELF32 加载进树；冒烟双跑逐字节一致；realmode 套件
  双跑对拍通过。isa-debug-exit 实测校准 status=(v<<1)|1。

## 工具链现状

- nasm：D:/nasm（2.16.03 也在 D:/SSDOWN/tools/nasm-2.16.03/）
- LLVM 23.1：D:/LLVM（clang/lld/llvm-objcopy）——**cemu 本体编译器**
  （2026-09-13 起用户决定换 clang：`tools/clang.cmake` 工具链文件，
  `cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=tools/clang.cmake`，
  mingw-w64 目标，lld 链接）
- mingw64（D:/mingw64）：头文件与 CRT sysroot（不再是编译器）；objcopy/
  objdump 平二进制工具仍在用
- qemu-system-i386：D:/qemu
- **bash 指的是 Git Bash**（`C:\Program Files\Git\bin\bash.exe`；VSCode
  `terminal.integrated.defaultProfile.windows` 已指向它，Cline 创建终端跟随此
  profile）。不能在 WSL bash 里跑回归：WSL 会把 `/mnt/...` 路径原样传给
  Windows 版 cemu.exe，loader 全部 cannot open，127/127 假 FAIL
  （2026-09-04 实测）。Git Bash 命令行调用全路径：
  `& 'C:\Program Files\Git\bin\bash.exe' -c '...'`
- 回归：`bash test/run.sh`；单独 riscv `bash test/riscv64/run.sh`、
  x86 `bash test/x86/run.sh`
- 回归基线（2026-09-13，DR/触发器落地后实测）：riscv64 **136 passed /
  0 failed**；x86 **8 passed / 0 failed**（smoke + pm 24/24 + realmode
  122/122 + kvm taskswitch/taskswitch2/cmpxchg8b/memory/debug；debug 为
  32 位移植件，QEMU 对拍 8/8 见片 1d 节；套件尾部 fninit #UD 死循环由
  D13 登记容纳，run.sh 的 timeout 判据容纳）
- 回归基线（2026-09-19，阶段 4 片 1 后实测）：riscv64 **136 passed / 0 failed**、
  x86 **9 passed / 0 failed**；依赖边 `bash tools/depcheck.sh` → ok。
- debug：`CEMU_DEBUG=...`（见 AGENTS.md 第十节），例
  `CEMU_DEBUG="trace:table,state,mem,budget=200" build/cemu.exe --machine x86 --isa x86 test/x86/realmode/realmode.elf`

## git 惯例

main 分支单线性，每轮工作一个检查点（git log 见历史）。test/ 只入库
源与脚本（*.txt 对拍捕获、rc.*/probe.* 中间产物清除过一轮）。
