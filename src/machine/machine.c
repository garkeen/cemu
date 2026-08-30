#include <string.h>
#include "machine/machine.h"
#include "util/log.h"

Machine *MachineCreate(const char *name, const MachineOpts *opts) {
  if (strcmp(name, "spike") == 0) return SpikeMachineCreate(opts);
  LogError("unknown machine '%s'", name);
  return NULL;
}
