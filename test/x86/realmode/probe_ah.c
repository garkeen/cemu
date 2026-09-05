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

MK_INSN(lahf, "pushfw; mov %al, (%esp); popfw; lahf")
MK_INSN(movsxah, "movsx %ah, %ebx")
MK_INSN(movzxah, "movzx %ah, %ebx")
MK_INSN(movsxal, "movsx %al, %ebx")
MK_INSN(das, "das")

// First 32 rows of test_das' 1024-case truth table (kvm-unit-tests realmode.c):
// in AL = tmp&0xff, in EFLAGS low byte = (tmp>>16)&0xff, expected AL =
// (tmp>>8)&0xff, expected EFLAGS low byte = tmp>>24.
static unsigned das_cases[] = {
    0x46000000, 0x8701a000, 0x9710fa00, 0x97119a00, 0x02000101, 0x8301a101,
    0x9310fb01, 0x93119b01, 0x02000202, 0x8301a202, 0x9710fc02, 0x97119c02,
    0x06000303, 0x8701a303, 0x9310fd03, 0x93119d03, 0x02000404, 0x8301a404,
    0x9310fe04, 0x93119e04, 0x06000505, 0x8701a505, 0x9710ff05, 0x97119f05,
    0x06000606, 0x8701a606, 0x56100006, 0x9711a006, 0x02000707, 0x8301a707,
    0x12100107, 0x9311a107,
};

void realmode_start(void) {
  // Shift scan: GCC lowers x >> 8/16/24 to C1 (shr imm8), x >> i to D3 (shr
  // %cl). The realmode failures all trace to immediate-count forms.
  {
    volatile unsigned v = 0x12345678;
    unsigned i = 12;
    print_serial("shr4=");
    print_hex(v >> 4);
    print_serial("shr8=");
    print_hex(v >> 8);
    print_serial("shr16=");
    print_hex(v >> 16);
    print_serial("shr24=");
    print_hex(v >> 24);
    print_serial("shl8=");
    print_hex(v << 8);
    print_serial("shrcl=");
    print_hex(v >> i);
    print_serial("shr1=");
    print_hex(v >> 1);
  }

  init_inregs(&(struct regs){.eax = 0xc7});
  exec_in_big_real_mode(&insn_lahf);
  print_serial("lahf ah=");
  print_hex(outregs.eax >> 8);
  print_serial((outregs.eax >> 8) == inregs.eax ? " OK\n" : " BAD\n");

  init_inregs(&(struct regs){.eax = 0x1234569c});
  exec_in_big_real_mode(&insn_movsxah);
  print_serial("movsxah ebx=");
  print_hex(outregs.ebx);
  print_serial(outregs.ebx == (unsigned)(signed char)(inregs.eax >> 8) ? " OK\n" : " BAD\n");

  exec_in_big_real_mode(&insn_movzxah);
  print_serial("movzxah ebx=");
  print_hex(outregs.ebx);
  print_serial(outregs.ebx == (unsigned char)(inregs.eax >> 8) ? " OK\n" : " BAD\n");

  exec_in_big_real_mode(&insn_movsxal);
  print_serial("movsxal ebx=");
  print_hex(outregs.ebx);
  print_serial(outregs.ebx == (unsigned)(signed char)inregs.eax ? " OK\n" : " BAD\n");

  for (unsigned i = 0; i < sizeof(das_cases) / sizeof(das_cases[0]); ++i) {
    unsigned tmp = das_cases[i];
    init_inregs(&(struct regs){.eax = tmp & 0xff, .eflags = (tmp >> 16) & 0xff});
    exec_in_big_real_mode(&insn_das);
    if (outregs.eax != ((tmp >> 8) & 0xff) || (outregs.eflags & 0xff) != (tmp >> 24)) {
      print_serial("das case ");
      print_hex(i);
      print_serial("in_al=");
      print_hex(tmp & 0xff);
      print_serial("in_fl=");
      print_hex((tmp >> 16) & 0xff);
      print_serial("out_ax=");
      print_hex(outregs.eax);
      print_serial("exp_al=");
      print_hex((tmp >> 8) & 0xff);
      print_serial("out_fl=");
      print_hex(outregs.eflags & 0xff);
      print_serial("exp_fl=");
      print_hex(tmp >> 24);
      print_serial("\n");
    }
  }
  print_serial("das scan done\n");

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
