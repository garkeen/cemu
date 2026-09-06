# cemu 当前状态（progress.md）

本文是原 任务与计划.md 的状态部分，按轮次记录。架构与路线图见 arch.md；
开发铁律见 AGENTS.md。最近的记录在最上。

## 阶段 3 项 5：LDT 机制整体（D16 销账，2026-09-06）

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
- LLVM 23.1：D:/LLVM（clang/lld/llvm-objcopy，seabios 与 x86 构建用）
- mingw gcc（D:/mingw64/bin/gcc.exe，版本号以实际为准）：编 cemu 本体
  （CMake：`cmake --build build`）
- qemu-system-i386：D:/qemu
- **bash 指的是 Git Bash**（`C:\Program Files\Git\bin\bash.exe`；VSCode
  `terminal.integrated.defaultProfile.windows` 已指向它，Cline 创建终端跟随此
  profile）。不能在 WSL bash 里跑回归：WSL 会把 `/mnt/...` 路径原样传给
  Windows 版 cemu.exe，loader 全部 cannot open，127/127 假 FAIL
  （2026-09-04 实测）。Git Bash 命令行调用全路径：
  `& 'C:\Program Files\Git\bin\bash.exe' -c '...'`
- 回归：`bash test/run.sh`；单独 riscv `bash test/riscv64/run.sh`、
  x86 `bash test/x86/run.sh`
- 回归基线（2026-09-06，LDT 落地后实测）：riscv64 **127 passed /
  0 failed**；x86 smoke PASS + pm **24/24** + realmode 122/122（pm 判据
  expected_pm=24，QEMU 对拍 22/24 见 pm 节；套件尾部 fninit #UD 死循环由
  D13 登记容纳，run.sh 的 timeout 判据容纳）
- debug：`CEMU_DEBUG=...`（见 AGENTS.md 第十节），例
  `CEMU_DEBUG="trace:table,state,mem,budget=200" build/cemu.exe --machine x86 --isa x86 test/x86/realmode/realmode.elf`

## git 惯例

main 分支单线性，每轮工作一个检查点（git log 见历史）。test/ 只入库
源与脚本（*.txt 对拍捕获、rc.*/probe.* 中间产物清除过一轮）。
