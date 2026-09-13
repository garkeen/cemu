#ifndef CEMU_DEBUG_GDBSTUB_H
#define CEMU_DEBUG_GDBSTUB_H

// The gdb Remote Serial Protocol stub (stage 3.5, arch.md): a debug session
// over TCP, single-threaded by design. While the guest runs, the stub only
// acts through the per-step stop callback (connection polling, breakpoint
// match, the async-interrupt byte); when stopped, it blocks on the socket
// and services the protocol until continue/step/detach/kill.
//
// Protocol reference: the GDB "Remote Serial Protocol" document and gem5
// src/base/remote_gdb.cc; packet behavior cross-checked against QEMU's
// gdbstub with a real gdb client.

#include <stdint.h>

struct Board;

typedef struct GdbStub GdbStub;

// Opens the listening socket. wait (the -S flag) means the guest must not
// run until a client resumes it. NULL on failure.
GdbStub* GdbStubStart(struct Board* board, int port, int wait);

// Owns run control until the emulation ends: sessions (attach/stop/step/
// continue) alternate with free runs. Returns when cpu->halted or the
// --max-inst limit is reached.
void GdbStubRun(GdbStub* g, uint64_t max_inst);

void GdbStubFree(GdbStub* g);

#endif
