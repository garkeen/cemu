// The x86 ops table: the machine layer's handle on the interpreter (init,
// step, irq, dumps) plus the debug hooks the hub queries. The interpreter
// itself is exec.c.
#include <stdio.h>
#include "x86.h"
#include "cpu/isa/isa.h"
#include "debug/debug.h"
#include "host/host.h"

// ---- debug hooks (AGENTS.md §X) ----------------------------------------------

// Set-bit names indexed by EFLAGS bit position, NULL-terminated (the walker
// in debug.c stops at the terminator; holes are "").
static const char *const k_x86_flag_names[] = {
    "cf", "", "pf", "", "af", "", "zf", "sf", "tf", "if", "df", "of",
    NULL,
};

static uint64_t x86_flag_word(frame *f) {
  return ((x86_state *)f->priv)->fl.word;
}

static const char *x86_cause_name(uint64_t cause) {
  switch (cause) {
    case vec_de: return "de";
    case vec_ud: return "ud";
    default: return NULL;  // INTR vectors: debug.c prints the raw number
  }
}

static int x86_has_fpr(frame *f) {
  (void)f;
  return 0;  // no FPR bank in the state stream
}

static void x86_debug_state_line(char *buf, int cap, frame *f) {
  x86_state *s = (x86_state *)f->priv;
  snprintf(buf, (size_t)cap,
           "x: cr0=%08llx cs=%04x ss=%04x ds=%04x es=%04x fs=%04x gs=%04x",
           (unsigned long long)s->cr0, s->sreg[cs_i], s->sreg[ss_i],
           s->sreg[ds_i], s->sreg[es_i], s->sreg[fs_i], s->sreg[gs_i]);
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
};
