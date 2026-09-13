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
// group gdb keeps in the same feature (st0-7 at 10 bytes, fctrl..fop at 4;
// this machine has no FPU, D13, so those read as zeros and ignore writes).
static int x86_gdb_read_regs(CpuState* cpu, uint8_t* buf, int cap) {
  x86_state* s = (x86_state*)cpu->priv;
  if (cap < 16 * 4 + 8 * 10 + 8 * 4) return 0;
  uint8_t* p = buf;
  for (int i = 0; i < 8; i++) Le32Put(&p, s->r[i].e);
  Le32Put(&p, (uint32_t)cpu->pc);
  Le32Put(&p, s->fl.word);
  static const int order[6] = {cs_i, ss_i, ds_i, es_i, fs_i, gs_i};
  for (int i = 0; i < 6; i++) Le32Put(&p, s->sreg[order[i]]);
  memset(p, 0, 8 * 10 + 8 * 4);  // st0-7, fctrl..fop: no x87 state (D13)
  p += 8 * 10 + 8 * 4;
  return (int)(p - buf);
}

static int x86_gdb_write_regs(CpuState* cpu, const uint8_t* buf, int len) {
  x86_state* s = (x86_state*)cpu->priv;
  if (len < 16 * 4) return -1;
  const uint8_t* p = buf;
  for (int i = 0; i < 8; i++) s->r[i].e = Le32Get(&p);
  cpu->pc = Le32Get(&p);
  // The flags image never loads RF/VM (D14) and keeps bit 1 set (SDM).
  s->fl.word = (Le32Get(&p) & ~0x30000u) | 2;
  // Segment selectors follow QEMU's gdbstub: the visible value is stored,
  // the descriptor caches are not re-walked (step.h hook comment).
  static const int order[6] = {cs_i, ss_i, ds_i, es_i, fs_i, gs_i};
  for (int i = 0; i < 6; i++) s->sreg[order[i]] = (uint16_t)Le32Get(&p);
  return 0;  // the x87 tail (if present) has no state to write into
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
};
