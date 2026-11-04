# cesdk — 自举链的客户机软件

cemu 阶段 5（见 `../arch.md` 与 `PLAN.md`）：自家固件、引导契约、内核、安装器。
编译器用 clang/LLVM；客户机侧没有宿主 OS，也没有 libc。

```
include/cemu_boot.h   固件→内核的运行期契约（版本化，权威定义）
include/cesdk.h       crun/klib 公共头
crun/                 _start（每 ISA）、console、halt、klib
hello/                5.0 冒烟程序
linker/               每个 ISA 的链接脚本
PLAN.md               方案、目录、里程碑与验收
```

## 构建

```sh
cmake -G Ninja -B build \
  -DCESDK_CLANG=D:/LLVM/bin/clang.exe \
  -DCESDK_LD=D:/LLVM/bin/ld.lld.exe
cmake --build build
# -> build/hello-riscv64.elf, build/hello-x86.elf
```

## 运行（5.0）

```sh
../build/cemu.exe --machine virt --isa riscv64 \
  -bios build/hello-riscv64.elf             # 打印，退出 0（sifive_test PASS）

../build/cemu.exe --machine x86 --isa x86 \
  build/hello-x86.elf                       # 在 COM1 打印，退出 11（debug-exit 5）
```

virt 板的 `-bios` 把 ELF 装到 0x80000000、从 M 模式起跑；x86 镜像是 multiboot
ELF，cemu 以 32 位平坦保护模式（CS=0x08 / DS=0x10）进入。

## 排期

5.0 运行时（本目录已跑通）→ 5.1 riscv 固件 → 5.2 x86 固件 →
5.3 内核 → 5.4 安装器。详见 `PLAN.md`。
