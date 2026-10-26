// The raise channel: the one piece of execution machinery shared by every
// interpreter (AGENTS.md). Faults unwind the whole step — the landing pad's
// consumer installs the resume point; no partial state survives.
#include "cpu/step.h"

_Noreturn void raise_(frame* f, uint64_t cause, uint64_t tval) {
  f->trap.cause = cause;
  f->trap.tval = tval;
  f->trap.has_paddr = 0;
  longjmp(f->raise, 1);
}

_Noreturn void raise_paddr_(frame* f, uint64_t cause, uint64_t tval, uint64_t paddr) {
  f->trap.cause = cause;
  f->trap.tval = tval;
  f->trap.paddr = paddr;
  f->trap.has_paddr = 1;
  longjmp(f->raise, 1);
}
