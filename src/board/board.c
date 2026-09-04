#include <string.h>
#include "board/board.h"
#include "util/log.h"

Board *BoardCreate(const char *name, const BoardOpts *opts) {
  if (strcmp(name, "spike") == 0) return SpikeBoardCreate(opts);
  if (strcmp(name, "x86") == 0) return X86BoardCreate(opts);
  if (strcmp(name, "virt") == 0) return VirtBoardCreate(opts);
  LogError("unknown machine '%s'", name);
  return NULL;
}
