#include <stdlib.h>

#include "board/board.h"
#include "debug/debug.h"
#include "debug/gdbstub.h"
#include "host/host.h"
#include "util/log.h"

// Physical-memory reader for the debug hub's dump= items (debug/debug.h): the
// hub owns no address space, the board layer does. A region the bus does not
// fully claim is a failed read, not open-bus noise, so a mistyped address
// reports instead of producing a plausible-looking file.
static int DebugReadPhys(void* ctx, uint64_t addr, uint8_t* buf, int len) {
  Bus* bus = (Bus*)ctx;
  if (len <= 0 || BusProbe(bus, addr, len, NULL) != 0) return 0;
  for (int i = 0; i < len; i++) buf[i] = (uint8_t)BusRead(bus, addr + (uint64_t)i, 1);
  return 1;
}

// Linear reader for the hub's dumpl= items (debug/debug.h): a VxD heap page or
// a pageable segment has no stable physical address across runs, so the range
// is named in the address space the guest uses. The walk belongs to the ISA
// (cpu/step.h isa_ops.read_lin) — the hub cannot translate, and the board owns
// no page tables either — so the board only routes the call, the same way the
// physical reader routes to the bus.
static int DebugReadLin(void* ctx, uint64_t lin, uint8_t* buf, int len) {
  Board* m = (Board*)ctx;
  if (len <= 0 || !m->isa || !m->isa->read_lin) return 0;
  return m->isa->read_lin(&m->cpu, lin, buf, len);
}

// The display card's picture, for fbdump= (debug/debug.h). The hub owns no
// device, so the board hands it the card's own vtable calls: the size it
// reports now, and the XRGB buffer behind them (device/video/display.h).
static int DebugFbInfo(void* ctx, int* w, int* h) {
  Board* m = (Board*)ctx;
  if (!m->display_ops) return 0;
  *w = m->display_ops->Width(m->display_dev);
  *h = m->display_ops->Height(m->display_dev);
  return 1;
}

static const uint32_t* DebugFbPixels(void* ctx) {
  Board* m = (Board*)ctx;
  if (!m->display_ops) return NULL;
  return m->display_ops->Framebuffer(m->display_dev);
}

// Host-side servicing (SCANOUT checks, serial FIFOs, virtio pumps, stdin,
// window messages) is paced rather than run on every retired instruction.
// Each of those paths self-gates on wall time, and the clock read behind that
// gate was the interpreter's dominant per-instruction cost. 1024 instructions
// keeps every device edge inside a millisecond at the speeds this machine runs.
enum { kPollStrideLog2 = 10 };

int BoardRunSteps(Board* m, uint64_t max_inst, int (*stop_cb)(void* ctx, CpuState* cpu),
                  void* cb_ctx) {
  // Two step entry points built from one template: the normal run is compiled
  // with STEP_DBG=0 (no debug blocks at all), and an observing run takes the
  // STEP_DBG=1 function that fills the frame's rec fields and calls the hub.
  // Resolved once: DebugInit fixes g_dbg_any before the first step.
  void (*step)(CpuState*) = m->isa->step;
  if (g_dbg_any && m->isa->step_debug) step = m->isa->step_debug;
  // One choice for the whole run, never per access. Every debug step uses the
  // observing bus for both data and fetch: the fetch side is what records RAW
  // bytes, and keeping both sides on wrappers with no host pointer stops a
  // data TLB fill from handing the fetch path a cached host pointer that would
  // bypass that recording. Normal runs use the real bus on both sides.
  if (m->cpu.real_bus) {
    int debug_run = g_dbg_any && m->cpu.debug_bus && m->cpu.debug_fetch_bus;
    m->cpu.bus = debug_run ? m->cpu.debug_bus : m->cpu.real_bus;
    m->cpu.fetch_bus = debug_run ? m->cpu.debug_fetch_bus : m->cpu.real_bus;
  }
  while (!m->cpu.halted) {
    if ((m->cpu.inst_count & ((1u << kPollStrideLog2) - 1)) == 0 || m->cpu.wait) {
      if (m->poll) m->poll(m);
      // Host input (stdin: the console or a pipe) goes to the board's sinks.
      HostInputPoll();
      // CEMU_DEBUG=mouse=/key=: the synthetic pointer and keyboard
      // (debug/debug.h). A headless run has no window, and a guest-driven test
      // cannot press a button or a key, so the host input paths would otherwise
      // be unreachable from a test; these events enter through the same sinks the
      // window drives.
      DebugInjection inj;
      while (DebugNextInjection(m->cpu.inst_count, &inj)) {
        if (inj.kind == kInjMouse) {
          if (m->mouse_in) m->mouse_in(m->mouse_ctx, inj.dx, inj.dy, inj.dz, inj.buttons);
          // Synthetic events are test fixtures: one event is one packet, even
          // though the real window path samples at the guest's programmed rate.
          if (m->mouse_flush) m->mouse_flush(m->mouse_flush_ctx);
        } else {
          if (m->key_in) m->key_in(m->key_ctx, inj.scan, inj.extended, inj.up);
        }
      }
      if (m->display) {
        // Attached display window (-display): pump its message queue and stop
        // the emulation when the user closes it, like QEMU quitting.
        HostDisplayPump(m->display);
        if (HostDisplayClosed(m->display)) {
          LogInfo("display window closed");
          m->cpu.halted = kCpuExited;
          break;
        }
      }
    }
    int asleep = m->cpu.wait;
    if (asleep) {
      // Asleep (wfi/hlt): either jump the emulated clock to the next device
      // deadline (--skip-idle: the guest's idle costs no host time, QEMU's
      // virtual-clock warp) or yield the host for a millisecond and let the
      // wait elapse in wall time. The fallback is not optional when no device
      // is due: only the host can produce a key press.
      // -icount takes the jump unconditionally: its clock only moves through
      // retired instructions, so letting a halt wait out in wall time would
      // advance no virtual time and the interrupt meant to wake it would never
      // arrive (QEMU advances the icount deadline across a halt for the same
      // reason).
      int warp_idle = m->skip_idle || HostTimerIcountOn();
      int64_t wake_ns = (warp_idle && m->next_event_ns) ? m->next_event_ns(m) : 0;
      if (wake_ns > 0)
        HostTimerWarpNs(wake_ns);
      else
        HostSleepMs(1);
    }
    step(&m->cpu);
    if (asleep && m->cpu.wait) {
      // Still asleep (no interrupt yet): no instruction retired. The per-step
      // observers still get their look — the gdb stub polls for connecting
      // clients here, the same way the board poll (PitPoll) keeps running
      // "more often while the CPU sleeps" (i8254.h).
      if (stop_cb && stop_cb(cb_ctx, &m->cpu)) return 1;
      continue;
    }
    m->cpu.inst_count++;
    HostTimerIcountAdvance();
    if (m->reset_pending) {
      // A device asked for a machine reset (port 0x92 bit 0, RCR 0xCF9, the
      // keyboard controller's 0xFE): the guest restarts at the reset vector,
      // so the instruction that requested it is the last one of this life.
      m->reset_pending = 0;
      if (m->reset) m->reset(m);
      continue;
    }
    if (stop_cb && stop_cb(cb_ctx, &m->cpu)) return 1;
    if (max_inst && m->cpu.inst_count >= max_inst) {
      LogError("instruction limit %llu reached at pc=%llx", (unsigned long long)max_inst,
               (unsigned long long)m->cpu.pc);
      return 0;
    }
  }
  return 0;
}

void BoardRun(Board* m, uint64_t max_inst) {
  // The debug hub owns no address space, so the board layer — which does —
  // supplies the reader its dump= items need (debug/debug.h). Reads go through
  // the bus, so a region no device claims fails the dump instead of returning
  // open-bus noise.
  DebugSetMemReader(DebugReadPhys, &m->bus);
  DebugSetLinReader(DebugReadLin, m);
  DebugSetFbReader(DebugFbInfo, DebugFbPixels, m);
  DebugSetClock(&m->cpu.inst_count);
  DebugInit();
  if (m->gdb) {
    // The stub owns run control: sessions (attach/step/continue) alternate
    // with free runs until the emulation ends.
    GdbStubRun(m->gdb, max_inst);
  } else {
    BoardRunSteps(m, max_inst, NULL, NULL);
  }
  frame end;
  end.cpu = &m->cpu;
  end.priv = m->cpu.priv;
  end.isa = m->isa;
  DebugSessionEnd(&end, "halt");
}

void BoardDestroy(Board* m) {
  if (!m) return;
  RamDestroy(m->ram);
  if (m->destroy) m->destroy(m);
  free(m);
}
