# cemu 当前状态（progress.md）

本文是原 任务与计划.md 的状态部分，按轮次记录。架构与路线图见 arch.md；
开发铁律见 AGENTS.md。最近的记录在最上。

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

## 阶段 3.5 片 1d：D6 销账——Sdtrig 触发器 + x86 DR 断点（2026-09-13）

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
