// The x86 ops table: the machine layer's handle on the interpreter (init,
// step, irq, dumps) plus the debug hooks the hub queries. The interpreter
// itself is exec.c.
#include <stdio.h>
#include <string.h>

#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "host/host.h"
#include "x86.h"

// ---- debug hooks (AGENTS.md §X) ----------------------------------------------

// Set-bit names indexed by EFLAGS bit position, NULL-terminated (the walker
// in debug.c stops at the terminator; holes are "").
static const char* const k_x86_flag_names[] = {
    "cf", "", "pf", "", "af", "", "zf", "sf", "tf", "if", "df", "of", NULL,
};

static uint64_t x86_flag_word(frame* f) { return ((x86_state*)f->priv)->fl.word; }

static const char* x86_cause_name(uint64_t cause) {
  switch (cause) {
    case vec_de:
      return "de";
    case vec_ud:
      return "ud";
    default:
      return NULL;  // INTR vectors: debug.c prints the raw number
  }
}

static int x86_has_fpr(frame* f) {
  (void)f;
  return 0;  // no FPR bank in the state stream
}

static void x86_debug_state_line(char* buf, int cap, frame* f) {
  x86_state* s = (x86_state*)f->priv;
  snprintf(buf, (size_t)cap,
           "x: cr0=%08llx cs=%04x ss=%04x ds=%04x es=%04x fs=%04x gs=%04x dr6=%08x dr7=%08x",
           (unsigned long long)s->cr0, s->sreg[cs_i], s->sreg[ss_i], s->sreg[ds_i], s->sreg[es_i],
           s->sreg[fs_i], s->sreg[gs_i], s->dr[6], s->dr[7]);
}

// ---- gdb stub (stage 3.5) -----------------------------------------------------

static void Le32Put(uint8_t** p, uint32_t v) {
  (*p)[0] = (uint8_t)v;
  (*p)[1] = (uint8_t)(v >> 8);
  (*p)[2] = (uint8_t)(v >> 16);
  (*p)[3] = (uint8_t)(v >> 24);
  *p += 4;
}

static uint32_t Le32Get(const uint8_t** p) {
  uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) | ((uint32_t)(*p)[2] << 16) |
               ((uint32_t)(*p)[3] << 24);
  *p += 4;
  return v;
}

// gdb's i386 core register file (org.gnu.gdb.i386.core): eax..edi, eip,
// eflags, cs ss ds es fs gs — 16 registers, 4 bytes each — plus the x87
// group gdb keeps in the same feature (st0-7 at 10 bytes in stack order,
// fctrl..fop at 4).
// The address gdb names the program counter by, and the address the stub
// matches breakpoints against: CS's base plus the committed offset — the same
// linear space its memory packets read (cpu/step.h gdb_pc). Reporting the
// offset alone would make `x/i $pc` disassemble the wrong bytes and
// `break *<linear>` never fire, which is exactly what a real-mode guest (BIOS
// at F0000, DOS loaded high) needs.
static uint64_t x86_gdb_pc(CpuState* cpu) {
  x86_state* s = (x86_state*)cpu->priv;
  return s->base[cs_i] + (uint32_t)cpu->pc;
}

// Which stub-installed watchpoints fired on the step that just committed
// (cpu/step.h gdb_watch_hits). The bits are the DR6 B bits the trap carried, so
// the stub can tell its own slots from ones the guest armed.
static uint32_t x86_gdb_watch_hits(CpuState* cpu) {
  return ((x86_state*)cpu->priv)->watch_hits;
}

static int x86_gdb_read_regs(CpuState* cpu, uint8_t* buf, int cap) {
  x86_state* s = (x86_state*)cpu->priv;
  if (cap < 16 * 4 + 8 * 10 + 8 * 4) return 0;
  uint8_t* p = buf;
  for (int i = 0; i < 8; i++) Le32Put(&p, s->r[i].e);
  Le32Put(&p, (uint32_t)x86_gdb_pc(cpu));
  Le32Put(&p, s->fl.word);
  static const int order[6] = {cs_i, ss_i, ds_i, es_i, fs_i, gs_i};
  for (int i = 0; i < 6; i++) Le32Put(&p, s->sreg[order[i]]);
  // gdb's x87 tail: st0-7 as 10-byte extended values in stack order, then
  // fctrl/fstat/ftag/fiseg/fioff/foseg/fooff/fop (org.gnu.gdb.i386.core).
  for (int i = 0; i < 8; i++) {
    extFloat80_t v = s->fpu.st[(fpu_top(&s->fpu) + i) & 7];
    memcpy(p, &v.signif, 8);
    p[8] = (uint8_t)v.signExp;
    p[9] = (uint8_t)(v.signExp >> 8);
    p += 10;
  }
  Le32Put(&p, s->fpu.cw);
  Le32Put(&p, s->fpu.sw);
  Le32Put(&p, s->fpu.tw);
  Le32Put(&p, s->fpu.fcs);
  Le32Put(&p, s->fpu.fip);
  Le32Put(&p, s->fpu.fds);
  Le32Put(&p, s->fpu.fdp);
  Le32Put(&p, s->fpu.fop);
  return (int)(p - buf);
}

static int x86_gdb_write_regs(CpuState* cpu, const uint8_t* buf, int len) {
  x86_state* s = (x86_state*)cpu->priv;
  if (len < 16 * 4) return -1;
  const uint8_t* p = buf;
  uint32_t gpr[8];
  for (int i = 0; i < 8; i++) gpr[i] = Le32Get(&p);
  uint32_t lin = Le32Get(&p);
  uint32_t flags = Le32Get(&p);
  static const int order[6] = {cs_i, ss_i, ds_i, es_i, fs_i, gs_i};
  uint16_t sel[6];
  for (int i = 0; i < 6; i++) sel[i] = (uint16_t)Le32Get(&p);

  for (int i = 0; i < 8; i++) s->r[i].e = gpr[i];
  // The flags image never loads RF/VM (D14) and keeps bit 1 set (SDM).
  s->fl.word = (flags & ~0x30000u) | 2;
  // Segment selectors follow QEMU's gdbstub: the visible value is stored. In
  // real mode the descriptor cache is not a table lookup -- base = selector
  // << 4 (SDM vol.3 3.4.4) -- so it is synthesized here and a packet that
  // changes CS actually moves the fetch. A protected-mode selector names a
  // descriptor, which this stub does not re-walk (step.h hook comment); there
  // the visible value is stored and the cached base stays. VM86 is not a
  // synthesis case: the flags image never loads VM (D14), so a G packet cannot
  // leave the CPU in VM86.
  const int synth = !(s->cr0 & 1);
  for (int i = 0; i < 6; i++) {
    s->sreg[order[i]] = sel[i];
    if (synth) {
      s->base[order[i]] = (uint64_t)sel[i] << 4;
      s->dbit[order[i]] = 0;
    }
  }
  // gdb sends the linear address x86_gdb_pc reported; the conversion therefore
  // runs after the selectors, so a packet that writes CS and EIP together uses
  // the base the CPU will actually fetch from (the real-mode case this seam was
  // added for), not the one it had before the packet.
  cpu->pc = (uint64_t)(lin - (uint32_t)s->base[cs_i]);
  // The x87 tail, when the packet carries it. The stack image is in ST order,
  // so it loads into (TOP+i); the tag word then follows the packet verbatim
  // if it is present, otherwise the values' own classes.
  if (len >= 16 * 4 + 8 * 10 + 8 * 4) {
    for (int i = 0; i < 8; i++) {
      extFloat80_t v;
      memset(&v, 0, sizeof(v));
      memcpy(&v.signif, p, 8);
      v.signExp = (uint16_t)(p[8] | (p[9] << 8));
      p += 10;
      int phys = (fpu_top(&s->fpu) + i) & 7;
      s->fpu.st[phys] = v;
      fpu_mark(&s->fpu, phys);
    }
    s->fpu.cw = (uint16_t)Le32Get(&p);
    s->fpu.sw = (uint16_t)Le32Get(&p);
    uint16_t tw = (uint16_t)Le32Get(&p);
    s->fpu.fcs = (uint16_t)Le32Get(&p);
    s->fpu.fip = Le32Get(&p);
    s->fpu.fds = (uint16_t)Le32Get(&p);
    s->fpu.fdp = Le32Get(&p);
    s->fpu.fop = (uint16_t)Le32Get(&p);
    // The tag word is authoritative when gdb sends one; a zero tag image
    // would otherwise claim every register is valid.
    if (tw != 0) fpu_set_tags(&s->fpu, tw);
    fpu_refresh(&s->fpu);
  }
  return 0;
}

static int x86_gdb_last_trap(CpuState* cpu, uint64_t* seq) {
  x86_state* s = (x86_state*)cpu->priv;
  *seq = s->trap_seq;
  return s->trap_signal;
}

const isa_ops k_isa_x86 = {
    "x86",
    3,  // EM_386
    x86_init,
    x86_step,
    x86_dump_regs,
    // debug hooks
    x86_cause_name,
    k_x86_flag_names,
    x86_flag_word,
    x86_has_fpr,
    x86_debug_state_line,
    // gdb stub hooks
    x86_gdb_read_regs,
    x86_gdb_write_regs,
    x86_gdb_last_trap,
    x86_gdb_pc,
    x86_gdb_set_watch,
    x86_gdb_clear_watch,
    x86_gdb_watch_hits,
    1,  // ELFCLASS32 (EM_386)
};
