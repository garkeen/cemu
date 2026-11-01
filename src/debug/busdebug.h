#ifndef CEMU_DEBUG_BUSDEBUG_H
#define CEMU_DEBUG_BUSDEBUG_H

#include "bus/bus.h"

// The observing bus: a copy of the machine's bus whose regions forward to the
// real devices and emit DebugMem/DebugBus at the forwarding point. Its regions
// deliberately carry no host pointer, so the CPU's TLB cannot bypass it; every
// fetch and data access reaches the wrapper.
//
// One instance owns two Bus objects: the data bus (DebugMem + DebugBus) and
// the fetch bus (DebugMem only). Keeping fetch on its own Bus is what lets the
// wrapper honour the bus category's "no instruction fetch" rule without a
// per-access is_fetch test in the interpreter.
typedef struct BusDebug BusDebug;

BusDebug* BusDebugCreate(Bus* real);
void BusDebugDestroy(BusDebug* d);
// Rebuilds the two wrapper buses when the real bus's region set moved (PCI
// BAR programming, VGA detach/attach). Cheap when nothing changed.
void BusDebugSync(BusDebug* d);
Bus* BusDebugDataBus(BusDebug* d);
Bus* BusDebugFetchBus(BusDebug* d);

#endif
