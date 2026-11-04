#include "device/char/uart16550.h"

#include <string.h>

#include "debug/debug.h"
#include "host/host.h"

// Register bits and interrupt identities per the 16550D datasheet (the same
// layout QEMU hw/char/serial.c programs, so a guest driver written against
// either agrees with us).
enum {
  kIerErbfi = 1 << 0,  // received data available
  kIerEtbei = 1 << 1,  // transmitter holding register empty
  kIerElsi = 1 << 2,   // receiver line status
  kIerEdssi = 1 << 3,  // modem status
  kIerMask = 0x0f,     // bits 7:4 are reserved and read back 0

  // IIR bits 3:1 name the pending source; bit 0 set means "none".
  kIirMsi = 0x00,     // modem status                 (priority 4, lowest)
  kIirThri = 0x02,    // transmitter holding register empty (priority 3)
  kIirRdi = 0x04,     // received data available      (priority 2)
  kIirLsri = 0x06,    // receiver line status         (priority 1, highest)
  kIirCti = 0x0c,     // character timeout, FIFO mode (priority 2)
  kIirNoInt = 0x01,
  kIirFifoBits = 0xc0,  // bits 7:6 read 0b11 when FCR bit 0 enables the FIFO

  kFcrEnable = 1 << 0,
  kFcrClearRx = 1 << 1,
  kFcrClearTx = 1 << 2,

  kLcrSbc = 1 << 6,   // set break
  kLcrDlab = 1 << 7,  // divisor latch access

  kLsrDr = 1 << 0,    // data ready
  kLsrOe = 1 << 1,    // overrun
  kLsrPe = 1 << 2,    // parity error
  kLsrFe = 1 << 3,    // framing error
  kLsrBi = 1 << 4,    // break interrupt
  kLsrThre = 1 << 5,  // transmit holding register empty
  kLsrTemt = 1 << 6,  // transmitter empty
  kLsrErr = kLsrOe | kLsrPe | kLsrFe | kLsrBi,

  kMcrDtr = 1 << 0,
  kMcrRts = 1 << 1,
  kMcrOut1 = 1 << 2,
  kMcrOut2 = 1 << 3,
  kMcrLoop = 1 << 4,

  kMsrDcts = 1 << 0,
  kMsrDdsr = 1 << 1,
  kMsrTeri = 1 << 2,
  kMsrDdcd = 1 << 3,
  kMsrCts = 1 << 4,
  kMsrDsr = 1 << 5,
  kMsrRi = 1 << 6,
  kMsrDcd = 1 << 7,
  kMsrDelta = 0x0f,  // the four delta bits clear when MSR is read

  kRxFifoSize = 16,  // the 16550's receiver FIFO depth (16550D)
  // The character-timeout timer runs for 4 character times (16550D
  // "Character Timeout"; QEMU arms fifo_timeout_timer the same way).
  kTimeoutChars = 4,
  // One character is 10 bit times (start + 8 data + stop) at 16x the divisor
  // rate, and the 8250 family's reference clock is 1.152 MHz / 16 = 115200
  // (16550D "Baud Rate Generator"; QEMU baudbase).
  kBaudBase = 115200,
};

// FCR bits 7:6 select how full the FIFO must be before the receiver asks for
// service (16550D "FIFO Control Register").
static const int kTriggerLevels[4] = {1, 4, 8, 14};

static int64_t CharTimeUs(const Uart16550* u) {
  // A zero divisor is not a baud rate the chip can generate; the datasheet
  // leaves it undefined, and a zero here would arm a timer that never fires.
  uint32_t div = u->divider ? u->divider : 1;
  return (int64_t)(10u * div) * 1000000 / kBaudBase;
}

// LSR is a view: DR follows the FIFO, THRE/TEMT follow the transmitter (always
// empty, because a THR write leaves at once), and the error bits are latched
// until the guest reads LSR.
static void RefreshLsr(Uart16550* u) {
  uint8_t v = (uint8_t)(u->lsr & kLsrErr);
  if (u->rx_count > 0) v |= kLsrDr;
  v |= kLsrThre | kLsrTemt;
  u->lsr = v;
}

// The interrupt line is level and follows the highest-priority enabled source
// (16550D interrupt identification table; QEMU serial_update_irq).
static void UpdateIrq(Uart16550* u) {
  uint8_t iir = kIirNoInt;
  if ((u->ier & kIerElsi) && (u->lsr & kLsrErr)) {
    iir = kIirLsri;
  } else if ((u->ier & kIerErbfi) && (u->lsr & kLsrDr) &&
             (!(u->fcr & kFcrEnable) || u->rx_count >= u->rx_trigger)) {
    iir = kIirRdi;
  } else if ((u->ier & kIerErbfi) && u->rx_timeout_ip) {
    iir = kIirCti;
  } else if ((u->ier & kIerEtbei) && u->thr_ip) {
    iir = kIirThri;
  } else if ((u->ier & kIerEdssi) && (u->msr & kMsrDelta)) {
    iir = kIirMsi;
  }
  u->iir = (uint8_t)(iir | ((u->fcr & kFcrEnable) ? kIirFifoBits : 0));
  if (u->set_irq) u->set_irq(u->irq_ctx, 0, iir != kIirNoInt);
}

// The modem inputs. Nothing is wired to them on a PC's on-board port, so they
// read low — except in loopback, where the four modem outputs drive the four
// inputs (16550D "Loopback mode": DTR->DSR, RTS->CTS, OUT1->RI, OUT2->DCD).
// That is the mode a driver probes the chip with.
static void UpdateMsr(Uart16550* u) {
  uint8_t in = 0;
  if (u->mcr & kMcrLoop) {
    if (u->mcr & kMcrDtr) in |= kMsrDsr;
    if (u->mcr & kMcrRts) in |= kMsrCts;
    if (u->mcr & kMcrOut1) in |= kMsrRi;
    if (u->mcr & kMcrOut2) in |= kMsrDcd;
  }
  // A changed input latches its delta bit until MSR is read.
  if ((in ^ u->msr) & kMsrCts) in |= kMsrDcts;
  if ((in ^ u->msr) & kMsrDsr) in |= kMsrDdsr;
  if ((in ^ u->msr) & kMsrRi) in |= kMsrTeri;
  if ((in ^ u->msr) & kMsrDcd) in |= kMsrDdcd;
  u->msr = (uint8_t)(in | (u->msr & kMsrDelta));
  UpdateIrq(u);
}

// Empties the receiver: the queued bytes, the index pair and the character
// timeout that was riding on them (QEMU serial_ioport_write's RFR branch).
static void FlushRx(Uart16550* u) {
  u->rx_count = 0;
  u->rx_head = u->rx_tail = 0;
  u->rx_timeout_armed = 0;
  u->rx_timeout_ip = 0;
}

// One byte into the receiver, from SIN or from the loopback path. With the FIFO
// off the receiver holds a single byte, so a byte that arrives while that one is
// still unread replaces it and latches OE (16550D "FIFO Control Register" bit 0;
// QEMU serial_receive1's else branch keeps the newest byte in RBR and sets
// UART_LSR_OE). This is the wire path, so a loopback burst still loses its front
// exactly as the chip would; host input goes through Uart16550HostReceive's hold
// first, which is where QEMU's chardev would keep it.
static void PushRx(Uart16550* u, uint8_t ch) {
  if (!(u->fcr & kFcrEnable)) {
    // QEMU keeps the newest byte in rbr; a read after DR clears still returns
    // it (serial_ioport_read's non-FIFO branch).
    u->rbr = ch;
    if (u->rx_count > 0) {
      u->lsr |= kLsrOe;  // the byte waiting there is lost, this one takes its place
      u->rx_fifo[u->rx_tail] = ch;
    } else {
      u->rx_fifo[u->rx_head] = ch;
      u->rx_head = (u->rx_head + 1) % kRxFifoSize;
      u->rx_count = 1;
    }
    RefreshLsr(u);
    UpdateIrq(u);
    return;
  }
  if (u->rx_count >= kRxFifoSize) {
    u->lsr |= kLsrOe;  // the FIFO is full: the byte is lost and OE latches
  } else {
    u->rx_fifo[u->rx_head] = ch;
    u->rx_head = (u->rx_head + 1) % kRxFifoSize;
    u->rx_count++;
  }
  RefreshLsr(u);
  // With the FIFO on, a burst that stops short of the trigger level is reported
  // by the character timeout instead of by the level.
  u->rx_timeout_armed = 1;
  u->rx_timeout_at = HostTimerNow() + CharTimeUs(u) * kTimeoutChars;
  UpdateIrq(u);
}

// The receiver can take another byte: one with the FIFO off, up to 16 with it
// on. This is QEMU's serial_can_receive, used by the host hold below.
static int ReceiverHasRoom(const Uart16550* u) {
  return (u->fcr & kFcrEnable) ? u->rx_count < kRxFifoSize : u->rx_count == 0;
}

// Feed held host bytes into the receiver while it has room. The hold exists so
// a host burst (a serial-mouse packet train, a pasted console block) waits
// outside the one-deep receiver instead of being overwritten there; QEMU's
// chardev does the same with serial_can_receive.
static void DrainHost(Uart16550* u) {
  while (u->host_count > 0 && ReceiverHasRoom(u)) {
    uint8_t ch = u->host_fifo[u->host_tail];
    u->host_tail = (u->host_tail + 1) % (int)sizeof(u->host_fifo);
    u->host_count--;
    PushRx(u, ch);
  }
}

void Uart16550Init(Uart16550* u) {
  // Preserve the sink: a board may install it before or after Init.
  void (*sink)(void*, int, int) = u->set_irq;
  void* ctx = u->irq_ctx;
  memset(u, 0, sizeof(*u));
  u->set_irq = sink;
  u->irq_ctx = ctx;
  u->rx_trigger = kTriggerLevels[0];
  u->thr_ip = 1;  // the transmitter is empty out of reset, so THRE asks at once
  RefreshLsr(u);
  UpdateMsr(u);
  UpdateIrq(u);
}

uint8_t Uart16550Read(Uart16550* u, uint64_t off) {
  switch (off) {
    case 0:
      if (u->lcr & kLcrDlab) return (uint8_t)(u->divider & 0xff);
      {
        uint8_t v;
        if (u->rx_count > 0) {
          v = u->rx_fifo[u->rx_tail];
          u->rx_tail = (u->rx_tail + 1) % kRxFifoSize;
          u->rx_count--;
        } else {
          // QEMU serial_ioport_read: with the FIFO off, RBR keeps the last
          // received byte; an empty FIFO reads 0.
          v = (u->fcr & kFcrEnable) ? 0 : u->rbr;
        }
        // Reading the data retires the timeout interrupt and its timer: the
        // FIFO it was waiting on is no longer waiting. A freed slot lets the
        // held host burst continue, which may arm a new timeout.
        u->rx_timeout_ip = 0;
        u->rx_timeout_armed = 0;
        DrainHost(u);
        RefreshLsr(u);
        UpdateIrq(u);
        return v;
      }
    case 1:
      return (u->lcr & kLcrDlab) ? (uint8_t)(u->divider >> 8) : u->ier;
    case 2: {
      uint8_t v = u->iir;
      // Reading IIR retires the THR-empty interrupt; it comes back when the
      // guest writes THR again (16550D: the THRE interrupt is not a level).
      u->thr_ip = 0;
      UpdateIrq(u);
      return v;
    }
    case 3:
      return u->lcr;
    case 4:
      return u->mcr;
    case 5: {
      uint8_t v = u->lsr;
      u->lsr = (uint8_t)(u->lsr & ~kLsrErr);  // error bits clear when LSR is read
      UpdateIrq(u);
      return v;
    }
    case 6: {
      uint8_t v = u->msr;
      u->msr = (uint8_t)(u->msr & ~kMsrDelta);
      UpdateIrq(u);
      return v;
    }
    default:
      return u->scr;
  }
}

void Uart16550Write(Uart16550* u, uint64_t off, uint8_t val) {
  switch (off) {
    case 0:
      if (u->lcr & kLcrDlab) {
        u->divider = (uint16_t)((u->divider & 0xff00) | val);
      } else if (u->mcr & kMcrLoop) {
        // Loopback disconnects SOUT from the pin and ties the transmitter to
        // the receiver, so the byte comes straight back (16550D loopback).
        PushRx(u, val);
      } else {
        // Both host-facing byte streams get a row of their own: a guest that
        // prints through the debug port or the UART is the one channel whose
        // content the two models must reproduce byte for byte (AGENTS.md §五).
        DebugMark("uart-tx", (int)(unsigned char)val, 0);
        char ch = (char)val;
        HostWriteOut(&ch, 1);  // transmit immediately: THRE/TEMT stay set
      }
      u->thr_ip = 1;  // a THR write re-arms the THR-empty interrupt
      UpdateIrq(u);
      break;
    case 1:
      if (u->lcr & kLcrDlab) {
        u->divider = (uint16_t)((u->divider & 0x00ff) | ((uint16_t)val << 8));
      } else {
        u->ier = (uint8_t)(val & kIerMask);
        UpdateIrq(u);
      }
      break;
    case 2: {
      // Turning the FIFO on or off empties it: a one-deep receiver cannot
      // inherit a queue (QEMU folds a change of FCR.FE into the two FIFO
      // resets). The write-1 clear of the receiver does the same, and also
      // drops the latched error bits.
      const int mode_changed = (val ^ u->fcr) & kFcrEnable;
      u->fcr = val;
      if (val & kFcrClearRx) {
        FlushRx(u);
        u->lsr = (uint8_t)(u->lsr & ~kLsrErr);
      } else if (mode_changed) {
        FlushRx(u);
      }
      // Clearing the transmitter FIFO has nothing to clear: the transmitter is
      // never busy (a THR write leaves at once).
      u->rx_trigger = kTriggerLevels[(val >> 6) & 3];
      RefreshLsr(u);
      DrainHost(u);  // a cleared/enlarged receiver can take the held burst
      UpdateIrq(u);
      break;
    }
    case 3:
      u->lcr = val;
      // Break control holds SOUT low, which the receiver reports as a break
      // (16550D "Break Control"; QEMU raises LSR.BI the same way).
      if (val & kLcrSbc)
        u->lsr |= kLsrBi;
      else
        u->lsr = (uint8_t)(u->lsr & ~kLsrBi);
      RefreshLsr(u);
      UpdateIrq(u);
      break;
    case 4:
      u->mcr = val;
      UpdateMsr(u);  // loopback and the modem-status deltas follow MCR
      break;
    case 5:
      break;  // LSR is read-only
    case 6:
      break;  // MSR is read-only (its inputs are the modem lines)
    default:
      u->scr = val;
      break;
  }
}

void Uart16550HostReceive(Uart16550* u, int ch) {
  if (u->mcr & kMcrLoop) return;  // loopback disconnects SIN
  if (u->host_count >= (int)sizeof(u->host_fifo)) {
    // The host source cannot be paused here (stdin and a synthetic mouse push
    // without a can_receive callback); QEMU's chardev would stop reading the
    // source instead. 4096 is QEMU's own read-buffer size, so any burst that
    // would fit its frontend fits this hold.
    return;
  }
  u->host_fifo[u->host_head] = (uint8_t)ch;
  u->host_head = (u->host_head + 1) % (int)sizeof(u->host_fifo);
  u->host_count++;
  DrainHost(u);
}

void Uart16550Poll(Uart16550* u) {
  // The receiver may have room since the last read (a guest that drains a
  // burst); feed it before deciding whether the character timeout is due.
  DrainHost(u);
  if (!u->rx_timeout_armed || HostTimerNow() < u->rx_timeout_at) return;
  u->rx_timeout_armed = 0;
  // The timeout reports data that has waited long enough; with the FIFO
  // already drained there is nothing to report.
  if (u->rx_count > 0) {
    u->rx_timeout_ip = 1;
    UpdateIrq(u);
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
  // The line is deliberately not driven here. A board installs its sinks while
  // it is being built, and the path from the sink to the CPU runs through
  // CpuState.set_irq, which the ISA installs only once the loader has picked
  // one — driving it at install time dereferences a hook that does not exist
  // yet. Nothing is lost: out of reset IER is 0, so there is no interrupt to
  // report, and the first IER or data write recomputes the line.
}
