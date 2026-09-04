#include "device/char/uart16550.h"

#include "host/host.h"

// Register bits per the 16550 datasheet layout used by QEMU hw/char/serial.c.
enum {
  kIerRdi = 1 << 0,   // received data interrupt enable (no receiver here)
  kIerThri = 1 << 1,  // transmitter holding register empty
  kLcrDlab = 1 << 7,  // divisor latch access bit
  kLsrDr = 1 << 0,    // data ready
  kLsrThre = 1 << 5,  // transmit holding register empty
  kLsrTemt = 1 << 6,  // transmitter empty
  kIirNoInt = 0x01,
  kIirThri = 0x02,
  kIirFe = 0xc0,  // FIFO enabled, reported in IIR bits 6-7
};

static void UpdateIir(Uart16550* u) {
  // With no receiver the only raiseable source is TX empty.
  u->iir = (u->ier & kIerThri) ? kIirThri : kIirNoInt;
  // Level output: asserted while THRI is enabled and the transmitter is
  // empty (dearchap serial_update_irq).
  if (u->set_irq) u->set_irq(u->irq_ctx, 0, (u->ier & kIerThri) && (u->lsr & kLsrThre));
}

void Uart16550Init(Uart16550* u) {
  u->iir = kIirNoInt;
  u->lsr = kLsrThre | kLsrTemt;
}

uint8_t Uart16550Read(Uart16550* u, uint64_t off) {
  switch (off) {
    case 0:  // RBR (or DLL with DLAB); RX is not wired, RBR reads 0
      return (u->lcr & kLcrDlab) ? (uint8_t)(u->divider & 0xff) : 0;
    case 1:
      return (u->lcr & kLcrDlab) ? (uint8_t)(u->divider >> 8) : u->ier;
    case 2:
      return (uint8_t)(u->iir | ((u->fcr & 1) ? kIirFe : 0));
    case 3:
      return u->lcr;
    case 4:
      return u->mcr;
    case 5:
      return u->lsr;
    case 6:
      return u->msr;
    default:
      return u->scr;
  }
}

void Uart16550Write(Uart16550* u, uint64_t off, uint8_t val) {
  switch (off) {
    case 0:
      if (u->lcr & kLcrDlab) {
        u->divider = (uint16_t)((u->divider & 0xff00) | val);
      } else {
        char ch = (char)val;
        HostWriteOut(&ch, 1);  // transmit immediately: THRE/TEMT stay set
      }
      break;
    case 1:
      if (u->lcr & kLcrDlab) {
        u->divider = (uint16_t)((u->divider & 0x00ff) | (val << 8));
      } else {
        u->ier = val;
        UpdateIir(u);
      }
      break;
    case 2:
      u->fcr = val;
      break;
    case 3:
      u->lcr = val;
      break;
    case 4:
      u->mcr = val;
      break;
    case 5:
      break;  // LSR is read-only
    case 6:
      u->msr = val;
      break;
    default:
      u->scr = val;
      break;
  }
}

static uint64_t UartRead(void* dev, uint64_t addr, int size) {
  (void)size;  // 1-byte registers; wider reads take the low byte
  return Uart16550Read((Uart16550*)dev, addr & 7);  // 8-byte register window
}

static void UartWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  Uart16550Write((Uart16550*)dev, addr & 7, (uint8_t)val);
}

const DeviceOps kUart16550Ops = {"uart16550", UartRead, UartWrite};

void Uart16550SetIrqSink(Uart16550* u, void (*set_irq)(void*, int, int), void* ctx) {
  u->set_irq = set_irq;
  u->irq_ctx = ctx;
}
