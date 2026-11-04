#ifndef CEMU_DEVICE_AUDIO_SB16_H
#define CEMU_DEVICE_AUDIO_SB16_H

#include <stdint.h>

#include "bus/bus.h"

struct HostAudio;

// Creative Sound Blaster 16 (ISA), the card QEMU models in hw/audio/sb16.c:
// DSP command/data ports at 0x220-0x22F, IRQ 5, 8-bit DMA channel 1 and 16-bit
// DMA channel 5 -- the fixed wiring of a real SB16 and of QEMU's -device sb16
// (QEMU's defaults: iobase=0x220, irq=5, dma=1, dma16=5). The DSP is the part
// a guest programs to play PCM: reset returns 0xAA, 0xE1 returns DSP version
// 4.5, 0x41/0x40 set the sample rate, 0x48 sets the block length, and the
// 0x14/0x1C/0x90/0xB0-0xCF family starts a DMA transfer which raises IRQ5 at
// the end of each block. The mixer at 0x224/0x225 holds the volume registers,
// the IRQ/DMA identity bytes and the interrupt-status byte the handler
// acknowledges through 0x22E/0x22F.
//
// The device follows QEMU's sb16.c rather than a clean-room reading of the
// card: command byte order (0x41's two bytes are fetched high-first by
// dsp_get_hilo), the single-rate 0x41/0x42 behaviour, the block-size shifts
// for 16-bit and stereo, and the IRQ-status/ack ports are all QEMU's. The
// gaps this model inherits (OPL3 FM, MPU-401, ADPCM, the 0xE2 identification
// answer, mixer volume scaling) are registered as D43 in AGENTS.md. The
// recording direction is out of scope (user ruling).
//
// DMA is the board's 8237 (device/dma/i8237.c): the card only asks for bytes
// and the controller walks guest memory, the same split the floppy uses. The
// playback clock is the emulated clock (HostTimerNowNs), so the sample rate
// and the block IRQ follow guest time; the host audio queue only limits how
// far ahead the card may run.
typedef struct Sb16Device {
  // DSP command channel: one command byte followed by `needed_bytes` data
  // bytes, both in the order the guest writes them (QEMU's in2_data queue).
  int in_index;
  uint8_t in_data[10];
  int out_data_len;
  uint8_t out_data[50];
  uint8_t last_read_byte;
  int cmd;
  int needed_bytes;
  int v2x6;
  uint8_t csp_param;
  uint8_t csp_value;
  uint8_t csp_mode;
  uint8_t csp_regs[256];
  uint8_t csp_reg83[4];
  int csp_reg83r;
  int csp_reg83w;
  uint8_t test_reg;

  // Playback format and the current block.
  int freq;             // sample rate the DSP is running at
  int time_const;       // 0x40's time constant, -1 = use freq
  int fmt_bits;         // 8 or 16
  int fmt_signed;       // format byte bit 4 (16-bit commands)
  int fmt_stereo;       // format byte bit 5 / mixer 0x0E bit 1
  int dma_auto;         // auto-initialize transfer
  int block_size;       // bytes per IRQ block, -1 = not programmed
  int left_till_irq;    // bytes left in the current block
  int dma_running;      // the DSP has DREQ asserted
  int use_hdma;         // 16-bit DMA channel (5) rather than 1
  int speaker;          // 0xD1/0xD3; off still consumes DMA but is silent
  int align;            // bytes per sample frame minus one
  int bytes_per_second; // the DMA byte rate the DSP programmed

  // Mixer register file (0x00-0xFF).
  int mixer_nreg;
  uint8_t mixer_regs[256];

  // IRQ5. The line is level; the status byte's two low bits are the 8-bit and
  // 16-bit request latches, cleared by reading 0x22E/0x22F.
  int irq_level;
  void (*set_irq)(void* ctx, int line, int level);
  void* irq_ctx;

  // The board's 8237 transfer: returns bytes moved and reports whether the
  // controller reached its terminal count on this call.
  int (*dma)(void* ctx, int channel, uint8_t* buf, int len, int* terminal);
  void* dma_ctx;

  // Host PCM output, opened lazily when the guest starts a transfer.
  struct HostAudio* audio;
  int audio_rate;
  int audio_channels;
  int audio_bits;
  int audio_signed;
  int audio_tried;    // a format has been attempted (do not retry on failure)
  int audio_warned;   // the "no host output" message was printed once

  // Playback clock: bytes consumed since the transfer (re)started, measured
  // against the emulated clock so a paused/continued transfer keeps its place.
  int clock_on;
  int64_t clock_start_ns;
  uint64_t clock_bytes;

  // DSP command 0x80 (silence): the IRQ is due after the silence interval.
  int silence_pending;
  int64_t silence_at_ns;
} Sb16Device;

void Sb16Init(Sb16Device* d);
// A machine reset: the card back to power-on (mixer defaults, DSP reset,
// host stream closed). A DSP reset command goes through the same DSP state but
// leaves the mixer alone -- the card's reset pin and the DSP's reset command
// are not the same thing.
void Sb16Reset(Sb16Device* d);
void Sb16Register(Bus* io, Sb16Device* d);
void Sb16SetIrqSink(Sb16Device* d, void (*set_irq)(void* ctx, int line, int level), void* ctx);
void Sb16SetDmaSink(Sb16Device* d,
                    int (*dma)(void* ctx, int channel, uint8_t* buf, int len, int* terminal),
                    void* ctx);
// Advances the sample clock and moves DMA bytes into the host stream. Called
// from the board's poll (the run loop already calls it while the CPU sleeps).
void Sb16Poll(Sb16Device* d);
void Sb16Destroy(Sb16Device* d);

extern const DeviceOps kSb16Ops;

#endif
