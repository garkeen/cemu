// The raise channel: the one piece of execution machinery shared by every
// interpreter (AGENTS.md). Faults unwind the whole step — the landing pad's
// consumer installs the resume point; no partial state survives.
#include "cpu/step.h"

_Noreturn void raise_(frame* f, uint64_t cause, uint64_t tval) {
  f->trap.cause = cause;
  f->trap.tval = tval;
  longjmp(f->raise, 1);
}
