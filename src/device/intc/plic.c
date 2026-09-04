#include "device/intc/plic.h"

enum {
  kPlicOffPriority = 0x000000,
  kPlicOffPending = 0x001000,
  kPlicOffEnable = 0x002000,
  kPlicEnableStride = 0x80,
  kPlicOffContext = 0x200000,
  kPlicContextStride = 0x1000,
};

void PlicInit(PlicDevice* p) {
  for (int i = 0; i <= kPlicNumSrc; i++) p->priority[i] = 0;
  for (int w = 0; w < kPlicNumWords; w++) p->pending[w] = 0;
  for (int c = 0; c < kPlicNumCtx; c++) {
    for (int w = 0; w < kPlicNumWords; w++) p->enable[c][w] = 0;
    p->threshold[c] = 0;
  }
}

static void UpdateOutput(PlicDevice* p) {
  if (!p->set_irq) return;
  for (int ctx = 0; ctx < kPlicNumCtx; ctx++) {
    int claimable = 0;
    for (int src = 1; src <= kPlicNumSrc && !claimable; src++) {
      uint32_t bit = 1u << (src % 32);
      if ((p->pending[src / 32] & bit) && (p->enable[ctx][src / 32] & bit) &&
          p->priority[src] > p->threshold[ctx])
        claimable = 1;
    }
    p->set_irq(p->irq_ctx, ctx, claimable);
  }
}

void PlicDeviceIrq(PlicDevice* p, int src, int level) {
  if (src < 1 || src > kPlicNumSrc) return;
  uint32_t bit = 1u << (src % 32);
  if (level)
    p->pending[src / 32] |= bit;
  else
    p->pending[src / 32] &= ~bit;
  UpdateOutput(p);
}

// Highest-priority claimable source for a context, or 0 (lowest source wins
// ties, per the PLIC manual's strict priority then ID order).
static int Claim(PlicDevice* p, int ctx) {
  int best = 0;
  uint32_t best_prio = 0;
  for (int src = 1; src <= kPlicNumSrc; src++) {
    uint32_t bit = 1u << (src % 32);
    if (!(p->pending[src / 32] & bit) || !(p->enable[ctx][src / 32] & bit)) continue;
    if (p->priority[src] <= p->threshold[ctx]) continue;
    if (best == 0 || p->priority[src] > best_prio) {
      best = src;
      best_prio = p->priority[src];
    }
  }
  return best;
}

// The register file is 4-byte granular (QEMU sifive_plic impl access size);
// wider accesses see two adjacent words, narrower ones the one word.
static uint32_t ReadWord(PlicDevice* p, uint64_t off) {
  int src = (int)(off / 4);
  int word = (int)((off - kPlicOffPending) / 4);
  int ctx, widx;
  if (off < kPlicOffPending) {  // priority block, base offset 0
    return (src >= 1 && src <= kPlicNumSrc) ? p->priority[src] : 0;
  }
  if (off >= kPlicOffPending && off < kPlicOffEnable) {
    return (word >= 0 && word < kPlicNumWords) ? p->pending[word] : 0;
  }
  if (off >= kPlicOffEnable && off < kPlicOffContext) {
    uint64_t rel = off - kPlicOffEnable;
    ctx = (int)(rel / kPlicEnableStride);
    widx = (int)((rel % kPlicEnableStride) / 4);
    return (ctx >= 0 && ctx < kPlicNumCtx && widx >= 0 && widx < kPlicNumWords)
               ? p->enable[ctx][widx]
               : 0;
  }
  if (off >= kPlicOffContext) {
    uint64_t rel = off - kPlicOffContext;
    ctx = (int)(rel / kPlicContextStride);
    int field = (int)((rel % kPlicContextStride) / 4);
    if (ctx >= 0 && ctx < kPlicNumCtx) {
      if (field == 0) return p->threshold[ctx];
      if (field == 1) {
        int src = Claim(p, ctx);
        if (src) {
          p->pending[src / 32] &= ~(1u << (src % 32));
          UpdateOutput(p);
        }
        return (uint32_t)src;
      }
    }
  }
  return 0;
}

static void WriteWord(PlicDevice* p, uint64_t off, uint32_t val) {
  int src = (int)(off / 4);
  int ctx, widx;
  if (off < kPlicOffPending) {  // priority block, base offset 0
    if (src >= 1 && src <= kPlicNumSrc) p->priority[src] = val & 7;
    return;
  }
  if (off >= kPlicOffEnable && off < kPlicOffContext) {
    uint64_t rel = off - kPlicOffEnable;
    ctx = (int)(rel / kPlicEnableStride);
    widx = (int)((rel % kPlicEnableStride) / 4);
    if (ctx >= 0 && ctx < kPlicNumCtx && widx >= 0 && widx < kPlicNumWords)
      p->enable[ctx][widx] = val;
    UpdateOutput(p);
    return;
  }
  if (off >= kPlicOffContext) {
    uint64_t rel = off - kPlicOffContext;
    ctx = (int)(rel / kPlicContextStride);
    int field = (int)((rel % kPlicContextStride) / 4);
    if (ctx >= 0 && ctx < kPlicNumCtx) {
      if (field == 0) p->threshold[ctx] = val;
      // field 1 = completion: QEMU treats the write as a no-op apart from
      // refreshing the output lines.
    }
    UpdateOutput(p);
    return;
  }
  // pending and undecoded offsets ignore writes
}

static uint64_t PlicRead(void* dev, uint64_t addr, int size) {
  PlicDevice* p = (PlicDevice*)dev;
  uint64_t off = addr - p->base;
  uint64_t v = ReadWord(p, off & ~3ULL);
  if (size > 4 && (off & 4)) v |= (uint64_t)ReadWord(p, (off & ~3ULL) + 4) << 32;
  return v;
}

static void PlicWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  PlicDevice* p = (PlicDevice*)dev;
  uint64_t off = addr - p->base;
  WriteWord(p, off & ~3ULL, (uint32_t)val);
  if (size > 4 && (off & 4)) WriteWord(p, (off & ~3ULL) + 4, (uint32_t)(val >> 32));
}

static const DeviceOps kPlicOps = {"plic", PlicRead, PlicWrite};

void PlicRegister(Bus* bus, PlicDevice* p, uint64_t base, uint64_t size) {
  p->base = base;
  BusAddRegion(bus, base, size, &kPlicOps, p);
}

void PlicSetIrqSink(PlicDevice* p, void (*set_irq)(void*, int, int), void* ctx) {
  p->set_irq = set_irq;
  p->irq_ctx = ctx;
}
