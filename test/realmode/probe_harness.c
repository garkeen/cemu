/* Minimal reproduction of the kvm-unit-tests realmode harness (from
   v86/tests/kvm-unit-tests/x86/realmode.c, GPLv2) plus register dumps, used
   to debug cemu's x86 execution. Not an acceptance test. */
#ifndef USE_SERIAL
#define USE_SERIAL
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;
typedef unsigned long long u64;
#define NULL ((void *)0)

asm(".code16gcc");

static void outb(u8 data, u16 port) {
  asm volatile("out %0, %1" : : "a"(data), "d"(port));
}

static u8 inb(u16 port) {
  u8 data;
  asm volatile("in %1, %0" : "=a"(data) : "d"(port));
  return data;
}

static void serial_outb(char ch) {
  u8 lsr;
  do {
    lsr = inb(0x3f8 + 0x05);
  } while (!(lsr & 0x20));
  outb(ch, 0x3f8 + 0x00);
}

static void print_serial(const char *buf) {
  while (*buf) serial_outb(*buf++);
}

static void print_hex(u32 v) {
  int i;
  for (i = 28; i >= 0; i -= 4) {
    u8 d = (v >> i) & 0xf;
    serial_outb(d < 10 ? '0' + d : 'a' + d - 10);
  }
  serial_outb(' ');
}

struct regs {
  u32 eax, ebx, ecx, edx, esi, edi, esp, ebp, eip, eflags;
};

static struct regs inregs, outregs;

static struct {
  u32 stack[128];
  char top[];
} tmp_stack;

static inline void init_inregs(struct regs *regs) {
  inregs = (struct regs){0};
  if (regs) inregs = *regs;
  if (!inregs.esp) inregs.esp = (unsigned long)&tmp_stack.top;
}

#define MK_INSN(name, str)                               \
  asm(".pushsection .data.insn \n"                       \
      "insn_" #name ": \n"                               \
      ".word 1001f, 1002f - 1001f \n"                    \
      ".popsection \n"                                   \
      ".pushsection .text.insn, \"ax\" \n"               \
      "1001: \n"                                         \
      "insn_code_" #name ": " str " \n"                  \
      "1002: \n"                                         \
      ".popsection \n");                                 \
  extern struct insn_desc insn_##name;

struct insn_desc {
  u16 ptr;
  u16 len;
};

static u64 gdt[] = {
    0,
    0x00cf9b000000ffffull,
    0x00cf93000000ffffull,
};

static struct {
  u16 limit;
  void *base;
} __attribute__((packed)) gdt_descr = {sizeof(gdt) - 1, gdt};

static void exec_in_big_real_mode(struct insn_desc *insn) {
  unsigned long tmp;
  static struct regs save;
  int i;
  extern u8 test_insn[], test_insn_end[];

  for (i = 0; i < insn->len; ++i)
    test_insn[i] = ((u8 *)(unsigned long)insn->ptr)[i];
  for (; i < test_insn_end - test_insn; ++i) test_insn[i] = 0x90;

  save = inregs;
  asm volatile(
      "lgdtl %[gdt_descr] \n\t"
      "mov %%cr0, %[tmp] \n\t"
      "or $1, %[tmp] \n\t"
      "mov %[tmp], %%cr0 \n\t"
      "mov %[bigseg], %%gs \n\t"
      "and $-2, %[tmp] \n\t"
      "mov %[tmp], %%cr0 \n\t"
      "pushw %%es \n\t"
      "pushw %[save]+36; popfw \n\t"
      "xchg %%eax, %[save]+0 \n\t"
      "xchg %%ebx, %[save]+4 \n\t"
      "xchg %%ecx, %[save]+8 \n\t"
      "xchg %%edx, %[save]+12 \n\t"
      "xchg %%esi, %[save]+16 \n\t"
      "xchg %%edi, %[save]+20 \n\t"
      "xchg %%esp, %[save]+24 \n\t"
      "xchg %%ebp, %[save]+28 \n\t"
      "test_insn: . = . + 32\n\t"
      "test_insn_end: \n\t"
      "xchg %%eax, %[save]+0 \n\t"
      "xchg %%ebx, %[save]+4 \n\t"
      "xchg %%ecx, %[save]+8 \n\t"
      "xchg %%edx, %[save]+12 \n\t"
      "xchg %%esi, %[save]+16 \n\t"
      "xchg %%edi, %[save]+20 \n\t"
      "xchg %%esp, %[save]+24 \n\t"
      "xchg %%ebp, %[save]+28 \n\t"
      "pushfl \n\t"
      "popl %[save]+36 \n\t"
      "popw %%es \n\t"
      "cld\n\t"
      "xor %[tmp], %[tmp] \n\t"
      "mov %[tmp], %%gs \n\t"
      : [tmp] "=&r"(tmp), [save] "+m"(save)
      : [gdt_descr] "m"(gdt_descr), [bigseg] "r"((short)16)
      : "cc", "memory");
  outregs = save;
}

MK_INSN(mov_r16_imm_1, "mov $1234, %ax")
MK_INSN(push32, "mov $0x12345678, %eax\n\t"
                "push %eax\n\t"
                "pop %ebx\n\t")
MK_INSN(shld_1, "shld $8, %ebx, %eax")

void realmode_start(void) {
  init_inregs(NULL);
  print_serial("in: ");
  print_hex(inregs.eax);
  print_hex(inregs.ebx);
  print_hex(inregs.esp);
  print_serial("\n");

  exec_in_big_real_mode(&insn_shld_1);
  exec_in_big_real_mode(&insn_shld_1);
  exec_in_big_real_mode(&insn_shld_1);
  exec_in_big_real_mode(&insn_mov_r16_imm_1);
  print_serial("mov1 out: ");
  print_hex(outregs.eax);
  print_hex(outregs.ebx);
  print_hex(outregs.ecx);
  print_hex(outregs.edx);
  print_hex(outregs.esi);
  print_hex(outregs.edi);
  print_hex(outregs.esp);
  print_hex(outregs.ebp);
  print_hex(outregs.eip);
  print_hex(outregs.eflags);
  print_serial(outregs.eax == 1234 ? " OK\n" : " BAD\n");

  exec_in_big_real_mode(&insn_push32);
  print_serial("push32 out: ");
  print_hex(outregs.eax);
  print_hex(outregs.ebx);
  print_hex(outregs.esp);
  print_serial(outregs.eax == 0x12345678 && outregs.ebx == 0x12345678
                   ? " OK\n"
                   : " BAD\n");

  outb(0, 0xf4);
  while (1) asm volatile("hlt" ::: "memory");
}

asm(".section .init \n\t"
    ".code32 \n\t"
    "mb_magic = 0x1BADB002 \n\t"
    "mb_flags = 0x0 \n\t"
    ".long mb_magic, mb_flags, 0 - (mb_magic + mb_flags) \n\t"
    ".globl start \n\t"
    ".data \n\t"
    ". = . + 4096 \n\t"
    "stacktop: \n\t"
    ".text \n\t"
    "start: \n\t"
    "lgdt r_gdt_descr \n\t"
    "lidt r_idt_descr \n\t"
    "ljmp $8, $1f; 1: \n\t"
    ".code16gcc \n\t"
    "mov $16, %eax \n\t"
    "mov %ax, %ds \n\t"
    "mov %ax, %es \n\t"
    "mov %ax, %fs \n\t"
    "mov %ax, %gs \n\t"
    "mov %ax, %ss \n\t"
    "mov %cr0, %eax \n\t"
    "btc $0, %eax \n\t"
    "mov %eax, %cr0 \n\t"
    "ljmp $0, $realmode_entry \n\t"
    "realmode_entry: \n\t"
    "xor %ax, %ax \n\t"
    "mov %ax, %ds \n\t"
    "mov %ax, %es \n\t"
    "mov %ax, %ss \n\t"
    "mov %ax, %fs \n\t"
    "mov %ax, %gs \n\t"
    "mov $stacktop, %esp\n\t"
    "ljmp $0, $realmode_start \n\t"
    ".code16gcc \n\t");

unsigned long long r_gdt[] = {0, 0x9b000000ffff, 0x93000000ffff};
struct table_descr {
  u16 limit;
  void *base;
} __attribute__((packed));
struct table_descr r_gdt_descr = {sizeof(r_gdt) - 1, r_gdt};
struct table_descr r_idt_descr = {0x3ff, 0};
