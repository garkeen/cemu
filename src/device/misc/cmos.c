#include "device/misc/cmos.h"

#include <stdlib.h>
#include <string.h>

#include "host/host.h"

// Ports (PC/AT Technical Reference).
enum { kCmosIndexPort = 0x70, kCmosDataPort = 0x71 };

// Register map (seabios src/hw/rtc.h names the same offsets).
enum {
  kRegSeconds = 0x00,
  kRegMinutes = 0x02,
  kRegHours = 0x04,
  kRegDayOfWeek = 0x06,
  kRegDayOfMonth = 0x07,
  kRegMonth = 0x08,
  kRegYear = 0x09,
  kRegStatusA = 0x0a,
  kRegStatusB = 0x0b,
  kRegStatusC = 0x0c,
  kRegStatusD = 0x0d,
  kRegMemExtmemLow = 0x30,
  kRegMemExtmemHigh = 0x31,
  kRegCentury = 0x32,
  kRegMemExtmem2Low = 0x34,
  kRegMemExtmem2High = 0x35,
};

// Status register A: the update-in-progress flag and the periodic-rate field.
// The model updates instantaneously, so reads always see UIP clear — firmware
// that waits for the bit to fall (seabios rtc_updating) passes straight
// through.
enum { kStatusAUip = 0x80, kStatusARate = 0x0f };
// Status register B (MC146818 datasheet): the three interrupt enables gate
// IRQ8, SET holds the clock still while the guest writes it.
enum {
  kStatusBSet = 0x80,
  kStatusBPie = 0x40,
  kStatusBAie = 0x20,
  kStatusBUie = 0x10,
  kStatusBBin = 0x04,
  kStatusB24Hour = 0x02,
};
// Status register C (MC146818 datasheet): one flag per interrupt source plus
// IRQF, which mirrors the line. A read returns them and clears them all. The
// flag positions line up with status B's enables (PF/PIE 0x40, AF/AIE 0x20,
// UF/UIE 0x10), which is how QEMU gates the line with one mask.
enum { kStatusCIrqf = 0x80, kStatusCPf = 0x40, kStatusCAf = 0x20, kStatusCUf = 0x10 };
// Status register D: bit 7 reports the battery-backed RAM as valid.
enum { kStatusDVrt = 0x80 };

// The alarm registers (MC146818 datasheet): the RTC compares the live clock
// against these every second.
enum { kRegSecondsAlarm = 0x01, kRegMinutesAlarm = 0x03, kRegHoursAlarm = 0x05 };

// The RTC's interrupt line in the PC/AT wiring (the 8259 slave's line 0).
static const int kCmosIrqLine = 8;

// Memory-size registers (seabios src/fw/paravirt.c qemu_preinit reads exactly
// these): 0x30/0x31 hold the memory above 1MiB in KiB, 0x34/0x35 the memory
// above 16MiB in 64KiB units.
static const uint64_t kFirstMeg = 1ULL << 20;
static const uint64_t kSixteenMeg = 16ULL << 20;
static const uint64_t kKiB = 1024;
static const uint64_t k64KiB = 64 * 1024;

struct CmosState {
  uint8_t index;    // last index written to 0x70, NMI bit stripped
  uint8_t nmi_off;  // bit 7 of the last 0x70 write (stored, not acted on)
  uint8_t regs[128];
  uint64_t ram_size;
  int64_t bias_sec;      // offset the guest's last clock write asked for
  uint64_t periodic_at;  // HostTimerNow() us the next PF is due; 0 = not armed
  uint64_t second_at;    // ... the next update-ended/alarm second is due
  int irq;               // the IRQ8 line's current level
};

// The RTC's time base is a 32768Hz crystal, and status A's RS field says how
// many of its ticks make one periodic-interrupt period: RS=0 is none, RS=1/2
// are 128/256 ticks, RS=3..15 are 4,8,16,32,64,128,256,512,1024,2048,4096,
// 8192,16384 — 122us at RS=3 through 500ms at RS=15 (MC146818 datasheet table;
// QEMU mc146818rtc_regs.h writes the same numbers as rates[] =
// {0,256,128,8192,4096,2048,1024,512,256,128,64,32,16,8,4,2} Hz, i.e.
// 32768/ticks).
static const uint32_t kRtcPeriodTicks[16] = {
  0, 128, 256, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384,
};

// The periodic period in microseconds of host time; 0 when RS selects none.
static uint64_t CmosPeriodUs(const struct CmosState* st) {
  uint32_t ticks = kRtcPeriodTicks[st->regs[kRegStatusA] & kStatusARate];
  return ticks ? (uint64_t)ticks * 1000000 / 32768 : 0;
}

// ---- calendar (Howard Hinnant's civil-from-days pair, public domain) --------

static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 2 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static void CivilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
  const unsigned mm = mp + (mp < 10 ? 3 : -9);
  *y = yy + (mm <= 2);
  *m = mm;
  *d = dd;
}

static int ToBcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int FromBcd(int v) { return ((v >> 4) & 0xf) * 10 + (v & 0xf); }

// ---- the clock --------------------------------------------------------------

// Seconds since 1970-01-01 (UTC): the host wall clock plus the bias the
// guest's last write asked for.
static int64_t CmosEpoch(const struct CmosState* st) {
  return HostTimerNow() / 1000000 + st->bias_sec;
}

// The stored time registers decoded into seconds since the epoch: that is what
// a guest's write sequence means, and how the bias is derived from it.
static int64_t CmosShadowEpoch(const struct CmosState* st) {
  int bin = st->regs[kRegStatusB] & kStatusBBin;
  uint8_t sec = st->regs[kRegSeconds], min = st->regs[kRegMinutes], hour = st->regs[kRegHours];
  uint8_t day = st->regs[kRegDayOfMonth], mon = st->regs[kRegMonth], year = st->regs[kRegYear];
  int h = bin ? (hour & 0x7f) : FromBcd(hour & 0x7f);
  if (!(st->regs[kRegStatusB] & kStatusB24Hour) && (hour & 0x80)) h += 12;
  int y = (bin ? year : FromBcd(year)) +
          100 * (bin ? st->regs[kRegCentury] : FromBcd(st->regs[kRegCentury]));
  int64_t days = DaysFromCivil(1900 + y, (unsigned)(bin ? mon : FromBcd(mon)),
                               (unsigned)(bin ? day : FromBcd(day)));
  return days * 86400 + h * 3600 + (bin ? min : FromBcd(min)) * 60 + (bin ? sec : FromBcd(sec));
}

// What the guest reads from a time register: the live clock, unless it is in
// the middle of setting the clock (status B's SET bit holds it still,
// MC146818 datasheet).
static uint8_t CmosReadTime(const struct CmosState* st, int reg) {
  if (st->regs[kRegStatusB] & kStatusBSet) return st->regs[reg];
  int64_t secs = CmosEpoch(st);
  int64_t days = secs / 86400, rem = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  int64_t y;
  unsigned mo, da;
  CivilFromDays(days, &y, &mo, &da);
  int bin = st->regs[kRegStatusB] & kStatusBBin;
  int v = 0;
  switch (reg) {
    case kRegSeconds:
      v = (int)(rem % 60);
      break;
    case kRegMinutes:
      v = (int)((rem / 60) % 60);
      break;
    case kRegHours:
      v = (int)(rem / 3600);
      if (!(st->regs[kRegStatusB] & kStatusB24Hour)) {
        // 12-hour mode: the register's bit 7 is the PM flag (MC146818
        // datasheet).
        int pm = v >= 12;
        v %= 12;
        if (v == 0) v = 12;
        return (uint8_t)((bin ? v : ToBcd(v)) | (pm ? 0x80 : 0));
      }
      break;
    case kRegDayOfWeek:
      // Day 0 (1970-01-01) was a Thursday; the register counts 1..7 from
      // Sunday (MC146818 datasheet).
      v = (int)((days + 4) % 7) + 1;
      break;
    case kRegDayOfMonth:
      v = (int)da;
      break;
    case kRegMonth:
      v = (int)mo;
      break;
    default:
      v = (int)(y % 100);
      break;
  }
  return (uint8_t)(bin ? v : ToBcd(v));
}

static void CmosWriteTime(struct CmosState* st, int reg, uint8_t val) {
  st->regs[reg] = val;
  // The write sequence just redefined the clock: pick up what the registers
  // now say and bias the host clock to it — a real RTC keeps running from the
  // values written into it.
  st->bias_sec = CmosShadowEpoch(st) - HostTimerNow() / 1000000;
}

// ---- interrupts: the status C flags and IRQ8 ---------------------------------

// An alarm field, decoded to 24-hour form; -1 means don't care. The mask is
// the top two bits both set, which is how QEMU rtc_from_bcd() reads it: the
// datasheet puts the don't-care bit in the alarm register's bit 7 and spends
// bit 6 on the 12-hour PM flag, so the pair is what a mask looks like once the
// mode is folded in.
static int CmosAlarmField(const struct CmosState* st, int reg) {
  uint8_t v = st->regs[reg];
  if ((v & 0xc0) == 0xc0) return -1;
  int bin = st->regs[kRegStatusB] & kStatusBBin;
  int h = bin ? (v & 0x3f) : FromBcd(v & 0x3f);
  if (reg == kRegHoursAlarm && !(st->regs[kRegStatusB] & kStatusB24Hour)) {
    // 12-hour mode: bit 7 is the PM flag and the value counts 1..12, exactly
    // as the hours register itself does (see CmosReadTime).
    if (h == 12) h = 0;
    if (v & 0x80) h += 12;
  }
  return h;
}

// Does the alarm match the live clock right now? A don't-care field matches
// every value — an all-don't-care alarm fires once a second. (QEMU computes
// the next matching instant instead, which is the same relation asked
// continuously.)
static int CmosAlarmDue(const struct CmosState* st) {
  int64_t rem = CmosEpoch(st) % 86400;
  if (rem < 0) rem += 86400;
  int a_s = CmosAlarmField(st, kRegSecondsAlarm);
  int a_m = CmosAlarmField(st, kRegMinutesAlarm);
  int a_h = CmosAlarmField(st, kRegHoursAlarm);
  if (a_s >= 0 && a_s != (int)(rem % 60)) return 0;
  if (a_m >= 0 && a_m != (int)((rem / 60) % 60)) return 0;
  if (a_h >= 0 && a_h != (int)(rem / 3600)) return 0;
  return 1;
}

// IRQ8 follows the enabled flags: the line is a level, and reading status C
// clears the flags, which is what drops it (MC146818 datasheet; QEMU lowers
// its IRQ on exactly that read). Enabling an interrupt whose flag is already
// set raises the line at once — QEMU does the same when status B is written.
static void CmosSyncIrq(CmosDevice* d) {
  struct CmosState* st = d->st;
  int level =
      ((st->regs[kRegStatusC] & kStatusCPf) && (st->regs[kRegStatusB] & kStatusBPie)) ||
      ((st->regs[kRegStatusC] & kStatusCAf) && (st->regs[kRegStatusB] & kStatusBAie)) ||
      ((st->regs[kRegStatusC] & kStatusCUf) && (st->regs[kRegStatusB] & kStatusBUie));
  // Status C's IRQF mirrors the line (MC146818 datasheet; QEMU keeps the same
  // bit in step with its qemu_irq_raise/lower).
  if (level)
    st->regs[kRegStatusC] |= kStatusCIrqf;
  else
    st->regs[kRegStatusC] &= (uint8_t)~kStatusCIrqf;
  if (level == st->irq) return;
  st->irq = level;
  if (d->set_irq) d->set_irq(d->irq_ctx, kCmosIrqLine, level);
}

void CmosSetIrqSink(CmosDevice* d, void (*set_irq)(void*, int, int), void* ctx) {
  d->set_irq = set_irq;
  d->irq_ctx = ctx;
}

void CmosPoll(CmosDevice* d) {
  struct CmosState* st = d->st;
  if (!st) return;
  uint64_t now = HostTimerNow();

  // The periodic flag. The deadline is re-derived from the registers on every
  // poll rather than latched when armed, so a guest reprogramming status A or
  // B takes effect at once (QEMU re-arms its timer on those writes). One flag
  // per poll is enough: PF is a single bit and the guest clears it by reading
  // status C. QEMU arms the timer only while PIE is set, so PF does not
  // accumulate with the interrupt disabled (the datasheet's block diagram
  // feeds the flag from the rate generator alone) — cemu follows QEMU, this
  // device's behavioural reference.
  uint64_t period = (st->regs[kRegStatusB] & kStatusBPie) ? CmosPeriodUs(st) : 0;
  if (period == 0) {
    st->periodic_at = 0;
  } else if (st->periodic_at == 0) {
    st->periodic_at = now + period;
  } else if (now >= st->periodic_at) {
    st->periodic_at = now + period;
    st->regs[kRegStatusC] |= kStatusCPf;
  }

  // The update cycle ends once a second: that is when UF sets and when the
  // alarm is compared. Both flags set whatever their enables say — only the
  // line is gated (QEMU sets UF/AF in status C unconditionally and masks the
  // IRQ with status B).
  if (st->second_at == 0) {
    st->second_at = now + 1000000;
  } else if (now >= st->second_at) {
    st->second_at = now + 1000000;
    st->regs[kRegStatusC] |= kStatusCUf;
    if (CmosAlarmDue(st)) st->regs[kRegStatusC] |= kStatusCAf;
  }

  CmosSyncIrq(d);
}

// ---- registers --------------------------------------------------------------

// The registers the live clock answers for: 0x00, 0x02, 0x04, 0x06..0x09.
// 0x01/0x03/0x05 sit inside that range but are the ALARM registers — they hold
// whatever the guest wrote and take part in the comparison, not in the clock
// (MC146818 datasheet). Treating them as time registers made a read of the
// seconds alarm answer with the year.
static int IsTimeReg(int reg) {
  return reg <= kRegYear && reg != kRegSecondsAlarm && reg != kRegMinutesAlarm &&
         reg != kRegHoursAlarm;
}

static uint8_t CmosReadReg(CmosDevice* d, int reg) {
  struct CmosState* st = d->st;
  if (IsTimeReg(reg)) return CmosReadTime(st, reg);
  switch (reg) {
    case kRegStatusA:
      return (uint8_t)(st->regs[reg] & ~kStatusAUip);
    case kRegStatusC: {
      // A read hands back the three flags and clears them, which is what drops
      // IRQ8 (MC146818 datasheet; QEMU lowers its IRQ on the same read).
      uint8_t v = st->regs[reg];
      st->regs[reg] = 0;
      CmosSyncIrq(d);
      return v;
    }
    case kRegStatusD:
      return (uint8_t)(st->regs[reg] | kStatusDVrt);
    case kRegMemExtmemLow:
    case kRegMemExtmemHigh: {
      uint64_t kib = st->ram_size > kFirstMeg ? (st->ram_size - kFirstMeg) / kKiB : 0;
      return (uint8_t)(kib >> (8 * (reg - kRegMemExtmemLow)));
    }
    case kRegMemExtmem2Low:
    case kRegMemExtmem2High: {
      uint64_t units = st->ram_size > kSixteenMeg ? (st->ram_size - kSixteenMeg) / k64KiB : 0;
      return (uint8_t)(units >> (8 * (reg - kRegMemExtmem2Low)));
    }
    default:
      return st->regs[reg];
  }
}

static void CmosWriteReg(CmosDevice* d, int reg, uint8_t val) {
  struct CmosState* st = d->st;
  if (IsTimeReg(reg)) {
    CmosWriteTime(st, reg, val);
    return;
  }
  if (reg == kRegStatusC) return;  // read-only (MC146818 datasheet)
  if (reg == kRegStatusB && (val & kStatusBSet)) {
    // SET stops the update cycle, and the update-ended interrupt goes with it
    // (MC146818 datasheet; QEMU clears UIE on the same write).
    val = (uint8_t)(val & ~kStatusBUie);
  }
  st->regs[reg] = val;
  // A status B write can unmask a flag that is already pending, or mask one
  // that holds the line up (QEMU re-checks the line on that write).
  if (reg == kRegStatusB) CmosSyncIrq(d);
}

static uint64_t CmosRead(void* dev, uint64_t addr, int size) {
  (void)size;
  CmosDevice* d = (CmosDevice*)dev;
  struct CmosState* st = d->st;
  if (addr == kCmosIndexPort) return (uint64_t)(st->index | (st->nmi_off << 7));
  return CmosReadReg(d, st->index);
}

static void CmosWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  CmosDevice* d = (CmosDevice*)dev;
  struct CmosState* st = d->st;
  if (addr == kCmosIndexPort) {
    st->nmi_off = (uint8_t)((val >> 7) & 1);
    st->index = (uint8_t)(val & 0x7f);
    return;
  }
  CmosWriteReg(d, st->index, (uint8_t)val);
}

static const DeviceOps kCmosOps = {"cmos", CmosRead, CmosWrite};

void CmosReset(CmosDevice* d) {
  // A machine reset leaves the battery-backed contents alone — the clock keeps
  // running across it, and the guest re-reads the same date and time after the
  // reboot. Only the chip's access state (the address pointer and the
  // NMI-disable latch) returns to its reset value.
  if (!d->st) return;
  d->st->index = 0;
  d->st->nmi_off = 0;
  // The interrupt enables are not battery-backed either: a reset masks all
  // three, clears the pending flags and drops the line (QEMU clears
  // PIE/AIE/SQWE on its reset).
  d->st->regs[kRegStatusB] &= (uint8_t)~(kStatusBPie | kStatusBAie | kStatusBUie);
  d->st->regs[kRegStatusC] = 0;
  d->st->periodic_at = 0;
  d->st->second_at = 0;
  CmosSyncIrq(d);
}

void CmosInit(CmosDevice* d) {
  if (!d->st) {
    d->st = (struct CmosState*)calloc(1, sizeof(struct CmosState));
    if (!d->st) return;
  } else {
    memset(d->st, 0, sizeof(*d->st));
  }
  struct CmosState* st = d->st;
  // Power-on contents: 32.768kHz source with a 1024Hz periodic rate in status
  // A, 24-hour mode, status D reporting valid RAM, a century register so the
  // two-digit year has a century, and a stored date of 2000-01-01 so a guest
  // that writes only part of the clock still decodes to something sane.
  st->regs[kRegStatusA] = 0x26;
  st->regs[kRegStatusB] = kStatusB24Hour;
  st->regs[kRegStatusD] = kStatusDVrt;
  st->regs[kRegCentury] = (uint8_t)ToBcd(20);
  st->regs[kRegDayOfMonth] = (uint8_t)ToBcd(1);
  st->regs[kRegMonth] = (uint8_t)ToBcd(1);
  st->bias_sec = 0;
}

void CmosRegister(Bus* io, CmosDevice* d) {
  BusAddRegion(io, kCmosIndexPort, 2, &kCmosOps, d);
}

void CmosSetMemory(CmosDevice* d, uint64_t ram_size) {
  if (d->st) d->st->ram_size = ram_size;
}
