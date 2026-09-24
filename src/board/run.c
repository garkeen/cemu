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

int BoardRunSteps(Board* m, uint64_t max_inst, int (*stop_cb)(void* ctx, CpuState* cpu),
                  void* cb_ctx) {  while (!m->cpu.halted) {
    if (m->poll) m->poll(m);
    // Host input (stdin: the console or a pipe) goes to the board's sinks; the
    // poll self-gates, so calling it every loop costs almost nothing.
    HostInputPoll();
    // CEMU_DEBUG=mouse=: the synthetic pointer (debug/debug.h). A headless run
    // has no window and a guest-driven test cannot press a button, so the host
    // pointer path would otherwise be unreachable from a test; these events
    // enter through the same sink the window drives.
    DebugInjection inj;
    while (DebugNextMouseEvent(m->cpu.inst_count, &inj)) {
      if (m->mouse_in) m->mouse_in(m->mouse_ctx, inj.dx, inj.dy, inj.dz, inj.buttons);
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
    int asleep = m->cpu.wait;
    if (asleep) {
      // Asleep (wfi/hlt): either jump the emulated clock to the next device
      // deadline (--skip-idle: the guest's idle costs no host time, QEMU's
      // virtual-clock warp) or yield the host for a millisecond and let the
      // wait elapse in wall time. The fallback is not optional when no device
      // is due: only the host can produce a key press.
      int64_t wake_us = (m->skip_idle && m->next_event_us) ? m->next_event_us(m) : 0;
      if (wake_us > 0)
        HostTimerWarp(wake_us);
      else
        HostSleepMs(1);
    }
    m->isa->step(&m->cpu);
    if (asleep && m->cpu.wait) {
      // Still asleep (no interrupt yet): no instruction retired. The per-step
      // observers still get their look — the gdb stub polls for connecting
      // clients here, the same way the board poll (PitPoll) keeps running
      // "more often while the CPU sleeps" (i8254.h).
      if (stop_cb && stop_cb(cb_ctx, &m->cpu)) return 1;
      continue;
    }
    m->cpu.inst_count++;
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
