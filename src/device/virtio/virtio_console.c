// VirtIO console port 0 (spec 5.3). Queue 1 is the driver's transmit queue:
// guest bytes reach the host console at notify time. Queue 0 is the receive
// queue, drained manually: the host stages input bytes and Poll pushes them
// into whatever buffers the driver has posted, raising the used-ring
// interrupt -- which is how Linux's hvc console reads. The config page
// carries cols/rows (VIRTIO_CONSOLE_F_SIZE).

#include <stdlib.h>
#include <string.h>

#include "debug/debug.h"
#include "device/virtio/virtio_console.h"
#include "host/host.h"

enum { kConsoleQueueRx = 0, kConsoleQueueTx = 1 };
enum { kConsoleFeatureSize = 1u << 0 };

static int ConsoleRecv(VirtioDevice* v, int q, int desc, int rd, int wr) {
  (void)wr;
  if (q != kConsoleQueueTx) return 0;
  uint8_t* buf = malloc(rd);
  int copied = rd > 0 ? VirtioCopyFromChain(v, q, desc, 0, buf, rd) : -1;
  if (copied == 0) HostWriteOut((const char*)buf, rd);
  DebugVirtio("console-tx", v->device_id, (uint64_t)q, (uint64_t)rd, (uint64_t)copied);
  free(buf);
  VirtioConsumeChain(v, q, desc, 0);
  return 0;
}

static void ConsoleReset(VirtioDevice* v) {
  VirtioConsole* con = (VirtioConsole*)v;
  con->rx_head = con->rx_count = 0;
}

VirtioConsole* VirtioConsoleCreate(Bus* bus, uint64_t base, uint64_t size, int irq_src,
                                   void (*set_irq)(void*, int, int), void* irq_ctx) {
  VirtioConsole* con = calloc(1, sizeof *con);
  VirtioInit(&con->vdev, bus, 3 /*virtio-console id, spec 5.3*/,
             kVirtioVendorId, kConsoleFeatureSize, 4 /*cols/rows*/,
             ConsoleRecv);
  con->vdev.reset_hook = ConsoleReset;
  con->vdev.queue[kConsoleQueueRx].manual = 1;
  con->vdev.config[0] = 128;  // cols: the simplefb text grid the guest draws
  con->vdev.config[1] = 0;
  con->vdev.config[2] = 48;  // rows
  con->vdev.config[3] = 0;
  con->vdev.irq_src = irq_src;
  con->vdev.set_irq = set_irq;
  con->vdev.irq_ctx = irq_ctx;
  BusAddRegion(bus, base, size, &kVirtioMmioOps, &con->vdev);
  return con;
}

void VirtioConsoleReceive(VirtioConsole* con, int ch) {
  if (con->rx_count < kVirtioConsoleRxBuf) {
    con->rx[(con->rx_head + con->rx_count) % kVirtioConsoleRxBuf] = (uint8_t)ch;
    con->rx_count++;
  }
}

void VirtioConsolePoll(VirtioConsole* con) {
  // Drain pending transmit chains even when the guest's QueueNotify did not
  // arrive. The guest's put_chars waits on the used ring (Linux 4.15
  // virtio_console __send_to_port), so a missed kick turns into a boot-wide
  // stall. One drain per millisecond of emulated time is invisible to the
  // console and bounds the cost on the per-instruction board poll.
  int64_t now = HostTimerNow();
  if (now - con->last_tx_poll_us >= 1000) {
    con->last_tx_poll_us = now;
    VirtioQueue* qs = &con->vdev.queue[kConsoleQueueTx];
    uint16_t avail = VirtioQueueAvail(&con->vdev, kConsoleQueueTx);
    if (avail != qs->last_avail_idx)
      DebugVirtio("poll-work", con->vdev.device_id, avail, qs->last_avail_idx, qs->num);
    static int64_t dbg_us;
    if (DebugOn(kDbgVirtio) && now - dbg_us >= 1000000) {
      dbg_us = now;
      DebugVirtio("poll", con->vdev.device_id, qs->num, qs->avail_addr,
                  (uint64_t)avail << 16 | qs->last_avail_idx);
    }
    VirtioNotifyQueue(&con->vdev, kConsoleQueueTx);
  }
  while (con->rx_count > 0) {
    uint8_t ch = con->rx[con->rx_head];
    int n = VirtioManualFill(&con->vdev, kConsoleQueueRx, &ch, 1);
    if (n == 0) break;  // driver has no receive buffer posted yet
    con->rx_head = (con->rx_head + 1) % kVirtioConsoleRxBuf;
    con->rx_count--;
  }
}

void VirtioConsoleDestroy(VirtioConsole* con) {
  free(con);
}
