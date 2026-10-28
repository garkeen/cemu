// The x86 instruction set: fetch prefixes, decode modrm/SIB, execute — one C
// scope per instruction, one switch case per manual page, opcode order.
// Every name is the manual's name: eax/ax/al for the registers, cf/of for
// the flags, rm8/reg8/imm8 for the operands; the width lives in the name
// (rd8, rm16, push32) — never in a parameter. C operators do the arithmetic;
// helpers exist only for what C has no operator for (flags, stack, segments,
// interrupt dispatch). Scope: the 386 real-mode set plus protected mode
// through segment protection (SDM vol.3 5.3), the gate machinery (vol.3
// 6-7), task switching and two-level paging (vol.3 4); LDT is not in yet.
// The descriptor cache surviving CR0.PE=0 IS big real mode.
//
// The step protocol around these switches (INTR sampling, prefix consumption,
// the single commit) lives in step.c.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus/bus.h"
#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "host/host.h"
#include "util/log.h"

#include "exec.h"  // last: it defines the short register macros x/f/eax/...

// The per-step view (cpu/s/fr/fl), the register and eip spellings, the decoder
// context `d` and the opcode switches' entry points' declarations all come
// from exec.h; these are the definitions — except `cpu`, whose single
// definition lives in the riscv64 exec.c (one ISA per process; see there).
x86_state* s;
frame* fr;
eflags* fl;

// ---- memory -----------------------------------------------------------------

// Open bus: nobody's address reads all ones and drops writes — real x86
// never faults there (SDM; QEMU's unassigned behavior agrees). All accesses
// funnel through bus_load/bus_store (the debug hub's observation points),
// which walk the page tables first when CR0.PG=1; the debug events report
// the PHYSICAL address — what the bus and the devices actually see.
// A20 gate (PC/AT): while the chipset holds the line low, bit 20 of the
// physical address is forced to 0, so accesses above 1 MiB alias into the
// first megabyte — the state firmware runs in until it opens the gate. The
// board drives the line through CpuState.set_a20 (keyboard controller output
// port bit 1, or port 0x92 bit 1).
static uint64_t a20_gate(uint64_t addr) {
  return s->a20 ? addr : (addr & ~(1ULL << 20));
}

// is_fetch marks the instruction stream: the bus category reports device
// accesses (MMIO and port hits), and a fetch is not one. Firmware executes from
// its ROM window for most of a boot, and those fetch rows would otherwise fill
// the session's output cap before any real device access happens.
static uint64_t phys_load(uint64_t addr, int size, int is_fetch) {
  BusRegion* r;
  addr = a20_gate(addr);
  if (BusProbe(cpu->bus, addr, size, &r) != 0) return size == 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
  uint64_t v;
  if (r->host && !r->overlaps) {
    // RAM: one region lookup, no device call. The bytes are the same ones
    // kRamOps would copy; page-table walks and the non-host fallback both use
    // this.
    memcpy(&v, r->host + (addr - r->base), (size_t)size);
  } else {
    v = r->ops->Read(r->dev, addr, size);
  }
  if (g_mem_slow) {
    if (DebugOn(kDbgBus) && !r->host && !is_fetch) DebugBus(fr, r->ops->name, addr, size, 1, v);
    if (DebugOn(kDbgMem)) DebugMem(fr, addr, addr, size, acc_read, v, 1);
  }
  return v;
}

static void phys_store(uint64_t addr, int size, uint64_t v) {
  BusRegion* r;
  addr = a20_gate(addr);
  if (BusProbe(cpu->bus, addr, size, &r) != 0) return;
  if (g_mem_slow) {
    if (DebugOn(kDbgBus) && !r->host) {
      DebugBus(fr, r->ops->name, addr, size, 0, v);
    } else if (DebugOn(kDbgMem)) {
      DebugMem(fr, addr, addr, size, acc_write, v, 0);
    }
  }
  if (r->host && !r->overlaps) {
    memcpy(r->host + (addr - r->base), &v, (size_t)size);
  } else {
    r->ops->Write(r->dev, addr, size, v);
  }
}

// Linear accesses go through the TLB/fetch fast path unless an observation
// category needs every walk (debug.h g_mem_slow). The page walk itself lives
// with the segment machinery below; these are the only entry points.
static uint64_t lin_load(uint64_t lin, int size, int user, int fetch);
static void lin_store(uint64_t lin, int size, int user, uint64_t v);
static uint64_t page_translate_as(uint64_t lin, int write, int user, int fetch);
static int cpl(void);
static x86_tlb_entry* TlbLookup(uint64_t lin, int write);
static x86_tlb_entry* TlbFill(uint64_t lin, int write, int fetch, int user);
static void TlbCheck(x86_tlb_entry* e, uint64_t lin, int write, int fetch, int user);
static void AcCheckSlow(uint64_t lin, int size, int user, int fetch, int write);
static void X87UpdateFerr(void);
static void PerfUpdateActive(void);

static uint64_t bus_load(uint64_t lin, int size) {
  return lin_load(lin, size, cpl() == 3, 0);
}
static void bus_store(uint64_t lin, int size, uint64_t v) {
  lin_store(lin, size, cpl() == 3, v);
}

// Processor-internal accesses — the GDT/LDT/IDT/TSS, and the frames the
// delivery and gate paths push after an outward switch — are SUPERVISOR
// accesses whatever the live CPL is: a ring-3 segment load reads a U=0 GDT,
// `int n` from ring 3 reads a U=0 IDT, and an interrupt taken in ring 3 writes
// the U=0 kernel stack. None of those may fault on U/S (SDM vol.3 4.6: the
// check is on the CPL the *program* runs at; the tables and the switched stack
// are the processor's own state), and every OS depends on it — its GDT/IDT/TSS
// live in supervisor pages.
static uint64_t kbus_load(uint64_t lin, int size) {
  return lin_load(lin, size, 0, 0);
}
static void kbus_store(uint64_t lin, int size, uint64_t v) {
  lin_store(lin, size, 0, v);
}
static uint8_t krd8(uint64_t lin) { return (uint8_t)kbus_load(lin, 1); }
static uint16_t krd16(uint64_t lin) { return (uint16_t)kbus_load(lin, 2); }
static uint32_t krd32(uint64_t lin) { return (uint32_t)kbus_load(lin, 4); }
static void kwr16(uint64_t lin, uint16_t v) { kbus_store(lin, 2, v); }
static void kwr32(uint64_t lin, uint32_t v) { kbus_store(lin, 4, v); }

static uint8_t rd8(uint64_t lin) { return (uint8_t)bus_load(lin, 1); }
static uint16_t rd16(uint64_t lin) { return (uint16_t)bus_load(lin, 2); }
static uint32_t rd32(uint64_t lin) { return (uint32_t)bus_load(lin, 4); }
static void wr8(uint64_t lin, uint8_t v) { bus_store(lin, 1, v); }
static void wr16(uint64_t lin, uint16_t v) { bus_store(lin, 2, v); }
static void wr32(uint64_t lin, uint32_t v) { bus_store(lin, 4, v); }
static uint64_t rd64(uint64_t lin) { return bus_load(lin, 8); }
static void wr64(uint64_t lin, uint64_t v) { bus_store(lin, 8, v); }
// Byte-block copies through the bus: x87 environment/state images are byte
// strings with reserved fields, and FXSAVE is 512 bytes of mixed widths.
static void rd_bytes(uint64_t lin, uint8_t* out, int n) {
  for (int i = 0; i < n; i++) out[i] = rd8(lin + (uint64_t)i);
}
static void wr_bytes(uint64_t lin, const uint8_t* in, int n) {
  for (int i = 0; i < n; i++) wr8(lin + (uint64_t)i, in[i]);
}

// ---- debug registers (SDM vol.3 ch.17) ---------------------------------------

// LENi is a code; these are the sizes it names. The 8-byte entry is kept even
// though this machine is 32-bit because QEMU/KVM honour it there and the kvm
// debug test arms DR1 with LEN=11 (AGENTS.md 附录's adjudication).
static const uint32_t kDrLenBytes[kDrLenCount] = {1, 2, 4, 8};

// Breakpoint i is enabled by DR7.Li/Gi (bits 2i..2i+1) and matches by its
// R/W field: 00 execute (LEN must be 00), 01 data writes, 10 I/O ports,
// 11 data read/write. The R/Wi fields sit at DR7[17:16+4i] and LENi at
// DR7[19:18+4i] (SDM vol.3 figure 17-3; the kvm debug test's 0x00d0040a
// arms DR1 write/8-byte only under this layout).
static int dr_enabled(int i) { return (s->dr[7] >> (2 * i + kDr7LBase)) & 3; }
static int dr_rw(int i) { return (int)((s->dr[7] >> (kDr7RwBase + 4 * i)) & 3); }
static int dr_len_code(int i) { return (int)((s->dr[7] >> (kDr7LenBase + 4 * i)) & 3); }

// Data watchpoints: the #DB is a trap delivered after the instruction
// completes (SDM vol.3 17.3.1), so the match only records DR6.Bn here and
// step.c delivers. Reads match only the read/write encoding.
static void WatchData(uint64_t lin, int size, int write) {
  if (!(s->dr[7] & kDr7AnyEnable)) return;
  for (int i = 0; i < kDrCount; i++) {
    if (!dr_enabled(i)) continue;
    int rw = dr_rw(i);
    if (rw == kDr7RwExec || rw == kDr7RwIo) continue;  // not data breakpoints
    if (rw == kDr7RwWrite && !write) continue;
    uint64_t lo = s->dr[i], hi = lo + kDrLenBytes[dr_len_code(i)];
    // The trap fires when any byte of the access lies in the watch range.
    if (lin < hi && lo < lin + size) d.watch_hit |= 1u << i;
  }
}

// I/O breakpoints (SDM vol.3 17.2.5): R/W=10 with LEN 00 = a 1-byte port
// range or LEN 10 = a 4-byte range; the reserved LEN encodings never match.
// Like data watchpoints this is a trap, recorded for post-instruction
// delivery.
static void IoBpHit(uint16_t port) {
  if (!(s->dr[7] & kDr7AnyEnable)) return;
  for (int i = 0; i < kDrCount; i++) {
    if (!dr_enabled(i) || dr_rw(i) != kDr7RwIo) continue;
    int lenc = dr_len_code(i);
    int len = lenc == 0 ? 1 : lenc == 2 ? 4 : 0;
    if (len && port >= s->dr[i] && port < s->dr[i] + len) d.watch_hit |= 1u << i;
  }
}

// DR7.GD (SDM vol.3 17.2.4): any access to the debug registers raises #DB
// with DR6.BD, and GD self-clears so the handler can read DR6/DR7.
static void dr_guard(void) {
  if (s->dr[7] & kDr7Gd) {
    s->dr[7] &= ~kDr7Gd;
    s->dr[6] |= kDr6Bd;
    raise_(fr, vec_db, 0);
  }
}

// MOV DRn, r32 (SDM vol.3 17.2.2): DR6's B0-B3 clear on ANY write (the kvm
// debug test pins this: set_dr6(0x4002) leaves B1 clear), BD/BS/BT take the
// written value, and the reserved bits read 1. DR7 keeps bit 10 set and the
// reserved bits 11/12, 14/15 clear.
static void DrWrite(int reg, uint32_t v) {
  if (reg == 6) {
    s->dr[6] = kDr6Rsvd1 | (v & (kDr6Bd | kDr6Bs | kDr6Bt));
  } else if (reg == 7) {
    s->dr[7] = (v & 0xffff23ffu) | kDr7Rsvd1;
    s->dr_active = (s->dr[7] & kDr7AnyEnable) != 0;
  } else {
    s->dr[reg] = v;
  }
}

// The gdb stub's door onto the same debug registers MOV DRn programs (SDM
// vol.3 17.2), so an RSP `Z2`/`Z4` and a guest debugger's own watchpoint are one
// machine state with one matcher, and the #DB that stops the session is the one
// the CPU already raises. Representability is the encoding's, not ours to
// stretch: gdb's write and access kinds are the 01 and 11 R/W encodings, a
// *read-only* watchpoint has no encoding at all (11 means read OR write), the
// size has to be one of the four LEN codes, and a data breakpoint must be
// aligned to its size. Anything the encoding cannot say is refused here and
// answered "unsupported" by the stub. DR7 goes through DrWrite so the reserved
// bits obey the same rule as they do for a guest's own MOV.
int x86_gdb_set_watch(CpuState* c, uint64_t addr, uint32_t len, int kind) {
  cpu = c;
  s = (x86_state*)cpu->priv;
  fl = &s->fl;
  // RSP kind 2 names a write watchpoint and 4 an access one. Kind 3 (read only)
  // has no encoding in DR7's R/W field, so it is refused here rather than
  // silently widened to "read or write" -- a widened watchpoint would stop the
  // session on stores the guest never asked to trap on.
  int rw = kind == 2 ? kDr7RwWrite : kind == 4 ? kDr7RwAccess : -1;
  if (rw < 0) return -1;
  int lenc = -1;
  for (int i = 0; i < kDrLenCount; i++)
    if (kDrLenBytes[i] == len) lenc = i;
  if (lenc < 0 || (addr & (len - 1)) != 0) return -1;
  for (int i = 0; i < kDrCount; i++) {
    if (dr_enabled(i)) continue;
    DrWrite(i, (uint32_t)addr);
    uint32_t v = s->dr[7];
    v &= ~(uint32_t)((3u << (2 * i + kDr7LBase)) | (3u << (kDr7RwBase + 4 * i)) |
                     (3u << (kDr7LenBase + 4 * i)));
    // Only the local enable: the general bit means "survive a task switch"
    // (SDM vol.3 17.2.3), and dr_enabled already matches on either.
    v |= (uint32_t)(1u << (2 * i + kDr7LBase)) | (uint32_t)(rw << (kDr7RwBase + 4 * i)) |
         (uint32_t)(lenc << (kDr7LenBase + 4 * i));
    DrWrite(7, v);
    s->gdb_watch_mask |= 1u << i;  // step.c reports hits on this slot to the stub
    return i;
  }
  return -1;  // all four debug registers are taken
}

int x86_gdb_clear_watch(CpuState* c, int slot) {
  if (slot < 0 || slot >= kDrCount) return -1;
  cpu = c;
  s = (x86_state*)cpu->priv;
  fl = &s->fl;
  // The guest may have reprogrammed this slot since the stub armed it (MOV
  // DRn revokes the ownership below). Clearing it then would disable a
  // watchpoint the guest owns, so report "already gone" and leave the register
  // file alone; the stub drops its table entry either way.
  if (!(s->gdb_watch_mask & (1u << slot))) return -1;
  uint32_t v = s->dr[7];
  v &= ~(uint32_t)((3u << (2 * slot + kDr7LBase)) | (3u << (kDr7RwBase + 4 * slot)) |
                   (3u << (kDr7LenBase + 4 * slot)));
  DrWrite(7, v);
  DrWrite(slot, 0);
  s->gdb_watch_mask &= ~(1u << slot);
  return 0;
}

// ---- protection faults (SDM vol.3 5.3 / vol.2 exception pages) ---------------

// Segment-protection faults carry an error code: the selector (index+TI,
// RPL cleared) when a descriptor is the problem, 0 for a null selector or a
// limit break on a usable segment (SDM vol.3 5.3).
_Noreturn static void gp_fault(uint32_t code) { raise_(fr, vec_gp, code); }
_Noreturn static void ss_fault(uint32_t code) { raise_(fr, vec_ss, code); }
_Noreturn static void np_fault(uint32_t code) { raise_(fr, vec_np, code); }
_Noreturn static void ts_fault(uint32_t code) { raise_(fr, vec_ts, code); }
// #PF (SDM vol.3 4.7): CR2 takes the faulting linear address, the error code
// carries P|W|U (vol.3 table 6-1; the 386 defines no RSVD/I bits).
_Noreturn static void pf_fault(uint64_t lin, uint32_t code) {
  s->cr2 = (uint32_t)lin;
  raise_(fr, vec_pf, code);
}

// The per-access segment checks, protected mode only (SDM vol.3 5.3): the
// segment must be usable (a null selector loads into DS/ES/FS/GS fine and
// faults on use), the access type must fit (writes go through writable data
// segments only; execute-only code is unreadable), and the offset must lie
// inside the limit — expand-up below the limit, expand-down data above it.
// A stack-segment violation is #SS(0), every other segment #GP(0) (SDM vol.3
// table 6-1). B bounds no ordinary data segment: it selects the 16/32-bit
// stack pointer (handled by stack_push/stack_pop) and an expand-down
// segment's upper bound only (SDM vol.3 3.4.5).
_Noreturn static void seg_fault(int seg, uint32_t code) {
  if (seg == ss_i) ss_fault(code);
  gp_fault(code);
}

static void seg_use(int seg, uint32_t off, int size, int write) {
  if (!(s->cr0 & 1)) return;
  uint8_t ar = s->ar[seg];
  if (ar == 0) gp_fault(0);
  if (write ? (ar & 0x1a) != 0x12 : (ar & 0x1a) == 0x18) seg_fault(seg, 0);
  if ((ar & 0x1c) == 0x14) {  // expand-down data: S=1, E=0, DC=1
    uint32_t upper = s->dbit[seg] ? 0xffffffffu : 0xffffu;
    if (off <= s->limit[seg] || (uint64_t)off + size - 1 > upper) seg_fault(seg, 0);
  } else {
    if (off > s->limit[seg] || (uint64_t)off + size - 1 > s->limit[seg]) seg_fault(seg, 0);
  }
}

// ---- flag rules (SDM "Flags Affected": one helper per family) ----------------

static uint32_t vmask(int size) { return size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1; }
static uint32_t vmsb(int size) { return 1u << (size * 8 - 1); }

// PF/ZF/SF from a result at a width (shared by every family).
// Odd parity of each nibble: PF is the complement of the XOR of the two
// nibble parities (SDM: PF set when the low byte has an even number of 1s).
static const uint8_t kNibbleParity[16] = {0, 1, 1, 0, 1, 0, 0, 1,
                                          1, 0, 0, 1, 0, 1, 1, 0};

// The six arithmetic flags as bits of eflags.word (SDM vol.1 3.4.3). Packing
// them once per instruction avoids a separate read-modify-write for each
// bitfield; the `fl->cf` spelling remains for the places that update one flag.
enum {
  kFlagCf = 1u << 0,
  kFlagPf = 1u << 2,
  kFlagAf = 1u << 4,
  kFlagZf = 1u << 6,
  kFlagSf = 1u << 7,
  kFlagOf = 1u << 11,
  kFlagArith = kFlagCf | kFlagPf | kFlagAf | kFlagZf | kFlagSf | kFlagOf,
  kFlagSzp = kFlagPf | kFlagZf | kFlagSf,
};

static uint32_t szp_bits(uint32_t r, int size) {
  uint32_t f = 0;
  if (!(kNibbleParity[r & 0xf] ^ kNibbleParity[(r >> 4) & 0xf])) f |= kFlagPf;
  if ((r & vmask(size)) == 0) f |= kFlagZf;
  if ((r & vmsb(size)) != 0) f |= kFlagSf;
  return f;
}
static void szp(uint32_t r, int size) { fl->word = (fl->word & ~kFlagSzp) | szp_bits(r, size); }

void flags_add(uint32_t a, uint32_t b, uint32_t r, int size, int cin) {
  uint32_t f = szp_bits(r, size);
  if ((uint64_t)(a & vmask(size)) + (b & vmask(size)) + cin > vmask(size)) f |= kFlagCf;
  if (((a ^ b ^ r) >> 4) & 1) f |= kFlagAf;
  if ((((~(a ^ b)) & (a ^ r)) >> (size * 8 - 1)) & 1) f |= kFlagOf;
  fl->word = (fl->word & ~kFlagArith) | f;
}

void flags_sub(uint32_t a, uint32_t b, uint32_t r, int size, int bin) {
  uint32_t f = szp_bits(r, size);
  if ((uint64_t)(a & vmask(size)) < (uint64_t)(b & vmask(size)) + bin) f |= kFlagCf;
  if (((a ^ b ^ r) >> 4) & 1) f |= kFlagAf;
  if (((a ^ b) & (a ^ r) & vmsb(size)) != 0) f |= kFlagOf;
  fl->word = (fl->word & ~kFlagArith) | f;
}

void flags_logic(uint32_t r, int size) {
  // CF=OF=AF=0; the packed result leaves those bits clear.
  fl->word = (fl->word & ~kFlagArith) | szp_bits(r, size);
}

// INC/DEC keep CF (SDM).
void flags_inc(uint32_t r, int size) {
  uint32_t f = szp_bits(r, size);
  if (r == vmsb(size)) f |= kFlagOf;
  if ((r & 0xf) == 0) f |= kFlagAf;
  fl->word = (fl->word & ~(kFlagSzp | kFlagAf | kFlagOf)) | f;
}

void flags_dec(uint32_t r, int size) {
  uint32_t f = szp_bits(r, size);
  if (r == vmsb(size) - 1) f |= kFlagOf;
  if ((r & 0xf) == 0xf) f |= kFlagAf;
  fl->word = (fl->word & ~(kFlagSzp | kFlagAf | kFlagOf)) | f;
}

// MUL/IMUL: CF=OF=1 when the high half is significant (SDM MUL/IMUL).
void flags_mul(uint64_t prod, int size) { fl->cf = fl->of = prod >> (size * 8) ? 1 : 0; }

void flags_imul(int64_t prod, int size) {
  int64_t low = (int64_t)((uint64_t)prod << (64 - size * 8)) >> (64 - size * 8);
  fl->cf = fl->of = prod != low;
}

// One decode context for the instruction being executed; the `dec` type and
// the `d` spelling live in exec.h (step.c shares them).
dec g;

// The instruction stream. fetch8 also records raw bytes for the trace.
uint8_t fetch8(void) {
  uint32_t off = (uint32_t)(eip + d.nxt);
  // CS limit (SDM vol.3 5.3): every fetched byte lies inside the segment.
  if ((s->cr0 & 1) && off > s->limit[cs_i]) gp_fault(0);
  uint64_t lin = s->base[cs_i] + off;
  int user = cpl() == 3;
  uint8_t v;
  if (g_mem_slow) {
    v = (uint8_t)lin_load(lin, 1, user, 1);
  } else {
    uint32_t page = (uint32_t)(lin >> 12);
    if (s->fetch_valid && s->fetch_page == page) {
      v = s->fetch_host[(uint32_t)lin & 0xfff];
    } else {
      uint8_t* host = NULL;
      uint64_t paddr;
      if (!(s->cr0 & kCr0Pg)) {
        paddr = a20_gate(lin);
        BusRamPage(cpu->bus, paddr & ~(uint64_t)0xfff, &host);
      } else {
        x86_tlb_entry* e = TlbLookup(lin, 0);
        if (!e) {
          e = TlbFill(lin, 0, 1, user);
        } else {
          TlbCheck(e, lin, 0, 1, user);
        }
        paddr = e->pbase + ((uint32_t)lin & 0xfff);
        if (e->flags & kTlbHost) host = e->host;
      }
      if (host) {
        s->fetch_page = page;
        s->fetch_host = host;
        s->fetch_valid = 1;
        v = host[(uint32_t)lin & 0xfff];
      } else {
        // ROM/MMIO has no host backing; every byte goes through the bus.
        s->fetch_valid = 0;
        v = (uint8_t)phys_load(paddr, 1, 1);
      }
    }
  }
  if (g_record_raw && fr->rec.raw_len < sizeof(fr->rec.raw))
    fr->rec.raw[fr->rec.raw_len++] = v;
  d.nxt++;
  return v;
}
static uint16_t fetch16(void) {
  uint16_t v = fetch8();
  return v | (uint16_t)fetch8() << 8;
}
static uint32_t fetch32(void) {
  uint32_t v = fetch16();
  return v | (uint32_t)fetch16() << 16;
}
static uint8_t imm8(void) { return fetch8(); }
static uint16_t imm16(void) { return fetch16(); }
static uint32_t imm32(void) { return fetch32(); }

// modrm/SIB (SDM vol.1 3.4.2-3.4.4). A BP/EBP base means SS, else DS; a
// segment override replaces the default for every reference.
static void modrm(void) {
  uint8_t enc = fetch8();
  d.enc = enc;
  d.mod = enc >> 6;
  d.reg = (enc >> 3) & 7;
  d.rm = enc & 7;
  d.mseg = d.seg;
  if (d.mod == 3) {
    d.is_mem = 0;
    return;
  }
  d.is_mem = 1;
  d.m_esp_base = 0;
  uint32_t off = 0;
  if (!d.a32) {
    // 16-bit addressing (SDM vol.1 table 2-1): BP+SI, BP+DI, SI, DI, BP or
    // disp16. The sum wraps at 64K (tiny386 modsib16 masks with 0xffff).
    static const int base[8] = {ebx_i, ebx_i, ebp_i, ebp_i, esi_i, edi_i, ebp_i, ebx_i};
    static const int index[8] = {esi_i, edi_i, esi_i, edi_i, -1, -1, -1, -1};
    off = s->r[base[d.rm]].x;
    if (index[d.rm] >= 0) off += s->r[index[d.rm]].x;
    if (d.mod == 1)
      off += (int8_t)fetch8();
    else if (d.mod == 2)
      off += fetch16();
    else if (d.rm == 6)
      off = fetch16();  // [disp16]: no base register
    off &= 0xffff;
    if (d.mseg < 0 && (d.rm == 2 || d.rm == 3 || (d.rm == 6 && d.mod != 0))) d.mseg = ss_i;
  } else if (d.rm == 4) {
    // SIB (table 2-3): scale*index + base (+ disp). Index 4 = no index;
    // base 5 with mod 0 = no base, disp32 instead — the index term applies
    // in every form.
    uint8_t sib = fetch8();
    int scale = 1 << (sib >> 6);
    int idx = (sib >> 3) & 7;
    int base = sib & 7;
    // A base of ESP is always a real base (the no-base form is base 5 with mod
    // 0), and ESP can never be an index — so this flag marks exactly the
    // operands whose effective address POP must recompute after the pop
    // (SDM vol.2 POP: a memory destination's address is computed *after* ESP
    // is incremented).
    if (base == 4) d.m_esp_base = 1;
    if (base == 5 && d.mod == 0) {
      off = fetch32();
    } else {
      off = s->r[base].e;
      // ESP and EBP bases default to SS (SDM vol.1 2.1.2); every other base
      // stays on the override-or-DS default.
      if (d.mseg < 0 && (base == 4 || base == 5)) d.mseg = ss_i;
    }
    if (idx != 4) off += (uint32_t)scale * s->r[idx].e;
    if (d.mod == 1)
      off += (int8_t)fetch8();
    else if (d.mod == 2)
      off += fetch32();
  } else {
    if (d.rm == 5 && d.mod == 0) {
      off = fetch32();  // disp32, no base
    } else {
      off = s->r[d.rm].e;
      if (d.mod == 1)
        off += (int8_t)fetch8();
      else if (d.mod == 2)
        off += fetch32();
    }
    if (d.mseg < 0 && d.rm == 5 && d.mod != 0) d.mseg = ss_i;
  }
  if (d.mseg < 0) d.mseg = ds_i;
  d.moff = off;
  d.mlin = s->base[d.mseg] + off;
}

// ---- the rm/reg operands (SDM operand columns, one pair per width) ---------

// Byte registers 4..7 are the high halves of EAX..EBX (SDM 3.4.1).
static uint8_t reg8(void) { return d.reg < 4 ? s->r[d.reg].l : s->r[d.reg - 4].h; }
static void set_reg8(uint8_t v) {
  if (d.reg < 4)
    s->r[d.reg].l = v;
  else
    s->r[d.reg - 4].h = v;
}
static uint16_t reg16(void) { return s->r[d.reg].x; }
static void set_reg16(uint16_t v) { s->r[d.reg].x = v; }
static uint32_t reg32(void) { return s->r[d.reg].e; }
static void set_reg32(uint32_t v) { s->r[d.reg].e = v; }

static uint8_t rm8(void) {
  if (d.is_mem) {
    seg_use(d.mseg, d.moff, 1, 0);
    WatchData(d.mlin, 1, 0);
    return rd8(d.mlin);
  }
  return d.rm < 4 ? s->r[d.rm].l : s->r[d.rm - 4].h;
}
static void set_rm8(uint8_t v) {
  if (d.is_mem) {
    seg_use(d.mseg, d.moff, 1, 1);
    WatchData(d.mlin, 1, 1);
    wr8(d.mlin, v);
  } else if (d.rm < 4) {
    s->r[d.rm].l = v;
  } else {
    s->r[d.rm - 4].h = v;
  }
}
static uint16_t rm16(void) {
  if (d.is_mem) {
    seg_use(d.mseg, d.moff, 2, 0);
    WatchData(d.mlin, 2, 0);
    return rd16(d.mlin);
  }
  return s->r[d.rm].x;
}
static void set_rm16(uint16_t v) {
  if (d.is_mem) {
    seg_use(d.mseg, d.moff, 2, 1);
    WatchData(d.mlin, 2, 1);
    wr16(d.mlin, v);
  } else {
    s->r[d.rm].x = v;
  }
}
static uint32_t rm32(void) {
  if (d.is_mem) {
    seg_use(d.mseg, d.moff, 4, 0);
    WatchData(d.mlin, 4, 0);
    return rd32(d.mlin);
  }
  return s->r[d.rm].e;
}
static void set_rm32(uint32_t v) {
  if (d.is_mem) {
    seg_use(d.mseg, d.moff, 4, 1);
    WatchData(d.mlin, 4, 1);
    wr32(d.mlin, v);
  } else {
    s->r[d.rm].e = v;
  }
}

// The destination of SLDT/STR (SDM vol.2 both): a memory operand is always
// written as a 16-bit quantity regardless of operand size, and a 32-bit
// register destination receives the selector zero-extended (the upper 16 bits
// are cleared). Storing 32 bits to memory would clobber the two bytes above
// the destination — the interrupt stack's error-code slot, in the kvm
// taskswitch2 #PF case.
static void set_sys_sel(uint16_t sel) {
  if (d.is_mem || !d.w32)
    set_rm16(sel);
  else
    set_rm32(sel);
}

// The 8/16/32 rm access at the size the case wants, in one name each arm.
#define RM8() rm8()
#define SET_RM8(v) set_rm8(v)
#define RM16() rm16()
#define SET_RM16(v) set_rm16(v)
#define RM32() rm32()
#define SET_RM32(v) set_rm32(v)

// ---- the stack (SDM PUSH/POP; SP wraps at 64K in 16-bit stacks) -------------

// ---- the stack (SDM PUSH/POP) -------------------------------------------------
// The stack's address size follows SS.B (SDM vol.3 3.4.5), not the operand
// prefix: a B=0 stack wraps SP at 64K and keeps ESP's high bits; d.w32 only
// picks the pushed width.

static void stack_push(int size, uint32_t v) {
  // A B=0 stack decrements and addresses through SP only: the memory offset
  // stays under 64K while ESP's high bits ride along untouched (SDM vol.3
  // 3.4.5; pinned by kvm realmode's push_pop_high_esp_bits).
  uint32_t sp16 = (uint16_t)(esp - size);
  uint32_t off = s->dbit[ss_i] ? esp - (uint32_t)size : sp16;
  seg_use(ss_i, off, size, 1);
  WatchData(s->base[ss_i] + off, size, 1);
  if (size == 4)
    wr32(s->base[ss_i] + off, v);
  else
    wr16(s->base[ss_i] + off, (uint16_t)v);
  esp = s->dbit[ss_i] ? off : (esp & 0xffff0000u) | sp16;
}
static uint32_t stack_pop(int size) {
  uint32_t off = s->dbit[ss_i] ? esp : (uint16_t)esp;
  seg_use(ss_i, off, size, 0);
  WatchData(s->base[ss_i] + off, size, 0);
  uint32_t v = size == 4 ? rd32(s->base[ss_i] + off) : rd16(s->base[ss_i] + off);
  esp = s->dbit[ss_i] ? esp + (uint32_t)size
                      : (esp & 0xffff0000u) | (uint16_t)(esp + size);
  return v;
}
static void push16(uint16_t v) { stack_push(2, v); }
static void push32(uint32_t v) { stack_push(4, v); }
static uint16_t pop16(void) { return (uint16_t)stack_pop(2); }
static uint32_t pop32(void) { return stack_pop(4); }

// The frames a delivery writes are the processor's own writes too: after an
// outward switch the stack is the new supervisor one while CS still names the
// old ring-3 segment, so the store may not take the U/S check.
static void kstack_push(int size, uint32_t v) {
  uint32_t sp16 = (uint16_t)(esp - size);
  uint32_t off = s->dbit[ss_i] ? esp - (uint32_t)size : sp16;
  seg_use(ss_i, off, size, 1);
  WatchData(s->base[ss_i] + off, size, 1);
  if (size == 4)
    kwr32(s->base[ss_i] + off, v);
  else
    kwr16(s->base[ss_i] + off, (uint16_t)v);
  esp = s->dbit[ss_i] ? off : (esp & 0xffff0000u) | sp16;
}
static void kpush16(uint16_t v) { kstack_push(2, v); }
static void kpush32(uint32_t v) { kstack_push(4, v); }
// A stack read that does not move ESP, at a byte offset above ESP. The
// far-control paths (IRET/RETF/gate entry) peek their whole frame and
// validate it before a single pop commits: raise_ unwinds without rolling
// back register writes, so a faulting return must never find its stack
// half-consumed.
static uint32_t stack_peek(int size, uint32_t bytes) {
  uint32_t off = s->dbit[ss_i] ? esp + bytes : (uint16_t)(esp + bytes);
  seg_use(ss_i, off, size, 0);
  return size == 4 ? rd32(s->base[ss_i] + off) : rd16(s->base[ss_i] + off);
}
// The operand-size polymorphic pair: `w32` picks the width.
static void push_w(uint32_t v) { stack_push(d.w32 ? 4 : 2, v); }
static uint32_t pop_w(void) { return stack_pop(d.w32 ? 4 : 2); }

// ---- segments and interrupts --------------------------------------------------

// The descriptor cache view (SDM vol.3 3.4.5): what a segment-register load
// commits — base, effective limit (G already expanded), the access-rights
// byte (P DPL S Type), the D/B flag, and the descriptor's high dword for
// LAR. The cache SURVIVES CR0.PE=0 — that survival is big real mode (the
// kvm-unit-tests harness relies on it).
typedef struct seg_view {
  uint64_t base;
  uint32_t limit;
  uint32_t hi;  // descriptor bytes 4-7, LAR's raw answer
  uint8_t ar;
  uint8_t dbit;
} seg_view;

// A descriptor-table entry, as the system paths read it (TSS, LDT, gate
// targets, task switches) and as a protected-mode segment load reads it. No
// checks here — the loaders validate before committing, in the SDM vol.3 5.3
// pseudocode order (the checks live in the SDM, not in tiny386, whose
// limit/type checks are partial). TI=1 reads the LDT through its descriptor
// cache (SDM vol.3 2.4.4): a null LDTR caches limit 0, so the caller's
// table-limit check fails with the selector and no special case exists
// (tiny386 read_desc, v86 lookup_segment_selector).
static void desc_parse(uint16_t sel, seg_view* v) {
  // The GDT and LDT bases are linear addresses (SDM vol.3 3.5.1), so an entry
  // comes through the paging unit: a kernel whose tables sit above the identity
  // map (xv6's GDT and IDT live at 0x8011xxxx) has no other way to reach them —
  // and the read is the processor's own, so it is a supervisor access (kbus_load)
  // however small the guest's CPL is.
  uint64_t table = sel & 4 ? s->ldtr_base : s->gdtr;
  uint64_t desc = kbus_load(table + (uint64_t)(sel >> 3) * 8, 8);
  v->hi = (uint32_t)(desc >> 32);
  v->base = ((desc >> 16) & 0xffffff) | (((desc >> 56) & 0xff) << 24);
  uint32_t lim = (uint32_t)(desc & 0xffff) | (uint32_t)((desc >> 32) & 0xf0000);
  v->limit = (desc >> 55) & 1 ? (lim << 12) | 0xfff : lim;
  v->ar = (uint8_t)(desc >> 40);  // type/DPL/S/P: descriptor bytes 5:4 hi
  v->dbit = (uint8_t)((desc >> 54) & 1);
}

// Real mode and virtual-8086 mode synthesize the descriptor cache from the
// selector (SDM vol.3 3.4.4 and 17.3.1): base = selector*16, 64K limit, 16-bit,
// and no table is consulted. VM86's segments all carry DPL 3 — that is where
// cpl() reads the mode's CPL 3 from — and there is no descriptor whose A bit
// could be written back.
static void seg_synth(uint16_t sel, seg_view* v) {
  v->base = (uint64_t)sel << 4;
  v->limit = 0xffff;
  v->hi = 0;
  v->ar = fl->vm ? 0xf3 : 0x93;  // present, DPL 3 (VM86) / DPL 0 (real), data RW
  v->dbit = 0;
}

// The limit of the descriptor table a selector names (SDM vol.3 2.4/3.5).
static uint32_t table_limit(uint16_t sel) { return sel & 4 ? s->ldtr_limit : s->gdtr_limit; }

// Commits a validated load: fills the cache and marks the descriptor
// accessed (SDM vol.3 5.3: the CPU sets the A bit when a segment register
// is loaded from it) — in whichever table the selector names. Both halves
// are processor accesses: IRET commits the ring-3 CS first and writes that
// descriptor's A bit back afterwards, which is only legal because the write
// is a supervisor access to a U=0 GDT page. Real mode and VM86 name no
// descriptor, so there is nothing to mark.
static void seg_commit(int seg, uint16_t sel, const seg_view* v) {
  s->sreg[seg] = sel;
  s->base[seg] = v->base;
  s->limit[seg] = v->limit;
  s->ar[seg] = v->ar;
  s->dbit[seg] = v->dbit;
  if ((s->cr0 & 1) && !fl->vm && (sel & 0xfffc) && !(v->ar & 1))
    kbus_store((sel & 4 ? s->ldtr_base : s->gdtr) + (uint64_t)(sel >> 3) * 8 + 5, 1,
               (uint8_t)(v->ar | 1));
}

// CPL is the CS descriptor's DPL (SDM vol.1 3.4.5); the cache is the source.
// VM86's segments are synthesized with DPL 3, so the mode's CPL 3 comes from
// the same place every other mode's does.
static int cpl(void) { return (s->ar[cs_i] >> 5) & 3; }

// I/O privilege (SDM vol.2 IN/OUT Operation): real mode always allows, and so
// does protected mode at CPL <= IOPL. Everything else — CPL above IOPL, and
// any VM86 access, where the mode ignores IOPL — is allowed only where the
// TSS's I/O permission bitmap holds a 0 bit for every byte of the port range.
// The bitmap starts at the TSS's iomap_base (TSS +0x66) with one bit per port;
// a bitmap that does not reach the port, or a TR that holds no 32-bit TSS,
// denies.
static int io_allowed(uint16_t port, int size) {
  if (!(s->cr0 & 1)) return 1;
  if (!fl->vm && cpl() <= fl->iopl) return 1;
  uint8_t ty = s->tr_ar & 0xf;
  if (!(s->tr_ar & 0x80) || (ty != 9 && ty != 0xb)) return 0;
  uint32_t iobase = krd16(s->tr_base + 0x66);
  for (int i = 0; i < size; i++) {
    uint32_t off = iobase + ((uint32_t)(port + i) >> 3);
    if (off > s->tr_limit) return 0;
    if (krd8(s->tr_base + off) & (1u << ((port + i) & 7))) return 0;
  }
  return 1;
}

// The port-I/O funnel: every IN/OUT runs the privilege check, then the I/O
// breakpoint check after the access, and — like the memory funnel — reports the
// device hit to the debug hub's bus category (the winning region's name; the
// unclaimed region has one too, so an accidental touch of a port nobody
// implements is visible).
static uint32_t io_in(uint16_t port, int size) {
  if (!io_allowed(port, size)) gp_fault(0);
  BusRegion* r;
  uint32_t v = (uint32_t)BusRead(cpu->io, port, size);
  if (DebugOn(kDbgBus) && BusProbe(cpu->io, port, size, &r) == 0)
    DebugBus(fr, r->ops->name, port, size, 1, v);
  IoBpHit(port);
  return v;
}
static void io_out(uint16_t port, int size, uint32_t v) {
  if (!io_allowed(port, size)) gp_fault(0);
  BusRegion* r;
  if (DebugOn(kDbgBus) && BusProbe(cpu->io, port, size, &r) == 0)
    DebugBus(fr, r->ops->name, port, size, 0, v);
  BusWrite(cpu->io, port, size, v);
  IoBpHit(port);
}

// ---- paging: walker, TLB and the linear access paths --------------------------
//
// The 32-bit paging unit, complete for the i686 target: two-level 4KB/4MB
// pages, PAE's three-level 64-bit-entry walk with 2MB leaves, PGE global
// translations and the PAE NX bit. The walker sets A/D on every fill (a TLB
// hit does not revisit the tables, QEMU's behavior); changing a PTE therefore
// requires INVLPG or a CR3 reload, exactly as the SDM requires of software.

typedef struct walk_res {
  uint64_t paddr;  // A20 already applied
  uint32_t flags;  // kTlbWritable / kTlbUser / kTlbNx / kTlbGlobal
  uint32_t lin_base;   // page start for INVLPG's range match
  uint32_t page_shift; // 12 = 4KB, 21 = 2MB, 22 = 4MB
} walk_res;

// PAE entry masks (SDM vol.3 figs 4-5/4-6/4-7): physical base 51:12, 2MB leaf
// base 51:21, bits 62:52 reserved, bit 63 the NX bit.
static const uint64_t kPaeAddrMask = 0x000ffffffffff000ull;
static const uint64_t kPae2mMask = 0x000fffffffe00000ull;
static const uint64_t kPaeRsvdHi = 0x7ffull << 52;
static const uint64_t kNxBit = 1ull << 63;

static void PfRsvd(uint64_t lin, uint32_t code) {
  pf_fault(lin, code | kPfPresent | kPfRsvd);
}
static void PfNx(uint64_t lin, uint32_t code) {
  pf_fault(lin, code | kPfPresent | kPfInstr);
}

// The shared permission rule (SDM vol.3 4.6 table 4-2): a user access needs
// U on every level and a user write needs R/W whatever CR0.WP says; a
// supervisor write needs R/W only when WP=1. `writable` and `user_ok` are the
// AND of the levels.
static void CheckPerm(uint64_t lin, int write, int user, int writable, int user_ok,
                      uint32_t code) {
  if (user ? (!user_ok || (write && !writable))
           : (write && (s->cr0 & kCr0Wp) && !writable))
    pf_fault(lin, code | kPfPresent);
}

// Two-level 386/486 page walk (SDM vol.3 4.3, 4.6, 4.7). PSE-36 is advertised
// (CPUID.01H:EDX.PSE36), so a 4MB leaf's bits 20:13 are physical address
// bits 39:32 and only bit 21 stays reserved (SDM vol.3 fig 4-4); a set
// reserved bit faults as an ordinary present fault (the 386-class #PF code
// has no RSVD flag).
static void WalkNonPae(uint64_t lin, int write, int user, walk_res* w) {
  uint32_t code = (write ? kPfWrite : 0) | (user ? kPfUser : 0);
  uint32_t pde_addr = (uint32_t)(s->cr3 & ~0xfffu) | ((uint32_t)(lin >> 20) & 0xffc);
  uint32_t pde = (uint32_t)phys_load(pde_addr, 4, 0);
  if (!(pde & kPdeP)) pf_fault(lin, code);
  if ((pde & kPdePs) && (s->cr4 & kCr4Pse)) {
    if (pde & kPde4mReserved) pf_fault(lin, code | kPfPresent);
    CheckPerm(lin, write, user, (pde & kPdeRw) != 0, (pde & kPdeUs) != 0, code);
    uint32_t new_pde = pde | kPdeA | (write ? kPdeD : 0u);
    if (new_pde != pde) phys_store(pde_addr, 4, new_pde);
    uint64_t frame = (pde & 0xffe00000u) |
                     ((uint64_t)(pde & 0x001fe000u) << (32 - 13));
    w->paddr = a20_gate(frame | (lin & 0x3fffff));
    w->flags = ((pde & kPdeRw) ? kTlbWritable : 0) |
               ((pde & kPdeUs) ? kTlbUser : 0) | ((pde & kPdeG) ? kTlbGlobal : 0);
    w->lin_base = (uint32_t)lin & ~0x3fffffu;
    w->page_shift = 22;
    return;
  }
  uint32_t pte_addr = (pde & ~0xfffu) | ((uint32_t)(lin >> 10) & 0xffc);
  uint32_t pte = (uint32_t)phys_load(pte_addr, 4, 0);
  if (!(pte & kPteP)) pf_fault(lin, code);
  int writable = (pde & kPdeRw) && (pte & kPteRw);
  int user_ok = (pde & kPdeUs) && (pte & kPteUs);
  CheckPerm(lin, write, user, writable, user_ok, code);
  uint32_t new_pte = pte | kPteA | (write ? kPteD : 0u);
  if (new_pte != pte) phys_store(pte_addr, 4, new_pte);
  if (!(pde & kPdeA)) phys_store(pde_addr, 4, pde | kPdeA);
  w->paddr = a20_gate(((uint64_t)(pte & ~0xfffu)) | (lin & 0xfff));
  w->flags = (writable ? kTlbWritable : 0) | (user_ok ? kTlbUser : 0) |
             ((pte & kPteG) ? kTlbGlobal : 0);
  w->lin_base = (uint32_t)lin & ~0xfffu;
  w->page_shift = 12;
}

// PAE: CR3 names a 32-byte-aligned PDPT; PDPTE -> PDE (2MB leaf or a page
// table) -> PTE. Every entry is 64-bit and carries A; D lives in the leaf
// (PDE for 2MB, PTE for 4KB). NX in any level forbids instruction fetch.
static void WalkPae(uint64_t lin, int write, int user, int fetch, walk_res* w) {
  uint32_t code = (write ? kPfWrite : 0) | (user ? kPfUser : 0);
  uint64_t pdpte_addr = (s->cr3 & 0xffffffe0u) + (((lin >> 30) & 3) * 8);
  uint64_t pdpte = phys_load(pdpte_addr, 8, 0);
  if (!(pdpte & kPdeP)) pf_fault(lin, code);
  uint64_t nx = pdpte & kNxBit;
  if (pdpte & kPdePs) PfRsvd(lin, code);  // 1GB pages do not exist in 32-bit PAE
  if (pdpte & kPaeRsvdHi) PfRsvd(lin, code);
  if (nx && !(s->efer & kEferNxe)) PfRsvd(lin, code);

  uint64_t pde_addr = (pdpte & kPaeAddrMask) | (((lin >> 21) & 0x1ff) * 8);
  uint64_t pde = phys_load(pde_addr, 8, 0);
  if (!(pde & kPdeP)) pf_fault(lin, code);
  nx |= pde & kNxBit;
  if (pde & kPaeRsvdHi) PfRsvd(lin, code);
  if (nx && !(s->efer & kEferNxe)) PfRsvd(lin, code);
  int writable = (pdpte & kPdeRw) && (pde & kPdeRw);
  int user_ok = (pdpte & kPdeUs) && (pde & kPdeUs);

  if (pde & kPdePs) {  // 2MB leaf: base 51:21, PDE holds A/D
    if (pde & 0x1fe000) PfRsvd(lin, code);
    CheckPerm(lin, write, user, writable, user_ok, code);
    if (fetch && nx) PfNx(lin, code);
    uint64_t new_pde = pde | kPdeA | (write ? kPdeD : 0u);
    if (new_pde != pde) phys_store(pde_addr, 8, new_pde);
    if (!(pdpte & kPdeA)) phys_store(pdpte_addr, 8, pdpte | kPdeA);
    w->paddr = a20_gate((pde & kPae2mMask) | (lin & 0x1fffff));
    w->flags = (writable ? kTlbWritable : 0) | (user_ok ? kTlbUser : 0) |
               (nx ? kTlbNx : 0) | ((pde & kPdeG) ? kTlbGlobal : 0);
    w->lin_base = (uint32_t)lin & ~0x1fffffu;
    w->page_shift = 21;
    return;
  }

  uint64_t pte_addr = (pde & kPaeAddrMask) | (((lin >> 12) & 0x1ff) * 8);
  uint64_t pte = phys_load(pte_addr, 8, 0);
  if (!(pte & kPteP)) pf_fault(lin, code);
  nx |= pte & kNxBit;
  if (pte & kPaeRsvdHi) PfRsvd(lin, code);
  if (nx && !(s->efer & kEferNxe)) PfRsvd(lin, code);
  writable = writable && (pte & kPteRw);
  user_ok = user_ok && (pte & kPteUs);
  CheckPerm(lin, write, user, writable, user_ok, code);
  if (fetch && nx) PfNx(lin, code);
  uint64_t new_pte = pte | kPteA | (write ? kPteD : 0u);
  if (new_pte != pte) phys_store(pte_addr, 8, new_pte);
  if (!(pde & kPdeA)) phys_store(pde_addr, 8, pde | kPdeA);
  if (!(pdpte & kPdeA)) phys_store(pdpte_addr, 8, pdpte | kPdeA);
  w->paddr = a20_gate((pte & kPaeAddrMask) | (lin & 0xfff));
  w->flags = (writable ? kTlbWritable : 0) | (user_ok ? kTlbUser : 0) |
             (nx ? kTlbNx : 0) | ((pte & kPteG) ? kTlbGlobal : 0);
  w->lin_base = (uint32_t)lin & ~0xfffu;
  w->page_shift = 12;
}

static void WalkPage(uint64_t lin, int write, int user, int fetch, walk_res* w) {
  if (s->cr4 & kCr4Pae)
    WalkPae(lin, write, user, fetch, w);
  else
    WalkNonPae(lin, write, user, w);
}

// The slow translation entry: one walk, no cache. Used whenever g_mem_slow is
// set (mem/bus/watch observation) and by processor-internal accessors.
static uint64_t page_translate_as(uint64_t lin, int write, int user, int fetch) {
  if (!(s->cr0 & kCr0Pg)) return lin;
  walk_res w;
  WalkPage(lin, write, user, fetch, &w);
  return w.paddr;
}

// ---- translation cache --------------------------------------------------------

static uint32_t TlbSet(uint32_t vpn) { return (vpn ^ (vpn >> 10)) & (kTlbSets - 1); }
static uint32_t TlbTag(uint32_t vpn, int write) {
  return kTlbValid | (vpn << 1) | (s->tlb_gen << kTlbGenShift) |
         (uint32_t)(write != 0);
}
static x86_tlb_entry* TlbSlot(uint32_t vpn, int write) {
  return &s->tlb[TlbSet(vpn) * kTlbWays + (write ? 1 : 0)];
}
static x86_tlb_entry* TlbLookup(uint64_t lin, int write) {
  uint32_t vpn = (uint32_t)(lin >> 12);
  x86_tlb_entry* e = TlbSlot(vpn, write);
  return e->tag == TlbTag(vpn, write) ? e : NULL;
}

static void TlbCheck(x86_tlb_entry* e, uint64_t lin, int write, int fetch, int user) {
  uint32_t code = (write ? kPfWrite : 0) | (user ? kPfUser : 0);
  if (user && !(e->flags & kTlbUser)) pf_fault(lin, code | kPfPresent);
  if (write && (user ? !(e->flags & kTlbWritable)
                     : ((s->cr0 & kCr0Wp) && !(e->flags & kTlbWritable))))
    pf_fault(lin, code | kPfPresent);
  if (fetch && (e->flags & kTlbNx) && (s->efer & kEferNxe))
    pf_fault(lin, code | kPfPresent | kPfInstr);
}

// Walk once, set A/D once, and cache the 4KB translation (huge leaves are
// cached at 4KB granularity; the walk's page-size choice is not observable).
static x86_tlb_entry* TlbFill(uint64_t lin, int write, int fetch, int user) {
  walk_res w;
  WalkPage(lin, write, user, fetch, &w);
  uint32_t vpn = (uint32_t)(lin >> 12);
  x86_tlb_entry* e = TlbSlot(vpn, write);
  e->tag = TlbTag(vpn, write);
  e->flags = w.flags;
  e->lin_base = w.lin_base;
  e->page_shift = w.page_shift;
  e->pbase = w.paddr & ~(uint64_t)0xfff;
  e->host = NULL;
  uint8_t* host;
  if (!g_mem_slow && BusRamPage(cpu->bus, e->pbase, &host) == 0) {
    e->host = host;
    e->flags |= kTlbHost;
  }
  return e;
}

static void TlbFlushAll(x86_state* st) {
  // Bump the generation instead of touching 2^13 entries (CR3 is written on
  // every context switch). A wrapped generation has to clear once so old
  // tags cannot alias.
  st->tlb_gen = (st->tlb_gen + 1) & kTlbGenMask;
  if (st->tlb_gen == 0) {
    memset(st->tlb, 0, sizeof(st->tlb));
    st->tlb_gen = 1;
  }
  st->fetch_valid = 0;
}

// A CR3 reload keeps only global translations when CR4.PGE=1; without PGE
// every entry goes (SDM vol.3 4.10.4.1).
static void TlbFlushNonGlobal(x86_state* st) {
  if (!(st->cr4 & kCr4Pge)) {
    TlbFlushAll(st);
    return;
  }
  for (int i = 0; i < (int)(kTlbSets * kTlbWays); i++)
    if (!(st->tlb[i].flags & kTlbGlobal)) st->tlb[i].tag = 0;
  st->fetch_valid = 0;
}

// INVLPG invalidates the page regardless of G. A huge leaf was cached as
// several 4KB slots, so the match is against the entry's linear range: every
// slot derived from the same 2MB/4MB leaf goes. The fetch shortcut is dropped
// conservatively.
static void TlbFlushPage(x86_state* st, uint32_t lin) {
  for (int i = 0; i < (int)(kTlbSets * kTlbWays); i++) {
    x86_tlb_entry* e = &st->tlb[i];
    if (!(e->tag & kTlbValid)) continue;
    if ((lin >> e->page_shift) == (e->lin_base >> e->page_shift)) e->tag = 0;
  }
  st->fetch_valid = 0;
}

// ---- linear access ------------------------------------------------------------

static uint64_t FastLoadOne(uint64_t lin, int size, int user, int fetch) {
  x86_tlb_entry* e = TlbLookup(lin, 0);
  if (!e) {
    e = TlbFill(lin, 0, fetch, user);
  } else {
    TlbCheck(e, lin, 0, fetch, user);
  }
  uint32_t off = (uint32_t)lin & 0xfff;
  if (e->flags & kTlbHost) {
    uint64_t v = 0;
    memcpy(&v, e->host + off, (size_t)size);
    return v;
  }
  return phys_load(e->pbase + off, size, fetch);
}

static void FastStoreOne(uint64_t lin, int size, int user, uint64_t v) {
  x86_tlb_entry* e = TlbLookup(lin, 1);
  if (!e) {
    e = TlbFill(lin, 1, 0, user);
  } else {
    TlbCheck(e, lin, 1, 0, user);
  }
  uint32_t off = (uint32_t)lin & 0xfff;
  if (e->flags & kTlbHost) {
    memcpy(e->host + off, &v, (size_t)size);
  } else {
    phys_store(e->pbase + off, size, v);
  }
}

static uint64_t SlowLoadOne(uint64_t lin, int size, int user, int fetch) {
  return phys_load(page_translate_as(lin, 0, user, fetch), size, fetch);
}

static void SlowStoreOne(uint64_t lin, int size, int user, uint64_t v) {
  phys_store(page_translate_as(lin, 1, user, 0), size, v);
}

static uint64_t lin_load(uint64_t lin, int size, int user, int fetch) {
  AcCheckSlow(lin, size, user, fetch, 0);
  uint32_t off = (uint32_t)lin & 0xfff;
  if (g_mem_slow) {
    if (off + (uint32_t)size <= 0x1000) return SlowLoadOne(lin, size, user, fetch);
    int n1 = (int)(0x1000 - off);
    uint64_t lo = SlowLoadOne(lin, n1, user, fetch);
    uint64_t hi = SlowLoadOne(lin + (uint64_t)n1, size - n1, user, fetch);
    return lo | (hi << (n1 * 8));
  }
  if (!(s->cr0 & kCr0Pg)) {
    if (off + (uint32_t)size <= 0x1000) return phys_load(lin, size, fetch);
    int n1 = (int)(0x1000 - off);
    uint64_t lo = phys_load(lin, n1, fetch);
    uint64_t hi = phys_load(lin + (uint64_t)n1, size - n1, fetch);
    return lo | (hi << (n1 * 8));
  }
  if (off + (uint32_t)size <= 0x1000) return FastLoadOne(lin, size, user, fetch);
  int n1 = (int)(0x1000 - off);
  uint64_t lo = FastLoadOne(lin, n1, user, fetch);
  uint64_t hi = FastLoadOne(lin + (uint64_t)n1, size - n1, user, fetch);
  return lo | (hi << (n1 * 8));
}

static void lin_store(uint64_t lin, int size, int user, uint64_t v) {
  AcCheckSlow(lin, size, user, 0, 1);
  uint32_t off = (uint32_t)lin & 0xfff;
  int crossing = off + (uint32_t)size > 0x1000;
  int n1 = crossing ? (int)(0x1000 - off) : 0;
  uint64_t lo = crossing ? (v & ((1ull << (n1 * 8)) - 1)) : v;
  uint64_t hi = crossing ? (v >> (n1 * 8)) : 0;
  if (g_mem_slow) {
    if (!crossing) {
      SlowStoreOne(lin, size, user, v);
    } else {
      SlowStoreOne(lin, n1, user, lo);
      SlowStoreOne(lin + (uint64_t)n1, size - n1, user, hi);
    }
    return;
  }
  if (!(s->cr0 & kCr0Pg)) {
    if (!crossing) {
      phys_store(lin, size, v);
    } else {
      phys_store(lin, n1, lo);
      phys_store(lin + (uint64_t)n1, size - n1, hi);
    }
    return;
  }
  if (!crossing) {
    FastStoreOne(lin, size, user, v);
  } else {
    FastStoreOne(lin, n1, user, lo);
    FastStoreOne(lin + (uint64_t)n1, size - n1, user, hi);
  }
}

// ---- time stamp counter (SDM vol.3 17.17) ------------------------------------

// The TSC is emulated time in nanoseconds plus the retired-instruction count:
// both terms are monotonic, -icount makes the clock exactly proportional to
// retired instructions, and a WRMSR to IA32_TSC parks the written value as an
// offset so the next read continues from it. 1 GHz is the nominal rate.
static uint64_t TscNow(void) {
  return s->tsc_offset + (uint64_t)HostTimerNow() * 1000u + cpu->inst_count;
}

// Alignment check (SDM vol.3 4.5, "CHECKING ALIGNMENT"): with CR0.AM=1,
// EFLAGS.AC=1 and CPL=3, a 2/4/8-byte data access at an address that is not a
// multiple of its size raises #AC. The page walk runs FIRST so a not-present
// or permission fault keeps its architectural priority; the walk only happens
// for the rare unaligned user case.
static void AcCheckSlow(uint64_t lin, int size, int user, int fetch, int write) {
  // CR0.AM is clear on every guest that is not running an AC test; test it
  // before the load-side conditions so the hot path is one branch.
  if (!(s->cr0 & kCr0Am) || fetch || !user || !fl->ac) return;
  if (size != 2 && size != 4 && size != 8) return;
  if ((lin & (uint64_t)(size - 1)) == 0) return;
  if (s->cr0 & kCr0Pg) {
    walk_res w;
    WalkPage(lin, write, user, fetch, &w);
  }
  raise_(fr, vec_ac, 0);
}

// ---- MSRs -------------------------------------------------------------------

// The bank this machine implements. Unknown indices #GP (SDM WRMSR/RDMSR
// "#GP(0) if the MSR is not implemented").
static uint64_t MsrRead(uint32_t idx) {
  if (idx == 0x10) return TscNow();
  if (idx == 0x1b) return s->msr_apic_base;
  if (idx == 0xfe) return 0x501;  // MTRRCAP: 8 variable ranges, WC, no FIX
  if (idx == 0x179) return 0x101;  // MCG_CAP: one bank, MCG_CTL present
  if (idx == 0x17a) return s->mcg_status;
  if (idx == 0x17b) return s->mcg_ctl;
  if (idx == 0x174) return s->sysenter_cs;
  if (idx == 0x175) return s->sysenter_esp;
  if (idx == 0x176) return s->sysenter_eip;
  if (idx == 0x186 || idx == 0x187) return s->perf_evtsel[idx - 0x186];
  if (idx == 0xc1 || idx == 0xc2) return s->perf_pmc[idx - 0xc1];
  if (idx >= 0x200 && idx <= 0x20f)
    return (idx & 1) ? s->mtrr_physmask[(idx - 0x200) >> 1]
                     : s->mtrr_physbase[(idx - 0x200) >> 1];
  if (idx == 0x277) return s->pat;
  if (idx == 0x2ff) return s->mtrr_def_type;
  if (idx >= 0x309 && idx <= 0x30b) return s->perf_fixed_ctr[idx - 0x309];
  if (idx == 0x345) return 0;  // IA32_PERF_CAPABILITIES: no LBR/PEBS
  if (idx == 0x38d) return s->perf_fixed_ctrl;
  if (idx == 0x38e) return s->perf_global_status;
  if (idx == 0x38f) return s->perf_global_ctrl;
  if (idx == 0x400) return s->mc0_ctl;
  if (idx == 0x401) return s->mc0_status;
  if (idx == 0x402) return s->mc0_addr;
  if (idx == 0x403) return s->mc0_misc;
  if (idx == 0xc0000080) return s->efer;
  if (idx == 0xc0000100 || idx == 0xc0000101) return s->msr_fs_gs_base[idx - 0xc0000100];
  if (idx == 0xc0000103) return s->tsc_aux;
  if (idx == 0x1a0) return 0x1801;  // IA32_MISC_ENABLE: fast strings on
  gp_fault(0);
}

static void MsrWrite(uint32_t idx, uint64_t v) {
  if (idx == 0x10) {
    s->tsc_offset = v - ((uint64_t)HostTimerNow() * 1000u + cpu->inst_count);
  } else if (idx == 0x1b) {
    s->msr_apic_base = v;
  } else if (idx == 0x174) {
    s->sysenter_cs = (uint32_t)v;
  } else if (idx == 0x175) {
    s->sysenter_esp = (uint32_t)v;
  } else if (idx == 0x176) {
    s->sysenter_eip = (uint32_t)v;
  } else if (idx == 0x17a) {
    s->mcg_status = v & 7;  // RIPV/EIPV/MCIP
  } else if (idx == 0x17b) {
    s->mcg_ctl = v;
  } else if (idx == 0x186 || idx == 0x187) {
    s->perf_evtsel[idx - 0x186] = (uint32_t)v;
    PerfUpdateActive();
  } else if (idx == 0xc1 || idx == 0xc2) {
    s->perf_pmc[idx - 0xc1] = v;
  } else if (idx >= 0x200 && idx <= 0x20f) {
    int i = (int)((idx - 0x200) >> 1);
    if (idx & 1)
      s->mtrr_physmask[i] = (v & 0xfffffffffffff000ULL) | (v & 0x800u);
    else
      s->mtrr_physbase[i] = (v & 0xfffffffffffff000ULL) | (v & 0xffu);
  } else if (idx == 0x277) {
    s->pat = v;
  } else if (idx == 0x2ff) {
    s->mtrr_def_type = v & 0xc07u;
  } else if (idx == 0x38d) {
    s->perf_fixed_ctrl = v & 0x7ffu;
    PerfUpdateActive();
  } else if (idx == 0x38f) {
    s->perf_global_ctrl = v & 0x700000003ULL;
    PerfUpdateActive();
  } else if (idx == 0x390) {
    // IA32_PERF_GLOBAL_OVF_CTRL: write-1-to-clear the status bits.
    s->perf_global_status &= ~v;
  } else if (idx >= 0x400 && idx <= 0x403) {
    // Bank 0's registers are storage: no error source raises #MC. MC0_STATUS
    // is written by software to clear its bits (SDM vol.3 15.3.2), so a write
    // clears rather than stores.
    if (idx == 0x400) s->mc0_ctl = v;
    else if (idx == 0x401) s->mc0_status &= ~v;
    else if (idx == 0x402) s->mc0_addr = v;
    else s->mc0_misc = v;
  } else if (idx == 0xc0000080) {
    s->efer = v;
    TlbFlushAll(s);
  } else if (idx == 0xc0000100 || idx == 0xc0000101) {
    s->msr_fs_gs_base[idx - 0xc0000100] = v;
  } else if (idx == 0xc0000103) {
    s->tsc_aux = (uint32_t)v;
  } else {
    gp_fault(0);
  }
}

// Rebuild the one-word enable mask the step commit tests. General counter i
// needs IA32_PERF_GLOBAL_CTRL[i] and IA32_PERFEVTSEL[i].EN; fixed counter i
// needs GLOBAL_CTRL[32+i] and a nonzero FIXED_CTR_CTRL[i] mode (OS/USR/any).
static void PerfUpdateActive(void) {
  uint32_t m = 0;
  for (int i = 0; i < 2; i++)
    if ((s->perf_global_ctrl & (1ull << i)) && (s->perf_evtsel[i] & (1u << 22)))
      m |= 1u << i;
  for (int i = 0; i < 3; i++)
    if ((s->perf_global_ctrl & (1ull << (32 + i))) &&
        ((s->perf_fixed_ctrl >> (2 * i)) & 7))
      m |= 1u << (2 + i);
  s->perf_active = m;
}

// Retired-instruction accounting for the architectural performance counters
// (SDM vol.3 ch.19). Event 0xC0 is instructions retired and 0x3C unhalted
// core cycles; on this single-threaded interpreter both advance once per
// retired instruction. USR/OS select the privilege levels, EN arms the
// counter, and the any-thread bit counts in both rings.
void x86_count_perf(void) {
  if (!s->perf_active) return;
  int ring = cpl();
  for (int i = 0; i < 2; i++) {
    if (!(s->perf_active & (1u << i))) continue;
    uint64_t sel = s->perf_evtsel[i];
    if (!((sel & (1u << 16)) && ring == 0) && !((sel & (1u << 17)) && ring != 0))
      continue;
    uint32_t ev = (uint32_t)sel & 0xff;
    if (ev == 0xc0 || ev == 0x3c) {
      if (++s->perf_pmc[i] == 0) s->perf_global_status |= 1ull << i;
    }
  }
  for (int i = 0; i < 3; i++) {
    if (!(s->perf_active & (1u << (2 + i)))) continue;
    int mode = (int)((s->perf_fixed_ctrl >> (2 * i)) & 7);
    int on = (mode & 1 && ring == 0) || (mode & 2 && ring != 0) || (mode & 4);
    if (on) {
      if (++s->perf_fixed_ctr[i] == 0) s->perf_global_status |= 1ull << (32 + i);
    }
  }
}

// DS/ES/FS/GS (SDM vol.2 MOV to Sreg Operation): a null selector loads and
// marks the segment unusable; otherwise the descriptor must be data or
// readable code, DPL >= CPL and DPL >= RPL, and present — faults in that
// order.
static void load_data(int seg, uint16_t sel) {
  seg_view v;
  if (!(s->cr0 & 1) || fl->vm) {  // real mode and VM86 load unchecked (SDM 17.3.1)
    seg_synth(sel, &v);
    seg_commit(seg, sel, &v);
    return;
  }
  if ((sel & 0xfffc) == 0) {  // null: legal for data, unusable until reloaded
    seg_view z = {0, 0, 0, 0, 0};
    seg_commit(seg, sel, &z);
    return;
  }
  if ((uint32_t)(sel >> 3) * 8 + 7 > table_limit(sel)) gp_fault(sel & ~3u);
  desc_parse(sel, &v);
  if (!(v.ar & 0x10) || ((v.ar & 0x08) && !(v.ar & 2)))
    gp_fault(sel & ~3u);  // system and execute-only descriptors don't load
  if (((v.ar >> 5) & 3) < cpl() || ((v.ar >> 5) & 3) < (sel & 3))
    gp_fault(sel & ~3u);
  if (!(v.ar & 0x80)) np_fault(sel & ~3u);
  seg_commit(seg, sel, &v);
}

// SS (SDM vol.2 MOV to Sreg / POP SS): writable data only, exactly at CPL
// (DPL == CPL and RPL == CPL). Table-limit, type and present faults are
// #SS; the privilege faults are #GP.
static void load_ss(uint16_t sel) {
  seg_view v;
  if (!(s->cr0 & 1) || fl->vm) {  // real mode and VM86 load unchecked (SDM 17.3.1)
    seg_synth(sel, &v);
    seg_commit(ss_i, sel, &v);
    return;
  }
  if ((sel & 0xfffc) == 0) gp_fault(0);
  if ((uint32_t)(sel >> 3) * 8 + 7 > table_limit(sel)) ss_fault(sel & ~3u);
  desc_parse(sel, &v);
  if ((v.ar & 0x1a) != 0x12) ss_fault(sel & ~3u);  // writable data: S=1 E=0 W=1
  if (((v.ar >> 5) & 3) != cpl() || (sel & 3) != cpl()) gp_fault(sel & ~3u);
  if (!(v.ar & 0x80)) ss_fault(sel & ~3u);
  seg_commit(ss_i, sel, &v);
}

// CS on a direct far JMP/CALL/RET/IRET (SDM vol.2 LJMP Operation): a code
// segment, conforming (DPL <= CPL) or not (DPL == CPL, RPL <= CPL). CPL
// follows a non-conforming load and is kept by a conforming one. Gate and
// TSS descriptors route through pm_far, which owns the system-descriptor
// forms.
static void load_cs(uint16_t sel) {
  seg_view v;
  if (!(s->cr0 & 1) || fl->vm) {  // real mode and VM86 load unchecked (SDM 17.3.1)
    seg_synth(sel, &v);
    seg_commit(cs_i, sel, &v);
    return;
  }
  if ((sel & 0xfffc) == 0) gp_fault(0);
  if ((uint32_t)(sel >> 3) * 8 + 7 > table_limit(sel)) gp_fault(sel & ~3u);
  desc_parse(sel, &v);
  if ((v.ar & 0x18) != 0x18) gp_fault(sel & ~3u);  // code: S=1, E=1
  int conf = v.ar & 4;
  if (conf ? ((v.ar >> 5) & 3) > cpl()
           : ((v.ar >> 5) & 3) != cpl() || (sel & 3) > cpl())
    gp_fault(sel & ~3u);
  if (!(v.ar & 0x80)) np_fault(sel & ~3u);
  if (conf) v.ar = (uint8_t)((v.ar & ~0x60u) | (cpl() << 5));
  seg_commit(cs_i, sel, &v);
}

// Vectors whose delivery pushes an error code (SDM vol.3 table 6-1, error
// code column); real-mode delivery pushes none.
static int vec_has_ec(int vec) {
  return vec == vec_df || vec == vec_ts || vec == vec_np || vec == vec_ss ||
         vec == vec_gp || vec == vec_pf || vec == vec_ac;
}

// The return-side checks shared by IRET and far RET (SDM vol.2 both). The
// code segment: null → GP(0); table limits, code type, non-conforming
// DPL == RPL (conforming DPL <= RPL) → GP(sel); present → NP(sel). CPL
// becomes the return RPL.
static void ret_cs_check(uint16_t sel, int newpl, seg_view* cv) {
  if ((sel & 0xfffc) == 0) gp_fault(0);
  if ((uint32_t)(sel >> 3) * 8 + 7 > table_limit(sel)) gp_fault(sel & ~3u);
  desc_parse(sel, cv);
  if ((cv->ar & 0x18) != 0x18) gp_fault(sel & ~3u);  // executable
  int conf = cv->ar & 4;
  if (conf ? (((cv->ar >> 5) & 3) > newpl) : (((cv->ar >> 5) & 3) != newpl))
    gp_fault(sel & ~3u);
  if (!(cv->ar & 0x80)) np_fault(sel & ~3u);
}

// The outward return's fresh stack: SS RPL == CS RPL, writable data,
// DPL == CS RPL — all #GP, not-present #SS (SDM vol.2 IRET/RET).
static void ret_ss_check(uint16_t new_ss, int newpl, seg_view* sv) {
  if ((new_ss & 3) != newpl) gp_fault(new_ss & ~3u);
  if ((new_ss & 0xfffc) == 0) gp_fault(0);
  if ((uint32_t)(new_ss >> 3) * 8 + 7 > table_limit(new_ss)) gp_fault(new_ss & ~3u);
  desc_parse(new_ss, sv);
  if ((sv->ar & 0x1a) != 0x12) gp_fault(new_ss & ~3u);  // writable data
  if (((sv->ar >> 5) & 3) != newpl) gp_fault(new_ss & ~3u);
  if (!(sv->ar & 0x80)) ss_fault(new_ss & ~3u);
}

// The TSS ring stack for a privilege drop (SDM vol.2 INT Operation, vol.3
// 7.2.1): SSn:ESPn at fixed offsets — 4+8n/8+8n in a 32-bit TSS, 2+4n/4+4n
// in a 286 one. Every defect faults #TS. Validates without committing; the
// callers commit after their own pre-commit reads so a faulting entry leaves
// the caller's stack untouched.
static void tss_stack(int newpl, uint16_t* ss0, uint32_t* esp0, seg_view* sv) {
  uint8_t ty = s->tr_ar & 0xf;
  if (!(s->tr_ar & 0x80) || (ty != 1 && ty != 3 && ty != 9 && ty != 0xb))
    ts_fault(0);  // TR holds no TSS
  int tss32 = ty & 8;
  *esp0 = tss32 ? krd32(s->tr_base + 4 + 8 * newpl) : krd16(s->tr_base + 2 + 4 * newpl);
  *ss0 = tss32 ? krd16(s->tr_base + 8 + 8 * newpl) : krd16(s->tr_base + 4 + 4 * newpl);
  if ((*ss0 & 0xfffc) == 0) ts_fault(0);
  if ((uint32_t)(*ss0 >> 3) * 8 + 7 > table_limit(*ss0)) ts_fault(*ss0 & ~3u);
  desc_parse(*ss0, sv);
  if ((sv->ar & 0x1a) != 0x12) ts_fault(*ss0 & ~3u);  // writable data
  if (((sv->ar >> 5) & 3) != newpl || (*ss0 & 3) != newpl) ts_fault(*ss0 & ~3u);
  if (!(sv->ar & 0x80)) ts_fault(*ss0 & ~3u);
}

// The LDTR load shared by LLDT and the task switch (SDM vol.2 LLDT,
// vol.3 7.2.1): a null selector clears the cache (no LDT); otherwise the
// selector must name a present LDT descriptor in the GDT — LDT descriptors
// never live in an LDT, so TI=1 faults outright. `task_switch` picks #TS
// for the structural faults (SDM 7.2.1) where the instruction uses #GP.
static void load_ldtr(uint16_t sel, int task_switch) {
  if ((sel & 0xfffc) == 0) {  // null: the task runs without an LDT
    s->ldtr = sel;
    s->ldtr_base = 0;
    s->ldtr_limit = 0;
    return;
  }
  if (sel & 4) {
    if (task_switch)
      ts_fault(sel & ~3u);
    else
      gp_fault(sel & ~3u);
  }
  if ((uint32_t)(sel >> 3) * 8 + 7 > s->gdtr_limit) {
    if (task_switch)
      ts_fault(sel & ~3u);
    else
      gp_fault(sel & ~3u);
  }
  seg_view v;
  desc_parse(sel, &v);
  if ((v.ar & 0x1f) != 0x02) {  // system, type 2 (SDM vol.3 table 3-2)
    if (task_switch)
      ts_fault(sel & ~3u);
    else
      gp_fault(sel & ~3u);
  }
  if (!(v.ar & 0x80)) np_fault(sel & ~3u);
  s->ldtr = sel;
  s->ldtr_base = v.base;
  s->ldtr_limit = v.limit;
}

// Task switching (SDM vol.3 7.2.1, cross-checked against v86 do_task_switch
// and tiny386 task_switch). `source` distinguishes the three callers: JMP
// clears the outgoing descriptor's busy bit, CALL/INT additionally writes
// the back-link and forces NT in the new image, IRET requires a busy target
// and clears NT in the saved image. The LDTR field is not written back to
// the outgoing TSS (tiny386 and v86 agree; the incoming one is loaded below
// so the new task's TI=1 selectors resolve through its own LDT).
enum { kTaskJmp, kTaskCall, kTaskIret };
static void do_task_switch(uint16_t sel, int source, uint32_t next_eip, int has_ec,
                           uint32_t ec) {
  if ((sel & 0xfffc) == 0) gp_fault(0);
  if (sel & 4) gp_fault(sel & ~3u);  // TSS descriptors are GDT-only (SDM 7.2)
  if ((uint32_t)(sel >> 3) * 8 + 7 > s->gdtr_limit) gp_fault(sel & ~3u);
  seg_view v;
  desc_parse(sel, &v);
  uint8_t ty = v.ar & 0xf;
  if ((v.ar & 0x10) || (ty != 1 && ty != 3 && ty != 9 && ty != 0xb)) gp_fault(sel & ~3u);
  if (source == kTaskIret && !(ty & 2)) ts_fault(sel & ~3u);  // must be busy
  if (source != kTaskIret && (ty & 2)) gp_fault(sel & ~3u);   // must be available
  if (!(v.ar & 0x80)) np_fault(sel & ~3u);
  // The two TSS shapes carry the same state at different offsets and widths
  // (SDM vol.3 fig 7-2 for the 16-bit one, fig 7-4 for the 32-bit one; the
  // offsets below are QEMU switch_tss_ra's). A 16-bit TSS ends at the LDT
  // selector — no CR3, no FS/GS and no debug-trap word — hence its smaller
  // minimum limit (SDM 7.2.1: 0x2b against 0x67).
  int tss16 = !(ty & 8);
  if (v.limit < (tss16 ? 0x2b : 0x67)) np_fault(sel & ~3u);

  // Save the outgoing task (SDM fig 7-2/7-4 dynamic fields): EIP, EFLAGS, the
  // GPRs and the segment selectors, at the width the outgoing TSS uses. The
  // saved EFLAGS drops NT on an IRET switch — the outgoing task is being
  // retired, not suspended (tiny386 TS_IRET, v86's busy-target rule). The TSS
  // is the processor's own structure, so every access below is a supervisor
  // access.
  // The save below writes into the OUTGOING TSS, whose shape is the one the
  // current TR names — not the incoming one `v`. Mixing them writes a 32-bit
  // TSS with 16-bit offsets (clobbering its CR3 field, which sits inside the
  // 16-bit register block's stride) or the other way round.
  int out16 = !(s->tr_ar & 0x8);
  uint32_t saved_fl = source == kTaskIret ? fl->word & ~0x4000u : fl->word;
  if (out16) {
    kwr16(s->tr_base + 0x0e, (uint16_t)next_eip);
    kwr16(s->tr_base + 0x10, (uint16_t)saved_fl);
    for (int i = 0; i < 8; i++) kwr16(s->tr_base + 0x12 + 2 * i, (uint16_t)s->r[i].e);
  } else {
    kwr32(s->tr_base + 0x20, next_eip);
    kwr32(s->tr_base + 0x24, saved_fl);
    for (int i = 0; i < 8; i++) kwr32(s->tr_base + 0x28 + 4 * i, s->r[i].e);
  }
  static const int seg_order[6] = {es_i, cs_i, ss_i, ds_i, fs_i, gs_i};
  // The selectors sit on a stride of their own in each shape (SDM fig 7-4:
  // ES 0x48, CS 0x4c, SS 0x50, DS 0x54, FS 0x58, GS 0x5c, each field two
  // bytes wide with two reserved bytes after it; fig 7-2: four fields at 0x22
  // step 2). Both shapes store them as 16-bit selectors.
  int nseg = out16 ? 4 : 6;
  for (int i = 0; i < nseg; i++)
    kwr16(s->tr_base + (out16 ? 0x22 + 2 * i : 0x48 + 4 * i), s->sreg[seg_order[i]]);

  // Busy bits (SDM 7.2.3): JMP/IRET clear the outgoing descriptor's,
  // JMP/CALL/INT set the incoming one's; IRET leaves the target busy (it
  // never stopped being the suspended task).
  if (source != kTaskCall)
    kbus_store(s->gdtr + (uint64_t)(s->tr >> 3) * 8 + 5, 1, (uint8_t)(s->tr_ar & ~2u));
  if (source != kTaskIret)
    kbus_store(s->gdtr + (uint64_t)(sel >> 3) * 8 + 5, 1, (uint8_t)(v.ar | 2));
  // The incoming task's T flag (SDM vol.3 7.2.1, fig 7-4 byte 0 bit 0): a
  // task-switch debug trap fires before the new task's first instruction. A
  // 16-bit TSS has no such word (QEMU switch_tss_ra reads it only when
  // type & 8), so the switch never traps in that shape.
  // Read before the back-link write below lands on the same word.
  s->bt_pending = tss16 ? 0 : (krd8(v.base) & 1);
  if (source == kTaskCall) kwr16(v.base, s->tr);  // the back-link (fig 7-2/7-4)

  // TR commits before the new state loads: a faulting load leaves the
  // machine in the new task's context (tiny386 follows the SDM order; v86
  // defers and flags its own XXX).
  s->tr = sel;
  s->tr_base = v.base;
  s->tr_limit = v.limit;
  s->tr_ar = (uint8_t)(v.ar | 2);

  // A 16-bit TSS has no CR3 field, so the switch leaves the page directory
  // alone (QEMU applies CR3 only when type & 8). Loading CR3 flushes
  // non-global translations, exactly like MOV CR3.
  if (!tss16) {
    s->cr3 = krd32(v.base + 0x1c);  // carried along (SDM 7.2.1)
    TlbFlushNonGlobal(s);
  }
  // The incoming LDT (TSS +0x60, or +0x2a in the 16-bit shape) loads BEFORE
  // the segment selectors: their TI=1 lookups go through the new task's own
  // LDT (SDM 7.2.1 step order). It is not written back to the outgoing TSS
  // (tiny386 and v86 agree).
  load_ldtr(krd16(v.base + (tss16 ? 0x2a : 0x60)), 1);

  // The TSS image supplies EFLAGS (SDM vol.3 7.2.1): the arithmetic and system
  // flags, IOPL and NT — and VM, which is how a task switch enters virtual-8086
  // mode. A 16-bit TSS carries only the low half of the register (QEMU masks
  // the image to 16 bits), so VM cannot be set through that shape. The
  // incoming CS and segment selectors are then real-mode values with
  // no descriptor behind them, so they load through seg_synth below. RF is
  // cleared by the switch.
  uint32_t nf = tss16 ? krd16(v.base + 0x10) : krd32(v.base + 0x24);
  d.rf_load = 1;
  fl->word = (fl->word & ~(0x7fd7u | 0x30000u)) | (nf & (0x7fd7u | 0x20000u)) | 2;
  if (source == kTaskCall) fl->nt = 1;

  // The incoming CS: every defect faults #TS, not-present #NP (v86
  // do_task_switch; this is the one segment load the switch treats
  // specially). The new CPL is the CS RPL. A VM86 task's CS is a real-mode
  // selector: it is synthesized, never validated (SDM vol.3 17.3.1).
  uint16_t new_cs = krd16(v.base + (tss16 ? 0x24 : 0x4c));
  seg_view cv;
  if (fl->vm) {
    seg_synth(new_cs, &cv);
  } else {
    if ((new_cs & 0xfffc) == 0) ts_fault(0);
    if ((uint32_t)(new_cs >> 3) * 8 + 7 > table_limit(new_cs)) ts_fault(new_cs & ~3u);
    desc_parse(new_cs, &cv);
    if ((cv.ar & 0x18) != 0x18) ts_fault(new_cs & ~3u);  // executable
    int conf = cv.ar & 4;
    int nrpl = new_cs & 3;
    if (conf ? (((cv.ar >> 5) & 3) > nrpl) : (((cv.ar >> 5) & 3) != nrpl))
      ts_fault(new_cs & ~3u);
    if (!(cv.ar & 0x80)) np_fault(new_cs & ~3u);
    if (conf) cv.ar = (uint8_t)((cv.ar & ~0x60u) | (nrpl << 5));
  }
  seg_commit(cs_i, new_cs, &cv);

  // A 16-bit TSS image holds only the low half of each register: the high half
  // survives the switch (QEMU switch_tss_ra loads 16 bits and keeps the rest).
  // Its four selectors are ES/CS/SS/DS; FS and GS have no field there and load
  // as null, which leaves them unusable (SDM fig 7-2).
  if (tss16) {
    for (int i = 0; i < 8; i++)
      s->r[i].e = (s->r[i].e & 0xffff0000u) | krd16(v.base + 0x12 + 2 * i);
  } else {
    for (int i = 0; i < 8; i++) s->r[i].e = krd32(v.base + 0x28 + 4 * i);
  }
  load_data(es_i, krd16(v.base + (tss16 ? 0x22 : 0x48)));
  load_ss(krd16(v.base + (tss16 ? 0x26 : 0x50)));
  load_data(ds_i, krd16(v.base + (tss16 ? 0x28 : 0x54)));
  load_data(fs_i, tss16 ? 0 : krd16(v.base + 0x58));
  load_data(gs_i, tss16 ? 0 : krd16(v.base + 0x5c));

  s->cr0 |= 0x8;  // CR0.TS: the FPU state is per-task (SDM 7.2.1)
  if (has_ec) kpush32(ec);  // an exception's error code lands on the new stack
  eip = tss16 ? krd16(v.base + 0x0e) : krd32(v.base + 0x20);
}

// The registers a BIOS/DOS service reads as its arguments, handed to the hub in
// its plain-integer form (debug.h DbgCallArgs). Emitted on both halves of a
// software INT n: at the request these are the guest's parameters, at the IRET
// they are whatever the handler left behind, which is the answer.
static void GateArgs(DbgCallArgs* o) {
  o->arg_ax = eax;
  o->arg_bx = ebx;
  o->arg_cx = ecx;
  o->arg_dx = edx;
  o->arg_ds = s->sreg[ds_i];
  o->arg_es = s->sreg[es_i];
}

// Protected-mode IRET (SDM vol.2 IRET Operation). Four shapes: a return
// within virtual-8086 mode (VM86 stays VM86 and pops only EIP/CS/EFLAGS), a
// nested-task return (NT=1: the current TSS's back-link names the interrupted
// task and no pops happen), a return *to* virtual-8086 mode (the EFLAGS image
// has VM=1 and CPL=0), and the protected-mode return — a same-privilege return
// pops EIP/CS/EFLAGS, an outward return (CS RPL > CPL) additionally pops
// ESP/SS, and only outward or CPL-0 returns may reload IOPL/NT.
static void pm_iret(void) {
  int w = d.w32 ? 4 : 2;

  // RETURN-FROM-VIRTUAL-8086-MODE: the processor is in VM86 and stays there, so
  // only EIP, CS and EFLAGS come off the stack. IOPL and VM never load, and the
  // instruction needs IOPL=3 — otherwise it traps to the virtual-8086 monitor
  // with #GP(0). The new CS is a VM86 segment, checked only against the 64K
  // limit its own base implies.
  if (fl->vm) {
    // VME lets a VM86 task IRET without IOPL=3; the image's IF bit then
    // restores VIF and bit 20 restores VIP (SDM vol.3 20.3.5). Without VME
    // the monitor's IOPL must be 3.
    int vme = (s->cr4 & kCr4Vme) != 0;
    if (!vme && fl->iopl != 3) gp_fault(0);
    uint32_t new_eip = stack_peek(w, 0);
    uint16_t sel = (uint16_t)stack_peek(w, w);
    uint32_t flv = stack_peek(w, 2 * (uint32_t)w);
    if (!d.w32) flv &= 0xffff;
    seg_view cv;
    seg_synth(sel, &cv);
    if (new_eip > cv.limit) gp_fault(0);
    seg_commit(cs_i, sel, &cv);
    // CF..OF, TF/IF/DF, NT and RF load; IOPL and VM keep their live values.
    uint32_t mask = d.w32 ? 0x14fd7u : 0x4fd7u;
    d.rf_load = 1;
    fl->word = (fl->word & ~mask) | (flv & mask) | 2;
    if (vme) {
      fl->vif = (flv & 0x200u) != 0;
      fl->vip = (flv & 0x100000u) != 0;
      fl->if_ = 1;
    }
    esp = s->dbit[ss_i] ? esp + (uint32_t)w * 3
                        : (esp & 0xffff0000u) | (uint16_t)(esp + (uint32_t)w * 3);
    eip = new_eip;
    return;
  }

  if (fl->nt) {
    uint8_t ty = s->tr_ar & 0xf;
    if (!(s->tr_ar & 0x80) || (ty != 1 && ty != 3 && ty != 9 && ty != 0xb))
      ts_fault(0);  // TR holds no TSS to read the link from
  uint16_t back = krd16(s->tr_base + 0);
    if ((back & 0xfffc) == 0) ts_fault(0);
    do_task_switch(back, kTaskIret, (uint32_t)(fr->rec.pc + d.nxt), 0, 0);
    return;
  }

  // The frame is peeked and checked before any pop commits — a faulting
  // return resumes at the IRET with the caller's stack intact.
  uint32_t new_eip = stack_peek(w, 0);
  uint16_t sel = (uint16_t)stack_peek(w, w);
  uint32_t flv = stack_peek(w, 2 * (uint32_t)w);
  if (!d.w32) flv &= 0xffff;
  // CF before the flags image below replaces it: this is the handler's verdict,
  // which is where every BIOS/DOS service reports failure (debug.h
  // DebugGateCall). IRET touches no general-purpose register, so AX still holds
  // the answer the caller is about to branch on.
  int cf = fl->cf;

  // RETURN-TO-VIRTUAL-8086-MODE: the interrupted procedure was a VM86 one and
  // this is a CPL-0 return, so the frame is the VM86 one a delivery pushed —
  // ESP, SS, ES, DS, FS and GS above the flags image — and the whole EFLAGS
  // image loads (VM=1 among it, which is what puts the processor back in VM86).
  // CPL becomes 3 with the new CS (SDM vol.2 IRET Operation). A VM=1 image at
  // any other CPL is #GP(0): only ring 0 can re-enter VM86.
  if (flv & 0x20000u) {
    if (cpl() != 0) gp_fault(0);
    uint32_t new_esp = stack_peek(w, 3 * (uint32_t)w);
    uint16_t new_ss = (uint16_t)stack_peek(w, 4 * (uint32_t)w);
    uint16_t new_es = (uint16_t)stack_peek(w, 5 * (uint32_t)w);
    uint16_t new_ds = (uint16_t)stack_peek(w, 6 * (uint32_t)w);
    uint16_t new_fs = (uint16_t)stack_peek(w, 7 * (uint32_t)w);
    uint16_t new_gs = (uint16_t)stack_peek(w, 8 * (uint32_t)w);
    d.rf_load = 1;
    fl->word = flv | 2;  // VM=1: seg_synth below caches DPL 3 from it
    seg_view cv;
    seg_synth(sel, &cv);
    if (new_eip > cv.limit) gp_fault(0);
    seg_commit(cs_i, sel, &cv);
    seg_view sv;
    seg_synth(new_ss, &sv);
    seg_commit(ss_i, new_ss, &sv);
    seg_synth(new_es, &sv);
    seg_commit(es_i, new_es, &sv);
    seg_synth(new_ds, &sv);
    seg_commit(ds_i, new_ds, &sv);
    seg_synth(new_fs, &sv);
    seg_commit(fs_i, new_fs, &sv);
    seg_synth(new_gs, &sv);
    seg_commit(gs_i, new_gs, &sv);
    esp = new_esp;
    eip = new_eip;
    return;
  }

  int newpl = sel & 3;
  if (newpl < cpl()) gp_fault(sel & ~3u);  // returns inward don't exist
  int outer = newpl > cpl();
  uint32_t new_esp = 0;
  uint16_t new_ss = 0;
  seg_view sv;
  if (outer) {
    new_esp = stack_peek(w, 3 * (uint32_t)w);
    new_ss = (uint16_t)stack_peek(w, 4 * (uint32_t)w);
  }

  seg_view cv;
  ret_cs_check(sel, newpl, &cv);
  if (outer) ret_ss_check(new_ss, newpl, &sv);

  // The flags image: CF..OF/TF/IF/DF always, plus IOPL, NT and RF on an
  // outward or CPL-0 return (SDM vol.2 IRET Operation). A protected-mode
  // return always leaves VM86, so VM is cleared and never loaded here — a VM=1
  // image was handled by RETURN-TO-VIRTUAL-8086-MODE above.
  uint32_t mask = (outer || cpl() == 0 ? 0x17fd7u : 0x0fd7u);
  // AC loads on an outward return when CR0.AM enables the alignment check
  // (SDM vol.2 IRET Operation).
  if ((outer || cpl() == 0) && (s->cr0 & kCr0Am)) mask |= 0x40000u;
  d.rf_load = 1;
  fl->word = (fl->word & ~(mask | 0x30000u)) | (flv & mask) | 2;

  if (cv.ar & 4) cv.ar = (uint8_t)((cv.ar & ~0x60u) | (newpl << 5));
  seg_commit(cs_i, sel, &cv);
  if (outer) {
    esp = new_esp;
    seg_commit(ss_i, new_ss, &sv);
  } else {
    esp = s->dbit[ss_i] ? esp + (uint32_t)w * 3
                        : (esp & 0xffff0000u) | (uint16_t)(esp + (uint32_t)w * 3);
  }
  eip = new_eip;
  // The answer half, paired with the gatecall row that shares this caller's
  // CS:IP (debug.h DebugGateCall). Everything has committed, so no fault can
  // follow and leave the row describing a return that never happened.
  if (DebugOn(kDbgMark)) {
    DbgCallArgs a;
    GateArgs(&a);
    DebugGateRet(&a, cf, (sel << 16) | (new_eip & 0xffff));
  }
}

// Far CALL/JMP through a call gate (SDM vol.3 5.8.4; cross-checked against
// v86 far_jump's gate arm). The gate's own DPL gates entry (DPL < CPL or
// DPL < RPL → #GP), the inner code segment is validated like an interrupt
// gate's target, and an inward non-conforming target switches to the TSS
// ring stack and copies the gate's parameter count from the caller's stack,
// landing between the return address and the saved SS:ESP. JMP copies
// nothing and never switches stacks (SDM vol.2 JMP: a non-conforming target
// with DPL != CPL → #GP).
static void call_gate(uint16_t sel, uint64_t desc, int is_call, uint32_t ret_eip,
                      uint16_t ret_cs) {
  int gate16 = ((desc >> 40) & 0xf) == 4;
  int gdpl = (int)((desc >> 45) & 3);
  if (gdpl < cpl() || gdpl < (sel & 3)) gp_fault(sel & ~3u);
  if (!((desc >> 47) & 1)) np_fault(sel & ~3u);
  uint16_t code_sel = (uint16_t)(desc >> 16);
  uint32_t off = (uint32_t)(desc & 0xffff) | (uint32_t)((desc >> 48) & 0xffff) << 16;
  int count = (int)((desc >> 32) & 0x1f);

  // The gate's code segment (same rules as an interrupt gate's target); a
  // call gate may aim into the LDT the way any code load may (SDM vol.3 3.5).
  if ((code_sel & 0xfffc) == 0) gp_fault(0);
  if ((uint32_t)(code_sel >> 3) * 8 + 7 > table_limit(code_sel)) gp_fault(code_sel & ~3u);
  seg_view cv;
  desc_parse(code_sel, &cv);
  if ((cv.ar & 0x18) != 0x18) gp_fault(code_sel & ~3u);  // executable
  int conf = cv.ar & 4;
  int tdpl = (cv.ar >> 5) & 3;
  if (tdpl > cpl()) gp_fault(code_sel & ~3u);
  int switched = !conf && tdpl < cpl();
  if (switched && !is_call) gp_fault(code_sel & ~3u);
  int newpl = switched ? tdpl : cpl();

  uint16_t ss0 = 0;
  uint32_t esp0 = 0;
  seg_view sv;
  if (switched) tss_stack(newpl, &ss0, &esp0, &sv);

  // The parameters are read off the caller's stack before the new SS
  // commits, so a faulting parameter read leaves everything untouched.
  int w = gate16 ? 2 : 4;
  uint32_t params[31];
  uint32_t old_esp = esp;
  uint16_t old_ss = s->sreg[ss_i];
  uint16_t old_cs = ret_cs;
  if (switched && is_call) {
    for (int i = 0; i < count; i++) {
      uint32_t o = s->dbit[ss_i] ? old_esp + (uint32_t)w * i
                                 : (uint16_t)(old_esp + (uint32_t)w * i);
      seg_use(ss_i, o, w, 0);
      params[i] = w == 4 ? rd32(s->base[ss_i] + o) : rd16(s->base[ss_i] + o);
    }
  }
  if (switched) {
    esp = esp0;
    seg_commit(ss_i, ss0, &sv);
  }
  if (is_call) {
    if (switched) {
      if (gate16) {
        kpush16(old_ss);
        kpush16((uint16_t)old_esp);
      } else {
        kpush32(old_ss);
        kpush32(old_esp);
      }
      for (int i = count - 1; i >= 0; i--) {
        if (gate16)
          kpush16((uint16_t)params[i]);
        else
          kpush32(params[i]);
      }
    }
    if (gate16) {
      kpush16(old_cs);
      kpush16((uint16_t)ret_eip);
    } else {
      kpush32(old_cs);
      kpush32(ret_eip);
    }
  }
  if (conf) cv.ar = (uint8_t)((cv.ar & ~0x60u) | (newpl << 5));
  seg_commit(cs_i, (uint16_t)((code_sel & ~3u) | newpl), &cv);
  eip = gate16 ? (off & 0xffff) : off;
}

// The far CALL/JMP target (SDM vol.2 CALL/JMP far Operation): protected mode
// routes system descriptors through the gate machinery — a call gate, a
// task gate, or the TSS itself. Code segments take the direct path either
// way. Task gates and TSS descriptors gate entry the same way the call gate
// does (v86 far_jump).
static void pm_far(uint16_t sel, uint32_t off, int is_call, uint32_t ret_eip) {
  uint16_t ret_cs = s->sreg[cs_i];
  // Real mode and VM86 take the far pointer as-is: CS is a segment number, not
  // a descriptor (SDM vol.3 17.3.1; tiny386 JMPFAR/CALLFAR take the same arm).
  if (!(s->cr0 & 1) || fl->vm) {
    load_cs(sel);
    if (is_call) {
      push_w(ret_cs);
      push_w(ret_eip);
    }
    eip = off;
    return;
  }
  if ((sel & 0xfffc) == 0) gp_fault(0);
  if ((uint32_t)(sel >> 3) * 8 + 7 > table_limit(sel)) gp_fault(sel & ~3u);
  // Call gates may live in the LDT (SDM vol.3 3.5); task gates and TSSes
  // are GDT-only, so the TI=1 system paths below reject first.
  uint64_t desc = kbus_load((sel & 4 ? s->ldtr_base : s->gdtr) + (uint64_t)(sel >> 3) * 8, 8);
  if (!((desc >> 44) & 1)) {  // S=0: a system descriptor
    uint8_t ty = (uint8_t)((desc >> 40) & 0xf);
    int gdpl = (int)((desc >> 45) & 3);
    if (ty == 4 || ty == 0xc) {
      call_gate(sel, desc, is_call, ret_eip, ret_cs);
      return;
    }
    if (sel & 4) gp_fault(sel & ~3u);
    if (gdpl < cpl() || gdpl < (sel & 3)) gp_fault(sel & ~3u);
    if (!((desc >> 47) & 1)) np_fault(sel & ~3u);
    if (ty == 5) {  // task gate: its selector field names the TSS
      do_task_switch((uint16_t)(desc >> 16), is_call ? kTaskCall : kTaskJmp, ret_eip,
                     0, 0);
      return;
    }
    if (ty == 9 || ty == 0xb || ty == 1 || ty == 3) {  // a direct task switch
      do_task_switch(sel, is_call ? kTaskCall : kTaskJmp, ret_eip, 0, 0);
      return;
    }
    gp_fault(sel & ~3u);  // LDT descriptors and the rest never far-load
  }
  load_cs(sel);
  if (is_call) {
    push_w(ret_cs);
    push_w(ret_eip);
  }
  eip = off;
}

// Protected-mode far RET (SDM vol.2 RET far Operation): a same-privilege
// return pops EIP/CS then adds imm once; an outward return (CS RPL > CPL)
// uses the imm twice — first to release the call gate's copied parameters
// from this stack so the saved SS:ESP comes into reach, then again on the
// caller's stack after the SS:ESP load. The frame is peeked and checked
// before any pop commits.
static void pm_ret(uint32_t n) {
  int w = d.w32 ? 4 : 2;
  uint32_t new_eip = stack_peek(w, 0);
  uint16_t sel = (uint16_t)stack_peek(w, w);
  int newpl = sel & 3;
  if (newpl < cpl()) gp_fault(sel & ~3u);  // returns inward don't exist
  int outer = newpl > cpl();
  uint32_t saved = 2 * (uint32_t)w + (outer ? n : 0);  // bytes above CS to
                                                       // the saved SS:ESP
  uint32_t new_esp = 0;
  uint16_t new_ss = 0;
  seg_view sv;
  if (outer) {
    new_esp = stack_peek(w, saved);
    new_ss = (uint16_t)stack_peek(w, saved + (uint32_t)w);
  }
  seg_view cv;
  ret_cs_check(sel, newpl, &cv);
  if (outer) ret_ss_check(new_ss, newpl, &sv);
  if (cv.ar & 4) cv.ar = (uint8_t)((cv.ar & ~0x60u) | (newpl << 5));
  seg_commit(cs_i, sel, &cv);
  // EIP/CS pops plus the inner release, then the outer frame replaces ESP.
  esp = s->dbit[ss_i] ? esp + 2 * (uint32_t)w + n
                      : (esp & 0xffff0000u) | (uint16_t)(esp + 2 * (uint32_t)w + n);
  if (outer) {
    esp = new_esp;
    seg_commit(ss_i, new_ss, &sv);
    esp += n;  // release the parameters from the caller's stack
  }
  eip = new_eip;
}

// SDM vol.3 table 6-1: #DE/#BR/#UD/#NM/#TS/#NP/#SS/#GP/#PF/#MF/#AC/#XM/
// #VE/#CP are faults, #DB/#BP/#OF are traps and #DF/#MC are aborts. A fault's
// saved EFLAGS image carries RF=1 so an instruction breakpoint cannot fault
// again when the instruction restarts (SDM vol.3 17.3.1.1; QEMU
// do_interrupt_protected's exception_is_fault + "eflags |= RF_MASK").
static int int_is_fault(int vec) {
  return vec != 1 && vec != 3 && vec != 4 && vec != 8 && vec != 18;
}

// Delivers a vector. Real mode: the IVT at IDTR (SDM vol.3 16.3). Protected
// mode: the IDT gate — interrupt/trap gates push flags/CS/IP, switching to the
// TSS SS0:ESP0 stack on a privilege change (SDM vol.2 INT Operation; behavior
// cross-checked against v86 call_interrupt_vector; task gates and NT returns
// are task switching, stage 3 item 3).
//
// `origin` says where the vector came from, and the three sources differ in the
// SDM: only a CPU-raised exception pushes the vector's error code (vol.2 INT
// Operation, vol.3 table 6-1 — a software INT n and the external interrupt pin
// push a bare flags/CS/IP frame); the EXT bit of gate-fault error codes is set
// for external events only; and only INT n/INT3/INTO check the gate DPL.
void do_int(int vec, uint32_t ret_eip, int origin, uint32_t ec) {
  int soft = origin == kIntSoft;
  int ext = origin == kIntExternal;
  uint32_t eflags_img = fl->word | 2;
  if (origin == kIntException && int_is_fault(vec)) eflags_img |= 0x10000;
  if (!(s->cr0 & 1)) {
    uint64_t tbl = s->idtr + (uint64_t)vec * 4;
    uint32_t off = krd16(tbl);
    uint32_t seg = krd16(tbl + 2);
    // The IVT entry a guest's INT n actually lands on (the protected-mode path
    // prints the same thing for its gate below). A real-mode guest's calls into
    // the firmware are how it asks the machine what it is -- int 15h for memory,
    // int 10h for video -- and the answers it acts on are invisible without this
    // row: nothing else in the event stream records that a service was called,
    // let alone which one (kDbgMark, the same note the gate path emits).
    if (DebugOn(kDbgMark)) DebugMark("gate", vec, (int)(((seg << 16) | off) & 0xffffffff));
    // The request half of the conversation (debug.h DebugGateCall): a real-mode
    // guest's AH names the BIOS/DOS service it is asking for, and the IVT row
    // above says nothing about which one that was. CS is still the caller's
    // here -- seg_commit below is what switches it.
    if (soft && DebugOn(kDbgMark)) {
      DbgCallArgs a;
      GateArgs(&a);
      DebugGateCall(vec, &a, ((uint32_t)s->sreg[cs_i] << 16) | (ret_eip & 0xffff));
    }
    kpush16(eflags_img);
    kpush16(s->sreg[cs_i]);
    kpush16((uint16_t)ret_eip);
    fl->if_ = 0;
    fl->tf = 0;
    seg_view v;
    seg_synth((uint16_t)seg, &v);
    seg_commit(cs_i, (uint16_t)seg, &v);
    eip = off;
    d.delivered = 1;  // the TF trap postpones to the handler's first insn
    return;
  }

  uint32_t gate_idx = (uint32_t)vec * 8;
  if (gate_idx + 7 > s->idtr_limit) gp_fault(gate_idx | 2 | ext);
  // The IDT base is a linear address (SDM vol.3 3.5.1), so the gate comes
  // through the paging unit like any other access.
  uint64_t gate = kbus_load(s->idtr + gate_idx, 8);
  uint8_t gt = (uint8_t)((gate >> 40) & 0xf);
  if (soft && ((gate >> 45) & 3) < (uint32_t)cpl()) gp_fault(gate_idx | 2);
  if (gt != 6 && gt != 7 && gt != 0xe && gt != 0xf && gt != 5)
    gp_fault(gate_idx | 2 | ext);
  if (!((gate >> 47) & 1)) np_fault(gate_idx | 2 | ext);
  if (gt == 5) {
    // Task gate: the switch replaces the whole context, and an exception's
    // error code lands on the new task's stack (v86 do_task_switch).
    int gate_ec = vec_has_ec(vec) && origin == kIntException;
    do_task_switch((uint16_t)(gate >> 16), kTaskCall, ret_eip, gate_ec, ec);
    d.delivered = 1;  // the exception went through a task gate
    return;
  }
  int gate16 = gt == 6 || gt == 7;

  // Gate layout (SDM vol.3 fig 3-8): [off15:0][selector][reserved][P DPL
  // type][off31:16] — offset is split around the middle, selector in bits
  // 16-31. Type/DPL/P decode from byte 5 (bits 40-47).
  uint16_t code_sel = (uint16_t)(gate >> 16);
  uint32_t off = (uint32_t)(gate & 0xffff) | (uint32_t)((gate >> 48) & 0xffff) << 16;
  // The gate actually read decides where a vector lands; when a guest's
  // handler never runs, the difference between "delivered to the right stub"
  // and "delivered to the early default handler" is this one number (kDbgMark).
  if (DebugOn(kDbgMark)) DebugMark("gate", vec, (int)off);
  // The request half of the conversation (debug.h DebugGateCall), software
  // INT n only: on an external interrupt or an exception AX is not a service
  // selector, and nothing is being asked for. CS is still the caller's here.
  if (soft && DebugOn(kDbgMark)) {
    DbgCallArgs a;
    GateArgs(&a);
    DebugGateCall(vec, &a, ((uint32_t)s->sreg[cs_i] << 16) | (ret_eip & 0xffff));
  }

  // The gate's code segment (SDM vol.2 INT: table limits, executable,
  // DPL <= CPL, present — a faulting gate is never entered; the target may
  // be an LDT code segment, SDM vol.3 3.5).
  if ((code_sel & 0xfffc) == 0) gp_fault(0);
  if ((uint32_t)(code_sel >> 3) * 8 + 7 > table_limit(code_sel)) gp_fault(code_sel & ~3u);
  seg_view cv;
  desc_parse(code_sel, &cv);
  if ((cv.ar & 0x18) != 0x18) gp_fault(code_sel & ~3u);  // executable
  int conf = cv.ar & 4;
  int tdpl = (cv.ar >> 5) & 3;
  if (tdpl > cpl()) gp_fault(code_sel & ~3u);
  // SDM vol.2 INT n, TRAP-OR-INTERRUPT-GATE: a gate taken while EFLAGS.VM=1
  // must name a non-conforming ring-0 code segment; every other target --
  // a ring-1/2 segment or any conforming one -- is #GP(selector). The gate's
  // own DPL (the software-INT permission) was checked above and is unrelated.
  if (fl->vm && (conf || tdpl != 0)) gp_fault(code_sel & ~3u);
  // The gate itself must be 32-bit: the VM86 frame it pushes carries the
  // 32-bit ESP and the segment images RETURN-TO-VIRTUAL-8086-MODE restores, so
  // a 16-bit gate cannot express the return context (SDM vol.3 17.3.3).
  if (fl->vm && gate16) gp_fault(code_sel & ~3u);
  int newpl = !conf && tdpl < cpl() ? tdpl : cpl();
  int switched = newpl < cpl();

  uint32_t old_ss = s->sreg[ss_i], old_esp = esp, old_cs = s->sreg[cs_i];
  uint16_t ss0;
  uint32_t esp0;
  seg_view sv;
  if (switched) {
    tss_stack(newpl, &ss0, &esp0, &sv);
    esp = esp0;
    seg_commit(ss_i, ss0, &sv);
  }

  // CS commits with its RPL forced to the new CPL (tiny386 call_isr); a
  // conforming target keeps CPL, so its cached DPL follows the kept one.
  if (conf) cv.ar = (uint8_t)((cv.ar & ~0x60u) | (newpl << 5));
  seg_commit(cs_i, (uint16_t)((code_sel & ~3u) | newpl), &cv);

  if (gate16) {
    if (switched) {
      // A delivery out of virtual-8086 mode pushes the mode's own frame (SDM
      // vol.3 17.3.3; QEMU do_interrupt_protected): GS, FS, DS and ES above the
      // usual SS/ESP, so IRET's RETURN-TO-VIRTUAL-8086-MODE can restore the
      // whole context. The EFLAGS image below keeps VM=1 — that is how the
      // return knows where it is going.
      if (fl->vm) {
        kpush16(s->sreg[gs_i]);
        kpush16(s->sreg[fs_i]);
        kpush16(s->sreg[ds_i]);
        kpush16(s->sreg[es_i]);
      }
      kpush16((uint16_t)old_ss);
      kpush16((uint16_t)old_esp);
    }
    kpush16(eflags_img);
    kpush16((uint16_t)old_cs);
    kpush16((uint16_t)ret_eip);
    // Only a CPU-raised exception carries the vector's error code: a software
    // INT n pushes a bare frame, and so does the external interrupt pin (SDM
    // vol.2 INT Operation, vol.3 table 6-1; v86 passes None for software ints).
    if (vec_has_ec(vec) && origin == kIntException) kpush16((uint16_t)ec);
  } else {
    if (switched) {
      if (fl->vm) {
        kpush32(s->sreg[gs_i]);
        kpush32(s->sreg[fs_i]);
        kpush32(s->sreg[ds_i]);
        kpush32(s->sreg[es_i]);
      }
      kpush32(old_ss);
      kpush32(old_esp);
    }
    kpush32(eflags_img);
    kpush32(old_cs);
    kpush32(ret_eip);
    if (vec_has_ec(vec) && origin == kIntException) kpush32(ec);
  }
  // SDM vol.2 INT n, INTERRUPT-FROM-VIRTUAL-8086-MODE: the V86 segment
  // registers were pushed on the frame above, and the handler now runs with
  // GS/FS/DS/ES null. The V86 values are segment numbers, not selectors, and
  // would fault on first use in protected mode; EMM386's exception
  // dispatcher pushes DS on entry and restores it with a bare `pop ds`, so
  // leaving the V86 value cached is a #GP(0x298) storm on a DOS boot.
  if (fl->vm) {
    seg_view z = {0, 0, 0, 0, 0};
    seg_commit(gs_i, 0, &z);
    seg_commit(fs_i, 0, &z);
    seg_commit(ds_i, 0, &z);
    seg_commit(es_i, 0, &z);
  }
  fl->tf = 0;
  fl->nt = 0;
  // The handler is a protected-mode one: the delivery leaves VM86 (the image
  // above keeps VM=1 for the return) — SDM vol.3 17.3.3, QEMU clears
  // TF|VM|RF|NT here.
  fl->vm = 0;
  if (gt == 6 || gt == 0xe) fl->if_ = 0;  // interrupt gates clear IF
  eip = gate16 ? (off & 0xffff) : off;
  d.delivered = 1;  // the TF trap postpones to the handler's first insn
}

_Noreturn static void ud(void) { raise_(fr, vec_ud, (uint64_t)eip); }

// Device not available (SDM vol.3 2.5): an x87 escape reached with CR0.TS or
// CR0.EM set. Raised before the opcode is decoded.
_Noreturn static void nm(void) { raise_(fr, vec_nm, (uint64_t)eip); }

// Divide errors (SDM DIV/IDIV): quotient overflow or zero divisor.
_Noreturn static void de(void) { raise_(fr, 0, (uint64_t)eip); }

// ---- conditions (SDM table Jcc; the low nibble is the condition) -----------

static int cond(int c) {
  int r;
  switch (c & ~1) {
    case 0x0:
      r = fl->of;
      break;
    case 0x2:
      r = fl->cf;
      break;
    case 0x4:
      r = fl->zf;
      break;
    case 0x6:
      r = fl->cf || fl->zf;
      break;
    case 0x8:
      r = fl->sf;
      break;
    case 0xa:
      r = fl->pf;
      break;
    case 0xc:
      r = fl->sf != fl->of;
      break;
    default:
      r = fl->zf || fl->sf != fl->of;
      break;  // 0xe
  }
  return r ^ (c & 1);
}

// Condition mnemonics in cc order (SDM table Jcc) for the jcc/setcc families.
static const char* const kJccNames[16] = {"jo",  "jno", "jb",   "jae", "je",  "jne",
                                          "jbe", "ja",  "js",   "jns", "jp",  "jnp",
                                          "jl",  "jge", "jle",  "jg"};
static const char* const kSetccNames[16] = {"seto",  "setno", "setb", "setae", "sete",  "setne",
                                            "setbe", "seta",  "sets", "setns", "setp",  "setnp",
                                            "setl",  "setge", "setle", "setg"};
static const char* const kCmovNames[16] = {"cmovo",  "cmovno", "cmovb", "cmovae", "cmove",  "cmovne",
                                           "cmovbe", "cmova",  "cmovs", "cmovns", "cmovp",  "cmovnp",
                                           "cmovl",  "cmovge", "cmovle", "cmovg"};

// ---- the shift/rotate family (SDM GRP2) --------------------------------------

static uint32_t rol(uint32_t v, uint32_t cnt, int size);
static uint32_t ror(uint32_t v, uint32_t cnt, int size);
static uint32_t rcl(uint32_t v, uint32_t cnt, int size);
static uint32_t rcr(uint32_t v, uint32_t cnt, int size);

static uint32_t shl(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t r = cnt >= width ? 0 : (v << cnt) & vmask(size);
  fl->cf = cnt < width && (v >> (width - cnt)) & 1;
  if (cnt == 1) fl->of = ((r & vmsb(size)) != 0) ^ (fl->cf != 0);
  fl->af = 0;
  szp(r, size);
  return r;
}

static uint32_t shr(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t r = cnt >= width ? 0 : v >> cnt;
  fl->cf = (v >> (cnt - 1)) & 1;
  fl->of = cnt == 1 && (v & vmsb(size));
  fl->af = 0;
  szp(r, size);
  return r;
}

static uint32_t sar(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t fill = (v & vmsb(size)) ? vmask(size) : 0;
  uint32_t r = cnt >= width ? fill : ((v >> cnt) | (fill << (width - cnt))) & vmask(size);
  // CF is the last bit shifted out (v >> (cnt-1)), not the sign bit of the
  // shifted value; only the cnt >= width case is undefined and takes the sign.
  fl->cf = cnt >= width ? ((v & vmsb(size)) != 0) : ((v >> (cnt - 1)) & 1);
  if (cnt == 1) fl->of = 0;
  fl->af = 0;
  szp(r, size);
  return r;
}

static uint32_t rol(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t k = cnt % width;
  uint32_t r = ((v << k) | (v >> (width - k))) & vmask(size);
  fl->cf = r & 1;
  // SDM ROL: for a 1-bit rotate OF = MSB(DEST) XOR CF after the rotate.
  if (cnt == 1) fl->of = ((r & vmsb(size)) != 0) ^ (fl->cf != 0);
  return r;
}

static uint32_t ror(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint32_t k = cnt % width;
  uint32_t r = ((v >> k) | (v << (width - k))) & vmask(size);
  fl->cf = (r & vmsb(size)) != 0;
  fl->of = ((r & vmsb(size)) != 0) ^ ((r & (vmsb(size) >> 1)) != 0);
  return r;
}

// Rotate through carry: a width+1 bit ring (SDM RCL/RCR).
static uint32_t rcl(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint64_t ring = ((uint64_t)fl->cf << width) | (v & vmask(size));
  uint32_t k = cnt % (width + 1);
  ring = (ring << k) | (ring >> (width + 1 - k));
  fl->cf = (ring >> width) & 1;
  fl->of = (((ring & vmsb(size)) != 0) ^ (fl->cf != 0));
  return (uint32_t)ring & vmask(size);
}

static uint32_t rcr(uint32_t v, uint32_t cnt, int size) {
  if (!cnt) return v;
  uint32_t width = (uint32_t)size * 8;
  uint64_t ring = ((uint64_t)fl->cf << width) | (v & vmask(size));
  uint32_t k = cnt % (width + 1);
  ring = (ring >> k) | (ring << (width + 1 - k));
  fl->cf = (ring >> width) & 1;
  fl->of = (((ring & vmsb(size)) != 0) ^ (fl->cf != 0));
  return (uint32_t)ring & vmask(size);
}

// ---- GRP dispatch families (SDM 2-1 escape groups; reg-field picks)

// ---- GRP1 arithmetic (SDM 80..83): the reg field picks the operation --------
// kind: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp.

static uint32_t grp1_math(int kind, uint32_t a, uint32_t b, int size) {
  int carry = fl->cf;
  uint32_t r = 0;
  switch (kind) {
    case 0:
      r = (a + b) & vmask(size);
      flags_add(a, b, r, size, 0);
      break;
    case 2:
      r = (a + b + carry) & vmask(size);
      flags_add(a, b, r, size, carry);
      break;
    case 5:
    case 7:
      r = (a - b) & vmask(size);
      flags_sub(a, b, r, size, 0);
      break;
    case 3:
      r = (a - b - carry) & vmask(size);
      flags_sub(a, b, r, size, carry);
      break;
    case 1:
      r = a | b;
      flags_logic(r, size);
      break;
    case 4:
      r = a & b;
      flags_logic(r, size);
      break;
    default:
      r = a ^ b;
      flags_logic(r, size);
      break;  // 6: xor
  }
  return r;
}

static void grp1(int size, int imm_form) {
  // imm_form: 0 = imm at the width, 1 = imm8 sign-extended (83), 2 = imm8 (82)
  static const int kind[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const char* const kGrp1Names[8] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
  modrm();
  fr->rec.mnemonic = kGrp1Names[d.reg];
  uint32_t src;
  if (imm_form == 1)
    src = (uint32_t)(int8_t)imm8();
  else if (imm_form == 2)
    src = imm8();
  else
    src = d.w32 ? imm32() : imm16();
  if (size == 1) {
    uint32_t a = RM8(), b = src & 0xff;
    uint32_t r = grp1_math(kind[d.reg], a, b, 1);
    if (d.reg != 7) SET_RM8(r);
  } else if (size == 2) {
    uint32_t a = RM16(), b = src & 0xffff;
    uint32_t r = grp1_math(kind[d.reg], a, b, 2);
    if (d.reg != 7) SET_RM16(r);
  } else {
    uint32_t a = RM32(), b = src;
    uint32_t r = grp1_math(kind[d.reg], a, b, 4);
    if (d.reg != 7) SET_RM32(r);
  }
}

// ---- GRP2 shifts/rotates (SDM c0/c1/d0..d3): reg field picks the op ---------

// The modrm byte is decoded by the caller before this runs — the C0/C1 count
// immediate follows the modrm byte in the encoding, so the caller must fetch
// it after modrm() (tiny386 loads the count through an accessor resolved the
// same way).
static void grp2(int size, uint32_t cnt) {
  // GRP2 reg-field order (SDM); 4 and 6 are both SHL/SAL.
  static const char* const kShiftNames[8] = {"rol", "ror", "rcl", "rcr",
                                             "shl", "shr", "shl", "sar"};
  fr->rec.mnemonic = kShiftNames[d.reg];
  cnt &= 31;
  uint32_t v, r;
  if (size == 1) {
    v = RM8();
    switch (d.reg) {
      case 0:
        r = rol(v, cnt, 1);
        break;
      case 1:
        r = ror(v, cnt, 1);
        break;
      case 2:
        r = rcl(v, cnt, 1);
        break;
      case 3:
        r = rcr(v, cnt, 1);
        break;
      case 4:
      case 6:
        r = shl(v, cnt, 1);
        break;
      case 5:
        r = shr(v, cnt, 1);
        break;
      default:
        r = sar(v, cnt, 1);
        break;
    }
    if (cnt) SET_RM8(r);  // count 0 changes nothing, not even the store path
  } else if (size == 2) {
    v = RM16();
    switch (d.reg) {
      case 0:
        r = rol(v, cnt, 2);
        break;
      case 1:
        r = ror(v, cnt, 2);
        break;
      case 2:
        r = rcl(v, cnt, 2);
        break;
      case 3:
        r = rcr(v, cnt, 2);
        break;
      case 4:
      case 6:
        r = shl(v, cnt, 2);
        break;
      case 5:
        r = shr(v, cnt, 2);
        break;
      default:
        r = sar(v, cnt, 2);
        break;
    }
    if (cnt) SET_RM16(r);
  } else {
    v = RM32();
    switch (d.reg) {
      case 0:
        r = rol(v, cnt, 4);
        break;
      case 1:
        r = ror(v, cnt, 4);
        break;
      case 2:
        r = rcl(v, cnt, 4);
        break;
      case 3:
        r = rcr(v, cnt, 4);
        break;
      case 4:
      case 6:
        r = shl(v, cnt, 4);
        break;
      case 5:
        r = shr(v, cnt, 4);
        break;
      default:
        r = sar(v, cnt, 4);
        break;
    }
    if (cnt) SET_RM32(r);
  }
}

// ---- multiply/divide (SDM GRP3 /4-/7) -----------------------------------------

// MUL r/m8: AX = AL * r/m8; the pair lives inside the EAX cell.
static void mul8(void) {
  uint32_t prod = (uint32_t)al * rm8();
  ax = (uint16_t)prod;
  flags_mul(prod, 1);
}

static void imul8(void) {
  int32_t prod = (int8_t)al * (int8_t)rm8();
  ax = (uint16_t)prod;
  flags_imul(prod, 1);
}

static void mul_w(uint32_t src) {
  uint64_t prod = (uint64_t)(d.w32 ? eax : ax) * src;
  if (d.w32) {
    eax = (uint32_t)prod;
    edx = (uint32_t)(prod >> 32);
  } else {
    ax = (uint16_t)prod;
    dx = (uint16_t)(prod >> 16);
  }
  flags_mul(prod, d.w32 ? 4 : 2);
}

static void imul_w(uint32_t src) {
  int bits = d.w32 ? 32 : 16;
  int64_t prod = ((int64_t)(d.w32 ? (int32_t)eax : (int16_t)ax)) *
                 (d.w32 ? (int32_t)src : (int16_t)(uint16_t)src);
  if (d.w32) {
    eax = (uint32_t)prod;
    edx = (uint32_t)((uint64_t)prod >> 32);
  } else {
    ax = (uint16_t)prod;
    dx = (uint16_t)((uint64_t)prod >> 16);
  }
  flags_imul(prod, bits / 8);
}

// DIV r/m8: AL = AX/src, AH = remainder; both halves of the EAX cell.
static void div8(void) {
  uint32_t dividend = ax;
  uint32_t divisor = rm8();
  if (divisor == 0 || dividend / divisor > 0xff) de();
  al = (uint8_t)(dividend / divisor);
  ah = (uint8_t)(dividend % divisor);
}

static void idiv8(void) {
  int32_t dividend = (int16_t)ax;
  int32_t divisor = (int8_t)rm8();
  if (divisor == 0) de();
  int32_t q = dividend / divisor;
  if (q != (int8_t)q) de();
  al = (uint8_t)q;
  ah = (uint8_t)(dividend % divisor);
}

static void div_w(uint32_t src) {
  uint32_t lo = d.w32 ? eax : ax;
  uint64_t dividend = ((uint64_t)(d.w32 ? edx : dx) << (d.w32 ? 32 : 16)) | lo;
  uint32_t divisor = src & vmask(d.w32 ? 4 : 2);
  if (divisor == 0 || dividend / divisor > vmask(d.w32 ? 4 : 2)) de();
  uint32_t q = (uint32_t)(dividend / divisor);
  uint32_t r = (uint32_t)(dividend % divisor);
  if (d.w32) {
    eax = q;
    edx = r;
  } else {
    ax = (uint16_t)q;
    dx = (uint16_t)r;
  }
}

static void idiv_w(uint32_t src) {
  int qbits = d.w32 ? 32 : 16;  // the quotient's width (SDM IDIV r/m32: #DE when
                                // the quotient is outside -2^31..2^31-1; r/m16 likewise)
  uint32_t lo = d.w32 ? eax : ax;
  int64_t dividend = (int64_t)((((uint64_t)(d.w32 ? edx : dx) << (d.w32 ? 32 : 16)) | lo) &
                               (d.w32 ? ~0ULL : 0xffffffffULL));
  if (!d.w32) dividend = (int32_t)(uint32_t)dividend;
  int64_t divisor = d.w32 ? (int32_t)src : (int16_t)(uint16_t)src;
  if (divisor == 0) de();
  int64_t q = dividend / divisor;
  int64_t hi = 1LL << (qbits - 1);  // 2^31 / 2^15, exact in int64 (no shift UB)
  if (q < -hi || q >= hi) de();
  int64_t r = dividend % divisor;
  if (d.w32) {
    eax = (uint32_t)q;
    edx = (uint32_t)r;
  } else {
    ax = (uint16_t)q;
    dx = (uint16_t)r;
  }
}

// ---- GRP3 (SDM f6/f7): test/not/neg/mul/imul/div/idiv ----------------------

static void grp3(int size) {
  static const char* const kGrp3Names[8] = {"test", "test", "not", "neg",
                                            "mul",  "imul", "div", "idiv"};
  modrm();
  fr->rec.mnemonic = kGrp3Names[d.reg];
  if (d.reg <= 1) {  // test rm, imm
    uint32_t imm = size == 1 ? imm8() : d.w32 ? imm32() : imm16();
    uint32_t a = size == 1 ? RM8() : size == 2 ? RM16() : RM32();
    flags_logic(a & imm, size);
    return;
  }
  if (d.reg == 2) {  // not: no flags
    if (size == 1)
      SET_RM8(~RM8());
    else if (size == 2)
      SET_RM16(~RM16());
    else
      SET_RM32(~RM32());
    return;
  }
  if (d.reg == 3) {  // neg
    if (size == 1) {
      uint32_t v = RM8();
      uint8_t r = (uint8_t)-v;
      flags_sub(0, v, r, 1, 0);
      SET_RM8(r);
    } else if (size == 2) {
      uint32_t v = RM16();
      uint16_t r = (uint16_t)-v;
      flags_sub(0, v, r, 2, 0);
      SET_RM16(r);
    } else {
      uint32_t v = RM32();
      uint32_t r = -v;
      flags_sub(0, v, r, 4, 0);
      SET_RM32(r);
    }
    return;
  }
  if (size == 1) {
    switch (d.reg) {
      case 4:
        mul8();
        return;
      case 5:
        imul8();
        return;
      case 6:
        div8();
        return;
      default:
        idiv8();
        return;
    }
  }
  uint32_t src = size == 2 ? RM16() : RM32();
  switch (d.reg) {
    case 4:
      mul_w(src);
      return;
    case 5:
      imul_w(src);
      return;
    case 6:
      div_w(src);
      return;
    default:
      idiv_w(src);
      return;
  }
}

// ---- string operations (SDM MOVS..SCAS, INS/OUTS, with REP/REPE/REPNE) -------
// SI uses DS (overridable); DI always ES. SI/DI advance by the address size,
// CX counts by the operand size. INS/OUTS swap the memory side for port DX and
// share the whole loop. One iteration per step: a REP that wants another
// re-executes the instruction (interruptible, like hardware).
static int str_uses_si(int op) {
  // SI uses DS (overridable). OUTS reads DS:[SI]; INS reads no memory at all.
  return (op >= 0xa4 && op <= 0xa7) || op == 0xac || op == 0xad || op == 0x6e || op == 0x6f;
}
static int str_uses_di(int op) {
  // MOVS/CMPS (a4-a7) and STOS/SCAS (aa-ab, ae-af); LODS (ac/ad) is SI only.
  // INS (6c/6d) writes ES:[DI]: the destination segment is always ES.
  return (op >= 0xa4 && op <= 0xa7) || op == 0xaa || op == 0xab || op == 0xae || op == 0xaf ||
         op == 0x6c || op == 0x6d;
}

static void string_op(uint8_t op) {
  // SDM vol.2 REP/REPE/REPZ/REPNE/REPNZ: with (E)CX = 0 the instruction
  // performs no operation at all -- no memory access, no counter or pointer
  // update. The guard has to stand before the single iteration below: entering
  // it with a zero counter and decrementing afterwards wraps (E)CX to
  // 0xffffffff and turns a no-op into a 2^32-iteration loop.
  if (d.rep && (d.w32 ? ecx : cx) == 0) return;
  // Odd opcodes are the word/dword forms for every member of the family
  // (A4/A5 MOVS, A6/A7 CMPS, AA/AB STOS, AC/AD LODS, AE/AF SCAS, 6C/6D INS,
  // 6E/6F OUTS): the operand size picks 4 or 2 bytes (SDM vol.2 per-opcode
  // "OperandSize" column), never a byte.
  int size = (op & 1) ? (d.w32 ? 4 : 2) : 1;
  int step = fl->df ? -size : size;
  uint32_t sio = d.a32 ? esi : (uint32_t)si;
  uint32_t dio = d.a32 ? edi : (uint32_t)di;
  int sseg = d.seg >= 0 ? d.seg : ds_i;
  // One check per direction, once per iteration (SDM vol.3 5.3): MOVS/CMPS/
  // LODS read DS:SI; MOVS/STOS write ES:DI; CMPS/SCAS read ES:DI.
  if (str_uses_si(op)) seg_use(sseg, sio, size, 0);
  if (str_uses_di(op))
    seg_use(es_i, dio, size,
            op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab || op == 0x6c || op == 0x6d);
  uint64_t slin = s->base[sseg] + sio;
  uint64_t dlin = s->base[es_i] + dio;
  switch (op) {
    case 0xa4:
    case 0xa5: {  // movs
      fr->rec.mnemonic = size == 1 ? "movsb" : size == 2 ? "movsw" : "movsd";
      WatchData(slin, size, 0);
      WatchData(dlin, size, 1);
      if (size == 1)
        wr8(dlin, rd8(slin));
      else if (size == 2)
        wr16(dlin, rd16(slin));
      else
        wr32(dlin, rd32(slin));
      break;
    }
    case 0xa6:
    case 0xa7: {  // cmps: [si] - [di]
      fr->rec.mnemonic = size == 1 ? "cmpsb" : size == 2 ? "cmpsw" : "cmpsd";
      WatchData(slin, size, 0);
      WatchData(dlin, size, 0);
      uint32_t a = size == 1 ? rd8(slin) : size == 2 ? rd16(slin) : rd32(slin);
      uint32_t b = size == 1 ? rd8(dlin) : size == 2 ? rd16(dlin) : rd32(dlin);
      uint32_t r = (a - b) & vmask(size);
      flags_sub(a, b, r, size, 0);
      break;
    }
    case 0xaa:
    case 0xab: {  // stos
      fr->rec.mnemonic = size == 1 ? "stosb" : size == 2 ? "stosw" : "stosd";
      WatchData(dlin, size, 1);
      if (size == 1)
        wr8(dlin, al);
      else if (size == 2)
        wr16(dlin, ax);
      else
        wr32(dlin, eax);
      break;
    }
    case 0xac:
    case 0xad: {  // lods
      fr->rec.mnemonic = size == 1 ? "lodsb" : size == 2 ? "lodsw" : "lodsd";
      WatchData(slin, size, 0);
      if (size == 1)
        al = rd8(slin);
      else if (size == 2)
        ax = rd16(slin);
      else
        eax = rd32(slin);
      break;
    }
    case 0x6c:
    case 0x6d: {  // ins: port DX -> ES:[DI] (SDM vol.2 INS)
      fr->rec.mnemonic = size == 1 ? "insb" : size == 2 ? "insw" : "insd";
      WatchData(dlin, size, 1);
      uint32_t v = io_in(dx, size);
      if (size == 1)
        wr8(dlin, (uint8_t)v);
      else if (size == 2)
        wr16(dlin, (uint16_t)v);
      else
        wr32(dlin, v);
      break;
    }
    case 0x6e:
    case 0x6f: {  // outs: DS:[SI] -> port DX (SDM vol.2 OUTS)
      fr->rec.mnemonic = size == 1 ? "outsb" : size == 2 ? "outsw" : "outsd";
      WatchData(slin, size, 0);
      uint32_t v = size == 1 ? rd8(slin) : size == 2 ? rd16(slin) : rd32(slin);
      io_out(dx, size, v);
      break;
    }
    default: {  // 0xae/0xaf scas: eAX - [di]
      fr->rec.mnemonic = size == 1 ? "scasb" : size == 2 ? "scasw" : "scasd";
      WatchData(dlin, size, 0);
      uint32_t a = size == 1 ? al : size == 2 ? ax : eax;
      uint32_t b = size == 1 ? rd8(dlin) : size == 2 ? rd16(dlin) : rd32(dlin);
      uint32_t r = (a - b) & vmask(size);
      flags_sub(a, b, r, size, 0);
      break;
    }
  }
  if (str_uses_si(op)) {
    if (d.a32)
      esi += (uint32_t)step;
    else
      si = (uint16_t)(sio + step);
  }
  if (str_uses_di(op)) {
    if (d.a32)
      edi += (uint32_t)step;
    else
      di = (uint16_t)(dio + step);
  }
  if (!d.rep) return;
  uint32_t cnt = (d.w32 ? ecx : cx) - 1;
  if (d.w32)
    ecx = cnt;
  else
    cx = (uint16_t)cnt;
  if (cnt == 0) return;
  // REPE/REPNE keep iterating while the comparison holds (SDM REP prefix).
  if ((op >= 0xa6 && op <= 0xa7) || op == 0xae || op == 0xaf)
    if (fl->zf != (d.rep == 1)) return;
  d.nxt = 0;  // REP continues: the step re-executes this instruction
}

// ---- BCD adjust (SDM DAA..AAD; SALC is the undocumented AL=CF<<8) -----------

static void daa(void) {
  uint8_t old_al = al;
  int old_cf = fl->cf;
  int c = 0;
  if ((al & 0xf) > 9 || fl->af) {
    al = (uint8_t)(old_al + 6);
    fl->af = 1;
    c = old_cf || old_al > 0xf9;
  } else {
    fl->af = 0;
  }
  if (old_al > 0x99 || old_cf) {  // original AL and original CF (tiny386 model)
    al += 0x60;
    c = 1;
  }
  // flags_logic clears AF; DAA/DAS keep the step-one AF (SDM "Flags Affected"
  // and tiny386 __DAA/__DAS_helper: only SF/ZF/PF are recomputed from AL).
  int keep_af = fl->af;
  flags_logic(al, 1);
  fl->af = keep_af;
  if (c) fl->cf = 1;
}

// DAS per the actual 386+ behavior, verified 0 fails against the
// kvm-unit-tests 1024-case truth table (dascheck offline harness): both
// conditions test the ORIGINAL AL and ORIGINAL CF (tiny386 model; SDM's
// step-two re-read of the decremented AL diverges on 26 cases). AF is the
// step-one value, never cleared.
static void das(void) {
  uint8_t old_al = al;
  int old_cf = fl->cf;
  int c = 0;
  if ((al & 0xf) > 9 || fl->af) {
    al = (uint8_t)(old_al - 6);
    fl->af = 1;
    c = old_cf || old_al < 6;
  } else {
    fl->af = 0;
  }
  if (old_al > 0x99 || old_cf) {  // original AL and original CF
    al -= 0x60;
    c = 1;
  }
  int keep_af = fl->af;
  flags_logic(al, 1);
  fl->af = keep_af;
  if (c) fl->cf = 1;
}

static void aaa(void) {
  if ((al & 0xf) > 9 || fl->af) {
    // SDM AAA: AL and AH are adjusted separately; the AL carry is discarded,
    // so "AX += 0x106" would add an extra 1 to AH when AL overflows.
    al = (uint8_t)(al + 6);
    ah += 1;
    fl->af = 1;
    fl->cf = 1;
  } else {
    fl->af = 0;
    fl->cf = 0;
  }
  al &= 0xf;
}

static void aas(void) {
  if ((al & 0xf) > 9 || fl->af) {
    // SDM AAS: likewise, the AL borrow is discarded and AH is decremented once.
    al = (uint8_t)(al - 6);
    ah -= 1;
    fl->af = 1;
    fl->cf = 1;
  } else {
    fl->af = 0;
    fl->cf = 0;
  }
  al &= 0xf;
}

// ---- the instruction switch ---------------------------------------------------
// One case per manual page, opcode order (SDM vol.2 table A-2); the comment
// is the manual's page title. The ALU family (00..3d) keeps the manual's own
// six forms per operation — rm8,r8 / rm,r / r8,rm8 / r,rm / al,imm8 / eAX,imm —
// so each case reads exactly like its page.

// The 0f two-byte dispatch.
// ---- MMX / SSE / SSE2 (SDM vol.1 ch.10-11, vol.2 instruction pages) --------
// The packed engine: elements are read/written at 1/2/4/8-byte widths out of
// the 16-byte XMM storage (or the low 64 bits of an x87 register for MMX),
// and the arithmetic goes through the same Berkeley SoftFloat calls the x87
// engine uses. MMX registers alias the low 64 bits of ST0-ST7; after an MMX
// write the x87 tag is "special" and EMMS empties the whole stack, exactly
// the x87/MMX state relationship the manual describes (SDM vol.1 9.2.2).

static uint64_t SimdElem(const uint8_t* p, int bytes, int i) {
  uint64_t v = 0;
  memcpy(&v, p + (size_t)bytes * i, (size_t)bytes);
  return v;
}
static void SimdSetElem(uint8_t* p, int bytes, int i, uint64_t v) {
  memcpy(p + (size_t)bytes * i, &v, (size_t)bytes);
}
static uint64_t SimdMask(int bytes) {
  return bytes == 8 ? ~0ULL : ((1ULL << (bytes * 8)) - 1);
}
static int64_t SimdSigned(uint64_t v, int bytes) {
  int shift = 64 - bytes * 8;
  return (int64_t)(v << shift) >> shift;
}
static int SimdNan32(uint32_t v) {
  return (v & 0x7f800000u) == 0x7f800000u && (v & 0x007fffffu) != 0;
}
static int SimdNan64(uint64_t v) {
  return (v & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL &&
         (v & 0x000fffffffffffffULL) != 0;
}
static int SimdDenorm32(uint32_t v) {
  return (v & 0x7f800000u) == 0 && (v & 0x007fffffu) != 0;
}
static int SimdDenorm64(uint64_t v) {
  return (v & 0x7ff0000000000000ULL) == 0 && (v & 0x000fffffffffffffULL) != 0;
}

static int MmxPhys(int i) { return (fpu_top(&s->fpu) + i) & 7; }
static uint64_t MmxGet(int i) { return s->fpu.st[MmxPhys(i)].signif; }
static void MmxSet(int i, uint64_t v) {
  int p = MmxPhys(i);
  s->fpu.st[p].signif = v;
  s->fpu.st[p].signExp = 0xffff;  // an MMX image reads as an x87 special
  fpu_mark(&s->fpu, p);
}
static void MmxEmms(void) {
  s->fpu.tw = 0xffff;
  fpu_set_top(&s->fpu, 0);
}

// The source operand (d.rm) as 16 bytes: register or the memory operand with
// `membytes` useful bytes (the rest zero). xmmreg selects XMM vs MMX for a
// register operand.
static void SimdSrc(uint8_t out[16], int membytes, int xmmreg) {
  memset(out, 0, 16);
  if (d.is_mem) {
    seg_use(d.mseg, d.moff, membytes, 0);
    WatchData(d.mlin, membytes, 0);
    for (int i = 0; i < membytes; i += 8) {
      int n = membytes - i > 8 ? 8 : membytes - i;
      uint64_t v = rd64(d.mlin + (uint64_t)i);
      memcpy(out + i, &v, (size_t)n);
    }
  } else if (xmmreg) {
    memcpy(out, s->fpu.xmm[d.rm], 16);
  } else {
    uint64_t v = MmxGet(d.rm);
    memcpy(out, &v, 8);
  }
}

static void SimdDst(const uint8_t in[16], int xmmreg) {
  if (xmmreg)
    memcpy(s->fpu.xmm[d.reg], in, 16);
  else
    MmxSet(d.reg, SimdElem(in, 8, 0));
}

// Numeric exceptions use the MXCSR masks; CR4.OSXMMEXCPT chooses #XM, and
// with it clear the processor reports #UD instead (SDM vol.3 13.5.2).
static void SimdCheckSse(void) {
  if (!fpu_sse_end(&s->fpu)) return;
  if (s->cr4 & kCr4Osxmmexcpt)
    raise_(fr, vec_xm, 0);
  ud();
}

static uint32_t SseF32(uint8_t op, uint32_t a, uint32_t b) {
  if (op == 0x52 || op == 0x53) {  // RCPPS/RSQRTPS: approximate, f32 path
    float af, rf;
    memcpy(&af, &a, 4);
    rf = op == 0x52 ? 1.0f / af : 1.0f / sqrtf(af);
    memcpy(&a, &rf, 4);
    return a;
  }
  if (op == 0x5d || op == 0x5f) {  // MINPS/MAXPS: NaN -> second source
    if (SimdNan32(a) || SimdNan32(b)) return b;
    float af, bf;
    memcpy(&af, &a, 4);
    memcpy(&bf, &b, 4);
    if (op == 0x5d) return af < bf ? a : b;
    return af > bf ? a : b;
  }
  float32_t fa = {a}, fb = {b}, fr;
  switch (op) {
    case 0x51: fr = f32_sqrt(fa); break;
    case 0x58: fr = f32_add(fa, fb); break;
    case 0x59: fr = f32_mul(fa, fb); break;
    case 0x5c: fr = f32_sub(fa, fb); break;
    case 0x5e: fr = f32_div(fa, fb); break;
    default: fr = fb; break;
  }
  return fr.v;
}

static uint64_t SseF64(uint8_t op, uint64_t a, uint64_t b) {
  if (op == 0x5d || op == 0x5f) {
    if (SimdNan64(a) || SimdNan64(b)) return b;
    double ad, bd;
    memcpy(&ad, &a, 8);
    memcpy(&bd, &b, 8);
    if (op == 0x5d) return ad < bd ? a : b;
    return ad > bd ? a : b;
  }
  float64_t fa = {a}, fb = {b}, fr;
  switch (op) {
    case 0x51: fr = f64_sqrt(fa); break;
    case 0x58: fr = f64_add(fa, fb); break;
    case 0x59: fr = f64_mul(fa, fb); break;
    case 0x5c: fr = f64_sub(fa, fb); break;
    case 0x5e: fr = f64_div(fa, fb); break;
    default: fr = fb; break;
  }
  return fr.v;
}

// DAZ: a denormal input element is replaced by a signed zero.
static uint64_t SseInput(uint64_t v, int bytes) {
  if (!fpu_sse_daz(&s->fpu)) return v;
  if (bytes == 4) {
    uint32_t x = (uint32_t)v;
    if ((x & 0x7f800000u) == 0 && (x & 0x007fffffu) != 0) return x & 0x80000000u;
  } else {
    if ((v & 0x7ff0000000000000ULL) == 0 && (v & 0x000fffffffffffffULL) != 0)
      return v & 0x8000000000000000ULL;
  }
  return v;
}

static void SimdFp(uint8_t op2) {
  modrm();
  // Mandatory-prefix choice: F2/F3 are scalar, 66 is packed double, none is
  // packed single (SDM vol.2 ADDPS/ADDPD/ADDSS/ADDSD).
  int sd = d.pf2, ss = d.pf3 && !d.pf2;
  int pd = d.p66 && !sd && !ss;
  int bytes = sd ? 8 : ss ? 4 : 16;
  uint8_t a[16], b[16], r[16];
  memcpy(a, s->fpu.xmm[d.reg], 16);
  SimdSrc(b, bytes, 1);
  int count = (sd || ss) ? 1 : (pd ? 2 : 4);
  int esz = (sd || pd) ? 8 : 4;
  fpu_sse_begin(&s->fpu);
  memcpy(r, a, 16);
  for (int i = 0; i < count; i++) {
    uint64_t raw_a = SimdElem(a, esz, i), raw_b = SimdElem(b, esz, i);
    // MXCSR.DE is the denormal-operand exception; with DAZ set the operand
    // is treated as zero instead (SDM vol.1 11.6.3). SoftFloat 3e has no
    // input-denormal flag, so the check is the encoding's exponent test.
    if (!fpu_sse_daz(&s->fpu) &&
        (esz == 8 ? (SimdDenorm64(raw_a) || SimdDenorm64(raw_b))
                  : (SimdDenorm32((uint32_t)raw_a) || SimdDenorm32((uint32_t)raw_b))))
      s->fpu.mxcsr |= 1u << 1;
    uint64_t av = SseInput(raw_a, esz);
    uint64_t bv = SseInput(raw_b, esz);
    uint64_t rv = esz == 8 ? SseF64(op2, av, bv) : SseF32(op2, (uint32_t)av, (uint32_t)bv);
    if (fpu_sse_ftz(&s->fpu)) {
      uint8_t tmp[8];
      memcpy(tmp, &rv, (size_t)esz);
      fpu_sse_flush(tmp, esz);
      memcpy(&rv, tmp, (size_t)esz);
    }
    SimdSetElem(r, esz, i, rv);
  }
  memcpy(s->fpu.xmm[d.reg], r, 16);
  SimdCheckSse();
}

// Legacy CMPPS/PD/SS/SD predicates (SDM CMPPS): 0 EQ, 1 LT, 2 LE, 3 UNORD,
// 4 NEQ, 5 NLT, 6 NLE, 7 ORD. 1/2/5/6 are signaling (a NaN raises #IA).
static uint32_t SimdPred32(int p, uint32_t a, uint32_t b) {
  if (SimdNan32(a) || SimdNan32(b)) {
    if (p == 3 || p == 7) return p == 3 ? 0xffffffffu : 0;
    if (p == 4) return 0xffffffffu;
    if (p == 0) return 0;
    softfloat_exceptionFlags |= softfloat_flag_invalid;
    return p == 1 || p == 2 ? 0 : 0xffffffffu;
  }
  float af, bf;
  memcpy(&af, &a, 4);
  memcpy(&bf, &b, 4);
  int eq = af == bf, lt = af < bf, le = lt || eq;
  switch (p) {
    case 0: return eq ? 0xffffffffu : 0;
    case 1: return lt ? 0xffffffffu : 0;
    case 2: return le ? 0xffffffffu : 0;
    case 3: return 0;
    case 4: return eq ? 0 : 0xffffffffu;
    case 5: return lt ? 0 : 0xffffffffu;
    case 6: return le ? 0 : 0xffffffffu;
    default: return 0xffffffffu;
  }
}
static uint64_t SimdPred64(int p, uint64_t a, uint64_t b) {
  if (SimdNan64(a) || SimdNan64(b)) {
    if (p == 3 || p == 7) return p == 3 ? ~0ULL : 0;
    if (p == 4) return ~0ULL;
    if (p == 0) return 0;
    softfloat_exceptionFlags |= softfloat_flag_invalid;
    return p == 1 || p == 2 ? 0 : ~0ULL;
  }
  double ad, bd;
  memcpy(&ad, &a, 8);
  memcpy(&bd, &b, 8);
  int eq = ad == bd, lt = ad < bd, le = lt || eq;
  switch (p) {
    case 0: return eq ? ~0ULL : 0;
    case 1: return lt ? ~0ULL : 0;
    case 2: return le ? ~0ULL : 0;
    case 3: return 0;
    case 4: return eq ? 0 : ~0ULL;
    case 5: return lt ? 0 : ~0ULL;
    case 6: return le ? 0 : ~0ULL;
    default: return ~0ULL;
  }
}

static void SimdCmp(uint8_t imm) {
  int sd = d.pf2, ss = d.pf3 && !d.pf2;
  int pd = d.p66 && !sd && !ss;
  int bytes = sd ? 8 : ss ? 4 : 16;
  int count = (sd || ss) ? 1 : (pd ? 2 : 4);
  int esz = (sd || pd) ? 8 : 4;
  uint8_t a[16], b[16], r[16];
  memcpy(a, s->fpu.xmm[d.reg], 16);
  SimdSrc(b, bytes, 1);
  memcpy(r, a, 16);
  fpu_sse_begin(&s->fpu);
  for (int i = 0; i < count; i++) {
    uint64_t av = SseInput(SimdElem(a, esz, i), esz);
    uint64_t bv = SseInput(SimdElem(b, esz, i), esz);
    uint64_t rv = esz == 8 ? SimdPred64(imm & 7, av, bv)
                           : SimdPred32(imm & 7, (uint32_t)av, (uint32_t)bv);
    SimdSetElem(r, esz, i, rv);
  }
  memcpy(s->fpu.xmm[d.reg], r, 16);
  SimdCheckSse();
}

// COMISS/UCOMISS/COMISD/UCOMISD: EFLAGS from the low element, unordered =
// ZF|PF|CF. COMIS* reports a quiet NaN as invalid; UCOMIS* only a signaling
// one (SDM COMISS/UCOMISS).
static void SimdComi(int ucomi) {
  modrm();
  int sd = d.pf2;
  int esz = sd ? 8 : 4;
  uint8_t b[16];
  SimdSrc(b, esz, 1);
  uint64_t a = SimdElem(s->fpu.xmm[d.reg], esz, 0);
  uint64_t bv = SimdElem(b, esz, 0);
  int nan = esz == 8 ? SimdNan64(a) || SimdNan64(bv) : SimdNan32((uint32_t)a) || SimdNan32((uint32_t)bv);
  int snan = 0;
  if (nan) {
    if (esz == 8) {
      uint64_t av = a, bvv = bv;
      snan = (((av >> 51) & 1) == 0 && (av & 0x000fffffffffffffULL) != 0) ||
             (((bvv >> 51) & 1) == 0 && (bvv & 0x000fffffffffffffULL) != 0);
    } else {
      uint32_t av = (uint32_t)a, bvv = (uint32_t)bv;
      snan = (((av >> 22) & 1) == 0 && (av & 0x003fffffu) != 0) ||
             (((bvv >> 22) & 1) == 0 && (bvv & 0x003fffffu) != 0);
    }
  }
  if (nan && (!ucomi || snan)) {
    s->fpu.mxcsr |= 1u;  // MXCSR.IE
    if (((s->fpu.mxcsr & 0x3f) & ~((s->fpu.mxcsr >> 7) & 0x3f)) != 0) {
      if (s->cr4 & kCr4Osxmmexcpt)
        raise_(fr, vec_xm, 0);
      ud();
    }
  }
  enum { kCf = 1u << 0, kPf = 1u << 2, kZf = 1u << 6 };
  uint32_t cc;
  if (nan) {
    cc = kCf | kPf | kZf;
  } else {
    int eq, lt;
    if (esz == 8) {
      double ad, bd;
      memcpy(&ad, &a, 8);
      memcpy(&bd, &bv, 8);
      eq = ad == bd;
      lt = ad < bd;
    } else {
      float af, bf;
      uint32_t a32 = (uint32_t)a, b32 = (uint32_t)bv;
      memcpy(&af, &a32, 4);
      memcpy(&bf, &b32, 4);
      eq = af == bf;
      lt = af < bf;
    }
    cc = eq ? kZf : lt ? kCf : 0;
  }
  fl->word = (fl->word & ~0x8d5u) | cc;
}

// MOVUPS/MOVUPD/MOVSS/MOVSD and the aligned MOVAPS/MOVAPD.
static void SimdMov(uint8_t op2) {
  modrm();
  int store = op2 == 0x11 || op2 == 0x29;
  int sd = d.pf2, ss = d.pf3 && !d.pf2;
  int bytes = sd ? 8 : ss ? 4 : 16;
  if (op2 == 0x28 || op2 == 0x29) {
    if (!d.p66 && (d.pf2 || d.pf3)) ud();
    if (d.is_mem && (d.mlin & 15)) gp_fault(0);
  }
  uint8_t tmp[16];
  if (!store) {
    if (d.is_mem) {
      seg_use(d.mseg, d.moff, bytes, 0);
      WatchData(d.mlin, bytes, 0);
      memset(tmp, 0, 16);
      for (int i = 0; i < bytes; i += 8) {
        int n = bytes - i > 8 ? 8 : bytes - i;
        uint64_t v = rd64(d.mlin + (uint64_t)i);
        memcpy(tmp + i, &v, (size_t)n);
      }
      if (sd || ss) memset(s->fpu.xmm[d.reg], 0, 16);
      memcpy(s->fpu.xmm[d.reg], tmp, (size_t)bytes);
    } else {
      // Register form of MOVSS/MOVSD leaves the destination's upper bits.
      memcpy(tmp, s->fpu.xmm[d.rm], 16);
      memcpy(s->fpu.xmm[d.reg], tmp, (size_t)bytes);
    }
  } else {
    memcpy(tmp, s->fpu.xmm[d.reg], 16);
    if (d.is_mem) {
      seg_use(d.mseg, d.moff, bytes, 1);
      WatchData(d.mlin, bytes, 1);
      for (int i = 0; i < bytes; i += 8) {
        int n = bytes - i > 8 ? 8 : bytes - i;
        uint64_t v = 0;
        memcpy(&v, tmp + i, (size_t)n);
        wr64(d.mlin + (uint64_t)i, v);
      }
    } else {
      // Register destination: MOVSS/MOVSD keep its upper bits.
      memcpy(s->fpu.xmm[d.rm], tmp, (size_t)bytes);
    }
  }
}

// 0F 12/13/16/17: MOVLPS/MOVHPS/MOVLPD/MOVHPD, MOVSLDUP/MOVSHDUP/MOVDDUP.
static void SimdMovHalf(uint8_t op2) {
  modrm();
  if (d.pf3 && op2 == 0x12) {  // MOVSLDUP
    uint8_t b[16];
    SimdSrc(b, 16, 1);
    for (int i = 0; i < 4; i++) SimdSetElem(s->fpu.xmm[d.reg], 4, i, SimdElem(b, 4, i & 1));
    return;
  }
  if (d.pf3 && op2 == 0x16) {  // MOVSHDUP
    uint8_t b[16];
    SimdSrc(b, 16, 1);
    for (int i = 0; i < 4; i++) SimdSetElem(s->fpu.xmm[d.reg], 4, i, SimdElem(b, 4, 2 + (i & 1)));
    return;
  }
  if (d.pf2 && op2 == 0x12) {  // MOVDDUP
    uint8_t b[16];
    SimdSrc(b, 16, 1);
    uint64_t v = SimdElem(b, 8, 0);
    memcpy(s->fpu.xmm[d.reg], &v, 8);
    memcpy(s->fpu.xmm[d.reg] + 8, &v, 8);
    return;
  }
  if (d.pf2 || (d.pf3 && op2 != 0x12 && op2 != 0x16)) ud();
  if (!d.is_mem && !d.p66) {
    // Register forms: 0F 12 is MOVHLPS (dest.low = src.high), 0F 16 is
    // MOVLHPS (dest.high = src.low).
    if (op2 == 0x12) {
      memcpy(s->fpu.xmm[d.reg], s->fpu.xmm[d.rm] + 8, 8);
    } else if (op2 == 0x16) {
      memcpy(s->fpu.xmm[d.reg] + 8, s->fpu.xmm[d.rm], 8);
    } else {
      ud();
    }
    return;
  }
  int high = op2 == 0x16 || op2 == 0x17;
  int store = op2 == 0x13 || op2 == 0x17;
  if (store && !d.is_mem) ud();
  if (store) {
    seg_use(d.mseg, d.moff, 8, 1);
    WatchData(d.mlin, 8, 1);
    uint64_t v;
    memcpy(&v, s->fpu.xmm[d.reg] + (high ? 8 : 0), 8);
    wr64(d.mlin, v);
  } else if (d.is_mem) {
    seg_use(d.mseg, d.moff, 8, 0);
    WatchData(d.mlin, 8, 0);
    uint64_t v = 0;
    for (int i = 0; i < 8; i += 8) v = rd64(d.mlin);
    memcpy(s->fpu.xmm[d.reg] + (high ? 8 : 0), &v, 8);
  } else {
    memcpy(s->fpu.xmm[d.reg] + (high ? 8 : 0), s->fpu.xmm[d.rm], 8);
  }
}

// 0F 6E/7E/6F/7F: MOVD/MOVQ between XMM/MMX and r/m, and the block moves.
static void SimdMovQ(uint8_t op2) {
  modrm();
  if (op2 == 0xd6) {  // MOVQ m64,xmm / MOVQ2DQ / MOVDQ2Q
    if (d.p66) {      // MOVQ m64, xmm
      uint64_t v;
      memcpy(&v, s->fpu.xmm[d.reg], 8);
      if (d.is_mem) {
        seg_use(d.mseg, d.moff, 8, 1);
        WatchData(d.mlin, 8, 1);
        wr64(d.mlin, v);
      } else {
        memcpy(s->fpu.xmm[d.rm], &v, 8);
      }
    } else if (d.pf3) {  // MOVQ2DQ xmm, mm
      uint64_t v = MmxGet(d.rm);
      memset(s->fpu.xmm[d.reg], 0, 16);
      memcpy(s->fpu.xmm[d.reg], &v, 8);
    } else if (d.pf2) {  // MOVDQ2Q mm, xmm
      uint64_t v;
      memcpy(&v, s->fpu.xmm[d.rm], 8);
      MmxSet(d.reg, v);
    } else {
      ud();
    }
    return;
  }
  int xmm = d.p66 || d.pf3;
  if (op2 == 0x6e) {
    uint32_t v;
    if (d.is_mem) {
      seg_use(d.mseg, d.moff, 4, 0);
      WatchData(d.mlin, 4, 0);
      v = (uint32_t)rd32(d.mlin);
    } else {
      v = s->r[d.rm].e;
    }
    if (xmm) {
      memset(s->fpu.xmm[d.reg], 0, 16);
      memcpy(s->fpu.xmm[d.reg], &v, 4);
    } else {
      MmxSet(d.reg, v);
    }
    return;
  }
  if (op2 == 0x7e) {
    if (d.pf3) {  // MOVQ xmm, r/m64 (low half, high zero)
      uint64_t v = 0;
      if (d.is_mem) {
        seg_use(d.mseg, d.moff, 8, 0);
        WatchData(d.mlin, 8, 0);
        v = rd64(d.mlin);
      } else {
        v = s->r[d.rm].q;
      }
      memset(s->fpu.xmm[d.reg], 0, 16);
      memcpy(s->fpu.xmm[d.reg], &v, 8);
      return;
    }
    uint32_t v;
    memcpy(&v, s->fpu.xmm[d.reg], 4);
    if (xmm) {  // 66 0F 7E MOVD r/m32, xmm
      if (d.is_mem) {
        seg_use(d.mseg, d.moff, 4, 1);
        WatchData(d.mlin, 4, 1);
        wr32(d.mlin, v);
      } else {
        s->r[d.rm].e = v;
      }
    } else {  // 0F 7E MOVD r/m32, mm
      uint32_t mv = (uint32_t)MmxGet(d.reg);
      if (d.is_mem) {
        seg_use(d.mseg, d.moff, 4, 1);
        WatchData(d.mlin, 4, 1);
        wr32(d.mlin, mv);
      } else {
        s->r[d.rm].e = mv;
      }
    }
    return;
  }
  // 0x6f load / 0x7f store: 66 = MOVDQA (aligned), F3 = MOVDQU, none = MOVQ.
  int store = op2 == 0x7f;
  int bytes = xmm ? 16 : 8;
  if (xmm && d.is_mem && (d.mlin & 15) && !d.pf3) gp_fault(0);
  if (!store) {
    uint8_t tmp[16];
    if (d.is_mem) {
      seg_use(d.mseg, d.moff, bytes, 0);
      WatchData(d.mlin, bytes, 0);
      memset(tmp, 0, 16);
      for (int i = 0; i < bytes; i += 8) {
        int n = bytes - i > 8 ? 8 : bytes - i;
        uint64_t v = rd64(d.mlin + (uint64_t)i);
        memcpy(tmp + i, &v, (size_t)n);
      }
    } else if (xmm) {
      SimdSrc(tmp, 16, 1);
    } else {
      uint64_t v = MmxGet(d.rm);
      memset(tmp, 0, 16);
      memcpy(tmp, &v, 8);
    }
    SimdDst(tmp, xmm);
  } else {
    uint8_t tmp[16];
    if (xmm) {
      memcpy(tmp, s->fpu.xmm[d.reg], 16);
    } else {
      uint64_t v = MmxGet(d.reg);
      memset(tmp, 0, 16);
      memcpy(tmp, &v, 8);
    }
    if (d.is_mem) {
      seg_use(d.mseg, d.moff, bytes, 1);
      WatchData(d.mlin, bytes, 1);
      for (int i = 0; i < bytes; i += 8) {
        int n = bytes - i > 8 ? 8 : bytes - i;
        uint64_t v = 0;
        memcpy(&v, tmp + i, (size_t)n);
        wr64(d.mlin + (uint64_t)i, v);
      }
    } else if (xmm) {
      memcpy(s->fpu.xmm[d.rm], tmp, 16);
    } else {
      MmxSet(d.rm, SimdElem(tmp, 8, 0));
    }
  }
}

// 0F 50 MOVMSKPS/PD: sign bits of the source elements into a GPR.
static void SimdMovmsk(void) {
  modrm();
  int pd = d.p66, count = pd ? 2 : 4;
  uint32_t m = 0;
  for (int i = 0; i < count; i++)
    m |= (uint32_t)((SimdElem(s->fpu.xmm[d.rm], pd ? 8 : 4, i) >> (pd ? 63 : 31)) & 1) << i;
  if (d.w32)
    s->r[d.reg].e = m;
  else
    s->r[d.reg].x = (uint16_t)m;
}

// 0F 70 PSHUFW/PSHUFD/PSHUFLW/PSHUFHW.
static void SimdShuf(uint8_t imm) {
  if (!d.p66 && !d.pf2 && !d.pf3) {  // PSHUFW mm, mm/m64, imm8
    uint64_t v = 0, r = 0;
    if (d.is_mem) {
      seg_use(d.mseg, d.moff, 8, 0);
      WatchData(d.mlin, 8, 0);
      v = rd64(d.mlin);
    } else {
      v = MmxGet(d.rm);
    }
    for (int i = 0; i < 4; i++) {
      uint64_t e = (v >> (((imm >> (2 * i)) & 3) * 16)) & 0xffff;
      r |= e << (16 * i);
    }
    MmxSet(d.reg, r);
    return;
  }
  if (d.pf3 || d.pf2) {  // PSHUFHW / PSHUFLW: half the words, rest preserved
    int high = d.pf3;
    uint8_t b[16], r[16];
    SimdSrc(b, 16, 1);
    memcpy(r, s->fpu.xmm[d.reg], 16);
    for (int i = 0; i < 4; i++) {
      int src = (imm >> (2 * i)) & 3;
      uint64_t e = SimdElem(b, 2, (high ? 4 : 0) + src);
      SimdSetElem(r, 2, (high ? 4 : 0) + i, e);
    }
    memcpy(s->fpu.xmm[d.reg], r, 16);
    return;
  }
  // PSHUFD
  uint8_t b[16];
  SimdSrc(b, 16, 1);
  for (int i = 0; i < 4; i++)
    SimdSetElem(s->fpu.xmm[d.reg], 4, i, SimdElem(b, 4, (imm >> (2 * i)) & 3));
}

// 0F C6 SHUFPS/SHUFPD.
static void SimdShufp(uint8_t imm) {
  int pd = d.p66;
  uint8_t a[16], b[16], r[16];
  memcpy(a, s->fpu.xmm[d.reg], 16);
  SimdSrc(b, 16, 1);
  if (!pd) {
    SimdSetElem(r, 4, 0, SimdElem(a, 4, imm & 3));
    SimdSetElem(r, 4, 1, SimdElem(a, 4, (imm >> 2) & 3));
    SimdSetElem(r, 4, 2, SimdElem(b, 4, (imm >> 4) & 3));
    SimdSetElem(r, 4, 3, SimdElem(b, 4, (imm >> 6) & 3));
  } else {
    SimdSetElem(r, 8, 0, SimdElem(a, 8, imm & 1));
    SimdSetElem(r, 8, 1, SimdElem(b, 8, (imm >> 1) & 1));
  }
  memcpy(s->fpu.xmm[d.reg], r, 16);
}

// 0F C4 PINSRW / 0F C5 PEXTRW.
static void SimdInsExtrWord(uint8_t op2, uint8_t imm) {
  int sel = imm & 7;
  int xmm = d.p66;
  if (op2 == 0xc4) {
    uint16_t v;
    if (d.is_mem) {
      seg_use(d.mseg, d.moff, 2, 0);
      WatchData(d.mlin, 2, 0);
      v = rd16(d.mlin);
    } else {
      v = (uint16_t)s->r[d.rm].x;
    }
    if (xmm) {
      SimdSetElem(s->fpu.xmm[d.reg], 2, sel, v);
    } else {
      uint64_t m = MmxGet(d.reg);
      m = (m & ~(0xffffULL << (16 * sel))) | ((uint64_t)v << (16 * sel));
      MmxSet(d.reg, m);
    }
    return;
  }
  // PEXTRW: destination is a GPR, source r/m is MMX/XMM (register only in
  // practice; a memory source is not encodable for this form).
  uint64_t src = xmm ? SimdElem(s->fpu.xmm[d.rm], 2, sel)
                     : ((MmxGet(d.rm) >> (16 * sel)) & 0xffff);
  s->r[d.reg].e = (uint32_t)src;
}

static uint64_t SatAddSigned(uint64_t a, uint64_t b, int bytes) {
  int bits = bytes * 8;
  int64_t lo = -(1LL << (bits - 1)), hi = (1LL << (bits - 1)) - 1;
  int64_t s = SimdSigned(a, bytes) + SimdSigned(b, bytes);
  if (s < lo) s = lo;
  if (s > hi) s = hi;
  return (uint64_t)s & SimdMask(bytes);
}
static uint64_t SatAddUnsigned(uint64_t a, uint64_t b, int bytes) {
  uint64_t m = SimdMask(bytes), s = (a & m) + (b & m);
  return s > m ? m : s;
}
static uint64_t SatSubSigned(uint64_t a, uint64_t b, int bytes) {
  int bits = bytes * 8;
  int64_t lo = -(1LL << (bits - 1)), hi = (1LL << (bits - 1)) - 1;
  int64_t s = SimdSigned(a, bytes) - SimdSigned(b, bytes);
  if (s < lo) s = lo;
  if (s > hi) s = hi;
  return (uint64_t)s & SimdMask(bytes);
}
static uint64_t SatSubUnsigned(uint64_t a, uint64_t b, int bytes) {
  uint64_t m = SimdMask(bytes), x = a & m, y = b & m;
  return x > y ? x - y : 0;
}
static uint64_t SatPackSigned(uint64_t v, int frombytes, int tobytes) {
  int bits = tobytes * 8;
  int64_t lo = -(1LL << (bits - 1)), hi = (1LL << (bits - 1)) - 1;
  int64_t s = SimdSigned(v, frombytes);
  if (s < lo) s = lo;
  if (s > hi) s = hi;
  return (uint64_t)s & SimdMask(tobytes);
}
static uint64_t SatPackUnsigned(uint64_t v, int frombytes, int tobytes) {
  int64_t s = SimdSigned(v, frombytes);
  uint64_t hi = SimdMask(tobytes);
  if (s < 0) return 0;
  return (uint64_t)s > hi ? hi : (uint64_t)s;
}

// The packed-integer / MMX opcode families (SDM vol.2 PADD*, PSUB*, PCMP*,
// PSHUF*, PACK*, PUNPCK*, PSLL/PSRL/PSRA, PMUL*, PSADBW, PAND/POR/PXOR).
static void SimdInteger(uint8_t op2) {
  if (op2 == 0x77) {
    fr->rec.mnemonic = "emms";
    MmxEmms();
    return;
  }
  if (op2 == 0xd7) {  // PMOVMSKB r32, mm/xmm
    modrm();
    fr->rec.mnemonic = "pmovmskb";
    int bytes = d.p66 ? 16 : 8;
    uint8_t b[16];
    SimdSrc(b, bytes, d.p66);
    uint32_t m = 0;
    for (int i = 0; i < bytes; i++) m |= (uint32_t)((b[i] >> 7) & 1) << i;
    s->r[d.reg].e = m;
    return;
  }
  if (op2 == 0xe7) {  // MOVNTQ / MOVNTDQ, memory only
    modrm();
    fr->rec.mnemonic = "movnt";
    if (!d.is_mem) ud();
    int bytes = d.p66 ? 16 : 8;
    seg_use(d.mseg, d.moff, bytes, 1);
    WatchData(d.mlin, bytes, 1);
    uint8_t b[16];
    if (d.p66) {
      memcpy(b, s->fpu.xmm[d.reg], 16);
    } else {
      uint64_t v = MmxGet(d.reg);
      memset(b, 0, 16);
      memcpy(b, &v, 8);
    }
    for (int i = 0; i < bytes; i += 8) {
      int n = bytes - i > 8 ? 8 : bytes - i;
      uint64_t v = 0;
      memcpy(&v, b + i, (size_t)n);
      wr64(d.mlin + (uint64_t)i, v);
    }
    return;
  }
  if (op2 == 0xf7) {  // MASKMOVQ/MASKMOVDQU: store bytes selected by mask MSBs
    modrm();
    int xmm = d.p66;
    int n = xmm ? 16 : 8;
    uint8_t src[16], mask[16];
    if (xmm) {
      memcpy(src, s->fpu.xmm[d.reg], 16);
      memcpy(mask, s->fpu.xmm[d.rm], 16);
    } else {
      uint64_t a = MmxGet(d.reg), b = MmxGet(d.rm);
      memset(src, 0, 16);
      memset(mask, 0, 16);
      memcpy(src, &a, 8);
      memcpy(mask, &b, 8);
    }
    uint32_t dioff = d.a32 ? s->r[edi_i].e : s->r[edi_i].x;
    for (int i = 0; i < n; i++) {
      if (!(mask[i] & 0x80)) continue;
      // Only the selected bytes are stored, so only they are checked: an
      // unselected byte past the ES limit is not accessed (SDM MASKMOVQ).
      seg_use(es_i, dioff + (uint32_t)i, 1, 1);
      WatchData(s->base[es_i] + dioff + (uint32_t)i, 1, 1);
      wr8(s->base[es_i] + dioff + (uint32_t)i, src[i]);
    }
    fr->rec.mnemonic = "maskmov";
    return;
  }
  if (op2 == 0xf0) {      // LDDQU (F2): unaligned 16-byte load
    modrm();
    fr->rec.mnemonic = "lddqu";
    if (!d.pf2) ud();
    uint8_t b[16];
    SimdSrc(b, 16, 1);
    memcpy(s->fpu.xmm[d.reg], b, 16);
    return;
  }
  if (op2 == 0x71 || op2 == 0x72 || op2 == 0x73) {
    // Shift group with an imm8; reg field selects the operation.
    modrm();
    uint8_t imm = imm8();
    int xmm = d.p66;
    int bytes = op2 == 0x71 ? 2 : op2 == 0x72 ? 4 : 8;
    uint64_t cnt = (imm > (uint64_t)bytes * 8) ? (uint64_t)bytes * 8 : imm;
    uint8_t a[16], b[16];
    int nreg = xmm ? 16 : 8;
    if (xmm)
      memcpy(a, s->fpu.xmm[d.rm], 16);
    else {
      uint64_t v = MmxGet(d.rm);
      memset(a, 0, 16);
      memcpy(a, &v, 8);
    }
    memset(b, 0, 16);
    for (int i = 0; i < nreg; i += bytes) {
      uint64_t v = SimdElem(a, bytes, i / bytes);
      switch (d.reg) {
        case 2: v = (v & SimdMask(bytes)) >> cnt; break;           // PSRLW/D/Q
        case 4: v = (uint64_t)(SimdSigned(v, bytes) >> cnt); break; // PSRAW/D
        case 6: v = (v & SimdMask(bytes)) << cnt; break;           // PSLLW/D/Q
        default: ud();
      }
      SimdSetElem(b, bytes, i / bytes, v);
    }
    // The reg/opcode fields: 71 /2 PSRLW, /4 PSRAW, /6 PSLLW; 72 /2 PSRLD,
    // /4 PSRAD, /6 PSLLD; 73 /2 PSRLQ, /6 PSLLQ (no PSRAQ in SSE2). The
    // destination of a group form is rm, not reg (SDM PSLLW et al.).
    if (xmm)
      memcpy(s->fpu.xmm[d.rm], b, 16);
    else
      MmxSet(d.rm, SimdElem(b, 8, 0));
    return;
  }
  modrm();
  int xmm = d.p66;
  int total = xmm ? 16 : 8;
  uint8_t a[16], b[16];
  if (xmm)
    memcpy(a, s->fpu.xmm[d.reg], 16);
  else {
    uint64_t v = MmxGet(d.reg);
    memset(a, 0, 16);
    memcpy(a, &v, 8);
  }
  SimdSrc(b, total, xmm);
  uint8_t r[16];
  memcpy(r, a, 16);
  int bytes = 1;
  switch (op2) {
    case 0x60: case 0x68: bytes = 1; goto unpack;
    case 0x61: case 0x69: bytes = 2; goto unpack;
    case 0x62: case 0x6a: bytes = 4; goto unpack;
    case 0x6c: case 0x6d: bytes = 8; goto unpack;
    default: break;
  }
  // PUNPCKL/H: interleave elements from a and b, low or high half.
  if (op2 == 0x60 || op2 == 0x61 || op2 == 0x62 || op2 == 0x6c ||
      op2 == 0x68 || op2 == 0x69 || op2 == 0x6a || op2 == 0x6d) {
  unpack: {
      int high = op2 == 0x68 || op2 == 0x69 || op2 == 0x6a || op2 == 0x6d;
      int n = total / bytes;
      int half = n / 2;
      for (int i = 0; i < n; i++) {
        int src = (i / 2) + (high ? half : 0);
        uint64_t e = (i & 1) ? SimdElem(b, bytes, src) : SimdElem(a, bytes, src);
        SimdSetElem(r, bytes, i, e);
      }
      SimdDst(r, xmm);
      return;
    }
  }
  switch (op2) {
    case 0x63: {  // PACKSSWB: 8/16 words -> 8/16 bytes
      int half = total / 2;
      for (int i = 0; i < total; i++) {
        const uint8_t* s = i < half ? a : b;
        SimdSetElem(r, 1, i, SatPackSigned(SimdElem(s, 2, i % half), 2, 1));
      }
      SimdDst(r, xmm);
      return;
    }
    case 0x67: {  // PACKUSWB
      int half = total / 2;
      for (int i = 0; i < total; i++) {
        const uint8_t* s = i < half ? a : b;
        SimdSetElem(r, 1, i, SatPackUnsigned(SimdElem(s, 2, i % half), 2, 1));
      }
      SimdDst(r, xmm);
      return;
    }
    case 0x6b: {  // PACKSSDW: 4/8 dwords -> 4/8 words
      int n = total / 2, half = n / 2;
      for (int i = 0; i < n; i++) {
        const uint8_t* s = i < half ? a : b;
        SimdSetElem(r, 2, i, SatPackSigned(SimdElem(s, 4, i % half), 4, 2));
      }
      SimdDst(r, xmm);
      return;
    }
    case 0x64: case 0x65: case 0x66: {  // PCMPGTB/W/D (signed)
      bytes = op2 == 0x64 ? 1 : op2 == 0x65 ? 2 : 4;
      int n = total / bytes;
      for (int i = 0; i < n; i++) {
        int64_t av = SimdSigned(SimdElem(a, bytes, i), bytes);
        int64_t bv = SimdSigned(SimdElem(b, bytes, i), bytes);
        SimdSetElem(r, bytes, i, av > bv ? SimdMask(bytes) : 0);
      }
      SimdDst(r, xmm);
      return;
    }
    case 0x74: case 0x75: case 0x76: {  // PCMPEQB/W/D
      bytes = op2 == 0x74 ? 1 : op2 == 0x75 ? 2 : 4;
      int n = total / bytes;
      for (int i = 0; i < n; i++)
        SimdSetElem(r, bytes, i, SimdElem(a, bytes, i) == SimdElem(b, bytes, i) ? SimdMask(bytes) : 0);
      SimdDst(r, xmm);
      return;
    }
    case 0xdb: case 0xdf: case 0xeb: case 0xef:  // PAND/PANDN/POR/PXOR
      for (int i = 0; i < total; i++) {
        uint8_t av = a[i], bv = b[i], z;
        if (op2 == 0xdb) z = av & bv;
        else if (op2 == 0xdf) z = (uint8_t)(~av & bv);
        else if (op2 == 0xeb) z = av | bv;
        else z = av ^ bv;
        r[i] = z;
      }
      SimdDst(r, xmm);
      return;
    case 0xd4: case 0xfb: {  // PADDQ / PSUBQ
      for (int i = 0; i < total; i += 8)
        SimdSetElem(r, 8, i / 8, SimdElem(a, 8, i / 8) + (op2 == 0xd4 ? SimdElem(b, 8, i / 8) : -SimdElem(b, 8, i / 8)));
      SimdDst(r, xmm);
      return;
    }
    case 0xd5: case 0xe5: case 0xe4: {  // PMULLW / PMULHW / PMULHUW
      for (int i = 0; i < total; i += 2) {
        uint64_t av = SimdElem(a, 2, i / 2), bv = SimdElem(b, 2, i / 2), z;
        if (op2 == 0xd5) z = (av * bv) & 0xffff;
        else if (op2 == 0xe4) z = ((av * bv) >> 16) & 0xffff;
        else z = ((uint64_t)(SimdSigned(av, 2) * SimdSigned(bv, 2)) >> 16) & 0xffff;
        SimdSetElem(r, 2, i / 2, z);
      }
      SimdDst(r, xmm);
      return;
    }
    case 0xf4: {  // PMULUDQ
      for (int i = 0; i < total; i += 8) {
        uint64_t av = SimdElem(a, 4, (i / 8) * 2), bv = SimdElem(b, 4, (i / 8) * 2);
        SimdSetElem(r, 8, i / 8, av * bv);
      }
      SimdDst(r, xmm);
      return;
    }
    case 0xf5: {  // PMADDWD
      for (int i = 0; i < total; i += 4) {
        int64_t a0 = SimdSigned(SimdElem(a, 2, (i / 4) * 2), 2);
        int64_t a1 = SimdSigned(SimdElem(a, 2, (i / 4) * 2 + 1), 2);
        int64_t b0 = SimdSigned(SimdElem(b, 2, (i / 4) * 2), 2);
        int64_t b1 = SimdSigned(SimdElem(b, 2, (i / 4) * 2 + 1), 2);
        SimdSetElem(r, 4, i / 4, (uint32_t)(a0 * b0 + a1 * b1));
      }
      SimdDst(r, xmm);
      return;
    }
    case 0xf6: {  // PSADBW
      for (int i = 0; i < total; i += 8) {
        uint32_t sum = 0;
        for (int j = 0; j < 8; j++) {
          int av = (int)SimdElem(a, 1, (i / 8) * 8 + j);
          int bv = (int)SimdElem(b, 1, (i / 8) * 8 + j);
          sum += (uint32_t)(av > bv ? av - bv : bv - av);
        }
        SimdSetElem(r, 8, i / 8, sum);
      }
      SimdDst(r, xmm);
      return;
    }
    case 0xd1: case 0xd2: case 0xd3: case 0xe1: case 0xe2:
    case 0xf1: case 0xf2: case 0xf3: {  // PSRL/PSRA/PSLL by xmm/m64 count
      int wbytes = (op2 == 0xd1 || op2 == 0xe1 || op2 == 0xf1) ? 2
                   : (op2 == 0xd3 || op2 == 0xf3) ? 8 : 4;
      uint64_t cnt = SimdElem(b, 8, 0);
      int n = total / wbytes;
      for (int i = 0; i < n; i++) {
        uint64_t v = SimdElem(a, wbytes, i), z;
        uint64_t c = cnt > (uint64_t)wbytes * 8 ? (uint64_t)wbytes * 8 : cnt;
        if (op2 == 0xd1 || op2 == 0xd2 || op2 == 0xd3) z = (v & SimdMask(wbytes)) >> c;
        else if (op2 == 0xe1 || op2 == 0xe2) z = (uint64_t)(SimdSigned(v, wbytes) >> c);
        else z = (v & SimdMask(wbytes)) << c;
        SimdSetElem(r, wbytes, i, z & SimdMask(wbytes));
      }
      SimdDst(r, xmm);
      return;
    }
    case 0xd8: case 0xd9: case 0xda: case 0xdc: case 0xdd: case 0xde:
    case 0xe0: case 0xe3: case 0xe8: case 0xe9: case 0xea: case 0xec: case 0xed: case 0xee:
    case 0xf8: case 0xf9: case 0xfa: case 0xfc: case 0xfd: case 0xfe: {
      // Saturating / averaging / min-max arithmetic at element widths.
      switch (op2) {
        case 0xd8: case 0xdc: case 0xe0: case 0xe8: case 0xec: case 0xf8: case 0xfc: bytes = 1; break;
        case 0xd9: case 0xdd: case 0xe3: case 0xe9: case 0xed: case 0xf9: case 0xfd: bytes = 2; break;
        default: bytes = 4; break;
      }
      int n = total / bytes;
      for (int i = 0; i < n; i++) {
        uint64_t av = SimdElem(a, bytes, i), bv = SimdElem(b, bytes, i), z;
        switch (op2) {
          case 0xd8: z = SatSubUnsigned(av, bv, bytes); break;
          case 0xd9: z = SatSubUnsigned(av, bv, bytes); break;
          case 0xda: z = (av & SimdMask(bytes)) < (bv & SimdMask(bytes)) ? av : bv; break;
          case 0xdc: z = SatAddUnsigned(av, bv, bytes); break;
          case 0xdd: z = SatAddUnsigned(av, bv, bytes); break;
          case 0xde: z = (av & SimdMask(bytes)) > (bv & SimdMask(bytes)) ? av : bv; break;
          case 0xe0: z = ((av & SimdMask(bytes)) + (bv & SimdMask(bytes)) + 1) >> 1; break;
          case 0xe3: z = ((av & SimdMask(bytes)) + (bv & SimdMask(bytes)) + 1) >> 1; break;
          case 0xe8: z = SatSubSigned(av, bv, bytes); break;
          case 0xe9: z = SatSubSigned(av, bv, bytes); break;
          case 0xea: z = SimdSigned(av, bytes) < SimdSigned(bv, bytes) ? av : bv; break;
          case 0xec: z = SatAddSigned(av, bv, bytes); break;
          case 0xed: z = SatAddSigned(av, bv, bytes); break;
          case 0xee: z = SimdSigned(av, bytes) > SimdSigned(bv, bytes) ? av : bv; break;
          case 0xf8: z = (av - bv) & SimdMask(bytes); break;
          case 0xf9: z = (av - bv) & SimdMask(bytes); break;
          case 0xfa: z = (av - bv) & SimdMask(bytes); break;
          case 0xfc: z = (av + bv) & SimdMask(bytes); break;
          case 0xfd: z = (av + bv) & SimdMask(bytes); break;
          default: z = (av + bv) & SimdMask(bytes); break;
        }
        SimdSetElem(r, bytes, i, z);
      }
      SimdDst(r, xmm);
      return;
    }
    default:
      ud();
  }
}

// 0F 2A/2C/2D/5A/5B/E6: the scalar and packed conversion families.
static void SimdConvert(uint8_t op2) {
  modrm();
  int sd = d.pf2, ss = d.pf3 && !d.pf2;
  uint8_t a[16], r[16];
  memcpy(a, s->fpu.xmm[d.reg], 16);
  memcpy(r, a, 16);
  if (op2 == 0x2a) {  // CVTSI2SS/SD, CVTPI2PS/PD
    if (ss || sd) {
      uint32_t iv;
      if (d.is_mem) {
        seg_use(d.mseg, d.moff, 4, 0);
        WatchData(d.mlin, 4, 0);
        iv = (uint32_t)rd32(d.mlin);
      } else {
        iv = s->r[d.rm].e;
      }
      fpu_sse_begin(&s->fpu);
      if (ss) {
        float32_t f = i32_to_f32((int32_t)iv);
        memcpy(r, &f.v, 4);
      } else {
        float64_t f = i32_to_f64((int32_t)iv);
        memcpy(r, &f.v, 8);
      }
      memcpy(s->fpu.xmm[d.reg], r, 16);
      SimdCheckSse();
    } else {
      uint64_t m;
      if (d.is_mem) {
        seg_use(d.mseg, d.moff, 8, 0);
        WatchData(d.mlin, 8, 0);
        m = rd64(d.mlin);
      } else {
        m = MmxGet(d.rm);
      }
      int pd = d.p66;
      fpu_sse_begin(&s->fpu);
      for (int i = 0; i < 2; i++) {
        int32_t iv = (int32_t)(uint32_t)(m >> (32 * i));
        if (pd) {
          float64_t f = i32_to_f64(iv);
          memcpy(r + 8 * i, &f.v, 8);
        } else {
          float32_t f = i32_to_f32(iv);
          memcpy(r + 4 * i, &f.v, 4);
        }
      }
      memset(s->fpu.xmm[d.reg], 0, 16);
      memcpy(s->fpu.xmm[d.reg], r, pd ? 16 : 8);
      SimdCheckSse();
    }
    return;
  }
  if (op2 == 0x2c || op2 == 0x2d) {  // CVT[T]SS2SI/SD2SI and MMX forms
    int trunc = op2 == 0x2c;
    uint8_t b[16];
    SimdSrc(b, (ss || sd) ? (ss ? 4 : 8) : 16, 1);
    fpu_sse_begin(&s->fpu);
    if (ss || sd) {
      int32_t q;
      if (ss) {
        float32_t f;
        memcpy(&f.v, b, 4);
        q = (int32_t)(trunc ? f32_to_i32_r_minMag(f, true)
                            : f32_to_i32(f, softfloat_roundingMode, true));
      } else {
        float64_t f;
        memcpy(&f.v, b, 8);
        q = (int32_t)(trunc ? f64_to_i32_r_minMag(f, true)
                            : f64_to_i32(f, softfloat_roundingMode, true));
      }
      s->r[d.reg].e = (uint32_t)q;
    } else {
      int pd = d.p66;
      memset(r, 0, 16);
      for (int i = 0; i < 2; i++) {
        int32_t q;
        if (pd) {
          float64_t f;
          memcpy(&f.v, b + 8 * i, 8);
          q = (int32_t)(trunc ? f64_to_i32_r_minMag(f, true)
                              : f64_to_i32(f, softfloat_roundingMode, true));
        } else {
          float32_t f;
          memcpy(&f.v, b + 4 * i, 4);
          q = (int32_t)(trunc ? f32_to_i32_r_minMag(f, true)
                              : f32_to_i32(f, softfloat_roundingMode, true));
        }
        memcpy(r + 4 * i, &q, 4);
      }
      SimdDst(r, 0);
    }
    SimdCheckSse();
    return;
  }
  if (op2 == 0x5a) {
    uint8_t b[16];
    if (ss) {  // CVTSS2SD
      SimdSrc(b, 4, 1);
      float32_t f;
      memcpy(&f.v, b, 4);
      fpu_sse_begin(&s->fpu);
      float64_t d64 = f32_to_f64(f);
      memcpy(r, &d64.v, 8);
      memcpy(s->fpu.xmm[d.reg], r, 16);
      SimdCheckSse();
    } else if (sd) {  // CVTSD2SS
      SimdSrc(b, 8, 1);
      float64_t f;
      memcpy(&f.v, b, 8);
      fpu_sse_begin(&s->fpu);
      float32_t f32 = f64_to_f32(f);
      memcpy(r, &f32.v, 4);
      memcpy(s->fpu.xmm[d.reg], r, 16);
      SimdCheckSse();
    } else if (d.p66) {  // CVTPD2PS: high 64 of dest zeroed
      SimdSrc(b, 16, 1);
      fpu_sse_begin(&s->fpu);
      memset(r, 0, 16);
      for (int i = 0; i < 2; i++) {
        float64_t f;
        memcpy(&f.v, b + 8 * i, 8);
        float32_t f32 = f64_to_f32(f);
        memcpy(r + 4 * i, &f32.v, 4);
      }
      memcpy(s->fpu.xmm[d.reg], r, 16);
      SimdCheckSse();
    } else {  // CVTPS2PD
      SimdSrc(b, 8, 1);
      fpu_sse_begin(&s->fpu);
      for (int i = 0; i < 2; i++) {
        float32_t f;
        memcpy(&f.v, b + 4 * i, 4);
        float64_t d64 = f32_to_f64(f);
        memcpy(r + 8 * i, &d64.v, 8);
      }
      memcpy(s->fpu.xmm[d.reg], r, 16);
      SimdCheckSse();
    }
    return;
  }
  if (op2 == 0x5b) {  // CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ
    uint8_t b[16];
    SimdSrc(b, 16, 1);
    fpu_sse_begin(&s->fpu);
    if (d.pf3) {  // CVTTPS2DQ
      for (int i = 0; i < 4; i++) {
        float32_t f;
        memcpy(&f.v, b + 4 * i, 4);
        int32_t q = (int32_t)f32_to_i32_r_minMag(f, true);
        memcpy(r + 4 * i, &q, 4);
      }
    } else if (d.p66) {  // CVTPS2DQ
      for (int i = 0; i < 4; i++) {
        float32_t f;
        memcpy(&f.v, b + 4 * i, 4);
        int32_t q = (int32_t)f32_to_i32(f, softfloat_roundingMode, true);
        memcpy(r + 4 * i, &q, 4);
      }
    } else {  // CVTDQ2PS
      for (int i = 0; i < 4; i++) {
        int32_t q;
        memcpy(&q, b + 4 * i, 4);
        float32_t f = i32_to_f32(q);
        memcpy(r + 4 * i, &f.v, 4);
      }
    }
    memcpy(s->fpu.xmm[d.reg], r, 16);
    SimdCheckSse();
    return;
  }
  if (op2 == 0xe6) {  // CVTDQ2PD (F3), CVTPD2DQ (66), CVTTPD2DQ (F2)
    uint8_t b[16];
    SimdSrc(b, 16, 1);
    fpu_sse_begin(&s->fpu);
    memset(r, 0, 16);
    if (d.pf3) {  // CVTDQ2PD: two dwords -> two doubles
      for (int i = 0; i < 2; i++) {
        int32_t q;
        memcpy(&q, b + 4 * i, 4);
        float64_t f = i32_to_f64(q);
        memcpy(r + 8 * i, &f.v, 8);
      }
    } else if (d.pf2) {  // CVTTPD2DQ
      for (int i = 0; i < 2; i++) {
        float64_t f;
        memcpy(&f.v, b + 8 * i, 8);
        int32_t q = (int32_t)f64_to_i32_r_minMag(f, true);
        memcpy(r + 4 * i, &q, 4);
      }
    } else {  // CVTPD2DQ (66)
      for (int i = 0; i < 2; i++) {
        float64_t f;
        memcpy(&f.v, b + 8 * i, 8);
        int32_t q = (int32_t)f64_to_i32(f, softfloat_roundingMode, true);
        memcpy(r + 4 * i, &q, 4);
      }
    }
    memcpy(s->fpu.xmm[d.reg], r, 16);
    SimdCheckSse();
    return;
  }
  ud();
}

static void SimdOp(uint8_t op2) {
  // MMX/SSE share the FPU's lazy-context machinery: TS means #NM and EM
  // means the unit is emulated by software, which for these encodings is
  // #UD (SDM vol.3 2.5, vol.1 11.6.3).
  if (s->cr0 & kCr0Ts) nm();
  if (s->cr0 & kCr0Em) ud();
  switch (op2) {
    case 0x10: case 0x11: case 0x28: case 0x29:
      fr->rec.mnemonic = op2 == 0x10 ? "movups" : op2 == 0x11 ? "movups" : "movaps";
      SimdMov(op2);
      break;
    case 0x12: case 0x13: case 0x16: case 0x17:
      fr->rec.mnemonic = "movlh";
      SimdMovHalf(op2);
      break;
    case 0x14: case 0x15: {  // UNPCKLPS/PD, UNPCKHPS/PD
      modrm();
      int pd = d.p66;
      int n = pd ? 2 : 4, esz = pd ? 8 : 4, half = n / 2;
      uint8_t a[16], b[16], r[16];
      memcpy(a, s->fpu.xmm[d.reg], 16);
      SimdSrc(b, 16, 1);
      int base = op2 == 0x15 ? half : 0;
      for (int i = 0; i < n; i++) {
        int src = base + i / 2;
        SimdSetElem(r, esz, i, SimdElem((i & 1) ? b : a, esz, src));
      }
      memcpy(s->fpu.xmm[d.reg], r, 16);
      fr->rec.mnemonic = op2 == 0x14 ? "unpckl" : "unpckh";
      break;
    }
    case 0x2b: {  // MOVNTPS/MOVNTPD: a non-temporal 16-byte store
      modrm();
      fr->rec.mnemonic = "movntps";
      if (!d.is_mem) ud();
      seg_use(d.mseg, d.moff, 16, 1);
      WatchData(d.mlin, 16, 1);
      for (int i = 0; i < 16; i += 8) {
        uint64_t v;
        memcpy(&v, s->fpu.xmm[d.reg] + i, 8);
        wr64(d.mlin + (uint64_t)i, v);
      }
      break;
    }
    case 0x2e: fr->rec.mnemonic = "ucomiss"; SimdComi(1); break;
    case 0x2f: fr->rec.mnemonic = "comiss"; SimdComi(0); break;
    case 0x2a: case 0x2c: case 0x2d: case 0x5a: case 0x5b: case 0xe6:
      fr->rec.mnemonic = "cvt";
      SimdConvert(op2);
      break;
    case 0x50:
      fr->rec.mnemonic = "movmsk";
      SimdMovmsk();
      break;
    case 0x51: case 0x52: case 0x53: case 0x58: case 0x59:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f:
      fr->rec.mnemonic = "sse-arith";
      SimdFp(op2);
      break;
    case 0x6e: case 0x7e: case 0x6f: case 0x7f: case 0xd6:
      fr->rec.mnemonic = "movd/q";
      SimdMovQ(op2);
      break;
    case 0x70: {
      modrm();
      uint8_t imm = imm8();
      fr->rec.mnemonic = "pshuf";
      SimdShuf(imm);
      break;
    }
    case 0x77:
      SimdInteger(op2);
      break;
    case 0xc2: {
      modrm();
      uint8_t imm = imm8();
      fr->rec.mnemonic = "cmpps";
      SimdCmp(imm);
      break;
    }
    case 0xc3: {  // MOVNTI m32, r32 (SDM vol.2): a 32-bit store to memory is
                  // the only form; a 16-bit operand size and the register
                  // encoding are #UD (QEMU checks dflag == 2 the same way).
      modrm();
      fr->rec.mnemonic = "movnti";
      if (!d.is_mem || !d.w32) ud();
      seg_use(d.mseg, d.moff, 4, 1);
      WatchData(d.mlin, 4, 1);
      wr32(d.mlin, s->r[d.reg].e);
      break;
    }
    case 0xc4: case 0xc5: {
      modrm();
      uint8_t imm = imm8();
      fr->rec.mnemonic = op2 == 0xc4 ? "pinsrw" : "pextrw";
      SimdInsExtrWord(op2, imm);
      break;
    }
    case 0xc6: {
      modrm();
      uint8_t imm = imm8();
      fr->rec.mnemonic = "shufps";
      SimdShufp(imm);
      break;
    }
    default:
      if ((op2 >= 0x60 && op2 <= 0x76) || (op2 >= 0xd1 && op2 <= 0xdf) ||
          (op2 >= 0xe0 && op2 <= 0xef) || (op2 >= 0xf0 && op2 <= 0xfe)) {
        fr->rec.mnemonic = "sse-int";
        SimdInteger(op2);
      } else {
        ud();
      }
      break;
  }
}

static void run_op2(uint8_t op2) {
  // The MMX/SSE/SSE2 map: 0F 10-17, 28-2F, 50-77, 7E/7F, C2-C6, D1-DF,
  // E0-EF and F0-FE. Their mandatory prefixes are consumed by SimdOp.
  if ((op2 >= 0x10 && op2 <= 0x17) || (op2 >= 0x28 && op2 <= 0x2f) ||
      (op2 >= 0x50 && op2 <= 0x77) || op2 == 0x7e || op2 == 0x7f ||
      (op2 >= 0xc2 && op2 <= 0xc6) || (op2 >= 0xd1 && op2 <= 0xdf) ||
      (op2 >= 0xe0 && op2 <= 0xef) || (op2 >= 0xf0 && op2 <= 0xfe)) {
    SimdOp(op2);
    return;
  }
  switch (op2) {
    case 0x00: {  // 0f 00 group: sldt/str/lldt/ltr/verr/verw (reg field)
      modrm();
      switch (d.reg) {
        case 0:  // sldt: the visible LDTR (SDM vol.2 SLDT; unprivileged, but
                 // the instruction is not recognized outside protected mode)
          fr->rec.mnemonic = "sldt";
          if (!(s->cr0 & 1) || fl->vm) ud();
          set_sys_sel(s->ldtr);
          break;
        case 1:  // str: the visible TR selector (SDM vol.2 STR; same mode rule)
          fr->rec.mnemonic = "str";
          if (!(s->cr0 & 1) || fl->vm) ud();
          set_sys_sel(s->tr);
          break;
        case 2: {  // lldt (privileged: SDM vol.2)
          fr->rec.mnemonic = "lldt";
          if (!(s->cr0 & 1)) ud();  // PM-only (SDM vol.2 LLDT)
          if (cpl() != 0) gp_fault(0);
          uint16_t sel = rm16();
          load_ldtr(sel, 0);
          break;
        }
        case 3: {  // ltr: a system descriptor for an available TSS (SDM vol.2
                   // LTR; privileged, and not recognized outside protected mode)
          fr->rec.mnemonic = "ltr";
          if (!(s->cr0 & 1)) ud();
          if (cpl() != 0) gp_fault(0);
          uint16_t sel = rm16();
          if ((sel & 0xfffc) == 0) gp_fault(0);
          if (sel & 4) gp_fault(sel & ~3u);  // TSS descriptors are GDT-only (SDM 7.2)
          if ((uint32_t)(sel >> 3) * 8 + 7 > s->gdtr_limit) gp_fault(sel & ~3u);
          seg_view v;
          desc_parse(sel, &v);
          if ((v.ar & 0x10) || ((v.ar & 0xf) != 1 && (v.ar & 0xf) != 9))
            gp_fault(sel & ~3u);  // system, available TSS (16- or 32-bit)
          if (!(v.ar & 0x80)) np_fault(sel & ~3u);
          // LTR marks the TSS busy (SDM 7.2.3): the current task's TSS is
          // by definition in use, and task switches key on the busy bit.
          s->tr = sel;
          s->tr_base = v.base;
          s->tr_limit = v.limit;
          s->tr_ar = (uint8_t)(v.ar | 2);
          bus_store(s->gdtr + (uint64_t)(sel >> 3) * 8 + 5, 1, (uint8_t)(v.ar | 2));
          break;
        }
        case 4:
        case 5: {  // verr/verw: ZF answers "readable/writable at this CPL"
          fr->rec.mnemonic = d.reg == 4 ? "verr" : "verw";
          if (!(s->cr0 & 1) || fl->vm) ud();  // PM-only (SDM vol.2)
          // A lookup defect (null selector, outside table) only clears ZF —
          // the SDM's exception list has no selector faults for VERR/VERW
          // (v86's verr/verw agree). VERR: any readable segment; VERW:
          // writable data. Conforming code skips the DPL rule (v86 both).
          uint16_t sel = rm16();
          seg_view v;
          int ok = 0;
          if ((sel & 0xfffc) != 0 && (uint32_t)(sel >> 3) * 8 + 7 <= table_limit(sel)) {
            desc_parse(sel, &v);
            int dpl_ok = ((v.ar >> 5) & 3) >= cpl() && ((v.ar >> 5) & 3) >= (sel & 3);
            if (v.ar & 0x10) {  // a code/data segment (S=1)
              int conf_exec = (v.ar & 0x1c) == 0x1c;
              if (d.reg == 4) {
                int readable = (v.ar & 0x08) ? (v.ar & 0x1a) == 0x1a : 1;  // code: R bit; data: always
                ok = readable && (conf_exec || dpl_ok);
              } else {
                ok = (v.ar & 0x1a) == 0x12 && dpl_ok;  // writable data
              }
            }
          }
          fl->zf = ok;
          break;
        }
        default:
          ud();
      }
      break;
    }
    case 0x02:
    case 0x03: {  // lar/lsl: the descriptor's rights byte / effective limit
      fr->rec.mnemonic = op2 == 0x02 ? "lar" : "lsl";
      if (!(s->cr0 & 1) || fl->vm) ud();  // PM-only (SDM vol.2)
      modrm();
      uint16_t sel = rm16();
      seg_view v;
      uint32_t val = 0;
      int ok = 0;
      if ((sel & 0xfffc) != 0 && (uint32_t)(sel >> 3) * 8 + 7 <= table_limit(sel)) {
        desc_parse(sel, &v);
        uint8_t ty = v.ar & 0xf;
        int conf_exec = (v.ar & 0x1c) == 0x1c;
        int dpl_ok = ((v.ar >> 5) & 3) >= cpl() && ((v.ar >> 5) & 3) >= (sel & 3);
        // Type validity (SDM vol.2 LAR/LSL valid-type lists, v86
        // LAR_INVALID_TYPE / LSL_INVALID_TYPE): gates and reserved types
        // fail, and every system type still needs DPL >= max(CPL, RPL).
        static const uint16_t kLarBadTypes = 1 << 0 | 1 << 6 | 1 << 7 | 1 << 8 |
                                             1 << 0xa | 1 << 0xd | 1 << 0xe | 1 << 0xf;
        static const uint16_t kLslBadTypes = 1 << 0 | 1 << 4 | 1 << 5 | 1 << 6 | 1 << 7 |
                                             1 << 8 | 1 << 0xa | 1 << 0xc | 1 << 0xd |
                                             1 << 0xe | 1 << 0xf;
        if (v.ar & 0x10) {  // code/data
          ok = conf_exec || dpl_ok;
        } else {
          ok = !((op2 == 0x02 ? kLarBadTypes : kLslBadTypes) >> ty & 1) && dpl_ok;
        }
        if (ok) val = op2 == 0x02 ? v.hi & 0x00f0ff00u : v.limit;
      }
      fl->zf = ok;
      if (ok) {
        if (d.w32)
          set_reg32(val);
        else
          set_reg16((uint16_t)val);
      }
      break;
    }
    case 0x01: {  // 0f 01 group: sgdt/sidt/lgdt/lidt/smsw (reg field)
      modrm();
      switch (d.reg) {
        case 0:
        case 1: {  // sgdt/sidt: store limit(2) then base(4)
          fr->rec.mnemonic = d.reg == 0 ? "sgdt" : "sidt";
          uint64_t base = d.reg == 0 ? s->gdtr : s->idtr;
          uint16_t limit = d.reg == 0 ? s->gdtr_limit : s->idtr_limit;
          // The pseudo-descriptor is one memory operand and owes its segment
          // its limit check like any other access (SDM vol.3 5.3).
          seg_use(d.mseg, d.moff, 6, 1);
          WatchData(d.mlin, 6, 1);
          wr16(d.mlin, limit);
          wr32(d.mlin + 2, (uint32_t)base & (d.w32 ? 0xffffffffu : kDescBase16Mask));
          break;
        }
        case 2:
        case 3: {  // lgdt/lidt (privileged: SDM vol.2)
          fr->rec.mnemonic = d.reg == 2 ? "lgdt" : "lidt";
          if (cpl() != 0) gp_fault(0);
          seg_use(d.mseg, d.moff, 6, 0);
          WatchData(d.mlin, 6, 0);
          // A 16-bit operand size loads only the low 24 base bits (SDM
          // LGDT/LIDT Operation; v86 lgdt/lidt mask 0xffffff). SeaBIOS's
          // int 15h AH=87h relies on it: the pseudo-descriptor is the first
          // six bytes of a data descriptor, whose byte 5 is the access byte.
          uint16_t limit = rd16(d.mlin);
          uint64_t base = rd32(d.mlin + 2) & (d.w32 ? 0xffffffffu : kDescBase16Mask);
          if (d.reg == 2) {
            s->gdtr = base;
            s->gdtr_limit = limit;
          } else {
            s->idtr = base;
            s->idtr_limit = limit;
          }
          // The descriptor-table bases decide every later gate/descriptor read;
          // a stale one is invisible until something is delivered (kDbgMark).
          if (DebugOn(kDbgMark))
            DebugMark(d.reg == 2 ? "lgdt" : "lidt", (int)base, (int)limit);
          break;
        }
        case 4:  // smsw: CR0 at the rm width
          fr->rec.mnemonic = "smsw";
          if (d.is_mem) {
            if (d.w32) {
              seg_use(d.mseg, d.moff, 4, 1);
              WatchData(d.mlin, 4, 1);
              wr32(d.mlin, s->cr0);
            } else {
              seg_use(d.mseg, d.moff, 2, 1);
              WatchData(d.mlin, 2, 1);
              wr16(d.mlin, (uint16_t)s->cr0);
            }
          } else {
            if (d.w32)
              set_rm32(s->cr0);
            else
              set_rm16((uint16_t)s->cr0);
          }
          break;
        case 6: {  // lmsw: loads CR0[3:0], but PE can only be set, never
                   // cleared, and PG is untouched (SDM vol.2 LMSW)
          fr->rec.mnemonic = "lmsw";
          if (cpl() != 0) gp_fault(0);
          uint16_t v = rm16();
          s->cr0 = (s->cr0 & ~0xfu) | (v & 0xfu) | (s->cr0 & kCr0Pe);
          TlbFlushAll(s);
          X87UpdateFerr();
          break;
        }
        case 7: {  // invlpg, or RDTSCP in the register form (0F 01 F9)
          if (!d.is_mem) {
            if (d.rm != 1) ud();  // SWAPGS is 64-bit only
            fr->rec.mnemonic = "rdtscp";
            if ((s->cr4 & kCr4Tsd) && cpl() != 0) gp_fault(0);
            uint64_t t = TscNow();
            eax = (uint32_t)t;
            edx = (uint32_t)(t >> 32);
            ecx = s->tsc_aux;
            break;
          }
          // invlpg: drop the page's translations; never faults on an
          // unmapped page (SDM vol.2)
          fr->rec.mnemonic = "invlpg";
          if (cpl() != 0) gp_fault(0);
          TlbFlushPage(s, (uint32_t)d.mlin);
          break;
        }
        default:
          ud();
      }
      break;
    }
    case 0x08:  // invd, wbinvd: cache-control (privileged, SDM vol.2). The
    case 0x09:  // interpreter keeps no cache, so there is nothing to flush or
                // invalidate and both retire as no-ops — the same reading QEMU
                // gives them. Firmware uses wbinvd to order writes before it
                // write-protects the BIOS area (seabios src/fw/shadow.c
                // make_bios_readonly_intel).
      fr->rec.mnemonic = d.op2 == 0x08 ? "invd" : "wbinvd";
      if (cpl() != 0) gp_fault(0);
      break;
    case 0x06:  // clts: clear CR0.TS (privileged: SDM vol.2)
      fr->rec.mnemonic = "clts";
      if (cpl() != 0) gp_fault(0);
      s->cr0 &= ~kCr0Ts;
      break;
    case 0x0b:
      fr->rec.mnemonic = "ud2";
      ud();
      break;
    case 0x1f:
      fr->rec.mnemonic = "nop";
      modrm();
      break;    // multi-byte nop
    case 0x20: {  // mov r32, crn: CR0/CR2/CR3 (386) and CR4 for PSE (SDM
                  // vol.2; CR1 is reserved)
      fr->rec.mnemonic = "mov";
      modrm();
      if (cpl() != 0) gp_fault(0);
      if (d.reg == 0)
        s->r[d.rm].e = s->cr0;
      else if (d.reg == 2)
        s->r[d.rm].e = s->cr2;
      else if (d.reg == 3)
        s->r[d.rm].e = s->cr3;
      else if (d.reg == 4)
        s->r[d.rm].e = s->cr4;
      else
        ud();
      break;
    }
    case 0x22: {  // mov crn, r32 (PE drives the mode switches)
      fr->rec.mnemonic = "mov";
      modrm();
      if (cpl() != 0) gp_fault(0);
      if (d.reg == 0) {
        uint32_t v = s->r[d.rm].e;
        // PG can rise only out of protected mode (SDM vol.2 MOV to CR0:
        // PG=1 with PE=0 is #GP(0); vol.3 9.9 sets PE first).
        if ((v & kCr0Pg) && !(v & kCr0Pe)) gp_fault(0);
        s->cr0 = v;
        // PG/PE/WP changes all alter translation or its permission check;
        // dropping the cache is cheap and always safe.
        TlbFlushAll(s);
        X87UpdateFerr();  // NE selects #MF vs the FERR# pin
      } else if (d.reg == 2) {
        s->cr2 = s->r[d.rm].e;
      } else if (d.reg == 3) {
        s->cr3 = s->r[d.rm].e;
        // MOV CR3 flushes non-global translations; PGE keeps global ones
        // (SDM vol.3 4.10.4.1).
        TlbFlushNonGlobal(s);
      } else if (d.reg == 4) {
        // CR4 is WARL: bits this machine does not implement read back zero
        // (SDM vol.3 2.5). PAE/PSE/PGE all change what the walk means.
        s->cr4 = s->r[d.rm].e & kCr4Supported;
        TlbFlushAll(s);
      } else {
        ud();
      }
      break;
    }
    case 0x21:  // mov r32, drn (privileged: SDM vol.2)
    case 0x23: {  // mov drn, r32
      fr->rec.mnemonic = "mov";
      modrm();
      if (cpl() != 0) gp_fault(0);
      // CR4.DE (SDM vol.3 17.2.1): DR4/DR5 alias DR6/DR7 with DE=0 and #UD
      // with DE=1. GD guards every debug-register access (vol.3 17.2.4).
      int drn = d.reg;
      if (drn == 4 || drn == 5) {
        if (s->cr4 & kCr4De) ud();
        drn = drn == 4 ? 6 : 7;
      }
      dr_guard();
      if (op2 == 0x21) {
        s->r[d.rm].e = s->dr[drn];
      } else {
        DrWrite(drn, s->r[d.rm].e);
        // The guest has taken the register. Ownership follows the register,
        // not the stub's table: a hit in a slot the guest has rewritten must
        // vector as the guest's own watchpoint (and a later RSP `z` must not
        // disable it). Writing DR7 revokes every slot it enabled.
        if (drn == 7)
          s->gdb_watch_mask = 0;
        else if (drn < 4)
          s->gdb_watch_mask &= ~(1u << drn);
      }
      break;
    }
    case 0x30: {  // wrmsr EDX:EAX -> MSR[ECX] (SDM vol.2; privileged)
      fr->rec.mnemonic = "wrmsr";
      if (cpl() != 0) gp_fault(0);
      MsrWrite(ecx, ((uint64_t)edx << 32) | eax);
      break;
    }
    case 0x31: {  // rdtsc (SDM vol.2): EDX:EAX = TSC
      fr->rec.mnemonic = "rdtsc";
      if ((s->cr4 & kCr4Tsd) && cpl() != 0) gp_fault(0);
      uint64_t t = TscNow();
      eax = (uint32_t)t;
      edx = (uint32_t)(t >> 32);
      break;
    }
    case 0x32: {  // rdmsr MSR[ECX] -> EDX:EAX (SDM vol.2 0F 32; privileged)
      fr->rec.mnemonic = "rdmsr";
      if (cpl() != 0) gp_fault(0);
      uint64_t v = MsrRead(ecx);
      eax = (uint32_t)v;
      edx = (uint32_t)(v >> 32);
      break;
    }
    case 0x34: {  // sysenter (SDM vol.2; flat ring-0 CS, no frame)
      fr->rec.mnemonic = "sysenter";
      if (!(s->cr0 & kCr0Pe) || (s->sysenter_cs & 0xfffc) == 0) gp_fault(0);
      uint16_t cs = (uint16_t)(s->sysenter_cs & 0xfffc);
      uint16_t ss = (uint16_t)(cs + 8);
      s->sreg[cs_i] = cs;
      s->base[cs_i] = 0;
      s->limit[cs_i] = 0xffffffff;
      s->ar[cs_i] = 0x9b;  // present, DPL0, code, readable, accessed
      s->dbit[cs_i] = 1;
      s->sreg[ss_i] = ss;
      s->base[ss_i] = 0;
      s->limit[ss_i] = 0xffffffff;
      s->ar[ss_i] = 0x93;
      s->dbit[ss_i] = 1;
      s->r[esp_i].e = s->sysenter_esp;
      eip = s->sysenter_eip;
      fl->word &= ~(0x100u | 0x10000u | 0x20000u);  // TF, RF, VM
      fl->if_ = 0;
      break;
    }
    case 0x35: {  // sysexit: return to ring 3 at EDX:EIP / ECX:ESP
      fr->rec.mnemonic = "sysexit";
      if (!(s->cr0 & kCr0Pe) || cpl() != 0 || (s->sysenter_cs & 0xfffc) == 0) gp_fault(0);
      uint16_t cs = (uint16_t)((s->sysenter_cs + 16) | 3);
      uint16_t ss = (uint16_t)((s->sysenter_cs + 24) | 3);
      s->sreg[cs_i] = cs;
      s->base[cs_i] = 0;
      s->limit[cs_i] = 0xffffffff;
      s->ar[cs_i] = 0xfb;  // present, DPL3, code, readable, accessed
      s->dbit[cs_i] = 1;
      s->sreg[ss_i] = ss;
      s->base[ss_i] = 0;
      s->limit[ss_i] = 0xffffffff;
      s->ar[ss_i] = 0xf3;
      s->dbit[ss_i] = 1;
      eip = edx;
      s->r[esp_i].e = ecx;
      break;
    }
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
    case 0x8c:
    case 0x8d:
    case 0x8e:
    case 0x8f: {  // jcc rel16/32
      fr->rec.mnemonic = kJccNames[op2 & 0xf];
      int32_t rel = d.w32 ? (int32_t)fetch32() : (int16_t)fetch16();
      if (cond(op2 & 0xf)) d.nxt += (uint32_t)rel;
      break;
    }
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9a:
    case 0x9b:
    case 0x9c:
    case 0x9d:
    case 0x9e:
    case 0x9f: {  // setcc rm8
      fr->rec.mnemonic = kSetccNames[op2 & 0xf];
      modrm();
      SET_RM8(cond(op2 & 0xf));
      break;
    }
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4a:
    case 0x4b:
    case 0x4c:
    case 0x4d:
    case 0x4e:
    case 0x4f: {  // cmovcc r, rm (SDM vol.2; 686+, flags untouched)
      fr->rec.mnemonic = kCmovNames[op2 & 0xf];
      modrm();
      if (d.w32) {
        uint32_t v = RM32();
        if (cond(op2 & 0xf)) set_reg32(v);
      } else {
        uint16_t v = RM16();
        if (cond(op2 & 0xf)) set_reg16(v);
      }
      break;
    }
    case 0xa0:
      fr->rec.mnemonic = "push";
      push_w(s->sreg[fs_i]);
      break;  // push fs
    case 0xa1:
      fr->rec.mnemonic = "pop";
      load_data(fs_i, (uint16_t)pop_w());
      break;      // pop fs
    case 0xa8:
      fr->rec.mnemonic = "push";
      push_w(s->sreg[gs_i]);
      break;  // push gs
    case 0xa9:
      fr->rec.mnemonic = "pop";
      load_data(gs_i, (uint16_t)pop_w());
      break;      // pop gs
    case 0xae: {  // 0f ae group: FXSAVE/FXRSTOR/LDMXCSR/STMXCSR/CLFLUSH
      modrm();
      // The state-image and MXCSR forms are #UD unless the OS enabled the
      // FXSAVE context (CR4.OSFXSR, SDM vol.3 13.5).
      if (d.reg <= 3 && !(s->cr4 & kCr4Osfxsr)) ud();
      switch (d.reg) {
        case 0: {  // fxsave m512 (16-byte aligned, SDM FXSAVE)
          fr->rec.mnemonic = "fxsave";
          if (!d.is_mem) ud();
          if (d.mlin & 15) gp_fault(0);
          seg_use(d.mseg, d.moff, 512, 1);
          WatchData(d.mlin, 512, 1);
          uint8_t buf[512];
          fpu_fxsave(&s->fpu, buf);
          wr_bytes(d.mlin, buf, 512);
          break;
        }
        case 1: {  // fxrstor
          fr->rec.mnemonic = "fxrstor";
          if (!d.is_mem) ud();
          if (d.mlin & 15) gp_fault(0);
          seg_use(d.mseg, d.moff, 512, 0);
          WatchData(d.mlin, 512, 0);
          uint8_t buf[512];
          rd_bytes(d.mlin, buf, 512);
          uint32_t mx = (uint32_t)buf[24] | ((uint32_t)buf[25] << 8) | ((uint32_t)buf[26] << 16) |
                        ((uint32_t)buf[27] << 24);
          if (mx & ~s->fpu.mxcsr_mask) gp_fault(0);  // reserved MXCSR bits
          fpu_fxrstor(&s->fpu, buf);
          X87UpdateFerr();
          break;
        }
        case 2: {  // ldmxcsr
          fr->rec.mnemonic = "ldmxcsr";
          if (!d.is_mem) ud();
          seg_use(d.mseg, d.moff, 4, 0);
          WatchData(d.mlin, 4, 0);
          uint32_t mx = (uint32_t)rd32(d.mlin);
          if (mx & ~s->fpu.mxcsr_mask) gp_fault(0);
          s->fpu.mxcsr = mx;
          break;
        }
        case 3:  // stmxcsr
          fr->rec.mnemonic = "stmxcsr";
          if (!d.is_mem) ud();
          seg_use(d.mseg, d.moff, 4, 1);
          WatchData(d.mlin, 4, 1);
          wr32(d.mlin, s->fpu.mxcsr);
          break;
        case 7:
          if (!d.is_mem) {
            // SFENCE (F8), LFENCE (E8, reg=5) and MFENCE (F0, reg=6): the
            // interpreter commits in program order, so the ordering they
            // promise is already the machine's. With the 66 prefix F8 is
            // PCOMMIT, which this CPUID does not advertise.
            if (d.reg == 7 && d.rm == 0) {
              if (d.p66) ud();  // 66 F8 = PCOMMIT
              fr->rec.mnemonic = "sfence";
            } else if (d.rm != 0) {
              ud();
            } else {
              fr->rec.mnemonic = "lfence";  // reg=5
            }
            break;
          }
          // clflush: no cache to write back. The 66 prefix selects
          // CLFLUSHOPT (not implemented), which the CPUID leaf does not
          // advertise either.
          fr->rec.mnemonic = "clflush";
          if (d.p66) ud();  // 66 /7 = CLFLUSHOPT
          break;
        case 5:  // LFENCE (0F AE E8: mod=3, rm=0)
          if (d.is_mem || d.rm != 0 || d.p66) ud();
          fr->rec.mnemonic = "lfence";
          break;
        case 6:  // MFENCE (0F AE F0); 66 F0 is the invalid "fake clwb"
          if (d.is_mem || d.rm != 0 || d.p66) ud();
          fr->rec.mnemonic = "mfence";
          break;
        default:
          ud();
      }
      break;
    }
    case 0xa2: {  // cpuid (SDM vol.2 CPUID; the advertisement follows what
                  // this machine implements, not what a Pentium III had)
      fr->rec.mnemonic = "cpuid";
      uint32_t leaf = eax;
      uint32_t sub = ecx;
      if (leaf == 0) {
        eax = 0x0b;  // highest basic leaf (0x0b APIC topology)
        ebx = 0x756e6547;
        edx = 0x49656e69;
        ecx = 0x6c65746e;
      } else if (leaf == 1) {
        // Family 6, model 3, stepping 3.
        eax = 0x633;
        // EBX: brand index 0; CLFLUSH line size 8 (64 bytes); one logical
        // processor; the BSP's APIC ID 0 (device/intc/lapic.c kLapicId).
        ebx = 0x00010800u;
        ecx = 0;
        // EDX: FPU, DE, PSE, TSC, MSR, PAE, MCE, CX8, APIC, SEP, MTRR, PGE,
        // MCA, CMOV, PAT, PSE36, CLFSH, MMX, FXSR, SSE, SSE2. Every bit here
        // has an execution path in this file; VME and the rest are advertised
        // only once their behaviour is complete.
        edx = 0x078bfbfdu;
      } else if (leaf == 0x0b) {
        // APIC topology (SDM vol.2 CPUID.0BH): the one SMT thread is level 0,
        // the one core is level 1, and subleaf 2 terminates the walk. The
        // x2APIC ID field is the APIC ID even without x2APIC.
        if (sub == 0) {
          eax = 0;      // shift to the next level (none)
          ebx = 1;      // logical processors at this level
          ecx = 0x0100;  // level 0, type 1 = SMT
          edx = 0;      // APIC ID 0
        } else if (sub == 1) {
          eax = 0;
          ebx = 1;
          ecx = 0x0201;  // level 1, type 2 = core
          edx = 0;
        } else {
          eax = ebx = ecx = edx = 0;
        }
      } else if (leaf == 0x0a) {
        // Architectural performance monitoring, version 1: two general
        // counters and three fixed counters (SDM vol.3 19.2).
        eax = 0x0101;
        ebx = 0;
        ecx = 0;
        edx = 2;
      } else if (leaf == 0x80000000) {
        eax = 0x80000001;
        ebx = ecx = edx = 0;
      } else if (leaf == 0x80000001) {
        // NX (implemented in the PAE walk) and RDTSCP.
        eax = ebx = ecx = 0;
        edx = 0x08100000u;
      } else {
        eax = ebx = ecx = edx = 0;
      }
      break;
    }
    case 0xa3:
    case 0xab:
    case 0xb3:
    case 0xbb: {  // bt/bts/btr/btc rm, reg
      fr->rec.mnemonic = op2 == 0xa3 ? "bt" : op2 == 0xab ? "bts" : op2 == 0xb3 ? "btr" : "btc";
      modrm();
      // SDM vol.2 BT/BTS/BTR/BTC, Operation: the memory operand is a bit
      // string — "BitBase ← BitOffset DIV OperandSize", so an offset past one
      // operand walks the base on by a whole operand (4 bytes per 32 bits for
      // a 32-bit operand, 2 bytes per 16). A register operand instead takes
      // the offset modulo its size and never moves.
      uint32_t v, bit, pos;
      if (d.w32) {
        bit = reg32();
        // The bit string walks the operand base on by whole operands; the
        // segment check must see the walked offset, not the encoding's.
        if (d.is_mem) {
          d.moff += 4 * (bit >> 5);
          d.mlin += 4 * (bit >> 5);
        }
        v = RM32();
        pos = bit & 31;
      } else {
        bit = reg16();
        if (d.is_mem) {
          d.moff += 2 * (bit >> 4);
          d.mlin += 2 * (bit >> 4);
        }
        v = RM16();
        pos = bit & 15;
      }
      fl->cf = (v >> pos) & 1;
      if (op2 == 0xab)
        v |= 1u << pos;
      else if (op2 == 0xb3)
        v &= ~(1u << pos);
      else if (op2 == 0xbb)
        v ^= 1u << pos;
      if (op2 != 0xa3) {
        if (d.w32)
          SET_RM32(v);
        else
          SET_RM16(v);
      }
      break;
    }
    case 0xa4:
    case 0xa5: {  // shld rm, reg, imm8/cl
      fr->rec.mnemonic = "shld";
      modrm();
      uint32_t cnt = (op2 == 0xa4 ? imm8() : cl) & 31;
      if (d.w32) {
        uint32_t dst = RM32(), src = reg32();
        uint64_t comb = ((uint64_t)dst << 32) | src;
        uint32_t r = (uint32_t)(comb << cnt >> 32);
        if (cnt) {
          fl->cf = (comb >> (64 - cnt)) & 1;
          fl->of = ((r & 0x80000000u) != 0) ^ ((dst & 0x40000000u) != 0);
          szp(r, 4);
        }
        SET_RM32(r);
      } else {
        uint16_t dst = RM16(), src = reg16();
        uint32_t comb = ((uint32_t)dst << 16) | src;
        uint16_t r = (uint16_t)(comb << cnt >> 16);
        if (cnt) {
          fl->cf = (comb >> (32 - cnt)) & 1;
          szp(r, 2);
        }
        SET_RM16(r);
      }
      break;
    }
    case 0xac:
    case 0xad: {  // shrd rm, reg, imm8/cl
      fr->rec.mnemonic = "shrd";
      modrm();
      uint32_t cnt = (op2 == 0xac ? imm8() : cl) & 31;
      if (d.w32) {
        uint32_t dst = RM32(), src = reg32();
        uint64_t comb = ((uint64_t)src << 32) | dst;
        uint32_t r = (uint32_t)(comb >> cnt);
        if (cnt) {
          fl->cf = (comb >> (cnt - 1)) & 1;
          szp(r, 4);
        }
        SET_RM32(r);
      } else {
        uint16_t dst = RM16(), src = reg16();
        uint32_t comb = ((uint32_t)src << 16) | dst;
        uint16_t r = (uint16_t)(comb >> cnt);
        if (cnt) {
          fl->cf = (comb >> (cnt - 1)) & 1;
          szp(r, 2);
        }
        SET_RM16(r);
      }
      break;
    }
    case 0xaf: {  // imul r, rm
      fr->rec.mnemonic = "imul";
      modrm();
      if (d.w32) {
        int64_t prod = (int64_t)(int32_t)RM32() * (int32_t)reg32();
        set_reg32((uint32_t)prod);
        flags_imul(prod, 4);
      } else {
        int32_t prod = (int32_t)(int16_t)RM16() * (int16_t)reg16();
        set_reg16((uint16_t)prod);
        flags_imul(prod, 2);
      }
      break;
    }
    case 0xb2:
    case 0xb4:
    case 0xb5: {  // lss/lfs/lgs r, m16:16/32
      fr->rec.mnemonic = op2 == 0xb2 ? "lss" : op2 == 0xb4 ? "lfs" : "lgs";
      modrm();
      if (!d.is_mem) ud();
      int seg = op2 == 0xb2 ? ss_i : op2 == 0xb4 ? fs_i : gs_i;
      if (d.w32) {
        set_reg32(RM32());
        seg_use(d.mseg, d.moff + 4, 2, 0);
        if (seg == ss_i)
          load_ss(rd16(d.mlin + 4));  // LSS: SS gets the full SS checks (SDM)
        else
          load_data(seg, rd16(d.mlin + 4));
      } else {
        set_reg16(RM16());
        seg_use(d.mseg, d.moff + 2, 2, 0);
        if (seg == ss_i)
          load_ss(rd16(d.mlin + 2));
        else
          load_data(seg, rd16(d.mlin + 2));
      }
      break;
    }
    case 0xb6: {  // movzx r16/32, rm8
      fr->rec.mnemonic = "movzx";
      modrm();
      if (d.w32)
        set_reg32(rm8());
      else
        set_reg16(rm8());
      break;
    }
    case 0xb7:  // movzx r32, rm16
      fr->rec.mnemonic = "movzx";
      modrm();
      set_reg32(rm16());
      break;
    case 0xba: {  // grp8: bt/bts/btr/btc rm, imm8 (reg field 4-7)
      modrm();
      if (d.reg < 4) ud();
      // The reg field is the sub-opcode, so the name is only known once modrm()
      // has decoded it; picking it earlier reads the previous instruction's reg
      // and labels all four "btc".
      fr->rec.mnemonic = d.reg == 4 ? "bt" : d.reg == 5 ? "bts" : d.reg == 6 ? "btr" : "btc";
      uint32_t v, pos = imm8();
      if (d.w32) {
        v = RM32();
        pos &= 31;
      } else {
        v = RM16();
        pos &= 15;
      }
      fl->cf = (v >> pos) & 1;
      if (d.reg == 5)
        v |= 1u << pos;
      else if (d.reg == 6)
        v &= ~(1u << pos);
      else if (d.reg == 7)
        v ^= 1u << pos;
      if (d.w32)
        SET_RM32(v);
      else
        SET_RM16(v);
      break;
    }
    case 0xbe: {  // movsx r16/32, rm8
      fr->rec.mnemonic = "movsx";
      modrm();
      uint32_t v = (uint32_t)(int8_t)rm8();
      if (d.w32)
        set_reg32(v);
      else
        set_reg16((uint16_t)v);
      break;
    }
    case 0xbf:  // movsx r32, rm16
      fr->rec.mnemonic = "movsx";
      modrm();
      set_reg32((uint32_t)(int32_t)(int16_t)rm16());
      break;
    case 0xb0:
    case 0xb1: {  // cmpxchg rm, reg (SDM vol.2 CMPXCHG: compare the accumulator
                  // with DEST -- equal sets ZF and stores SRC into DEST, else
                  // ZF clears and the accumulator takes DEST's old value. The
                  // arithmetic flags describe that comparison, so they come
                  // from the same helper CMP uses; the 8-bit form pairs with
                  // AL, the 16/32-bit forms with AX/EAX.)
      fr->rec.mnemonic = "cmpxchg";
      modrm();
      if (op2 & 1) {
        if (d.w32) {
          uint32_t dst = RM32();
          uint32_t r = dst - eax;
          flags_sub(eax, dst, r, 4, 0);
          if (r == 0)
            SET_RM32(reg32());
          else
            eax = dst;
        } else {
          uint16_t dst = RM16();
          uint16_t r = (uint16_t)(dst - (uint16_t)ax);
          flags_sub(ax, dst, r, 2, 0);
          if (r == 0)
            SET_RM16((uint16_t)reg16());
          else
            ax = dst;
        }
      } else {
        uint8_t dst = (uint8_t)RM8();
        uint8_t r = (uint8_t)(dst - al);
        flags_sub(al, dst, r, 1, 0);
        if (r == 0)
          SET_RM8((uint8_t)reg8());
        else
          al = dst;
      }
      break;
    }
    case 0xc0:
    case 0xc1: {  // xadd rm, reg (SDM vol.2 XADD: TEMP = SRC + DEST; SRC = DEST;
                  // DEST = TEMP). The source register must be written before
                  // the destination: with the two aliasing (xaddl %eax,%eax)
                  // the other order leaves the register holding the old value.
      fr->rec.mnemonic = "xadd";
      modrm();
      if (op2 & 1) {
        if (d.w32) {
          uint32_t dst = RM32(), src = reg32();
          uint32_t r = dst + src;
          set_reg32(dst);
          SET_RM32(r);
          flags_add(dst, src, r, 4, 0);
        } else {
          uint16_t dst = RM16(), src = reg16();
          uint16_t r = (uint16_t)(dst + src);
          set_reg16(dst);
          SET_RM16(r);
          flags_add(dst, src, r, 2, 0);
        }
      } else {
        uint8_t dst = RM8(), src = reg8();
        uint8_t r = (uint8_t)(dst + src);
        set_reg8(dst);
        SET_RM8(r);
        flags_add(dst, src, r, 1, 0);
      }
      break;
    }
    case 0xc7: {  // cmpxchg8b m64 (SDM vol.2: /1 only, memory only; the 66
                  // form is CMPXCHG16B and does not exist in 32-bit mode —
                  // the prefix flips w32 to 0, which is the tell)
      fr->rec.mnemonic = "cmpxchg8b";
      if (!d.w32) ud();
      modrm();
      if (d.reg != 1 || !d.is_mem) ud();
      seg_use(d.mseg, d.moff, 8, 0);
      WatchData(d.mlin, 8, 0);
      uint64_t old = (uint64_t)rd32(d.mlin) | ((uint64_t)rd32(d.mlin + 4) << 32);
      uint64_t acc = ((uint64_t)edx << 32) | eax;
      if (old == acc) {
        fl->zf = 1;
        uint64_t nv = ((uint64_t)ecx << 32) | ebx;
        seg_use(d.mseg, d.moff, 8, 1);
        WatchData(d.mlin, 8, 1);
        wr32(d.mlin, (uint32_t)nv);
        wr32(d.mlin + 4, (uint32_t)(nv >> 32));
      } else {
        fl->zf = 0;
        eax = (uint32_t)old;
        edx = (uint32_t)(old >> 32);
      }
      break;
    }
    case 0xc8:
    case 0xc9:
    case 0xca:
    case 0xcb:
    case 0xcc:
    case 0xcd:
    case 0xce:
    case 0xcf: {  // bswap r32
      fr->rec.mnemonic = "bswap";
      uint32_t v = s->r[op2 & 7].e;
      s->r[op2 & 7].e = ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
      break;
    }
    case 0xbc:
    case 0xbd: {  // bsf/bsr r, r/m (SDM vol.2 BSF/BSR: a zero source sets ZF
                  // and leaves DEST undefined -- hardware and QEMU leave it
                  // untouched; otherwise DEST is the bit index found from the
                  // low end for BSF and from the high end for BSR. CF/OF/SF/
                  // AF/PF are undefined, so only ZF is written here.)
      fr->rec.mnemonic = op2 == 0xbc ? "bsf" : "bsr";
      modrm();
      uint32_t v = d.w32 ? RM32() : RM16();  // 16-bit source is zero-extended
      if (v == 0) {
        fl->zf = 1;
        break;
      }
      fl->zf = 0;
      int idx = 0;
      if (op2 == 0xbc) {
        while (!(v & 1)) {
          v >>= 1;
          idx++;
        }
      } else {
        idx = 31;
        while (!(v & 0x80000000u)) {
          v <<= 1;
          idx--;
        }
      }
      if (d.w32)
        set_reg32((uint32_t)idx);
      else
        set_reg16((uint16_t)idx);
      break;
    }
    default:
      ud();
  }
}

// ---- x87 (D8-DF) ------------------------------------------------------------

static const char* const kX87Arith[8] = {"fadd", "fmul", "fcom", "fcomp",
                                         "fsub", "fsubr", "fdiv", "fdivr"};

// FSTENV/FNSAVE's memory operand is a destination for the whole environment,
// not the data operand the instruction records; FXSAVE likewise. Every other
// memory form updates the data pointer (SDM FSTENV: "the data pointer is not
// affected").
static void X87MemPtr(void) {
  s->fpu.fdp = (uint32_t)d.mlin;
  s->fpu.fds = s->sreg[d.mseg];
}

// The x87 escape addresses memory through the linear address directly, so
// each form states its footprint here and earns the segment-limit check and
// watchpoint report every other memory operand performs (SDM vol.3 5.3).
static void X87Access(int size, int write) {
  seg_use(d.mseg, d.moff, size, write);
  WatchData(d.mlin, size, write);
}

static extFloat80_t RdF80(void) {
  X87Access(10, 0);
  extFloat80_t v;
  v.signif = rd64(d.mlin);
  v.signExp = rd16(d.mlin + 8);
  return v;
}

static void WrF80(extFloat80_t v) {
  X87Access(10, 1);
  wr64(d.mlin, v.signif);
  wr16(d.mlin + 8, v.signExp);
}

// #MF is a fault with no error code (SDM vol.3 table 6-1). With CR0.NE=0 the
// processor instead drives FERR# low for as long as an unmasked x87 exception
// stands; the board wires that line to the coprocessor-error IRQ13. FNCLEX/
// FNINIT clears the exception flags and therefore drops the line (SDM vol.1
// 8.7.2). The level is derived, not latched: no separate acknowledgement
// state exists beyond the FPU's own status word.
static void X87UpdateFerr(void) {
  int level = !(s->cr0 & kCr0Ne) && s->fpu.pending_mf;
  if (level == s->ferr_level) return;
  s->ferr_level = level;
  if (cpu->ferr) cpu->ferr(cpu->ferr_dev, level);
}

static void X87FaultCheck(void) {
  X87UpdateFerr();
  if (s->fpu.pending_mf && (s->cr0 & kCr0Ne)) raise_(fr, vec_mf, 0);
}

static void X87ClearExceptions(void) {
  s->fpu.sw &= 0x7f00;  // keep TOP and C0-C3, clear ES/SF/exception flags
  s->fpu.pending_mf = 0;
}

static void X87LoadEnv(int wide) {
  X87Access(wide ? 28 : 14, 0);
  uint8_t buf[28];
  rd_bytes(d.mlin, buf, wide ? 28 : 14);
  fpu_load_env(&s->fpu, buf, wide);
}

static void X87StoreEnv(int wide) {
  X87Access(wide ? 28 : 14, 1);
  uint8_t buf[28];
  fpu_store_env(&s->fpu, buf, wide);
  wr_bytes(d.mlin, buf, wide ? 28 : 14);
}

static void X87LoadState(int wide) {
  X87Access(wide ? 108 : 94, 0);
  uint8_t buf[108];
  rd_bytes(d.mlin, buf, wide ? 108 : 94);
  fpu_load_state(&s->fpu, buf, wide);
}

static void X87StoreState(int wide) {
  X87Access(wide ? 108 : 94, 1);
  uint8_t buf[108];
  fpu_store_state(&s->fpu, buf, wide);
  wr_bytes(d.mlin, buf, wide ? 108 : 94);
  fpu_finit(&s->fpu);  // FNSAVE resets the FPU (SDM FNSAVE)
}

static void X87CompareMem(extFloat80_t b, int pop) {
  fpu_compare(&s->fpu, b, 0);
  if (pop) fpu_pop(&s->fpu);
}

static void X87IntegerMemArith(int op, extFloat80_t b, int pop) {
  if (op == 2 || op == 3) {
    X87CompareMem(b, pop);
  } else {
    fpu_arith(&s->fpu, op, 0, b);
  }
}

static void X87Fcmov(int i, int cond, int invert) {
  if (invert) cond = !cond;
  if (cond) fpu_set(&s->fpu, 0, fpu_get(&s->fpu, i));
}

static void X87Fcomi(int i, int quiet, int pop) {
  extFloat80_t b = fpu_get(&s->fpu, i);
  uint32_t cc = fpu_compare_i(&s->fpu, b, quiet);
  // FCOMI/FUCOMI set CF/PF/ZF from the comparison and clear OF/SF/AF
  // (SDM FCOMI); the rest of EFLAGS is untouched.
  fl->word = (fl->word & ~0x8d5u) | cc;
  if (pop) fpu_pop(&s->fpu);
}

static void X87MemForm(uint8_t op) {
  // Only value operands update the x87 data pointer; FSTENV/FNSAVE's save
  // area is not the data the instruction records (SDM FSTENV/FSAVE).
  int value_mem = op == 0xd8 || op == 0xda || op == 0xdc || op == 0xde ||
                  (op == 0xd9 && (d.reg == 0 || d.reg == 2 || d.reg == 3)) ||
                  (op == 0xdb && d.reg != 4 && d.reg != 6) ||
                  (op == 0xdd && d.reg < 4) || op == 0xdf;
  if (value_mem) X87MemPtr();
  switch (op) {
    case 0xd8: {  // m32fp arithmetic
      fr->rec.mnemonic = kX87Arith[d.reg];
      X87Access(4, 0);
      extFloat80_t b = fpu_load_f32(&s->fpu, (uint32_t)rd32(d.mlin));
      X87IntegerMemArith(d.reg, b, d.reg == 3);
      break;
    }
    case 0xd9:
      switch (d.reg) {
        case 0:
          fr->rec.mnemonic = "fld";
          X87Access(4, 0);
          fpu_push(&s->fpu, fpu_load_f32(&s->fpu, (uint32_t)rd32(d.mlin)));
          break;
        case 2:
          fr->rec.mnemonic = "fst";
          X87Access(4, 1);
          wr32(d.mlin, fpu_store_f32(&s->fpu, fpu_get(&s->fpu, 0)));
          break;
        case 3:
          fr->rec.mnemonic = "fstp";
          X87Access(4, 1);
          wr32(d.mlin, fpu_store_f32(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        case 4:
          fr->rec.mnemonic = "fldenv";
          X87LoadEnv(d.w32);
          break;
        case 5:
          fr->rec.mnemonic = "fldcw";
          X87Access(2, 0);
          s->fpu.cw = rd16(d.mlin);
          fpu_refresh(&s->fpu);
          break;
        case 6:
          fr->rec.mnemonic = "fnstenv";
          X87StoreEnv(d.w32);
          break;
        case 7:
          fr->rec.mnemonic = "fnstcw";
          X87Access(2, 1);
          wr16(d.mlin, s->fpu.cw);
          break;
        default:
          ud();
      }
      break;
    case 0xda: {  // m32int arithmetic
      fr->rec.mnemonic = kX87Arith[d.reg];
      X87Access(4, 0);
      extFloat80_t b = fpu_load_i32(&s->fpu, (int32_t)rd32(d.mlin));
      X87IntegerMemArith(d.reg, b, d.reg == 3);
      break;
    }
    case 0xdb:
      switch (d.reg) {
        case 0:
          fr->rec.mnemonic = "fild";
          X87Access(4, 0);
          fpu_push(&s->fpu, fpu_load_i32(&s->fpu, (int32_t)rd32(d.mlin)));
          break;
        case 1:
          fr->rec.mnemonic = "fisttp";
          X87Access(4, 1);
          wr32(d.mlin, (uint32_t)fpu_store_i32_trunc(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        case 2:
          fr->rec.mnemonic = "fist";
          X87Access(4, 1);
          wr32(d.mlin, (uint32_t)fpu_store_i32(&s->fpu, fpu_get(&s->fpu, 0)));
          break;
        case 3:
          fr->rec.mnemonic = "fistp";
          X87Access(4, 1);
          wr32(d.mlin, (uint32_t)fpu_store_i32(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        case 5:
          fr->rec.mnemonic = "fld";
          fpu_push(&s->fpu, RdF80());
          break;
        case 7:
          fr->rec.mnemonic = "fstp";
          WrF80(fpu_get(&s->fpu, 0));
          fpu_pop(&s->fpu);
          break;
        default:
          ud();
      }
      break;
    case 0xdc: {  // m64fp arithmetic
      fr->rec.mnemonic = kX87Arith[d.reg];
      X87Access(8, 0);
      extFloat80_t b = fpu_load_f64(&s->fpu, rd64(d.mlin));
      X87IntegerMemArith(d.reg, b, d.reg == 3);
      break;
    }
    case 0xdd:
      switch (d.reg) {
        case 0:
          fr->rec.mnemonic = "fld";
          X87Access(8, 0);
          fpu_push(&s->fpu, fpu_load_f64(&s->fpu, rd64(d.mlin)));
          break;
        case 1:
          fr->rec.mnemonic = "fisttp";
          X87Access(8, 1);
          wr64(d.mlin, (uint64_t)fpu_store_i64_trunc(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        case 2:
          fr->rec.mnemonic = "fst";
          X87Access(8, 1);
          wr64(d.mlin, fpu_store_f64(&s->fpu, fpu_get(&s->fpu, 0)));
          break;
        case 3:
          fr->rec.mnemonic = "fstp";
          X87Access(8, 1);
          wr64(d.mlin, fpu_store_f64(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        case 4:
          fr->rec.mnemonic = "frstor";
          X87LoadState(d.w32);
          break;
        case 6:
          fr->rec.mnemonic = "fnsave";
          X87StoreState(d.w32);
          break;
        case 7:
          fr->rec.mnemonic = "fnstsw";
          X87Access(2, 1);
          wr16(d.mlin, s->fpu.sw);
          break;
        default:
          ud();
      }
      break;
    case 0xde: {  // m16int arithmetic
      fr->rec.mnemonic = kX87Arith[d.reg];
      X87Access(2, 0);
      extFloat80_t b = fpu_load_i16(&s->fpu, (int16_t)rd16(d.mlin));
      X87IntegerMemArith(d.reg, b, d.reg == 3);
      break;
    }
    case 0xdf:
      switch (d.reg) {
        case 0:
          fr->rec.mnemonic = "fild";
          X87Access(2, 0);
          fpu_push(&s->fpu, fpu_load_i16(&s->fpu, (int16_t)rd16(d.mlin)));
          break;
        case 1:
          fr->rec.mnemonic = "fisttp";
          X87Access(2, 1);
          wr16(d.mlin, (uint16_t)fpu_store_i16_trunc(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        case 2:
          fr->rec.mnemonic = "fist";
          X87Access(2, 1);
          wr16(d.mlin, (uint16_t)fpu_store_i16(&s->fpu, fpu_get(&s->fpu, 0)));
          break;
        case 3:
          fr->rec.mnemonic = "fistp";
          X87Access(2, 1);
          wr16(d.mlin, (uint16_t)fpu_store_i16(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        case 4: {
          fr->rec.mnemonic = "fbld";
          X87Access(10, 0);
          uint8_t bcd[10];
          rd_bytes(d.mlin, bcd, 10);
          fpu_push(&s->fpu, fpu_load_bcd(&s->fpu, bcd));
          break;
        }
        case 5:
          fr->rec.mnemonic = "fild";
          X87Access(8, 0);
          fpu_push(&s->fpu, fpu_load_i64(&s->fpu, (int64_t)rd64(d.mlin)));
          break;
        case 6: {
          fr->rec.mnemonic = "fbstp";
          X87Access(10, 1);
          uint8_t bcd[10];
          fpu_store_bcd(&s->fpu, fpu_get(&s->fpu, 0), bcd);
          wr_bytes(d.mlin, bcd, 10);
          fpu_pop(&s->fpu);
          break;
        }
        case 7:
          fr->rec.mnemonic = "fistp";
          X87Access(8, 1);
          wr64(d.mlin, (uint64_t)fpu_store_i64(&s->fpu, fpu_get(&s->fpu, 0)));
          fpu_pop(&s->fpu);
          break;
        default:
          ud();
      }
      break;
    default:
      ud();
  }
}

// D8/DA/DC/DE register forms share the arithmetic encoding; DC and DE put
// ST(i) in the destination, and the sub/div operand order follows (SDM
// FSUB/FSUBR/FDIV/FDIVR: "DEST <- DEST - SRC" for the ST(i) forms). DE pops.
static void X87RegisterArith(uint8_t op, int i) {
  int grp = d.reg;
  fr->rec.mnemonic = kX87Arith[grp];
  if (op == 0xde && grp == 2) ud();  // DE D0-D7 are reserved (FCOMPP is DE D9)
  if (grp >= 2 && grp <= 3) {
    X87CompareMem(fpu_get(&s->fpu, i), grp == 3);
    return;
  }
  int aop = grp;
  int dst = 0;
  if (op == 0xdc || op == 0xde) {
    dst = i;
    if (grp == kFSub) aop = kFSubR;
    else if (grp == kFSubR) aop = kFSub;
    else if (grp == kFDiv) aop = kFDivR;
    else if (grp == kFDivR) aop = kFDiv;
  }
  fpu_arith(&s->fpu, aop, dst, fpu_get(&s->fpu, i));
  if (op == 0xde) fpu_pop(&s->fpu);
}

static void x87_exec(uint8_t op) {
  // CR0.EM turns every escape into #NM, and CR0.TS does too (SDM vol.3 2.5:
  // the TS check is the lazy-context-switch mechanism for the FPU).
  if (s->cr0 & (kCr0Em | kCr0Ts)) nm();
  s->fpu.fip = (uint32_t)(s->base[cs_i] + fr->rec.pc);
  s->fpu.fcs = s->sreg[cs_i];
  modrm();
  s->fpu.fop = (uint16_t)((op << 8) | d.enc);
  if (!d.is_mem) {
    int i = d.rm;
    switch (op) {
      case 0xd8:
      case 0xdc:
        X87RegisterArith(op, i);
        break;
      case 0xda:
        if (d.reg < 4) {
          fr->rec.mnemonic = "fcmov";
          X87Fcmov(i, d.reg == 0 ? fl->cf : d.reg == 1 ? fl->zf : d.reg == 2 ? (fl->cf || fl->zf) : fl->pf, 0);
        } else if (d.reg == 5 && i == 1) {
          fr->rec.mnemonic = "fucompp";
          X87CompareMem(fpu_get(&s->fpu, 1), 1);
          fpu_pop(&s->fpu);
        } else {
          ud();
        }
        break;
      case 0xde:
        if (d.reg == 3) {
          if (i != 1) ud();
          fr->rec.mnemonic = "fcompp";
          X87CompareMem(fpu_get(&s->fpu, 1), 1);
          fpu_pop(&s->fpu);
        } else {
          X87RegisterArith(op, i);
        }
        break;
      case 0xd9:
        switch (d.reg) {
          case 0:
            fr->rec.mnemonic = "fld";
            fpu_push(&s->fpu, fpu_get(&s->fpu, i));
            break;
          case 1:
            fr->rec.mnemonic = "fxch";
            fpu_exchange(&s->fpu, i);
            break;
          case 2:
            if (i != 0) ud();
            fr->rec.mnemonic = "fnop";
            break;
          case 3:
            fr->rec.mnemonic = "fstp";
            fpu_set(&s->fpu, i, fpu_get(&s->fpu, 0));
            fpu_pop(&s->fpu);
            break;
          case 4:
            switch (i) {
              case 0: fr->rec.mnemonic = "fchs"; fpu_change_sign(&s->fpu); break;
              case 1: fr->rec.mnemonic = "fabs"; fpu_abs(&s->fpu); break;
              case 4: fr->rec.mnemonic = "ftst"; fpu_test(&s->fpu); break;
              case 5: fr->rec.mnemonic = "fxam"; fpu_examine(&s->fpu); break;
              default: ud();
            }
            break;
          case 5:
            if (i == 7) ud();
            fr->rec.mnemonic = "fld";
            fpu_load_const(&s->fpu, i);
            break;
          case 6:
            switch (i) {
              case 0: fr->rec.mnemonic = "f2xm1"; fpu_f2xm1(&s->fpu); break;
              case 1: fr->rec.mnemonic = "fyl2x"; fpu_fyl2x(&s->fpu); break;
              case 2: fr->rec.mnemonic = "fptan"; fpu_fptan(&s->fpu); break;
              case 3: fr->rec.mnemonic = "fpatan"; fpu_fpatan(&s->fpu); break;
              case 4: fr->rec.mnemonic = "fxtract"; fpu_fxtract(&s->fpu); break;
              case 5: fr->rec.mnemonic = "fprem1"; fpu_fprem(&s->fpu, 1); break;
              case 6: fr->rec.mnemonic = "fdecstp"; fpu_fdecstp(&s->fpu); break;
              case 7: fr->rec.mnemonic = "fincstp"; fpu_fincstp(&s->fpu); break;
            }
            break;
          case 7:
            switch (i) {
              case 0: fr->rec.mnemonic = "fprem"; fpu_fprem(&s->fpu, 0); break;
              case 1: fr->rec.mnemonic = "fyl2xp1"; fpu_fyl2xp1(&s->fpu); break;
              case 2: fr->rec.mnemonic = "fsqrt"; fpu_fsqrt(&s->fpu); break;
              case 3: fr->rec.mnemonic = "fsincos"; fpu_fsincos(&s->fpu); break;
              case 4: fr->rec.mnemonic = "frndint"; fpu_frndint(&s->fpu); break;
              case 5: fr->rec.mnemonic = "fscale"; fpu_fscale(&s->fpu); break;
              case 6: fr->rec.mnemonic = "fsin"; fpu_fsin(&s->fpu); break;
              case 7: fr->rec.mnemonic = "fcos"; fpu_fcos(&s->fpu); break;
            }
            break;
        }
        break;
      case 0xdb:
        switch (d.reg) {
          case 0:
          case 1:
          case 2:
          case 3:
            fr->rec.mnemonic = "fcmov";
            X87Fcmov(i, d.reg == 0 ? fl->cf : d.reg == 1 ? fl->zf : d.reg == 2 ? (fl->cf || fl->zf) : fl->pf, 1);
            break;
          case 4:
            if (i == 2) {
              fr->rec.mnemonic = "fnclex";
              X87ClearExceptions();
            } else if (i == 3) {
              fr->rec.mnemonic = "fninit";
              fpu_finit(&s->fpu);
            } else {
              ud();
            }
            break;
          case 5:
            fr->rec.mnemonic = "fucomi";
            X87Fcomi(i, 1, 0);
            break;
          case 6:
            fr->rec.mnemonic = "fcomi";
            X87Fcomi(i, 0, 0);
            break;
          default:
            ud();
        }
        break;
      case 0xdd:
        switch (d.reg) {
          case 0:
            fr->rec.mnemonic = "ffree";
            fpu_free(&s->fpu, i);
            break;
          case 1:
            fr->rec.mnemonic = "fxch";
            fpu_exchange(&s->fpu, i);
            break;
          case 2:
            fr->rec.mnemonic = "fst";
            fpu_set(&s->fpu, i, fpu_get(&s->fpu, 0));
            break;
          case 3:
            fr->rec.mnemonic = "fstp";
            fpu_set(&s->fpu, i, fpu_get(&s->fpu, 0));
            fpu_pop(&s->fpu);
            break;
          case 4:
            fr->rec.mnemonic = "fucom";
            X87CompareMem(fpu_get(&s->fpu, i), 0);
            break;
          case 5:
            fr->rec.mnemonic = "fucomp";
            X87CompareMem(fpu_get(&s->fpu, i), 1);
            break;
          default:
            ud();
        }
        break;
      case 0xdf:
        switch (d.reg) {
          case 0:
            fr->rec.mnemonic = "ffreep";
            fpu_free(&s->fpu, i);
            fpu_pop(&s->fpu);
            break;
          case 1:
            fr->rec.mnemonic = "fxch";
            fpu_exchange(&s->fpu, i);
            break;
          case 2:
          case 3:
            fr->rec.mnemonic = "fstp";
            fpu_set(&s->fpu, i, fpu_get(&s->fpu, 0));
            fpu_pop(&s->fpu);
            break;
          case 4:
            if (i != 0) ud();
            fr->rec.mnemonic = "fnstsw";
            ax = s->fpu.sw;
            break;
          case 5:
            fr->rec.mnemonic = "fucomip";
            X87Fcomi(i, 1, 1);
            break;
          case 6:
            fr->rec.mnemonic = "fcomip";
            X87Fcomi(i, 0, 1);
            break;
          default:
            ud();
        }
        break;
    }
  } else {
    X87MemForm(op);
  }
  X87FaultCheck();
}

// The one-byte opcodes, manual order. The comment names the encoding; body
// is the manual's Operation section.
void run_op(uint8_t op) {
  if (op == 0x0f) {
    run_op2(fetch8());
    return;
  }  // two-byte escape (SDM 0f)
  switch (op) {
    case 0x00: {
      fr->rec.mnemonic = "add";
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a + b;
      SET_RM8(r);
      flags_add(a, b, r, 1, 0);
      break;
    }  // add rm8, r8
    case 0x01: {
      fr->rec.mnemonic = "add";
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a + b;
        SET_RM32(r);
        flags_add(a, b, r, 4, 0);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a + b;
        SET_RM16(r);
        flags_add(a, b, r, 2, 0);
      }
      break;
    }  // add rm, r
    case 0x02: {
      fr->rec.mnemonic = "add";
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a + b;
      set_reg8(r);
      flags_add(a, b, r, 1, 0);
      break;
    }  // add r8, rm8
    case 0x03: {
      fr->rec.mnemonic = "add";
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a + b;
        set_reg32(r);
        flags_add(a, b, r, 4, 0);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a + b;
        set_reg16(r);
        flags_add(a, b, r, 2, 0);
      }
      break;
    }  // add r, rm
    case 0x04: {
      fr->rec.mnemonic = "add";
      uint8_t a = al, b = imm8();
      al = a + b;
      flags_add(a, b, al, 1, 0);
      break;
    }  // add al, imm8
    case 0x05: {
      fr->rec.mnemonic = "add";
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a + b;
        flags_add(a, b, eax, 4, 0);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a + b;
        flags_add(a, b, ax, 2, 0);
      }
      break;
    }  // add eAX, imm
    case 0x06:
      fr->rec.mnemonic = "push es";
      push_w(s->sreg[es_i]);
      break;  // push es
    case 0x07:
      fr->rec.mnemonic = "pop es";
      load_data(es_i, (uint16_t)pop_w());
      break;  // pop es
    case 0x08: {
      fr->rec.mnemonic = "or";
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a | b;
      SET_RM8(r);
      flags_logic(r, 1);
      break;
    }  // or rm8, r8
    case 0x09: {
      fr->rec.mnemonic = "or";
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a | b;
        SET_RM32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a | b;
        SET_RM16(r);
        flags_logic(r, 2);
      }
      break;
    }  // or rm, r
    case 0x0a: {
      fr->rec.mnemonic = "or";
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a | b;
      set_reg8(r);
      flags_logic(r, 1);
      break;
    }  // or r8, rm8
    case 0x0b: {
      fr->rec.mnemonic = "or";
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a | b;
        set_reg32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a | b;
        set_reg16(r);
        flags_logic(r, 2);
      }
      break;
    }  // or r, rm
    case 0x0c: {
      fr->rec.mnemonic = "or";
      uint8_t a = al, b = imm8();
      al = a | b;
      flags_logic(al, 1);
      break;
    }  // or al, imm8
    case 0x0d: {
      fr->rec.mnemonic = "or";
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a | b;
        flags_logic(eax, 4);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a | b;
        flags_logic(ax, 2);
      }
      break;
    }  // or eAX, imm
    case 0x0e:
      fr->rec.mnemonic = "push cs";
      push_w(s->sreg[cs_i]);
      break;  // push cs
    case 0x10: {
      fr->rec.mnemonic = "adc";
      modrm();
      uint8_t a = RM8(), b = reg8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a + b + cin);
      SET_RM8(r);
      flags_add(a, b, r, 1, cin);
      break;
    }  // adc rm8, r8
    case 0x11: {
      fr->rec.mnemonic = "adc";
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a + b + cin;
        SET_RM32(r);
        flags_add(a, b, r, 4, cin);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = (uint16_t)(a + b + cin);
        SET_RM16(r);
        flags_add(a, b, r, 2, cin);
      }
      break;
    }  // adc rm, r
    case 0x12: {
      fr->rec.mnemonic = "adc";
      modrm();
      uint8_t a = reg8(), b = RM8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a + b + cin);
      set_reg8(r);
      flags_add(a, b, r, 1, cin);
      break;
    }  // adc r8, rm8
    case 0x13: {
      fr->rec.mnemonic = "adc";
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a + b + cin;
        set_reg32(r);
        flags_add(a, b, r, 4, cin);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = (uint16_t)(a + b + cin);
        set_reg16(r);
        flags_add(a, b, r, 2, cin);
      }
      break;
    }  // adc r, rm
    case 0x14: {
      fr->rec.mnemonic = "adc";
      uint8_t a = al, b = imm8();
      int cin = fl->cf;
      al = (uint8_t)(a + b + cin);
      flags_add(a, b, al, 1, cin);
      break;
    }  // adc al, imm8
    case 0x15: {
      fr->rec.mnemonic = "adc";
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a + b + cin;
        flags_add(a, b, eax, 4, cin);
      } else {
        uint16_t a = ax, b = imm16();
        ax = (uint16_t)(a + b + cin);
        flags_add(a, b, ax, 2, cin);
      }
      break;
    }  // adc eAX, imm
    case 0x16:
      fr->rec.mnemonic = "push ss";
      push_w(s->sreg[ss_i]);
      break;  // push ss
    case 0x17:
      fr->rec.mnemonic = "pop ss";
      load_ss((uint16_t)pop_w());
      s->intr_inhibit = 1;  // POP SS: one-instruction interrupt shadow (SDM)
      break;  // pop ss
    case 0x18: {
      fr->rec.mnemonic = "sbb";
      modrm();
      uint8_t a = RM8(), b = reg8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a - b - cin);
      SET_RM8(r);
      flags_sub(a, b, r, 1, cin);
      break;
    }  // sbb rm8, r8
    case 0x19: {
      fr->rec.mnemonic = "sbb";
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a - b - cin;
        SET_RM32(r);
        flags_sub(a, b, r, 4, cin);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = (uint16_t)(a - b - cin);
        SET_RM16(r);
        flags_sub(a, b, r, 2, cin);
      }
      break;
    }  // sbb rm, r
    case 0x1a: {
      fr->rec.mnemonic = "sbb";
      modrm();
      uint8_t a = reg8(), b = RM8();
      int cin = fl->cf;
      uint8_t r = (uint8_t)(a - b - cin);
      set_reg8(r);
      flags_sub(a, b, r, 1, cin);
      break;
    }  // sbb r8, rm8
    case 0x1b: {
      fr->rec.mnemonic = "sbb";
      modrm();
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a - b - cin;
        set_reg32(r);
        flags_sub(a, b, r, 4, cin);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = (uint16_t)(a - b - cin);
        set_reg16(r);
        flags_sub(a, b, r, 2, cin);
      }
      break;
    }  // sbb r, rm
    case 0x1c: {
      fr->rec.mnemonic = "sbb";
      uint8_t a = al, b = imm8();
      int cin = fl->cf;
      al = (uint8_t)(a - b - cin);
      flags_sub(a, b, al, 1, cin);
      break;
    }  // sbb al, imm8
    case 0x1d: {
      fr->rec.mnemonic = "sbb";
      int cin = fl->cf;
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a - b - cin;
        flags_sub(a, b, eax, 4, cin);
      } else {
        uint16_t a = ax, b = imm16();
        ax = (uint16_t)(a - b - cin);
        flags_sub(a, b, ax, 2, cin);
      }
      break;
    }  // sbb eAX, imm
    case 0x1e:
      fr->rec.mnemonic = "push ds";
      push_w(s->sreg[ds_i]);
      break;  // push ds
    case 0x1f:
      fr->rec.mnemonic = "pop ds";
      load_data(ds_i, (uint16_t)pop_w());
      break;  // pop ds
    case 0x20: {
      fr->rec.mnemonic = "and";
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a & b;
      SET_RM8(r);
      flags_logic(r, 1);
      break;
    }  // and rm8, r8
    case 0x21: {
      fr->rec.mnemonic = "and";
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a & b;
        SET_RM32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a & b;
        SET_RM16(r);
        flags_logic(r, 2);
      }
      break;
    }  // and rm, r
    case 0x22: {
      fr->rec.mnemonic = "and";
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a & b;
      set_reg8(r);
      flags_logic(r, 1);
      break;
    }  // and r8, rm8
    case 0x23: {
      fr->rec.mnemonic = "and";
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a & b;
        set_reg32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a & b;
        set_reg16(r);
        flags_logic(r, 2);
      }
      break;
    }  // and r, rm
    case 0x24: {
      fr->rec.mnemonic = "and";
      uint8_t a = al, b = imm8();
      al = a & b;
      flags_logic(al, 1);
      break;
    }  // and al, imm8
    case 0x25: {
      fr->rec.mnemonic = "and";
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a & b;
        flags_logic(eax, 4);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a & b;
        flags_logic(ax, 2);
      }
      break;
    }  // and eAX, imm
    case 0x27:
      fr->rec.mnemonic = "daa";
      daa();
      break;  // daa
    case 0x28: {
      fr->rec.mnemonic = "sub";
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a - b;
      SET_RM8(r);
      flags_sub(a, b, r, 1, 0);
      break;
    }  // sub rm8, r8
    case 0x29: {
      fr->rec.mnemonic = "sub";
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a - b;
        SET_RM32(r);
        flags_sub(a, b, r, 4, 0);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = (uint16_t)(a - b);
        SET_RM16(r);
        flags_sub(a, b, r, 2, 0);
      }
      break;
    }  // sub rm, r
    case 0x2a: {
      fr->rec.mnemonic = "sub";
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a - b;
      set_reg8(r);
      flags_sub(a, b, r, 1, 0);
      break;
    }  // sub r8, rm8
    case 0x2b: {
      fr->rec.mnemonic = "sub";
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a - b;
        set_reg32(r);
        flags_sub(a, b, r, 4, 0);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = (uint16_t)(a - b);
        set_reg16(r);
        flags_sub(a, b, r, 2, 0);
      }
      break;
    }  // sub r, rm
    case 0x2c: {
      fr->rec.mnemonic = "sub";
      uint8_t a = al, b = imm8();
      al = (uint8_t)(a - b);
      flags_sub(a, b, al, 1, 0);
      break;
    }  // sub al, imm8
    case 0x2d: {
      fr->rec.mnemonic = "sub";
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a - b;
        flags_sub(a, b, eax, 4, 0);
      } else {
        uint16_t a = ax, b = imm16();
        ax = (uint16_t)(a - b);
        flags_sub(a, b, ax, 2, 0);
      }
      break;
    }  // sub eAX, imm
    case 0x2f:
      fr->rec.mnemonic = "das";
      das();
      break;  // das
    case 0x30: {
      fr->rec.mnemonic = "xor";
      modrm();
      uint8_t a = RM8(), b = reg8();
      uint8_t r = a ^ b;
      SET_RM8(r);
      flags_logic(r, 1);
      break;
    }  // xor rm8, r8
    case 0x31: {
      fr->rec.mnemonic = "xor";
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        uint32_t r = a ^ b;
        SET_RM32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        uint16_t r = a ^ b;
        SET_RM16(r);
        flags_logic(r, 2);
      }
      break;
    }  // xor rm, r
    case 0x32: {
      fr->rec.mnemonic = "xor";
      modrm();
      uint8_t a = reg8(), b = RM8();
      uint8_t r = a ^ b;
      set_reg8(r);
      flags_logic(r, 1);
      break;
    }  // xor r8, rm8
    case 0x33: {
      fr->rec.mnemonic = "xor";
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        uint32_t r = a ^ b;
        set_reg32(r);
        flags_logic(r, 4);
      } else {
        uint16_t a = reg16(), b = RM16();
        uint16_t r = a ^ b;
        set_reg16(r);
        flags_logic(r, 2);
      }
      break;
    }  // xor r, rm
    case 0x34: {
      fr->rec.mnemonic = "xor";
      uint8_t a = al, b = imm8();
      al = a ^ b;
      flags_logic(al, 1);
      break;
    }  // xor al, imm8
    case 0x35: {
      fr->rec.mnemonic = "xor";
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        eax = a ^ b;
        flags_logic(eax, 4);
      } else {
        uint16_t a = ax, b = imm16();
        ax = a ^ b;
        flags_logic(ax, 2);
      }
      break;
    }  // xor eAX, imm
    case 0x37:
      fr->rec.mnemonic = "aaa";
      aaa();
      break;  // aaa
    case 0x38: {
      fr->rec.mnemonic = "cmp";
      modrm();
      uint8_t a = RM8(), b = reg8();
      flags_sub(a, b, (uint8_t)(a - b), 1, 0);
      break;
    }  // cmp rm8, r8
    case 0x39: {
      fr->rec.mnemonic = "cmp";
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        flags_sub(a, b, a - b, 4, 0);
      } else {
        uint16_t a = RM16(), b = reg16();
        flags_sub(a, b, (uint16_t)(a - b), 2, 0);
      }
      break;
    }  // cmp rm, r
    case 0x3a: {
      fr->rec.mnemonic = "cmp";
      modrm();
      uint8_t a = reg8(), b = RM8();
      flags_sub(a, b, (uint8_t)(a - b), 1, 0);
      break;
    }  // cmp r8, rm8
    case 0x3b: {
      fr->rec.mnemonic = "cmp";
      modrm();
      if (d.w32) {
        uint32_t a = reg32(), b = RM32();
        flags_sub(a, b, a - b, 4, 0);
      } else {
        uint16_t a = reg16(), b = RM16();
        flags_sub(a, b, (uint16_t)(a - b), 2, 0);
      }
      break;
    }  // cmp r, rm
    case 0x3c: {
      fr->rec.mnemonic = "cmp";
      uint8_t a = al, b = imm8();
      flags_sub(a, b, (uint8_t)(a - b), 1, 0);
      break;
    }  // cmp al, imm8
    case 0x3d: {
      fr->rec.mnemonic = "cmp";
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        flags_sub(a, b, a - b, 4, 0);
      } else {
        uint16_t a = ax, b = imm16();
        flags_sub(a, b, (uint16_t)(a - b), 2, 0);
      }
      break;
    }  // cmp eAX, imm
    case 0x3f:
      fr->rec.mnemonic = "aas";
      aas();
      break;  // aas
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47: {  // inc r
      fr->rec.mnemonic = "inc";
      if (d.w32) {
        uint32_t r = s->r[op & 7].e + 1;
        s->r[op & 7].e = r;
        flags_inc(r, 4);
      } else {
        uint16_t r = (uint16_t)(s->r[op & 7].x + 1);
        s->r[op & 7].x = r;
        flags_inc(r, 2);
      }
      break;
    }
    case 0x48:
    case 0x49:
    case 0x4a:
    case 0x4b:
    case 0x4c:
    case 0x4d:
    case 0x4e:
    case 0x4f: {  // dec r
      fr->rec.mnemonic = "dec";
      if (d.w32) {
        uint32_t r = s->r[op & 7].e - 1;
        s->r[op & 7].e = r;
        flags_dec(r, 4);
      } else {
        uint16_t r = (uint16_t)(s->r[op & 7].x - 1);
        s->r[op & 7].x = r;
        flags_dec(r, 2);
      }
      break;
    }
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:  // push r
      fr->rec.mnemonic = "push";
      push_w(d.w32 ? s->r[op & 7].e : s->r[op & 7].x);
      break;
    case 0x58:
    case 0x59:
    case 0x5a:
    case 0x5b:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f:  // pop r
      fr->rec.mnemonic = "pop";
      if (d.w32)
        s->r[op & 7].e = pop32();
      else
        s->r[op & 7].x = pop16();
      break;
    case 0x60: {  // pusha: ax cx dx bx sp bp si di, sp = the ORIGINAL value
      fr->rec.mnemonic = "pusha";
      uint32_t sp0 = d.w32 ? esp : sp;
      if (d.w32) {
        push32(eax);
        push32(ecx);
        push32(edx);
        push32(ebx);
        push32(sp0);
        push32(ebp);
        push32(esi);
        push32(edi);
      } else {
        push16(ax);
        push16(cx);
        push16(dx);
        push16(bx);
        push16((uint16_t)sp0);
        push16(bp);
        push16(si);
        push16(di);
      }
      break;
    }
    case 0x61: {  // popa: di si bp (skip sp) bx dx cx ax
      fr->rec.mnemonic = "popa";
      if (d.w32) {
        edi = pop32();
        esi = pop32();
        ebp = pop32();
        (void)pop32();
        ebx = pop32();
        edx = pop32();
        ecx = pop32();
        eax = pop32();
      } else {
        di = pop16();
        si = pop16();
        bp = pop16();
        (void)pop16();
        bx = pop16();
        dx = pop16();
        cx = pop16();
        ax = pop16();
      }
      break;
    }
    case 0x62: {  // bound r, m16&16 / m32&32 (SDM vol.2 BOUND; #BR)
      fr->rec.mnemonic = "bound";
      modrm();
      if (!d.is_mem) ud();  // mod=3 has no memory operand and is #UD
      int size = d.w32 ? 4 : 2;
      seg_use(d.mseg, d.moff, size, 0);
      WatchData(d.mlin, size, 0);
      uint32_t lo = d.w32 ? rd32(d.mlin) : rd16(d.mlin);
      uint64_t hi_lin = d.mlin + (uint64_t)size;
      seg_use(d.mseg, d.moff + (uint32_t)size, size, 0);
      WatchData(hi_lin, size, 0);
      uint32_t hi = d.w32 ? rd32(hi_lin) : rd16(hi_lin);
      uint32_t v = d.w32 ? reg32() : reg16();
      // The hardware checks the bounds as unsigned values (80286/80386 real
      // mode corpus; array-index semantics), not as signed integers.
      if (v < lo || v > hi) raise_(fr, vec_br, 0);
      break;
    }
    case 0x63: {  // arpl: raise r/m's RPL to the register's (SDM vol.2,
      fr->rec.mnemonic = "arpl";
                  // PM-only — v86 #UDs it outside protected mode)
      if (!(s->cr0 & 1)) ud();
      if (cpl() != 0) gp_fault(0);
      modrm();
      uint16_t dest = rm16();
      uint16_t src = reg16();
      if ((dest & 3) < (src & 3)) {
        fl->zf = 1;
        SET_RM16((uint16_t)((dest & ~3u) | (src & 3)));
      } else {
        fl->zf = 0;
      }
      break;
    }
  case 0x6c:
  case 0x6d:
  case 0x6e:
  case 0x6f:
    string_op(op);  // ins/outs: the string family's port-I/O pair
    break;
    case 0x68:
      fr->rec.mnemonic = "push";
      push_w(d.w32 ? imm32() : imm16());
      break;      // push imm
    case 0x69: {  // imul r, rm, imm
      fr->rec.mnemonic = "imul";
      modrm();
      // The immediate is instruction bytes and is fetched first; the r/m side
      // goes through the shared accessors so its segment check and watchpoint
      // hook apply like every other memory operand.
      if (d.w32) {
        int32_t imm = (int32_t)imm32();
        int64_t prod = (int64_t)RM32() * imm;
        set_reg32((uint32_t)prod);
        flags_imul(prod, 4);
      } else {
        int16_t imm = (int16_t)imm16();
        int32_t prod = (int32_t)(int16_t)RM16() * imm;
        set_reg16((uint16_t)prod);
        flags_imul(prod, 2);
      }
      break;
    }
    case 0x6a:
      fr->rec.mnemonic = "push";
      push_w((uint32_t)(int32_t)(int8_t)imm8());
      break;      // push imm8 (sign-extended)
    case 0x6b: {  // imul r, rm, imm8
      fr->rec.mnemonic = "imul";
      modrm();
      if (d.w32) {
        int8_t imm = (int8_t)imm8();
        int64_t prod = (int64_t)RM32() * imm;
        set_reg32((uint32_t)prod);
        flags_imul(prod, 4);
      } else {
        int8_t imm = (int8_t)imm8();
        int32_t prod = (int32_t)(int16_t)RM16() * imm;
        set_reg16((uint16_t)prod);
        flags_imul(prod, 2);
      }
      break;
    }
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7a:
    case 0x7b:
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f: {  // jcc rel8
      fr->rec.mnemonic = kJccNames[op & 0xf];
      int8_t rel = (int8_t)imm8();
      if (cond(op & 0xf)) d.nxt += (uint32_t)(int32_t)rel;
      break;
    }
    case 0x80:
      grp1(1, 2);
      break;
    case 0x81:
      grp1(d.w32 ? 4 : 2, 0);
      break;
    case 0x82:
      grp1(1, 2);
      break;
    case 0x83:
      grp1(d.w32 ? 4 : 2, 1);
      break;
    case 0x84: {
      fr->rec.mnemonic = "test";
      modrm();
      uint8_t a = RM8(), b = reg8();
      flags_logic(a & b, 1);
      break;
    }  // test rm8, r8
    case 0x85: {
      fr->rec.mnemonic = "test";
      modrm();
      if (d.w32) {
        uint32_t a = RM32(), b = reg32();
        flags_logic(a & b, 4);
      } else {
        uint16_t a = RM16(), b = reg16();
        flags_logic(a & b, 2);
      }
      break;
    }  // test rm, r
    case 0x86: {
      fr->rec.mnemonic = "xchg";
      modrm();
      uint8_t t = RM8();
      SET_RM8(reg8());
      set_reg8(t);
      break;
    }  // xchg rm8, r8
    case 0x87: {
      fr->rec.mnemonic = "xchg";
      modrm();
      if (d.w32) {
        uint32_t t = RM32();
        SET_RM32(reg32());
        set_reg32(t);
      } else {
        uint16_t t = RM16();
        SET_RM16(reg16());
        set_reg16(t);
      }
      break;
    }  // xchg rm, r
    case 0x88: {
      fr->rec.mnemonic = "mov";
      modrm();
      SET_RM8(reg8());
      break;
    }  // mov rm8, r8
    case 0x89: {
      fr->rec.mnemonic = "mov";
      modrm();
      if (d.w32)
        SET_RM32(reg32());
      else
        SET_RM16(reg16());
      break;
    }  // mov rm, r
    case 0x8a: {
      fr->rec.mnemonic = "mov";
      modrm();
      set_reg8(RM8());
      break;
    }  // mov r8, rm8
    case 0x8b: {
      fr->rec.mnemonic = "mov";
      modrm();
      if (d.w32)
        set_reg32(RM32());
      else
        set_reg16(RM16());
      break;
    }  // mov r, rm
    case 0x8c: {
      fr->rec.mnemonic = "mov";
      modrm();
      // MOV r/m16,Sreg (SDM vol.2): a MEMORY destination is always written as
      // 16 bits whatever the operand size; only a 32-bit register destination
      // takes the selector zero-extended. The reg field's 6/7 encodings are
      // reserved and #UD (a 32-bit store into a 16-bit slot clobbered the
      // field above it — EMM386's 32-bit monitor saves V86 segments with
      // exactly this instruction form).
      if (d.reg > gs_i) ud();
      set_sys_sel(s->sreg[d.reg]);
      break;
    }             // mov rm16, sreg
    case 0x8d: {  // lea r, m
      fr->rec.mnemonic = "lea";
      modrm();
      if (!d.is_mem) ud();
      if (d.w32)
        set_reg32(d.moff);
      else
        set_reg16((uint16_t)d.moff);
      break;
    }
    case 0x8e: {
      fr->rec.mnemonic = "mov";
      modrm();
      if (d.reg > gs_i) ud();
      uint16_t sel = rm16();
      if (d.reg == ss_i) {
        load_ss(sel);
        s->intr_inhibit = 1;  // MOV SS: one-instruction interrupt shadow (SDM)
      } else if (d.reg == cs_i) {
        ud();  // MOV to CS does not exist (SDM vol.2 MOV)
      } else {
        load_data(d.reg, sel);
      }
      break;
    }  // mov sreg, rm16
    case 0x8f: {  // pop r/m: a memory destination's effective address is
                  // computed *after* ESP is incremented (SDM vol.2 POP), so an
                  // ESP-based operand must be re-addressed here — otherwise the
                  // popped value lands four bytes below where the next
                  // instruction looks for it (seabios's CPUID test does
                  // pushf; pop [esp+0x20]; mov eax,[esp+0x20] and then loads
                  // EFLAGS from what it read).
      fr->rec.mnemonic = "pop";
      modrm();
      int esp_base = d.is_mem && d.m_esp_base;
      int width = d.w32 ? 4 : 2;
      uint32_t v = pop_w();
      if (esp_base) {
        d.moff += (uint32_t)width;
        d.mlin = s->base[d.mseg] + d.moff;
      }
      if (d.w32)
        SET_RM32(v);
      else
        SET_RM16((uint16_t)v);
      break;
    }  // pop rm
    case 0x90:
      fr->rec.mnemonic = "nop";
      break;  // nop
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97: {  // xchg eAX, r
      fr->rec.mnemonic = "xchg";
      if (d.w32) {
        uint32_t t = eax;
        eax = s->r[op & 7].e;
        s->r[op & 7].e = t;
      } else {
        uint16_t t = ax;
        ax = s->r[op & 7].x;
        s->r[op & 7].x = t;
      }
      break;
    }
    case 0x98:  // cbw/cwde: sign-extend AL->AX or AX->EAX
      fr->rec.mnemonic = d.w32 ? "cwde" : "cbw";
      if (d.w32)
        eax = (uint32_t)(int32_t)(int16_t)ax;
      else
        ax = (uint16_t)(int16_t)(int8_t)al;
      break;
    case 0x99:  // cwd/cdq: sign-extend AX->DX:AX or EAX->EDX:EAX
      fr->rec.mnemonic = d.w32 ? "cdq" : "cwd";
      if (d.w32)
        edx = (int32_t)eax < 0 ? 0xffffffffu : 0;
      else
        dx = (int16_t)ax < 0 ? 0xffffu : 0;
      break;
    case 0x9a: {  // call ptr16:16/32 — gate/code dispatch, then CS/IP pushed
      fr->rec.mnemonic = "lcall";
      uint32_t off = d.w32 ? imm32() : imm16();
      uint16_t sel = imm16();
      pm_far(sel, off, 1, (uint32_t)(fr->rec.pc + d.nxt));
      break;
    }
    case 0x9b: {
      // WAIT/FWAIT: #NM if CR0.TS (the lazy-switch check; EM is not part of
      // WAIT's check, SDM WAIT), then report a standing unmasked x87 exception.
      fr->rec.mnemonic = "wait";
      if (s->cr0 & kCr0Ts) nm();
      if (s->fpu.pending_mf && (s->cr0 & kCr0Ne)) raise_(fr, vec_mf, 0);
      break;
    }
    case 0x9c: {
      fr->rec.mnemonic = "pushf";
      // VM86 without VME/PVI may push the flags only at IOPL=3 (SDM vol.2
      // PUSHF Operation). With VME/PVI virtual interrupts the image's IF bit
      // carries VIF and bit 20 carries VIP; the image never carries VM or RF.
      int vme = fl->vm && fl->iopl != 3 && (s->cr4 & (kCr4Vme | kCr4Pvi));
      if (fl->vm && fl->iopl != 3 && !vme) gp_fault(0);
      uint32_t img = (fl->word & ~0x30000u) | 2;
      if (vme) {
        img = (img & ~0x200u) | (fl->vif ? 0x200u : 0);
        img = (img & ~0x100000u) | (fl->vip ? 0x100000u : 0);
        img &= ~0x80000u;  // VIF itself is not part of the image
      }
      push_w(img);
      break;      // pushf
    }
    case 0x9d: {  // popf: which bits the image may change depends on the mode
                  // (SDM vol.2 POPF Operation, table 4-16). VM never loads and
                  // RF is 0 after POPF; a 16-bit pop touches only the low half.
      fr->rec.mnemonic = "popf";
      uint32_t v = pop_w();
      int vme = fl->vm && fl->iopl != 3 && (s->cr4 & (kCr4Vme | kCr4Pvi));
      if (vme) {
        // Virtual-8086: IF and VIP bits update VIF/VIP; IF itself stays 1
        // and IOPL/VM are untouched.
        fl->vif = (v & 0x200u) != 0;
        fl->vip = (v & 0x100000u) != 0;
        fl->if_ = 1;
        break;
      }
      // IOPL loads only at CPL 0 (real mode counts as CPL 0) and IF only at
      // CPL <= IOPL; VM86 without VME needs IOPL=3 and keeps IOPL put.
      uint32_t mask = 0x7fd7u;  // CF PF AF ZF SF TF IF DF OF IOPL NT
      // EFLAGS.AC is writable at CPL 0, and at CPL 3 when CR0.AM enables the
      // alignment check (SDM vol.2 POPF: "the AC flag is modified if ... AM=1").
      if ((s->cr0 & 1) == 0 || cpl() == 0 || (s->cr0 & kCr0Am)) mask |= 0x40000u;
      if (fl->vm) {
        if (fl->iopl != 3) gp_fault(0);
        mask = 0x4fd7u;
      } else if ((s->cr0 & 1) && cpl() != 0) {
        mask = cpl() > fl->iopl ? 0x4dd7u : 0x4fd7u;
        // The CPL-3 mask above is rebuilt here, so re-add AC when CR0.AM
        // lets a user-mode POPF change it (SDM vol.2 POPF).
        if (s->cr0 & kCr0Am) mask |= 0x40000u;
      }
      if (!d.w32) mask &= 0xffffu;
      fl->word = (fl->word & ~(mask | 0x10000u)) | (v & mask) | 2;
      break;
    }
    case 0x9e: {  // sahf: AH -> SF ZF AF PF CF (SDM)
      fr->rec.mnemonic = "sahf";
      uint32_t ahv = ah;
      fl->word = (fl->word & ~0xd5u) | (ahv & 0xd5u) | 2;
      break;
    }
    case 0x9f:
      fr->rec.mnemonic = "lahf";
      ah = (uint8_t)(fl->word & 0xd5u) | 2;
      break;  // lahf
    case 0xa0: {
      fr->rec.mnemonic = "mov";
      int sseg = d.seg >= 0 ? d.seg : ds_i;
      uint32_t off = d.a32 ? imm32() : imm16();
      seg_use(sseg, off, 1, 0);
      WatchData(s->base[sseg] + off, 1, 0);
      al = rd8(s->base[sseg] + off);
      break;
    }  // mov al, moffs8
    case 0xa1: {
      fr->rec.mnemonic = "mov";
      int sseg = d.seg >= 0 ? d.seg : ds_i;
      uint32_t off = d.a32 ? imm32() : imm16();
      seg_use(sseg, off, d.w32 ? 4 : 2, 0);
      WatchData(s->base[sseg] + off, d.w32 ? 4 : 2, 0);
      if (d.w32)
        eax = rd32(s->base[sseg] + off);
      else
        ax = rd16(s->base[sseg] + off);
      break;
    }  // mov eAX, moffs
    case 0xa2: {
      fr->rec.mnemonic = "mov";
      int sseg = d.seg >= 0 ? d.seg : ds_i;
      uint32_t off = d.a32 ? imm32() : imm16();
      seg_use(sseg, off, 1, 1);
      WatchData(s->base[sseg] + off, 1, 1);
      wr8(s->base[sseg] + off, al);
      break;
    }  // mov moffs8, al
    case 0xa3: {
      fr->rec.mnemonic = "mov";
      int sseg = d.seg >= 0 ? d.seg : ds_i;
      uint32_t off = d.a32 ? imm32() : imm16();
      seg_use(sseg, off, d.w32 ? 4 : 2, 1);
      WatchData(s->base[sseg] + off, d.w32 ? 4 : 2, 1);
      if (d.w32)
        wr32(s->base[sseg] + off, eax);
      else
        wr16(s->base[sseg] + off, ax);
      break;
    }  // mov moffs, eAX
    case 0xa4:
    case 0xa5:
    case 0xa6:
    case 0xa7:
    case 0xaa:
    case 0xab:
    case 0xac:
    case 0xad:
    case 0xae:
    case 0xaf:
      string_op(op);
      break;
    case 0xa8: {
      fr->rec.mnemonic = "test";
      uint8_t a = al, b = imm8();
      flags_logic(a & b, 1);
      break;
    }  // test al, imm8
    case 0xa9: {
      fr->rec.mnemonic = "test";
      if (d.w32) {
        uint32_t a = eax, b = imm32();
        flags_logic(a & b, 4);
      } else {
        uint16_t a = ax, b = imm16();
        flags_logic(a & b, 2);
      }
      break;
    }  // test eAX, imm
    case 0xb0:
    case 0xb1:
    case 0xb2:
    case 0xb3:
    case 0xb4:
    case 0xb5:
    case 0xb6:
    case 0xb7:  // mov r8, imm8
      fr->rec.mnemonic = "mov";
      if (op < 0xb4)
        s->r[op & 7].l = imm8();
      else
        s->r[op & 3].h = imm8();
      break;
    case 0xb8:
    case 0xb9:
    case 0xba:
    case 0xbb:
    case 0xbc:
    case 0xbd:
    case 0xbe:
    case 0xbf:  // mov r, imm
      fr->rec.mnemonic = "mov";
      if (d.w32)
        s->r[op & 7].e = imm32();
      else
        s->r[op & 7].x = imm16();
      break;
    case 0xc0:  // grp2 EbIb: modrm byte, then the count immediate
      modrm();
      grp2(1, imm8());
      break;
    case 0xc1:
      modrm();
      grp2(d.w32 ? 4 : 2, imm8());
      break;
    case 0xc2: {  // ret imm16
      fr->rec.mnemonic = "ret";
      uint32_t n = imm16();
      eip = pop_w();
      if (d.w32)
        esp += n;
      else
        sp = (uint16_t)(sp + n);
      break;
    }
    case 0xc3:
      fr->rec.mnemonic = "ret";
      eip = pop_w();
      break;  // ret
    case 0xc4:
    case 0xc5: {  // les/lds r, m16:16/32
      fr->rec.mnemonic = op == 0xc4 ? "les" : "lds";
      modrm();
      if (!d.is_mem) ud();
      int seg = op == 0xc4 ? es_i : ds_i;
      if (d.w32) {
        set_reg32(RM32());
        seg_use(d.mseg, d.moff + 4, 2, 0);
        load_data(seg, rd16(d.mlin + 4));
      } else {
        set_reg16(RM16());
        seg_use(d.mseg, d.moff + 2, 2, 0);
        load_data(seg, rd16(d.mlin + 2));
      }
      break;
    }
    case 0xc6: {
      fr->rec.mnemonic = "mov";
      modrm();
      SET_RM8(imm8());
      break;
    }  // mov rm8, imm8
    case 0xc7: {
      fr->rec.mnemonic = "mov";
      modrm();
      if (d.w32)
        SET_RM32(imm32());
      else
        SET_RM16(imm16());
      break;
    }             // mov rm, imm
    case 0xc8: {  // enter imm16, imm8 (SDM ENTER)
      fr->rec.mnemonic = "enter";
      uint32_t alloc = imm16();
      uint32_t level = imm8() & 31;
      push_w(d.w32 ? ebp : bp);
      uint32_t frame = d.w32 ? esp : sp;
      if (level > 0) {
        for (uint32_t i = 1; i < level; i++) {
          if (d.w32) {
            ebp -= 4;
            esp = ebp;
            seg_use(ss_i, esp, 4, 0);
            push32(rd32(s->base[ss_i] + esp));
          } else {
            bp -= 2;
            sp = bp;
            seg_use(ss_i, sp, 2, 0);
            push16(rd16(s->base[ss_i] + sp));
          }
        }
        push_w(frame);
      }
      if (d.w32) {
        ebp = frame;
        esp -= alloc;
      } else {
        bp = (uint16_t)frame;
        sp = (uint16_t)(sp - alloc);
      }
      break;
    }
    case 0xc9: {  // leave: sp = bp, then pop bp
      fr->rec.mnemonic = "leave";
      if (d.w32) {
        esp = ebp;
        ebp = pop32();
      } else {
        sp = bp;
        bp = pop16();
      }
      break;
    }
    case 0xca: {  // retf imm16
      fr->rec.mnemonic = "retf";
      uint32_t n = imm16();
      if ((s->cr0 & 1) && !fl->vm) {  // VM86 pops the real-mode frame
        pm_ret(n);
        break;
      }
      eip = pop_w();
      load_cs((uint16_t)pop_w());
      if (d.w32)
        esp += n;
      else
        sp = (uint16_t)(sp + n);
      break;
    }
    case 0xcb:
      fr->rec.mnemonic = "retf";
      if ((s->cr0 & 1) && !fl->vm) {  // VM86 pops the real-mode frame
        pm_ret(0);
        break;
      }
      eip = pop_w();
      load_cs((uint16_t)pop_w());
      break;  // retf
    case 0xcc:
      fr->rec.mnemonic = "int3";
      do_int(3, (uint32_t)(fr->rec.pc + d.nxt), kIntSoft, 0);
      return;  // int3
    case 0xcd: {
      fr->rec.mnemonic = "int";
      int v = imm8();  // evaluate the immediate first: d.nxt must count it
                       // before the return address is formed (arg order in C
                       // is unspecified)
      do_int(v, (uint32_t)(fr->rec.pc + d.nxt), kIntSoft, 0);
      return;
    }  // int imm8
    case 0xce:
      fr->rec.mnemonic = "into";
      if (fl->of) {
        do_int(4, (uint32_t)(fr->rec.pc + d.nxt), kIntSoft, 0);
        return;
      }
      break;      // into
    case 0xcf: {
      fr->rec.mnemonic = "iret";
      if (s->cr0 & 1) {
        pm_iret();  // SDM vol.2 IRET: the gate-return rules apply
        s->nmi_blocked = 0;  // the NMI handler has returned (SDM vol.3 6.7.1)
        break;
      }
      // Real mode: pop (E)IP, CS, (E)FLAGS. VM never loads; RF does (SDM
      // IRET Operation — the #DB handler re-arms RF through its image).
      // 16-bit form loads only the low half.
      // CF before the flags image below replaces it: the handler's verdict
      // (debug.h DebugGateCall). IRET pops no general-purpose register, so AX
      // still holds the answer the caller branches on.
      int cf = fl->cf;
      uint32_t new_eip = pop_w();
      uint16_t sel = (uint16_t)pop_w();
      uint32_t flv = pop_w();
      seg_view v;
      seg_synth(sel, &v);
      seg_commit(cs_i, sel, &v);
      eip = new_eip;
      d.rf_load = 1;
      if (d.w32)
        fl->word = (flv & ~(0x20000u)) | 2;
      else
        fl->word = (fl->word & 0xffff0000u) | ((flv & 0xffffu) | 2);
      // The answer half of the conversation (debug.h DebugGateCall), paired
      // with the gatecall row carrying this same caller CS:IP. Real mode is
      // where a DOS guest's BIOS conversation happens, and it is the half of
      // the return QEMU's -d int never prints.
      if (DebugOn(kDbgMark)) {
        DbgCallArgs a;
        GateArgs(&a);
        DebugGateRet(&a, cf, ((uint32_t)sel << 16) | (new_eip & 0xffff));
      }
      s->nmi_blocked = 0;  // the NMI handler has returned (SDM vol.3 6.7.1)
      break;
    }
    case 0xd0:
      modrm();
      grp2(1, 1);
      break;
    case 0xd1:
      modrm();
      grp2(d.w32 ? 4 : 2, 1);
      break;
    case 0xd2:
      modrm();
      grp2(1, cl);
      break;
    case 0xd3:
      modrm();
      grp2(d.w32 ? 4 : 2, cl);
      break;
    case 0xd4: {  // aam imm8: AH = AL/base, AL = AL%base (base 0 -> #DE)
      fr->rec.mnemonic = "aam";
      uint32_t base = imm8();
      if (base == 0) de();
      uint8_t q = al / (uint8_t)base;
      al = al % (uint8_t)base;
      ah = q;
      flags_logic(al, 1);
      break;
    }
    case 0xd5: {  // aad imm8: AL = (AL + AH*base) & 0xff, AH = 0
      fr->rec.mnemonic = "aad";
      uint32_t base = imm8();
      uint8_t r = (uint8_t)(al + ah * (uint8_t)base);
      al = r;
      ah = 0;
      flags_logic(r, 1);
      break;
    }
    case 0xd6:
      fr->rec.mnemonic = "salc";
      al = fl->cf ? 0xff : 0x00;
      break;      // salc (undocumented)
    case 0xd7: {  // xlat: AL = [seg:(BX + AL) mod 2^addr_size] (SDM XLAT)
      fr->rec.mnemonic = "xlat";
      int sseg = d.seg >= 0 ? d.seg : ds_i;
      uint32_t off = d.a32 ? ebx + al : (uint16_t)(bx + al);
      seg_use(sseg, off, 1, 0);
      WatchData(s->base[sseg] + off, 1, 0);
      al = rd8(s->base[sseg] + off);
      break;
    }
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf:  // x87 escape (SDM vol.1 ch.8; decode in x87_exec above)
      x87_exec(op);
      break;
    case 0xe0:
    case 0xe1:
    case 0xe2: {  // loopne/loope/loop rel8
      fr->rec.mnemonic = op == 0xe0 ? "loopne" : op == 0xe1 ? "loope" : "loop";
      int8_t rel = (int8_t)imm8();
      uint32_t cnt = (d.w32 ? ecx : cx) - 1;
      if (d.w32)
        ecx = cnt;
      else
        cx = (uint16_t)cnt;
      int taken = cnt != 0 && (op == 0xe2 || cond(op == 0xe0 ? 5 : 4));
      if (taken) d.nxt += (uint32_t)(int32_t)rel;
      break;
    }
    case 0xe3: {  // jcxz/jecxz rel8 (counts at the ADDRESS size)
      fr->rec.mnemonic = d.a32 ? "jecxz" : "jcxz";
      int8_t rel = (int8_t)imm8();
      if ((d.a32 ? ecx : cx) == 0) d.nxt += (uint32_t)(int32_t)rel;
      break;
    }
    case 0xe4: {
      fr->rec.mnemonic = "in";
      uint16_t port = imm8();
      al = (uint8_t)io_in(port, 1);
      break;
    }  // in al, imm8
    case 0xe5: {
      fr->rec.mnemonic = "in";
      uint16_t port = imm8();
      if (d.w32)
        eax = io_in(port, 4);
      else
        ax = (uint16_t)io_in(port, 2);
      break;
    }  // in eAX, imm8
    case 0xe6:
      fr->rec.mnemonic = "out";
      io_out(imm8(), 1, al);
      break;  // out imm8, al
    case 0xe7:
      fr->rec.mnemonic = "out";
      uint16_t port = imm8();
      io_out(port, d.w32 ? 4 : 2, d.w32 ? eax : ax);
      break;      // out imm8, eAX
    case 0xe8: {  // call rel16/32
      fr->rec.mnemonic = "call";
      int32_t rel = d.w32 ? (int32_t)imm32() : (int16_t)imm16();
      push_w((uint32_t)(fr->rec.pc + d.nxt));  // return: next instruction
      d.nxt += (uint32_t)rel;
      break;
    }
    case 0xe9: {  // jmp rel16/32
      fr->rec.mnemonic = "jmp";
      int32_t rel = d.w32 ? (int32_t)imm32() : (int16_t)imm16();
      d.nxt += (uint32_t)rel;
      break;
    }
    case 0xea: {  // jmp ptr16:16/32 — gate/code dispatch
      fr->rec.mnemonic = "ljmp";
      uint32_t off = d.w32 ? imm32() : imm16();
      uint16_t sel = imm16();
      pm_far(sel, off, 0, (uint32_t)(fr->rec.pc + d.nxt));
      break;
    }
    case 0xeb: {
      fr->rec.mnemonic = "jmp";
      int8_t rel = (int8_t)imm8();
      d.nxt += (uint32_t)(int32_t)rel;
      break;
    }  // jmp rel8
    case 0xec:
      fr->rec.mnemonic = "in";
      al = (uint8_t)io_in(dx, 1);
      break;  // in al, dx
    case 0xed:
      fr->rec.mnemonic = "in";
      if (d.w32)
        eax = io_in(dx, 4);
      else
        ax = (uint16_t)io_in(dx, 2);
      break;  // in eAX, dx
    case 0xee:
      fr->rec.mnemonic = "out";
      io_out(dx, 1, al);
      break;  // out dx, al
    case 0xef:
      fr->rec.mnemonic = "out";
      io_out(dx, d.w32 ? 4 : 2, d.w32 ? eax : ax);
      break;      // out dx, eAX
    case 0xf1: {  // icebp/int1: #DB as a trap, rip after the byte (kvm debug:
                  // dr6 unchanged — no Bn/BS bits come from icebp itself)
      fr->rec.mnemonic = "icebp";
    do_int(vec_db, (uint32_t)(fr->rec.pc + d.nxt), kIntException, 0);
      return;
    }  // icebp
    case 0xf4: {  // hlt: sleeps until an unmasked external interrupt (SDM)
      fr->rec.mnemonic = "hlt";
      // HLT is privileged: #GP(0) unless CPL = 0, which also covers VM86's
      // CPL 3 (SDM vol.2 HLT; tiny386 HLT raises EX_GP the same way).
      if (cpl() != 0) gp_fault(0);
      if (fl->if_ && cpu->int_ack) {
        cpu->wait = 1;
      } else {
        LogInfo("hlt with no wake sources, stopping emulation");
        cpu->halted = kCpuExited;
        cpu->exit_code = 0;
      }
      break;
    }
    case 0xf5:
      fr->rec.mnemonic = "cmc";
      fl->cf = !fl->cf;
      break;  // cmc
    case 0xf8:
      fr->rec.mnemonic = "clc";
      fl->cf = 0;
      break;  // clc
    case 0xf9:
      fr->rec.mnemonic = "stc";
      fl->cf = 1;
      break;  // stc
    case 0xfa:
      fr->rec.mnemonic = "cli";
      // CLI needs CPL <= IOPL. In VM86 with VME/PVI and IOPL<3 the instruction
      // is legal and clears the virtual interrupt flag instead (SDM vol.3
      // 20.3.1, 21.3); without them only IOPL=3 allows it.
      if ((s->cr0 & 1) && fl->vm && fl->iopl != 3 && (s->cr4 & (kCr4Vme | kCr4Pvi))) {
        fl->vif = 0;
      } else {
        if ((s->cr0 & 1) && (fl->vm ? fl->iopl != 3 : cpl() > fl->iopl)) gp_fault(0);
        fl->if_ = 0;
      }
      break;    // cli
    case 0xfb:  // sti: the next instruction is not interruptible (SDM)
      fr->rec.mnemonic = "sti";
      if ((s->cr0 & 1) && fl->vm && fl->iopl != 3 && (s->cr4 & (kCr4Vme | kCr4Pvi))) {
        fl->vif = 1;
      } else {
        if ((s->cr0 & 1) && (fl->vm ? fl->iopl != 3 : cpl() > fl->iopl)) gp_fault(0);
        fl->if_ = 1;
      }
      s->intr_inhibit = 1;
      break;
    case 0xfc:
      fr->rec.mnemonic = "cld";
      fl->df = 0;
      break;  // cld
    case 0xfd:
      fr->rec.mnemonic = "std";
      fl->df = 1;
      break;  // std
    case 0xf6:
      grp3(1);
      break;
    case 0xf7:
      grp3(d.w32 ? 4 : 2);
      break;
    case 0xfe: {  // grp4: inc/dec rm8 (reg field)
      modrm();
      if (d.reg > 1) ud();
      fr->rec.mnemonic = d.reg ? "dec" : "inc";
      uint8_t v = RM8();
      uint8_t r = d.reg ? (uint8_t)(v - 1) : (uint8_t)(v + 1);
      SET_RM8(r);
      if (d.reg)
        flags_dec(r, 1);
      else
        flags_inc(r, 1);
      break;
    }
    case 0xff: {  // grp5: inc/dec/call/jmp/push (reg field)
      modrm();
      switch (d.reg) {
        case 0:
        case 1: {
          fr->rec.mnemonic = d.reg ? "dec" : "inc";
          if (d.w32) {
            uint32_t v = RM32();
            uint32_t r = d.reg ? v - 1 : v + 1;
            SET_RM32(r);
            if (d.reg)
              flags_dec(r, 4);
            else
              flags_inc(r, 4);
          } else {
            uint16_t v = RM16();
            uint16_t r = (uint16_t)(d.reg ? v - 1 : v + 1);
            SET_RM16(r);
            if (d.reg)
              flags_dec(r, 2);
            else
              flags_inc(r, 2);
          }
          break;
        }
        case 2: {  // call near rm
          fr->rec.mnemonic = "call";
          uint32_t t = d.w32 ? RM32() : RM16();
          push_w((uint32_t)(fr->rec.pc + d.nxt));
          eip = t;
          break;
        }
        case 3: {  // call far m16:16/32
          fr->rec.mnemonic = "lcall";
          if (!d.is_mem) ud();
          // Offset and selector are one memory operand: every byte of it lies
          // inside the segment (SDM vol.3 5.3). Checking only the selector
          // half lets a pointer that straddles the limit through.
          int w = d.w32 ? 4 : 2;
          seg_use(d.mseg, d.moff, w + 2, 0);
          WatchData(d.mlin, w + 2, 0);
          uint32_t t = w == 4 ? rd32(d.mlin) : rd16(d.mlin);
          uint16_t sel = rd16(d.mlin + w);
          pm_far(sel, t, 1, (uint32_t)(fr->rec.pc + d.nxt));
          break;
        }
        case 4:
          fr->rec.mnemonic = "jmp";
          eip = d.w32 ? RM32() : RM16();
          break;   // jmp near rm
        case 5: {  // jmp far m16:16/32
          fr->rec.mnemonic = "ljmp";
          if (!d.is_mem) ud();
          int w = d.w32 ? 4 : 2;
          seg_use(d.mseg, d.moff, w + 2, 0);
          WatchData(d.mlin, w + 2, 0);
          uint32_t t = w == 4 ? rd32(d.mlin) : rd16(d.mlin);
          uint16_t sel = rd16(d.mlin + w);
          pm_far(sel, t, 0, (uint32_t)(fr->rec.pc + d.nxt));
          break;
        }
        case 6:
          fr->rec.mnemonic = "push";
          push_w(d.w32 ? RM32() : RM16());
          break;  // push rm
        default:
          ud();
      }
      break;
    }
    default:
      ud();
  }
}

// ---- machine irq sink ----------------------------------------------------------

void x86_set_intr(CpuState* c, int level) {
  x86_state* st = (x86_state*)c->priv;
  // INTR is a level line (SDM vol.3 6.3.2: it is sampled, not latched by the
  // CPU), so the request stands only while the machine asserts it. The PIC
  // keeps its own pending bits in the IRR and drops the line the moment the
  // request is masked or acknowledged.
  st->intr_pending = level ? 1 : 0;
  if (level) c->wait = 0;
}

// NMI is edge-triggered and not maskable by IF (SDM vol.3 6.7): the request is
// latched until the processor takes it. While an NMI is being serviced a
// second one is held (the pin's request is latched, not discarded) and taken
// after the handler's IRET clears the block.
void x86_set_nmi(CpuState* c) {
  x86_state* st = (x86_state*)c->priv;
  st->nmi_pending = 1;
  c->wait = 0;
}

// ---- reset / boot ----------------------------------------------------------------

// Multiboot header magic the loader scans for (Multiboot 0.6.96 §3.1.1);
// the loader magic the kernel expects in EAX (§3.2). The same enum carries the
// reset vector a board hands the CPU when it boots firmware instead of an
// image: on a 16-bit PC board that is the linear address of F000:FFF0.
enum { k_mb_hdr_magic = 0x1badb002, k_mb_load_magic = 0x2badb002, k_bios_reset_vector = 0xffff0 };

// How boards drive our interrupt lines: x86 has one shared INTR line, so the
// line id is ignored (the PIC supplies the vector on the INTA acknowledge).
// Boards call this hook instead of naming a CPU-model function.
static void x86_set_irq_line(CpuState* cpu, uint64_t line, int level) {
  (void)line;
  x86_set_intr(cpu, level);
}

// The A20 line, driven by the board's keyboard controller output port or by
// port 0x92 (both funnel into CpuState.set_a20).
static void x86_set_a20_line(CpuState* c, int on) {
  x86_state* st = (x86_state*)c->priv;
  int v = on ? 1 : 0;
  if (st->a20 == v) return;
  st->a20 = v;
  // Every cached physical page was formed with the old gate state.
  TlbFlushAll(st);
}

void x86_init(CpuState* c) {
  x86_state* st;
  if (!c->priv) {
    st = (x86_state*)calloc(1, sizeof(x86_state));
    c->priv = st;
  } else {
    st = (x86_state*)c->priv;
    memset(st, 0, sizeof(*st));
  }
  fpu_init(&st->fpu);
  st->r = (cell*)c->gpr;
  st->fl.word = 0x202;  // IF set, reserved bit 1 on (SDM reset state)
  // Debug-register reset state (SDM vol.3 17.2.2/17.2.4): DR6's reserved
  // bits read 1; DR7's bit 10 reads 1 and every breakpoint is disabled.
  st->dr[6] = kDr6Rsvd1;
  st->dr[7] = kDr7Rsvd1;
  st->dr_active = 0;
  // A reset disables every breakpoint (SDM vol.3 17.2), the stub's with the
  // guest's: the ownership mask goes with the registers it names.
  st->gdb_watch_mask = 0;
  st->watch_hits = 0;
  // IA32_APIC_BASE reset state (SDM vol.3 11.4.3): default base, APIC
  // enabled, BSP.
  st->msr_apic_base = 0xFEE00000ULL | 0x800ULL | 0x100ULL;
  c->set_irq = x86_set_irq_line;
  c->set_a20 = x86_set_a20_line;
  c->set_nmi = x86_set_nmi;
  // The gate starts open (the reasoning lives in port92.h): every image cemu
  // accepts is handed flat memory above 1 MiB and never expects the alias.
  st->a20 = 1;

  // Multiboot images (header in the first 8 KiB, 4-aligned) enter in flat
  // protected mode, CS=0x08/data=0x10 with built-in descriptors (QEMU
  // contract); everything else is a BIOS boot sector.
  int multiboot = 0;
  for (uint64_t a = c->image_base; a < c->image_base + 0x2000; a += 4)
    if (BusRead(c->bus, a, 4) == k_mb_hdr_magic) {
      multiboot = 1;
      break;
    }
  if (multiboot) {
    st->cr0 = 1;  // PE
    // Boot stack (multiboot leaves ESP undefined; QEMU's loader hands the
    // image a stack just below the multiboot info struct — measured
    // 0x6f07 post-push on qemu-system-i386 -kernel). Our info scratch sits
    // at 0x7000, so the same region works.
    st->r[esp_i].e = 0x6f00;
    // Flat CS=0x08 / data=0x10 descriptors (QEMU multiboot GDT: 00cf9b00...,
    // 00cf93...): 4 GiB limit, DPL0, D/B set.
    st->sreg[cs_i] = 0x08;
    st->base[cs_i] = 0;
    st->limit[cs_i] = 0xffffffff;
    st->ar[cs_i] = 0x9b;  // present, DPL0, code, readable, accessed
    st->dbit[cs_i] = 1;
    for (int i = 0; i < 6; i++) {
      if (i == cs_i) continue;
      st->sreg[i] = 0x10;
      st->base[i] = 0;
      st->limit[i] = 0xffffffff;
      st->ar[i] = 0x93;  // present, DPL0, data, read-write, accessed
      st->dbit[i] = 1;
    }
    st->r[eax_i].e = k_mb_load_magic;
    st->r[ebx_i].e = 0x7000;  // multiboot info scratch
  } else if (c->pc == k_bios_reset_vector) {
    // Firmware reset: the first instruction is fetched at the reset vector,
    // which on a 16-bit PC board is CS:IP = F000:FFF0 (SDM vol.3 9.1.4) — the
    // board maps its BIOS ROM over the top of the first megabyte and hands the
    // CPU the linear address. SeaBIOS enters at exactly the same place QEMU
    // puts it.
    st->sreg[cs_i] = 0xf000;
    st->base[cs_i] = 0xf0000;
    st->limit[cs_i] = 0xffff;
    st->ar[cs_i] = 0x9b;
    c->pc = 0xfff0;  // the offset within CS: 0xf0000 + 0xfff0 = 0xffff0
    st->idtr = 0;
    st->idtr_limit = 0xffff;  // SDM reset state: no IDT until POST builds one
  } else {
    // BIOS boot-sector handoff: DL = 0x80 (boot drive), IVT at linear 0.
    st->r[edx_i].e = 0x80;
    st->idtr = 0;
    st->idtr_limit = 0x3ff;
  }
}

void x86_dump_regs(const CpuState* c) {
  const x86_state* st = (const x86_state*)c->priv;
  char buf[224];
  static const char names[8][4] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
  for (int i = 0; i < 8; i += 4) {
    int n = snprintf(buf, sizeof(buf), "%s=%08x %s=%08x %s=%08x %s=%08x\n", names[i], st->r[i].e,
                     names[i + 1], st->r[i + 1].e, names[i + 2], st->r[i + 2].e, names[i + 3],
                     st->r[i + 3].e);
    HostWriteErr(buf, (size_t)n);
  }
  static const char snames[6][3] = {"es", "cs", "ss", "ds", "fs", "gs"};
  int n = snprintf(buf, sizeof(buf),
                   "eip=%08x eflags=%08x cr0=%08x cr3=%08x cr4=%08x %s=%04x %s=%04x %s=%04x "
                   "%s=%04x %s=%04x %s=%04x\n",
                   (unsigned)c->pc, st->fl.word, st->cr0, (unsigned)st->cr3, (unsigned)st->cr4,
                   snames[0], st->sreg[0], snames[1],
                   st->sreg[1], snames[2], st->sreg[2], snames[3], st->sreg[3], snames[4],
                   st->sreg[4], snames[5], st->sreg[5]);
  HostWriteErr(buf, (size_t)n);
}
