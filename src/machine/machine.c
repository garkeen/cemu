#include <string.h>
#include "machine/machine.h"
#include "util/log.h"

Machine *MachineCreate(const char *name, const MachineOpts *opts) {
  if (strcmp(name, "spike") == 0) return SpikeMachineCreate(opts);
  if (strcmp(name, "x86") == 0) return X86MachineCreate(opts);
  if (strcmp(name, "virt") == 0) return VirtMachineCreate(opts);
  LogError("unknown machine '%s'", name);
  return NULL;
}
