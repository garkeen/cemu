// Console and stop devices for the two boards the SDK targets.  These are the
// machine's own registers, not firmware services: 5.0 runs before there is
// a boot protocol, so the address of the UART has to be known here.  The
// firmware stage replaces every one of these with a cemu_boot_info callback;
// keeping them in one file is what makes that swap a compile-time choice.

#include "cesdk.h"

#if defined(__riscv)

// cemu virt / QEMU virt: ns16550a at 0x10000000, sifive,test1 at 0x100000.
enum {
  kUartBase = 0x10000000,
  kUartThr = 0,   // transmit holding register
  kUartLsr = 5,   // line status
  kUartLsrThre = 1 << 5,
  kTestBase = 0x100000,
  kTestPass = 0x5555,
  kTestFail = 0x3333,
};

void Putch(char c) {
  volatile uint8_t *uart = (volatile uint8_t *)kUartBase;
  while ((uart[kUartLsr] & kUartLsrThre) == 0) {
  }
  uart[kUartThr] = (uint8_t)c;
}

// The virt board has no receive path wired to the host stdin in this stage;
// the firmware stage routes input through the boot-info console instead.
int Getch(void) { return -1; }

void Halt(int code) __attribute__((noreturn));
void Halt(int code) {
  volatile uint32_t *test = (volatile uint32_t *)kTestBase;
  // sifive_test: 0x5555 = pass, 0x3333 | (code << 16) = fail with the code.
  *test = code == 0 ? kTestPass : (kTestFail | ((uint32_t)code << 16));
  for (;;) {
  }
}

#elif defined(__i386__)

// cemu x86_min: COM1 at 0x3F8 (the board maps it), debug-exit at 0xF4.
// A multiboot image enters with memory and segments ready but the UART is
// still at its reset state, so the first byte programs it.
enum {
  kCom1 = 0x3F8,
  kCom1Ier = 1,
  kCom1Fcr = 2,
  kCom1Lcr = 3,
  kCom1Mcr = 4,
  kCom1Lsr = 5,
  kLsrThre = 1 << 5,
  kDebugExitPort = 0xF4,
};

static int g_uart_ready;

static void Outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }

static uint8_t Inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static void UartInit(void) {
  Outb(kCom1 + kCom1Ier, 0x00);  // no interrupts
  Outb(kCom1 + kCom1Lcr, 0x80);  // DLAB
  Outb(kCom1 + 0, 0x01);         // divisor 1 (115200)
  Outb(kCom1 + 1, 0x00);
  Outb(kCom1 + kCom1Lcr, 0x03);  // 8N1
  Outb(kCom1 + kCom1Fcr, 0xC7);  // FIFO on, cleared
  Outb(kCom1 + kCom1Mcr, 0x03);  // DTR/RTS
  g_uart_ready = 1;
}

void Putch(char c) {
  if (!g_uart_ready) UartInit();
  while ((Inb(kCom1 + kCom1Lsr) & kLsrThre) == 0) {
  }
  Outb(kCom1, (uint8_t)c);
}

int Getch(void) {
  if (!g_uart_ready) UartInit();
  if ((Inb(kCom1 + kCom1Lsr) & 1) == 0) return -1;  // data ready
  return (int)Inb(kCom1);
}

void Halt(int code) __attribute__((noreturn));
void Halt(int code) {
  // isa-debug-exit: status = (value << 1) | 1; the x86 suite treats payload 5
  // (status 11) as success, so code 0 maps onto it.
  Outb(kDebugExitPort, (uint8_t)(code == 0 ? 5 : code));
  for (;;) {
  }
}

#else
#error "cesdk: unsupported ISA"
#endif
