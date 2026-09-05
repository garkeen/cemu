# cemu 当前状态（progress.md）

本文是原 任务与计划.md 的状态部分，按轮次记录。架构与路线图见 arch.md；
开发铁律见 AGENTS.md。最近的记录在最上。

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
- 回归基线（2026-09-05，grp2 修复后实测）：riscv64 114 passed / 13 failed
  （rv64uf 7 + rv64ud 5 + rv64uc-p-rvc 1，全部浮点/RVC，与简化登记 E1 相关；
  progress 旧记录"fp 7 例钉死"与代码不符，V3 提交时未跑完回归）；x86
  smoke PASS + realmode 122/122（grp2 C0/C1 立即数计数在 modrm 前求值的
  bug 修复，见上文；套件尾部 fninit #UD 死循环由 D13 登记容纳，run.sh 的
  timeout 60 判据容纳）
- debug：`CEMU_DEBUG=...`（见 AGENTS.md 第十节），例
  `CEMU_DEBUG="trace:table,state,mem,budget=200" build/cemu.exe --machine x86 --isa x86 test/x86/realmode/realmode.elf`

## git 惯例

main 分支单线性，每轮工作一个检查点（git log 见历史）。test/ 只入库
源与脚本（*.txt 对拍捕获、rc.*/probe.* 中间产物清除过一轮）。
