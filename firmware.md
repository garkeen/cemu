# 固件树现状（firmware.md）

x86 主板的固件与显卡 option ROM 来自**库外的一棵 SeaBIOS 树**：
`D:\code\c\EMU\seabios`（与 cemu 平级的目录）。本文是那棵树的**唯一权威记录**——它不在
cemu 的版本控制里，所以改它必须同时改本文。

## 为什么在库外

- 它是 SeaBIOS 1.17.0 的第三方源码树（LGPL 等），不是 cemu 的代码；cemu 只**用它的产物**
  （`out/bios.bin` 与 `out/vgabios.bin`），不把它并进来。
- 那棵目录**不是 git 仓库**：里面的改动没有版本控制，本文（含文末的配置快照）就是它的历史。
- 上游的 kconfig 流程在本机跑不起来（`scripts/kconfig` 的 conf 是宿主 C 程序，需要 MSVC
  或 mingw 头，两者都违反本项目的工具链约束），因此 `.config` 与 `out/autoconf.h` 在这棵树
  里是**手工维护的输入**——上游 `.config` 头里那行 "Automatically generated file; DO NOT
  EDIT" 在这里不成立。

## 构建链

- `scripts/gen-ninja.py` 生成 `build.ninja`（纯 clang + lld + llvm 工具链，不许
  mingw/GNU binutils；ninja 不在 PATH 时前置 `PATH=/d/ninja:$PATH`）。它镜像上游 1.17.0
  Makefile 的命令序列：三个 whole-program 编译（16 位 / 32 位 flat / 32 位分段）、
  `layoutrom.py` 的链接脚本、`checkrom.py` 的尺寸与布局校验；`out/asm-offsets.h` 由
  clang -S + `gen-offsets.sh` 生成，objdump 形状的 dump 由 `scripts/llvmdump.py`
  （llvm-readobj + llvm-objdump）产出。
- 2026-09-29（片 27）按上游 Makefile 的 "VGA build rules" 给这条链补了 SRCVGA 管道：
  `out/.includesvga.txt`（whole-compile 的 include 清单）→ `out/vgaccode16.o` →
  `out/vgaentry.o`（`vgasrc/vgaentry.S`）→ `out/vgaversion.o`（`out/autovgaversion.h` 由
  buildversion.py 生成）→ `ld.lld --gc-sections -T out/vgasrc/vgalayout.lds` →
  `out/vgarom.o` → objcopy → `buildrom.py` 填 55AA/size/checksum → `out/vgabios.bin`。
  这组规则的开关读 `out/autoconf.h` 的 `CONFIG_BUILD_VGABIOS`（上游是
  `target-$(CONFIG_BUILD_VGABIOS)`）。
- `compare/` 存着官方 1.17.0 源码、tarball 与官方 `bios.bin`，供逐行对拍；不要动它。

## 重建

```sh
cd /d/code/c/EMU/seabios
python scripts/gen-ninja.py          # .config 或 autoconf.h 变过就要重生成
PATH=/d/ninja:$PATH ninja            # 产物：out/bios.bin、out/vgabios.bin
```

构建输出里有 layoutrom 的尺寸统计与 checkrom 的 `Total size / Fixed / Free`——**`Free` 就是
ROM 窗口底部留给 option ROM 的空间**，是本树最硬的约束（见下）。

## 当前配置：相对官方 1.17.0 默认的差异与理由

**开**（相对默认新增）：

- `# CONFIG_NO_VGABIOS is not set` + `CONFIG_VGA_STANDARD_VGA=y`（连带
  `CONFIG_VGA_STDVGA_PORTS=y`）+ `CONFIG_BUILD_VGABIOS=y`
  —— 机器的 int10 视频服务来自**显卡自己的 option ROM**（SeaVGABIOS），不是 SeaBIOS 核心；
  没有它，DOS/Windows 无法通过 int10 切出文本模式（片 27 的起点）。
- 保持开启（本机 x86_min 真实存在的设备）：`ATA`（PIIX IDE）、`CDROM_BOOT`/`CDROM_EMU`
  （ATAPI 光盘引导）、`FLOPPY`（软盘）、`PS2PORT`、`SERIAL`/`SERCON`、`RTC_TIMER`、
  `HARDWARE_IRQ`、`PCIBIOS`/`PNPBIOS`/`OPTIONROMS`、`PMM`、`BOOT`/`KEYBOARD`/`MOUSE`、
  `ACPI`/`SMBIOS`/`MPTABLE`/`PIRTABLE`、`MALLOC_UPPERMEMORY`、`RELOCATE_INIT`、
  `THREADS`、`QEMU`/`QEMU_HARDWARE`、`DEBUG_IO`。
- `CONFIG_PMTIMER=y`（默认就开着，但**此前本机没有产生它的设备**）：固件的微秒时基在
  `src/hw/timer.c pmtimer_setup()` 里切到 ACPI PM timer（3.579545MHz），前提是 PCI 上
  找得到 PIIX4 的 PM function（`src/fw/pciinit.c piix4_pm_config_setup` 编 PMBASE 并用
  PIIX_PMREGMISC bit0 打开那 32 字节块）。本机现在供得上这个函数（`device/misc/piix4pm.c`），
  于是引导日志里会出现 `Using pmtimer, ioport 0x...`；**在此之前固件是拿 8254 计时**——
  与时被对拍的模型差一个时钟源、也差一个分辨率，所有固件侧等待（软盘超时、键盘与串口
  等待、INT 15h AH=86h、它排队走的 timer list）都按不同的比例走。这条属于机器，不属于配置。

**关**（本机根本没有这些设备；关掉既省体积，也省掉 POST 里对不存在控制器的探测）：

- USB 全家：`USB`、`USB_UHCI`/`OHCI`/`EHCI`/`XHCI`、`USB_MSC`/`UAS`/`HUB`/`KEYBOARD`/`MOUSE`
- `NVME`、`VIRTIO_BLK`/`VIRTIO_SCSI`、`PVSCSI`/`ESP_SCSI`/`LSI_SCSI`/`MEGASAS`/`MPT_SCSI`、
  `SDCARD`、`AHCI`、`FLASH_FLOPPY`（QEMU 的 flash 软驱）、`TCGBIOS`（TPM）、`XEN`、`LPT`、
  `BOOTSPLASH`（连带 jpeg/bmp）
- `CONFIG_VGA_VBE`（线性帧缓冲与线框模式不做，登记在 AGENTS.md D17）、
  `CONFIG_VGA_PCI`（显卡**有** PCI function `00:02.0` = `1234:1111`，但它不带扩展 ROM
  ——vga.c 把 ROM 地址寄存器 0x30 做成读回 0，option ROM 仍由 fw_cfg 的 `vgaroms/`
  通路交给客机；关掉这条只是让固件不去 PCI ROM BAR 里找它，也不让固件替 ROM 排 BAR，
  客人自己枚举 PCI 不受影响）
- `# CONFIG_VGA_BOCHS is not set` 的连带后果，值得单独记一笔： SeaVGABIOS 里**唯一**
  按显存校验模式表的代码是 `vgasrc/bochsvga.c:371` 的 `Removing mode %x`，而它的入口
  `vgahw.h:55 bochsvga_setup()` 在这个配置下被编掉。**本机这份 ROM 因此不会读
  DISPI 的 `VIDEO_MEMORY_64K`，也不会因为卡报的显存小而剪模式**。任何"卡的显存答复
  经过 ROM 的模式表影响客人"的推理链，都必须先在一次实跑里确认这句日志出自哪一侧
  （QEMU 自带的 `vgabios-stdvga.bin` 是 Bochs 适配器构建，会打印它；我们这份不会）。

**`CONFIG_ROM_SIZE=0`（自动）**：布局照旧把 SeaBIOS 压在 ROM 窗口上部，**底部那段是留给
option ROM 的槽**。这就是固件内容必须瘦身的原委：官方 bios 内容从 0xD3FE0 起（底部约 80KB），
QEMU 的 `bios-256k.bin` 从 0xD4200 起；而我们这份曾从 0xC9C10 起，底部只剩 39952 字节，
38400 字节的 VGA ROM 只差 1552 字节塞进去。裁掉上面那些驱动后有效内容 222192 → **135232**
字节（checkrom: `Total size: 135232  Free: 126912`），VGA ROM 落在 C0000-C95FF。
`out/bios.bin` 仍是 256KiB（ROM 窗口大小），`out/vgabios.bin` = **38400** 字节
（QEMU 的 `vgabios-stdvga.bin` 是 39424）。

## 产物怎么喂给 cemu

```sh
build/cemu.exe --machine x86 -bios <seabios>/out/bios.bin \
  -vgabios <seabios>/out/vgabios.bin -fda win101.img
```

- `-bios`：映射进 ROM 窗口 0xC0000-0xFFFFF，复位向量 F000:FFF0（SeaBIOS 自己的布局）。
- `-vgabios`：主板把该文件作为 **fw_cfg romfile `vgaroms/vgabios.bin`** 发布给固件；
  SeaBIOS 的 `run_file_roms("vgaroms/", isvga=1)` 把它当 VGA option ROM 执行并
  `rom_reserve` 到 0xC0000（它不扫 C0000 裸内存去执行 ROM，只跑 PCI VGA ROM 与 fw_cfg 的
  `vgaroms/*`）。这一步是 int10 存在的前提。
- 测试套件自己找这两个文件：`test/x86/run.sh` 依次找 `../../seabios/out/bios.bin` 与
  `../../seabios/out/vgabios.bin`（再退一级），可用 `CEMU_BIOS` / `CEMU_VGAROM` 覆盖。

## 改这棵树时的纪律

1. 改 `.config` 就同步改 `out/autoconf.h`（后者是编译输入，两者必须一致）。
2. 跑 `python scripts/gen-ninja.py && ninja` 后看 checkrom 的 `Free`：小于
   `out/vgabios.bin` 的大小就放不下 option ROM。
3. 把改动与理由写进本文件，并按 AGENTS.md 的规矩在 progress.md 记一轮。
4. 跑 `bash test/x86/run.sh`：floppy 格与 vga 格都用这两个产物。

## 附：`.config` 快照（2026-09-29，与 `out/autoconf.h` 同步）

```
#
# Automatically generated file; DO NOT EDIT.
# SeaBIOS Configuration
#

#
# General Features
#
# CONFIG_COREBOOT is not set
CONFIG_QEMU=y
# CONFIG_CSM is not set
CONFIG_QEMU_HARDWARE=y
# CONFIG_XEN is not set
CONFIG_THREADS=y
CONFIG_RELOCATE_INIT=y
CONFIG_BOOTMENU=y
# CONFIG_BOOTSPLASH is not set
CONFIG_BOOTORDER=y
CONFIG_HOST_BIOS_GEOMETRY=y
CONFIG_ENTRY_EXTRASTACK=y
CONFIG_MALLOC_UPPERMEMORY=y
CONFIG_ROM_SIZE=0

#
# Hardware support
#
CONFIG_ATA=y
# CONFIG_ATA_DMA is not set
# CONFIG_ATA_PIO32 is not set
# CONFIG_AHCI is not set
# CONFIG_SDCARD is not set
# CONFIG_VIRTIO_BLK is not set
# CONFIG_VIRTIO_SCSI is not set
# CONFIG_PVSCSI is not set
# CONFIG_ESP_SCSI is not set
# CONFIG_LSI_SCSI is not set
# CONFIG_MEGASAS is not set
# CONFIG_MPT_SCSI is not set
CONFIG_FLOPPY=y
# CONFIG_FLASH_FLOPPY is not set
# CONFIG_NVME is not set
CONFIG_PS2PORT=y
# CONFIG_USB is not set
# CONFIG_USB_UHCI is not set
# CONFIG_USB_OHCI is not set
# CONFIG_USB_EHCI is not set
# CONFIG_USB_XHCI is not set
# CONFIG_USB_MSC is not set
# CONFIG_USB_UAS is not set
# CONFIG_USB_HUB is not set
# CONFIG_USB_KEYBOARD is not set
# CONFIG_USB_MOUSE is not set
CONFIG_SERIAL=y
CONFIG_SERCON=y
# CONFIG_LPT is not set
CONFIG_RTC_TIMER=y
CONFIG_HARDWARE_IRQ=y
# CONFIG_USE_SMM is not set
CONFIG_MTRR_INIT=y
CONFIG_PMTIMER=y
CONFIG_TSC_TIMER=y

#
# BIOS interfaces
#
CONFIG_DRIVES=y
CONFIG_CDROM_BOOT=y
CONFIG_CDROM_EMU=y
CONFIG_PCIBIOS=y
CONFIG_APMBIOS=y
CONFIG_PNPBIOS=y
CONFIG_OPTIONROMS=y
CONFIG_PMM=y
CONFIG_BOOT=y
CONFIG_KEYBOARD=y
CONFIG_KBD_CALL_INT15_4F=y
CONFIG_MOUSE=y
CONFIG_S3_RESUME=y
CONFIG_VGAHOOKS=y
# CONFIG_DISABLE_A20 is not set
# CONFIG_WRITABLE_UPPERMEMORY is not set
# CONFIG_TCGBIOS is not set

#
# BIOS Tables
#
CONFIG_PIRTABLE=y
CONFIG_MPTABLE=y
CONFIG_SMBIOS=y
CONFIG_ACPI=y
CONFIG_FW_ROMFILE_LOAD=y
CONFIG_ACPI_PARSE=y

#
# VGA ROM
#
# CONFIG_NO_VGABIOS is not set
CONFIG_VGA_STANDARD_VGA=y
# CONFIG_VGA_CIRRUS is not set
# CONFIG_VGA_ATI is not set
# CONFIG_VGA_BOCHS is not set
# CONFIG_VGA_GEODEGX2 is not set
# CONFIG_VGA_GEODELX is not set
# CONFIG_DISPLAY_BOCHS is not set
# CONFIG_VGA_RAMFB is not set
CONFIG_BUILD_VGABIOS=y
CONFIG_VGA_STDVGA_PORTS=y
CONFIG_VGA_EXTRA_STACK_SIZE=512
# CONFIG_VGA_VBE is not set
# CONFIG_VGA_PCI is not set

#
# Debugging
#
CONFIG_DEBUG_LEVEL=1
# CONFIG_DEBUG_SERIAL is not set
# CONFIG_DEBUG_SERIAL_MMIO is not set
CONFIG_DEBUG_IO=y
```
