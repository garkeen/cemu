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

// Status register A: the update-in-progress flag. The model updates
// instantaneously, so reads always see it clear — firmware that waits for the
// bit to fall (seabios rtc_updating) passes straight through.
enum { kStatusAUip = 0x80 };
// Status register B (MC146818 datasheet).
enum { kStatusB24Hour = 0x02, kStatusBBin = 0x04, kStatusBSet = 0x80 };
// Status register D: bit 7 reports the battery-backed RAM as valid.
enum { kStatusDVrt = 0x80 };

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
  int64_t bias_sec;  // offset the guest's last clock write asked for
};

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

// ---- registers --------------------------------------------------------------

static uint8_t CmosReadReg(const struct CmosState* st, int reg) {
  if (reg <= kRegYear) return CmosReadTime(st, reg);
  switch (reg) {
    case kRegStatusA:
      return (uint8_t)(st->regs[reg] & ~kStatusAUip);
    case kRegStatusC:
      return 0;  // no interrupt flags: nothing raises the RTC IRQ yet (D18)
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

static void CmosWriteReg(struct CmosState* st, int reg, uint8_t val) {
  if (reg <= kRegYear) {
    CmosWriteTime(st, reg, val);
    return;
  }
  if (reg == kRegStatusC) return;  // read-only (MC146818 datasheet)
  st->regs[reg] = val;
}

static uint64_t CmosRead(void* dev, uint64_t addr, int size) {
  (void)size;
  struct CmosState* st = ((CmosDevice*)dev)->st;
  if (addr == kCmosIndexPort) return (uint64_t)(st->index | (st->nmi_off << 7));
  return CmosReadReg(st, st->index);
}

static void CmosWrite(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  struct CmosState* st = ((CmosDevice*)dev)->st;
  if (addr == kCmosIndexPort) {
    st->nmi_off = (uint8_t)((val >> 7) & 1);
    st->index = (uint8_t)(val & 0x7f);
    return;
  }
  CmosWriteReg(st, st->index, (uint8_t)val);
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
