#ifndef CEMU_TOOLS_FRONT_RSP_H
#define CEMU_TOOLS_FRONT_RSP_H

// RSP client for the cemugui front-end (阶段 3.5 片 3): packet framing and
// the register-table walk over the gdb remote serial protocol. The peer is
// cemu's stub (debug/gdbstub.c) — cross-checked against the same protocol
// document and QEMU's gdbstub. All payloads this client sends are ASCII, so
// no binary escaping is produced; incoming '}' escapes are decoded.

#include <stdint.h>

typedef struct Rsp Rsp;

// Hex digit value; -1 for non-hex characters (shared by the UI's decoders).
int RspHexVal(uint8_t c);

// Connects to "host:port" (e.g. "127.0.0.1:1234"); NULL on error.
Rsp* RspConnect(const char* hostport);
void RspClose(Rsp* r);

// Sends one packet and consumes the ack; 0 = ok, -1 = connection lost.
int RspSend(Rsp* r, const char* pkt);
// Sends the raw async-interrupt byte (0x03); 0 = ok, -1 = connection lost.
int RspInterrupt(Rsp* r);
// Blocking receive; returns payload length (>= 0), -1 = connection lost.
int RspRecv(Rsp* r, char* pkt, int cap);
// Non-blocking receive: 1 = packet read, 0 = nothing waiting, -1 = lost.
int RspPoll(Rsp* r, char* pkt, int cap);

// Register table parsed from the target description (qXfer target.xml), in
// g-packet order. No ISA knowledge lives here — the description names and
// sizes every register.
enum { kRspMaxRegs = 128, kRspNameMax = 16 };
typedef struct {
  char name[kRspNameMax];
  int size;      // bytes in the g-packet image (bitsize / 8)
  int hidden;    // float-group / x87 members the UI does not list
  int code_ptr;  // the type="code_ptr" register is the program counter
} RspReg;
typedef struct {
  RspReg reg[kRspMaxRegs];
  int n;
} RspRegTable;
// Fetches and parses the target description; 0 = ok.
int RspReadRegTable(Rsp* r, RspRegTable* t);

#endif
