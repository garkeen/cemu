// The gdb Remote Serial Protocol stub (stage 3.5). Single-threaded: the
// session and the guest take turns. Two stop sources exist while the guest
// runs — the breakpoint list (pc-match, QEMU/gem5 style: no guest-memory
// patching, so Z0/Z1 behave like a hardware breakpoint) and the async
// interrupt byte 0x03; attaching to a running guest stops it (single-thread
// stub; QEMU answers the handshake from its io-thread instead — documented
// deviation, the guest pauses when gdb connects).
//
// Stub memory access is physical (the bus as the devices see it): real mode
// and bare-metal M-mode — the stage-3.5 debugging targets — have linear ==
// physical. A protected-mode virtual view needs a non-faulting translate
// probe and is future work.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board/board.h"
#include "debug/debug.h"
#include "debug/gdbstub.h"
#include "host/host.h"
#include "util/log.h"

enum {
  kPacketMax = 4096,       // matches the qSupported PacketSize we advertise
  kMaxBreakpoints = 64,
};

struct GdbStub {
  Board* board;
  HostSock* listen;
  HostSock* conn;
  int wait;  // -S: block before the first step until a client resumes
  uint64_t bps[kMaxBreakpoints];
  int nbps;
  int steps_left;        // single-step quota in the current batch
  uint8_t pending[256];  // bytes read while running that were not 0x03
  int npending;
  int stop_pending;      // the last run batch ended for a stop reason
  int stop_sig;
  uint64_t last_trap_seq;  // gdb_last_trap seq already reported/absorbed
};

// ---- hex helpers -----------------------------------------------------------------

static int HexVal(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static uint64_t HexU64(const char* s, const char** end) {
  uint64_t v = 0;
  while (*s) {
    int d = HexVal((uint8_t)*s);
    if (d < 0) break;
    v = (v << 4) | (uint64_t)d;
    s++;
  }
  *end = s;
  return v;
}

static void HexEncode(const uint8_t* src, int n, char* out) {
  for (int i = 0; i < n; i++) {
    out[2 * i] = "0123456789abcdef"[src[i] >> 4];
    out[2 * i + 1] = "0123456789abcdef"[src[i] & 15];
  }
  out[2 * n] = 0;
}

static int HexDecode(const char* s, uint8_t* dst, int cap) {
  int n = 0;
  while (s[0] && s[1] && n < cap) {
    int hi = HexVal((uint8_t)s[0]), lo = HexVal((uint8_t)s[1]);
    if (hi < 0 || lo < 0) break;
    dst[n++] = (uint8_t)((hi << 4) | lo);
    s += 2;
  }
  return n;
}

// ---- packet framing ---------------------------------------------------------------

// Reads one packet into out (NUL-terminated); returns its length, 0 if the
// connection dropped. Ack bytes ('+'/'-') are dropped; 0x03 outside a packet
// becomes the pending stop reason. A bad checksum answers '-' and re-syncs
// on the client's resend.
static int PktRead(GdbStub* g, char* out, int cap) {
  uint8_t b;
  for (;;) {
    if (!HostSockRead(g->conn, &b, 1)) return 0;
    if (b == 0x03) {
      g->stop_pending = 1;
      g->stop_sig = 2;
      continue;
    }
    if (b != '$') continue;  // acks and noise between packets
    int len = 0;
    uint8_t sum = 0;
    while (HostSockRead(g->conn, &b, 1) == 1 && b != '#') {
      sum += b;
      if (b == '}') {  // binary escape: the next byte is XOR 0x20
        uint8_t esc;
        if (!HostSockRead(g->conn, &esc, 1)) return 0;
        sum += esc;
        b = (uint8_t)(esc ^ 0x20);
      }
      if (len < cap - 1) out[len++] = (char)b;
    }
    if (b != '#') return 0;  // connection dropped mid-packet
    uint8_t hi, lo;
    if (!HostSockRead(g->conn, &hi, 1) || !HostSockRead(g->conn, &lo, 1)) return 0;
    if (HexVal(hi) < 0 || HexVal(lo) < 0) continue;  // garbage trailer: re-sync
    uint8_t want = (uint8_t)((HexVal(hi) << 4) | HexVal(lo));
    uint8_t csum = (uint8_t)(want == sum ? '+' : '-');
    HostSockWrite(g->conn, &csum, 1);
    if (want == sum) {
      out[len] = 0;
      DebugGdbPkt(0, out);
      return len;
    }
  }
}

static void PktSend(GdbStub* g, const char* data) {
  DebugGdbPkt(1, data);
  size_t dlen = strlen(data);
  if (dlen > kPacketMax - 4) dlen = kPacketMax - 4;
  uint8_t frame[kPacketMax + 4];
  uint8_t sum = 0;
  for (size_t i = 0; i < dlen; i++) sum += (uint8_t)data[i];
  int n = 0;
  frame[n++] = '$';
  memcpy(frame + n, data, dlen);
  n += (int)dlen;
  frame[n++] = '#';
  frame[n++] = (uint8_t)"0123456789abcdef"[sum >> 4];
  frame[n++] = (uint8_t)"0123456789abcdef"[sum & 15];
  for (int tries = 0; tries < 3; tries++) {
    if (!HostSockWrite(g->conn, frame, n)) return;
    uint8_t ack;
    if (!HostSockRead(g->conn, &ack, 1)) return;
    if (ack == '+') return;
    // '-' or noise: resend
  }
}

// ---- the target description (qXfer:features:read:target.xml) -----------------------

// Exactly the register files the isa_ops hooks serve: x86 = gdb's i386 core
// feature (with the i386_eflags flags type, per gdb's own features file —
// accepted by gdb's i386 validation only WITH the <architecture> element),
// riscv64 = the integer cpu feature. FP registers are absent (the state
// stream has no FPR rows on x86; the riscv F/D view rides on E1, not on the
// stub).
static const char* TargetXml(const char* isa_name) {
  static char xml[4096];
  static int built;
  static const char* cached = "";
  if (built) return cached;
  built = 1;
  if (strcmp(isa_name, "x86") == 0) {
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
             "<target><architecture>i386</architecture>"
             "<feature name=\"org.gnu.gdb.i386.core\">"
             "<flags id=\"i386_eflags\" size=\"4\">"
             "<field name=\"\" start=\"22\" end=\"31\"/>"
             "<field name=\"ID\" start=\"21\" end=\"21\"/>"
             "<field name=\"VIP\" start=\"20\" end=\"20\"/>"
             "<field name=\"VIF\" start=\"19\" end=\"19\"/>"
             "<field name=\"AC\" start=\"18\" end=\"18\"/>"
             "<field name=\"VM\" start=\"17\" end=\"17\"/>"
             "<field name=\"RF\" start=\"16\" end=\"16\"/>"
             "<field name=\"\" start=\"15\" end=\"15\"/>"
             "<field name=\"NT\" start=\"14\" end=\"14\"/>"
             "<field name=\"IOPL\" start=\"12\" end=\"13\"/>"
             "<field name=\"OF\" start=\"11\" end=\"11\"/>"
             "<field name=\"DF\" start=\"10\" end=\"10\"/>"
             "<field name=\"IF\" start=\"9\" end=\"9\"/>"
             "<field name=\"TF\" start=\"8\" end=\"8\"/>"
             "<field name=\"SF\" start=\"7\" end=\"7\"/>"
             "<field name=\"ZF\" start=\"6\" end=\"6\"/>"
             "<field name=\"\" start=\"5\" end=\"5\"/>"
             "<field name=\"AF\" start=\"4\" end=\"4\"/>"
             "<field name=\"\" start=\"3\" end=\"3\"/>"
             "<field name=\"PF\" start=\"2\" end=\"2\"/>"
             "<field name=\"\" start=\"1\" end=\"1\"/>"
             "<field name=\"CF\" start=\"0\" end=\"0\"/>"
             "</flags>"
             "<reg name=\"eax\" bitsize=\"32\" type=\"int32\" regnum=\"0\"/>"
             "<reg name=\"ecx\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"edx\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"ebx\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"esp\" bitsize=\"32\" type=\"data_ptr\"/>"
             "<reg name=\"ebp\" bitsize=\"32\" type=\"data_ptr\"/>"
             "<reg name=\"esi\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"edi\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"eip\" bitsize=\"32\" type=\"code_ptr\"/>"
             "<reg name=\"eflags\" bitsize=\"32\" type=\"i386_eflags\"/>"
             "<reg name=\"cs\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"ss\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"ds\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"es\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"fs\" bitsize=\"32\" type=\"int32\"/>"
             "<reg name=\"gs\" bitsize=\"32\" type=\"int32\"/>"
             // gdb's i386 validation keeps the x87 group inside the core
             // feature (gdb features/i386/32bit-core.xml). This machine has
             // no FPU (D13) — the registers exist in the description and
             // read/write as zeros.
             "<reg name=\"st0\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"st1\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"st2\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"st3\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"st4\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"st5\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"st6\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"st7\" bitsize=\"80\" type=\"i387_ext\"/>"
             "<reg name=\"fctrl\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "<reg name=\"fstat\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "<reg name=\"ftag\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "<reg name=\"fiseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "<reg name=\"fioff\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "<reg name=\"foseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "<reg name=\"fooff\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "<reg name=\"fop\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
             "</feature></target>");
    cached = xml;
  } else if (strcmp(isa_name, "riscv64") == 0) {
    static const char* names[32] = {"zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
                                    "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
                                    "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
                                    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
    size_t off = (size_t)snprintf(xml, sizeof(xml),
                                  "<?xml version=\"1.0\"?>"
                                  "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
                                  "<target><architecture>riscv:rv64</architecture>"
                                  "<feature name=\"org.gnu.gdb.riscv.cpu\">");
    for (int i = 0; i < 32 && off < sizeof(xml) - 96; i++)
      off += (size_t)snprintf(xml + off, sizeof(xml) - off,
                              "<reg name=\"%s\" bitsize=\"64\" type=\"int64\"/>", names[i]);
    snprintf(xml + off, sizeof(xml) - off,
             "<reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\"/>"
             "</feature></target>");
    cached = xml;
  }
  return cached;
}

// ---- stop sources --------------------------------------------------------------------

// Consumes bytes that arrived while the guest was running; returns 1 when an
// async interrupt (0x03) was among them. Everything else parks in pending —
// it can only be ack bytes or a packet that raced the stop.
static int PendingDrain(GdbStub* g) {
  uint8_t buf[64];
  int n;
  while (g->npending < (int)sizeof(g->pending) && HostSockReadable(g->conn) &&
         (n = HostSockRead(g->conn, buf, (int)sizeof buf)) > 0) {
    for (int i = 0; i < n; i++) {
      if (buf[i] == 0x03) {
        g->stop_sig = 2;
        return 1;
      }
      if (g->npending < (int)sizeof(g->pending)) g->pending[g->npending++] = buf[i];
    }
  }
  return 0;
}

static int BpHit(GdbStub* g, uint64_t pc) {
  for (int i = 0; i < g->nbps; i++)
    if (g->bps[i] == pc) return 1;
  return 0;
}

// The per-step callback BoardRunSteps invokes after every committed step.
// Post-commit is the right observation point: the committed pc is the NEXT
// instruction, so a match stops before that instruction executes — the
// observable behavior of a hardware breakpoint.
static int StopCb(void* ctx, CpuState* cpu) {
  GdbStub* g = (GdbStub*)ctx;
  const isa_ops* isa = g->board->isa;
  // A trap delivered by the step that just ran stops only a single-step
  // batch (QEMU: a free-running guest keeps its faults to itself).
  if (g->steps_left && isa->gdb_last_trap) {
    uint64_t seq;
    int sig = isa->gdb_last_trap(cpu, &seq);
    if (sig && seq != g->last_trap_seq) {
      g->last_trap_seq = seq;
      g->stop_sig = sig;
      return 1;
    }
  }
  if (BpHit(g, cpu->pc)) {
    g->stop_sig = 5;
    return 1;
  }
  if (g->conn && PendingDrain(g)) return 1;
  if (g->steps_left) {
    g->steps_left = 0;
    g->stop_sig = 5;
    return 1;
  }
  return 0;
}

// Free-run callback: the same stop sources plus connection polling.
static int FreeCb(void* ctx, CpuState* cpu) {
  GdbStub* g = (GdbStub*)ctx;
  if (!g->conn) {
    g->conn = HostSockAcceptPoll(g->listen);
    if (g->conn) {
      g->stop_sig = 5;  // fresh attach: generic trap
      return 1;
    }
    return 0;
  }
  return StopCb(ctx, cpu);
}

// ---- the session -----------------------------------------------------------------------

static void DoRun(GdbStub* g, int step) {
  Board* m = g->board;
  char r[16];
  if (m->cpu.halted) {
    snprintf(r, sizeof(r), "W%02x", m->cpu.exit_code & 0xff);
    PktSend(g, r);
    return;
  }
  g->steps_left = step ? 1 : 0;
  BoardRunSteps(m, 0, StopCb, g);
  if (m->cpu.halted) {
    snprintf(r, sizeof(r), "W%02x", m->cpu.exit_code & 0xff);
    PktSend(g, r);
    return;
  }
  snprintf(r, sizeof(r), "S%02x", g->stop_sig);
  PktSend(g, r);
}

// Serves one attached client until detach ('D'), kill ('k') or disconnect.
// Returns 1 = detached (the caller keeps running), 0 = emulation over.
static int Session(GdbStub* g) {
  Board* m = g->board;
  char pkt[kPacketMax];
  for (;;) {
    int n = PktRead(g, pkt, sizeof pkt);
    if (n <= 0) {  // client went away: drop it and keep running
      HostSockClose(g->conn);
      g->conn = NULL;
      return 1;
    }
    switch (pkt[0]) {
      case '?': {
        char r[16];
        snprintf(r, sizeof(r), "S%02x", g->stop_sig);
        PktSend(g, r);
        break;
      }
      case 'c':
      case 'C':
        DoRun(g, 0);
        break;
      case 's':
      case 'S':
        DoRun(g, 1);
        break;
      case 'g': {
        uint8_t img[512];
        int len = m->isa->gdb_read_regs ? m->isa->gdb_read_regs(&m->cpu, img, (int)sizeof img) : 0;
        char hex[2 * 512 + 1];
        HexEncode(img, len, hex);
        PktSend(g, hex);
        break;
      }
      case 'G': {
        uint8_t img[512];
        int len = HexDecode(pkt + 1, img, (int)sizeof img);
        int rc = m->isa->gdb_write_regs ? m->isa->gdb_write_regs(&m->cpu, img, len) : -1;
        PktSend(g, rc == 0 ? "OK" : "E11");
        break;
      }
      case 'm': {
        const char* p;
        uint64_t addr = HexU64(pkt + 1, &p);
        uint64_t len = (*p == ',') ? HexU64(p + 1, &p) : 0;
        if (len == 0 || len > kPacketMax / 2) {
          PktSend(g, "E14");
          break;
        }
        uint8_t buf[kPacketMax / 2];
        if (BusProbe(m->cpu.bus, addr, (int)len, NULL) == 0) {
          for (uint64_t i = 0; i < len; i++) buf[i] = (uint8_t)BusRead(m->cpu.bus, addr + i, 1);
          char hex[2 * kPacketMax / 2 + 1];
          HexEncode(buf, (int)len, hex);
          PktSend(g, hex);
        } else {
          PktSend(g, "E14");  // unmapped physical address
        }
        break;
      }
      case 'M': {
        const char* p;
        uint64_t addr = HexU64(pkt + 1, &p);
        uint64_t len = 0;
        const char* data = NULL;
        if (*p == ',') {
          len = HexU64(p + 1, &p);
          if (*p == ':') data = p + 1;
        }
        uint8_t buf[kPacketMax / 2];
        if (!data || len == 0 || len > kPacketMax / 2 ||
            HexDecode(data, buf, (int)sizeof buf) != (int)len) {
          PktSend(g, "E14");
          break;
        }
        if (BusProbe(m->cpu.bus, addr, (int)len, NULL) == 0) {
          for (uint64_t i = 0; i < len; i++) BusWrite(m->cpu.bus, addr + i, 1, buf[i]);
          PktSend(g, "OK");
        } else {
          PktSend(g, "E14");
        }
        break;
      }
      case 'Z':
      case 'z': {
        // Z(kind),addr,len — kinds 0 (software) and 1 (hardware) share the
        // pc-match list (no memory patching); kinds 2-4 (watchpoints) are
        // not implemented yet and answer empty.
        if (pkt[1] != '0' && pkt[1] != '1') {
          PktSend(g, "");
          break;
        }
        const char* p;
        uint64_t addr = HexU64(pkt + 3, &p);
        if (pkt[0] == 'Z') {
          if (g->nbps < kMaxBreakpoints) g->bps[g->nbps++] = addr;
          PktSend(g, "OK");
        } else {
          int w = 0;
          for (int i = 0; i < g->nbps; i++)
            if (g->bps[i] != addr) g->bps[w++] = g->bps[i];
          g->nbps = w;
          PktSend(g, "OK");
        }
        break;
      }
      case 'D':  // detach: the guest runs free
        PktSend(g, "OK");
        g->nbps = 0;
        HostSockClose(g->conn);
        g->conn = NULL;
        return 1;
      case 'k':  // kill: end the emulation
        m->cpu.halted = kCpuExited;
        m->cpu.exit_code = 0;
        return 0;
      case 'H':  // thread operations: one hart, nothing to select
        PktSend(g, "OK");
        break;
      case 'q':
        if (strncmp(pkt, "qSupported", 10) == 0) {
          PktSend(g, "PacketSize=1000;qXfer:features:read+");
        } else if (strncmp(pkt, "qXfer:features:read:", 20) == 0) {
          // qXfer:features:read:ANNEX:OFFSET,LENGTH
          const char* annex = pkt + 20;
          const char* colon = strchr(annex, ':');
          const char* p;
          if (colon && strncmp(annex, "target.xml", (size_t)(colon - annex)) == 0) {
            uint64_t off = HexU64(colon + 1, &p);
            uint64_t len = (*p == ',') ? HexU64(p + 1, &p) : 0;
            const char* xml = TargetXml(m->isa->name);
            size_t total = strlen(xml);
            if (off < total && len > 0) {
              size_t chunk = total - off;
              if (chunk > len) chunk = len;
              char r[kPacketMax];
              r[0] = off + chunk < total ? 'm' : 'l';
              memcpy(r + 1, xml + off, chunk);
              r[1 + chunk] = 0;
              PktSend(g, r);
            } else {
              PktSend(g, "l");
            }
          } else {
            PktSend(g, "E00");
          }
        } else if (strcmp(pkt, "qfThreadInfo") == 0) {
          PktSend(g, "m1");
        } else if (strcmp(pkt, "qsThreadInfo") == 0) {
          PktSend(g, "l");
        } else if (strcmp(pkt, "qAttached") == 0) {
          PktSend(g, "1");  // attached to an existing process
        } else {
          PktSend(g, "");
        }
        break;
      case 'v':
        if (strcmp(pkt, "vCont?") == 0) {
          PktSend(g, "");  // no vCont: gdb falls back to c/s
        } else if (strncmp(pkt, "vCont;", 6) == 0) {
          DoRun(g, pkt[6] == 's' ? 1 : 0);  // vCont;c / vCont;s (one action)
        } else {
          PktSend(g, "");
        }
        break;
      default:
        PktSend(g, "");  // unsupported packet: empty reply
        break;
    }
  }
}

// ---- entry points ------------------------------------------------------------------------

GdbStub* GdbStubStart(struct Board* board, int port, int wait) {
  HostSock* l = HostSockListen(port);
  if (!l) {
    LogError("gdb stub: cannot listen on port %d", port);
    return NULL;
  }
  GdbStub* g = (GdbStub*)calloc(1, sizeof(GdbStub));
  if (!g) {
    HostSockClose(l);
    return NULL;
  }
  g->board = board;
  g->listen = l;
  g->wait = wait;
  LogInfo("gdb stub listening on port %d%s", port, wait ? " (-S: stopped until resume)" : "");
  return g;
}

void GdbStubRun(GdbStub* g, uint64_t max_inst) {
  Board* m = g->board;
  if (g->wait) {  // -S: stopped before the first step until a client resumes
    g->conn = HostSockAccept(g->listen);
    if (!g->conn) return;
    g->stop_sig = 5;  // fresh attach: generic trap
    if (!Session(g)) return;
  }
  for (;;) {
    // Free run: the guest executes; the stub watches for connections,
    // breakpoints and Ctrl-C through the per-step callback.
    uint64_t left = max_inst ? max_inst - m->cpu.inst_count : 0;
    g->stop_pending = 0;
    BoardRunSteps(m, left, FreeCb, g);
    if (m->cpu.halted) break;
    if (!g->stop_pending) break;  // the --max-inst limit, logged in the loop
    if (!g->conn) break;          // defensive: a stop without a client
    if (!Session(g)) break;
  }
}

void GdbStubFree(GdbStub* g) {
  if (!g) return;
  if (g->conn) HostSockClose(g->conn);
  if (g->listen) HostSockClose(g->listen);
  free(g);
}
