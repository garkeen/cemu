// Intel 8259A programmable interrupt controller.
//
// Two chips cascaded (master at 0x20, slave at 0xA0) as in the IBM PC/AT.
// The QEMU i8259 model (Fabrice Bellard, MIT; tiny386 port) is the semantic
// source: edge-triggered IRR, rotating priority via `priority_add`, auto-EOI,
// special fully nested mode, ICW1..4 init handshake. The slave's INT line
// feeds master IRQ2; IRQs 8..15 thus arrive at the master as IRQ2 with the
// real vector supplied by the slave on acknowledge.
//
// Adapted to the cemu device model: registered as two MMIO regions in the
// I/O bus (DeviceOps), and a set_irq callback for the CPU-facing line
// (mirrors the CLINT/PLIC sink wiring on the riscv side).

#include "device/intc/i8259.h"

#include <stdlib.h>
#include <string.h>

#include "util/log.h"

typedef struct PicState {
  uint8_t last_irr;      // edge detection
  uint8_t irr;           // interrupt request register
  uint8_t imr;           // interrupt mask register
  uint8_t isr;           // interrupt service register
  uint8_t priority_add;  // rotating priority base (highest irq = priority_add)
  uint8_t irq_base;      // ICW2: vector number for IRQ0
  uint8_t read_reg_select;
  uint8_t poll;
  uint8_t special_mask;
  uint8_t init_state;  // ICW handshake position
  uint8_t auto_eoi;
  uint8_t rotate_on_auto_eoi;
  uint8_t special_fully_nested_mode;
  uint8_t init4;  // ICW4 expected
  uint8_t single_mode;
  PicDevice* pic;  // back-pointer for EOI callbacks
} PicState;

// I/O port offsets inside each chip's 16-byte window: only 0 and 1 are real.
enum { kPortCmd = 0, kPortData = 1 };

static int GetPriority(const PicState* s, int mask) {
  if (mask == 0) return 8;
  int priority = 0;
  while ((mask & (1 << ((priority + s->priority_add) & 7))) == 0) priority++;
  return priority;
}

// Returns the highest-priority deliverable IRQ (0..7), or -1 if none.
static int PicGetIrq(const PicState* s) {
  int mask = s->irr & ~s->imr;
  int priority = GetPriority(s, mask);
  if (priority == 8) return -1;
  int cur_mask = s->isr;
  if (s->special_mask) cur_mask &= ~s->imr;
  if (s->special_fully_nested_mode && s == &s->pic->pics[0])
    cur_mask &= ~(1 << 2);  // slave IRQ2 stays priority-transparent
  int cur_priority = GetPriority(s, cur_mask);
  if (priority < cur_priority) return (priority + s->priority_add) & 7;
  return -1;
}

static void PicUpdateIrq(PicDevice* pic) {
  // Slave's pending IRQ asserts master IRQ2 as an edge.
  int irq2 = PicGetIrq(&pic->pics[1]);
  if (irq2 >= 0) {
    pic->pics[0].last_irr |= (1 << 2);
    pic->pics[0].irr |= (1 << 2);
  } else {
    pic->pics[0].last_irr &= ~(1 << 2);
  }
  int irq = PicGetIrq(&pic->pics[0]);
  if (pic->set_irq && irq >= 0) pic->set_irq(pic->irq_ctx, irq, 1);
}

static void PicSetIrq1(PicState* s, int irq, int level) {
  int mask = 1 << irq;
  if (level) {
    if ((s->last_irr & mask) == 0) s->irr |= mask;  // rising edge sets IRR
    s->last_irr |= mask;
  } else {
    s->last_irr &= ~mask;
  }
}

void PicSetIrq(PicDevice* pic, int irq, int level) {
  // route to the right chip: IRQ 0..7 master, 8..15 slave.
  PicSetIrq1(&pic->pics[irq >> 3], irq & 7, level);
  PicUpdateIrq(pic);
}

static void PicIntack(PicState* s, int irq) {
  if (s->auto_eoi) {
    if (s->rotate_on_auto_eoi) s->priority_add = (irq + 1) & 7;
  } else {
    s->isr |= (1 << irq);
  }
  s->irr &= ~(1 << irq);
}

int PicAcknowledge(PicDevice* pic) {
  int irq = PicGetIrq(&pic->pics[0]);
  if (irq >= 0) {
    PicIntack(&pic->pics[0], irq);
    if (irq == 2) {
      int irq2 = PicGetIrq(&pic->pics[1]);
      if (irq2 >= 0) {
        PicIntack(&pic->pics[1], irq2);
        irq = irq2;
      } else {
        irq2 = 7;  // spurious on slave
        irq = irq2;
      }
      PicUpdateIrq(pic);
      return pic->pics[1].irq_base + irq2;
    }
    PicUpdateIrq(pic);
    return pic->pics[0].irq_base + irq;
  }
  // spurious on master
  irq = 7;
  PicUpdateIrq(pic);
  return pic->pics[0].irq_base + irq;
}

int PicHasPending(const PicDevice* pic) { return PicGetIrq(&pic->pics[0]) >= 0; }

static void PicReset(PicState* s) {
  s->last_irr = 0;
  s->irr = 0;
  s->imr = 0;
  s->isr = 0;
  s->priority_add = 0;
  s->irq_base = 0;
  s->read_reg_select = 0;
  s->poll = 0;
  s->special_mask = 0;
  s->init_state = 0;
  s->auto_eoi = 0;
  s->rotate_on_auto_eoi = 0;
  s->special_fully_nested_mode = 0;
  s->init4 = 0;
  s->single_mode = 0;
}

// Per-chip I/O write; `addr` is 0 or 1 (already masked to the chip window).
static void PicIoWrite(PicDevice* pic, PicState* s, uint16_t addr, uint8_t val) {
  if (addr == kPortCmd) {
    if (val & 0x10) {
      // ICW1: begin init.
      PicReset(s);
      s->init_state = 1;
      s->init4 = val & 1;
      s->single_mode = val & 2;
      if (val & 0x08) LogError("i8259: level-sensitive mode not supported");
    } else if (val & 0x08) {
      // OCW3.
      if (val & 0x04) s->poll = 1;
      if (val & 0x02) s->read_reg_select = val & 1;
      if (val & 0x40) s->special_mask = (val >> 5) & 1;
    } else {
      // OCW2: EOI commands.
      int cmd = val >> 5;
      switch (cmd) {
        case 0:
        case 4:
          s->rotate_on_auto_eoi = cmd >> 2;
          break;
        case 1:  // non-specific EOI
        case 5:  // non-specific EOI + rotate
        {
          int priority = GetPriority(s, s->isr);
          if (priority != 8) {
            int irq = (priority + s->priority_add) & 7;
            s->isr &= ~(1 << irq);
            if (cmd == 5) s->priority_add = (irq + 1) & 7;
          }
          break;
        }
        case 3:  // specific EOI
          s->isr &= ~(1 << (val & 7));
          break;
        case 6:  // set priority
          s->priority_add = (val + 1) & 7;
          break;
        case 7:  // specific EOI + rotate
        {
          int irq = val & 7;
          s->isr &= ~(1 << irq);
          s->priority_add = (irq + 1) & 7;
          break;
        }
      }
      PicUpdateIrq(pic);
    }
  } else {
    // OCW1 / ICW2..4 distinguished by init_state.
    switch (s->init_state) {
      case 0:
        s->imr = val;
        PicUpdateIrq(pic);
        break;
      case 1:  // ICW2: vector base
        s->irq_base = val & 0xf8;
        s->init_state = s->single_mode ? (s->init4 ? 3 : 0) : 2;
        break;
      case 2:  // ICW3
        s->init_state = s->init4 ? 3 : 0;
        break;
      case 3:  // ICW4
        s->special_fully_nested_mode = (val >> 4) & 1;
        s->auto_eoi = (val >> 1) & 1;
        s->init_state = 0;
        break;
    }
  }
}

static uint8_t PicIoRead(PicDevice* pic, PicState* s, uint16_t addr) {
  (void)pic;
  if (s->poll) {
    s->poll = 0;
    int ret = PicGetIrq(s);
    if (ret >= 0) {
      s->irr &= ~(1 << ret);
      s->isr &= ~(1 << ret);
      return ret | 0x80;
    }
    return 7;
  }
  if (addr == kPortCmd) return s->read_reg_select ? s->isr : s->irr;
  return s->imr;
}

// DeviceOps glue: master window (0x20-0x21) and slave window (0xA0-0xA1).
static uint64_t PicMasterRead(void* dev, uint64_t addr, int size) {
  (void)size;
  return PicIoRead((PicDevice*)dev, &((PicDevice*)dev)->pics[0], (uint16_t)(addr & 1));
}
static void PicMasterWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  PicIoWrite((PicDevice*)dev, &((PicDevice*)dev)->pics[0], (uint16_t)(addr & 1), (uint8_t)val);
}
static uint64_t PicSlaveRead(void* dev, uint64_t addr, int size) {
  (void)size;
  return PicIoRead((PicDevice*)dev, &((PicDevice*)dev)->pics[1], (uint16_t)(addr & 1));
}
static void PicSlaveWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  PicIoWrite((PicDevice*)dev, &((PicDevice*)dev)->pics[1], (uint16_t)(addr & 1), (uint8_t)val);
}

static const DeviceOps kPicMasterOps = {"8259-master", PicMasterRead, PicMasterWrite};
static const DeviceOps kPicSlaveOps = {"8259-slave", PicSlaveRead, PicSlaveWrite};

void PicInit(PicDevice* pic) {
  pic->pics = (PicState*)calloc(2, sizeof(PicState));
  pic->pics[0].pic = pic;
  pic->pics[1].pic = pic;
  PicReset(&pic->pics[0]);
  PicReset(&pic->pics[1]);
  pic->set_irq = NULL;
  pic->irq_ctx = NULL;
}

void PicRegister(Bus* io, PicDevice* pic, uint16_t master_base, uint16_t slave_base) {
  // Each chip's full 16-byte window decodes only the low bit; bytes 2..15
  // are aliases of 0..1 (legacy PC behavior, mirrored by the QEMU model's
  // addr&1).
  BusAddRegion(io, master_base, 16, &kPicMasterOps, pic);
  BusAddRegion(io, slave_base, 16, &kPicSlaveOps, pic);
}

void PicSetIrqSink(PicDevice* pic, void (*set_irq)(void*, int, int), void* ctx) {
  pic->set_irq = set_irq;
  pic->irq_ctx = ctx;
}
