#include "device/audio/sb16.h"

#include <string.h>

#include "host/host.h"
#include "util/log.h"

// The card's fixed PC/AT wiring and the DSP's externally visible identity.
// QEMU hw/audio/sb16.c's sb16_properties defaults are the same four values
// (iobase 0x220, irq 5, dma 1, dma16 5); the card has no PCI function and the
// machine does not offer alternate jumper settings.
enum {
  kSb16Base = 0x220,
  kSb16PortCount = 16,
  kSb16IrqLine = 5,
  kSb16Dma8Channel = 1,
  kSb16Dma16Channel = 5,
  kSb16Version = 0x0405,  // DSP 4.5
  kSb16DefaultRate = 11025,
  kSb16RateMin = 5000,
  kSb16RateMax = 45000,
  // One poll moves at most this many bytes. At 44.1 kHz stereo 16-bit that is
  // 23 ms of audio; the run loop polls every 1024 instructions, so the cap is
  // only reached after a stall (a warped idle, a slow host stream).
  kSb16ChunkBytes = 4096,
  // DSP command 0x80 raises the IRQ immediately below this interval; QEMU's
  // sb16.c uses `ticks < NANOSECONDS_PER_SECOND / 1024`.
  kSb16SilenceMinNs = 1000000000 / 1024,
  kSb16NsPerSecond = 1000000000,
};

// The two IRQ latches in mixer register 0x82 (QEMU sb16.c SB_read_DMA sets
// `(nchan & 4) ? 2 : 1`; the handler acknowledges them through 0x22E/0x22F).
enum {
  kSb16Irq8 = 1,   // 8-bit DMA block done
  kSb16Irq16 = 2,  // 16-bit DMA block done
};

// dma_cmd8's mask (QEMU DMA8_AUTO; its DMA8_HIGH bit only fed a commented-out
// `highspeed` field, so 0x90/0x91 use the same auto/single split without it).
enum { kSb16Dma8Auto = 1 };

// The DSP identification string (DSP command 0xE3). QEMU sb16.c's e3[].
static const char kSb16Copyright[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

static int RestrictRate(int freq) {
  if (freq < kSb16RateMin) return kSb16RateMin;
  if (freq > kSb16RateMax) return kSb16RateMax;
  return freq;
}

static void DspOut(Sb16Device* d, uint8_t val) {
  if (d->out_data_len < (int)sizeof(d->out_data)) d->out_data[d->out_data_len++] = val;
}

// The DSP's data buffer is a stack: the last byte written is the first read
// (QEMU sb16.c dsp_get_data). A command that runs short is a guest error; the
// hardware would return whatever the buffer holds, so 0 is the least
// surprising value and keeps the state machine moving.
static uint8_t DspGet(Sb16Device* d) {
  if (d->in_index > 0) return d->in_data[--d->in_index];
  return 0;
}

// QEMU's two byte-pair orders: 0x48/0x14/0x80 write low then high and read
// high first; 0x41/0x42 are the opposite (its dsp_get_hilo), which is what the
// SB16 drivers QEMU was calibrated against send.
static uint16_t DspGetLohi(Sb16Device* d) {
  uint8_t hi = DspGet(d);
  uint8_t lo = DspGet(d);
  return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static uint16_t DspGetHilo(Sb16Device* d) {
  uint8_t lo = DspGet(d);
  uint8_t hi = DspGet(d);
  return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static void UpdateIrq(Sb16Device* d) {
  int level = (d->mixer_regs[0x82] & (kSb16Irq8 | kSb16Irq16)) != 0;
  if (d->irq_level == level) return;
  d->irq_level = level;
  if (d->set_irq) d->set_irq(d->irq_ctx, kSb16IrqLine, level);
}

static void SetIrqLow(Sb16Device* d) {
  d->mixer_regs[0x82] &= (uint8_t)~(kSb16Irq8 | kSb16Irq16);
  d->silence_pending = 0;
  UpdateIrq(d);
}

// Close the host stream and forget which format it had, so the next transfer
// opens it fresh. HostAudioClose(NULL) is a no-op, which keeps the reset paths
// free of a separate "was it ever opened" flag.
static void CloseAudio(Sb16Device* d) {
  HostAudioClose(d->audio);
  d->audio = NULL;
  d->audio_tried = 0;
  d->audio_rate = 0;
  d->audio_channels = 0;
  d->audio_bits = 0;
  d->audio_signed = 0;
}

static void OpenAudio(Sb16Device* d) {
  // Do not restrict again here: dma_cmd8 already restricted the rate before
  // halving it for stereo, and a second clamp would open the host stream at a
  // different byte rate than the DMA clock (2500 Hz stereo is a legal 5000
  // byte/s stream).
  int rate = d->freq > 0 ? d->freq : kSb16DefaultRate;
  int channels = d->fmt_stereo ? 2 : 1;
  int bits = d->fmt_bits == 16 ? 16 : 8;
  int is_signed = d->fmt_signed ? 1 : 0;
  if (d->audio_tried && d->audio_rate == rate && d->audio_channels == channels &&
      d->audio_bits == bits && d->audio_signed == is_signed)
    return;
  CloseAudio(d);
  d->audio = HostAudioOpen(rate, channels, bits, is_signed);
  d->audio_rate = rate;
  d->audio_channels = channels;
  d->audio_bits = bits;
  d->audio_signed = is_signed;
  d->audio_tried = 1;
  if (!d->audio && HostAudioEnabled() && !d->audio_warned) {
    LogInfo("sb16: no host audio output; the guest runs silent");
    d->audio_warned = 1;
  }
}

static void Control(Sb16Device* d, int hold) {
  d->dma_running = hold;
  if (!hold) {
    d->clock_on = 0;
    return;
  }
  OpenAudio(d);
  d->clock_on = 1;
  d->clock_start_ns = HostTimerNowNs();
  d->clock_bytes = 0;
}

static void ContinueDma8(Sb16Device* d) {
  OpenAudio(d);
  Control(d, 1);
}

// The mixer's power-on defaults: QEMU sb16.c reset_mixer, which leaves the
// identity/status bytes at 0x80..0x82 alone (the caller owns those).
static void ResetMixer(Sb16Device* d) {
  memset(d->mixer_regs, 0xff, 0x7f);  // 0x00..0x7e
  memset(d->mixer_regs + 0x83, 0xff, sizeof(d->mixer_regs) - 0x83);
  d->mixer_regs[0x02] = 4;  // master volume, 3-bit
  d->mixer_regs[0x06] = 4;  // MIDI volume
  d->mixer_regs[0x08] = 0;  // CD volume
  d->mixer_regs[0x0a] = 0;  // voice volume
  d->mixer_regs[0x0c] = 0;  // input filter/source
  d->mixer_regs[0x0e] = 0;  // output filter, mono
  d->mixer_regs[0x04] = (4 << 5) | (4 << 1);  // voice L/R
  d->mixer_regs[0x22] = (4 << 5) | (4 << 1);  // master L/R
  d->mixer_regs[0x26] = (4 << 5) | (4 << 1);  // MIDI L/R
  for (int i = 0x30; i < 0x48; i++) d->mixer_regs[i] = 0x20;
}

// The DSP's reset state, shared by the reset command and the machine reset.
// It stops playback and puts 0xAA in the read buffer, which is the byte a
// driver polls for after toggling the reset port (QEMU sb16.c reset()).
static void DspResetState(Sb16Device* d) {
  d->dma_auto = 0;
  d->in_index = 0;
  d->out_data_len = 0;
  d->left_till_irq = 0;
  d->needed_bytes = 0;
  d->block_size = -1;
  d->v2x6 = 0;
  d->cmd = -1;
  d->silence_pending = 0;
  d->mixer_regs[0x82] = 0;
  DspOut(d, 0xaa);
  d->speaker = 0;
  Control(d, 0);
  CloseAudio(d);
  d->freq = kSb16DefaultRate;
  d->time_const = -1;
  d->fmt_signed = 0;
  d->fmt_bits = 8;
  d->fmt_stereo = 0;
  d->align = 0;
  d->bytes_per_second = kSb16DefaultRate;
  UpdateIrq(d);
}

// The DSP reset command (port 0x226's 1-then-0 sequence). A real card pulses
// the IRQ line when an auto-initialize transfer was running; QEMU reproduces
// that as raise-then-lower, so the PIC sees one edge.
static void DspReset(Sb16Device* d) {
  SetIrqLow(d);
  if (d->dma_auto) {
    d->mixer_regs[0x82] |= kSb16Irq8;
    UpdateIrq(d);
    d->mixer_regs[0x82] &= (uint8_t)~kSb16Irq8;
    UpdateIrq(d);
  }
  DspResetState(d);
}

static void Speaker(Sb16Device* d, int on) { d->speaker = on ? 1 : 0; }

static void DmaCmd8(Sb16Device* d, int mask, int dma_len) {
  d->fmt_bits = 8;
  d->fmt_signed = 0;
  d->use_hdma = 0;
  d->fmt_stereo = (d->mixer_regs[0x0e] & 2) != 0;
  if (d->time_const == -1) {
    if (d->freq <= 0) d->freq = kSb16DefaultRate;
  } else {
    int tmp = 256 - d->time_const;
    d->freq = (1000000 + (tmp / 2)) / tmp;
    d->time_const = -1;
  }
  d->freq = RestrictRate(d->freq);
  if (dma_len != -1) {
    d->block_size = dma_len << d->fmt_stereo;
  } else {
    // Command 0x1C's length came from 0x48; a stereo transfer must not split
    // a frame (QEMU sb16.c dma_cmd8's `block_size &= ~fmt_stereo`).
    d->block_size &= ~d->fmt_stereo;
  }
  if (d->block_size <= 0) {
    // 0x1C without a preceding 0x48 has no block to play. QEMU would arm a
    // zero-length block; keeping the command inert is the same guest-visible
    // outcome without a divide-by-zero in the clock.
    d->block_size = -1;
    return;
  }
  d->freq >>= d->fmt_stereo;
  d->left_till_irq = d->block_size;
  d->bytes_per_second = d->freq << d->fmt_stereo;
  d->dma_auto = (mask & kSb16Dma8Auto) != 0;
  d->align = (1 << d->fmt_stereo) - 1;
  ContinueDma8(d);
  Speaker(d, 1);
}

static void DmaCmd(Sb16Device* d, uint8_t cmd, uint8_t d0, int dma_len) {
  d->use_hdma = cmd < 0xc0;
  d->dma_auto = (cmd >> 2) & 1;
  d->fmt_signed = (d0 >> 4) & 1;
  d->fmt_stereo = (d0 >> 5) & 1;
  switch (cmd >> 4) {
    case 11:
      d->fmt_bits = 16;
      break;
    case 12:
      d->fmt_bits = 8;
      break;
    default:
      return;  // not a DMA command encoding; leave the previous format
  }
  if (d->time_const != -1) {
    int tmp = 256 - d->time_const;
    d->freq = (1000000 + (tmp / 2)) / tmp;
    d->time_const = -1;
  }
  d->block_size = dma_len + 1;
  d->block_size <<= (d->fmt_bits == 16);
  if (!d->dma_auto) d->block_size <<= d->fmt_stereo;
  d->left_till_irq = d->block_size;
  d->bytes_per_second = (d->freq << d->fmt_stereo) << (d->fmt_bits == 16);
  d->align = (1 << (d->fmt_stereo + (d->fmt_bits == 16))) - 1;
  if (d->freq > 0) OpenAudio(d);
  Control(d, 1);
  Speaker(d, 1);
}

static void Complete(Sb16Device* d) {
  if (d->cmd > 0xaf && d->cmd < 0xd0) {
    int d2 = DspGet(d);
    int d1 = DspGet(d);
    int d0 = DspGet(d);
    // Bit 3 of these commands selects the ADC (recording) direction. The
    // playback path is the one this model implements; recording is out of scope
    // (user ruling), and QEMU's command() leaves that direction inert too.
    if (!(d->cmd & 8)) DmaCmd(d, (uint8_t)d->cmd, (uint8_t)d0, d1 + (d2 << 8));
    d->cmd = -1;
    return;
  }
  switch (d->cmd) {
    case 0x04:
      d->csp_mode = DspGet(d);
      d->csp_reg83r = 0;
      d->csp_reg83w = 0;
      break;
    case 0x05:
      d->csp_param = DspGet(d);
      d->csp_value = DspGet(d);
      break;
    case 0x0e: {
      int d0 = DspGet(d);
      int d1 = DspGet(d);
      if (d1 == 0x83) {
        d->csp_reg83[d->csp_reg83r % 4] = (uint8_t)d0;
        d->csp_reg83r++;
      } else {
        d->csp_regs[d1] = (uint8_t)d0;
      }
      break;
    }
    case 0x0f: {
      int d0 = DspGet(d);
      if (d0 == 0x83) {
        DspOut(d, d->csp_reg83[d->csp_reg83w % 4]);
        d->csp_reg83w++;
      } else {
        DspOut(d, d->csp_regs[d0]);
      }
      break;
    }
    case 0x10:
      DspGet(d);  // direct ADC: recording is out of scope (user ruling)
      break;
    case 0x14:
      DmaCmd8(d, 0, DspGetLohi(d) + 1);
      break;
    case 0x40:
      d->time_const = DspGet(d);
      break;
    case 0x41:
    case 0x42:
      // QEMU treats both as one output rate: the SB16's real hardware has a
      // single rate under the hood, and its dsp_get_hilo is the byte order the
      // calibrated drivers use.
      d->freq = RestrictRate(DspGetHilo(d));
      break;
    case 0x48:
      d->block_size = DspGetLohi(d) + 1;
      break;
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x7d:
    case 0x7f:
      DspGet(d);  // ADPCM parameter bytes: the compression is D43
      DspGet(d);
      break;
    case 0x80: {
      int freq = d->freq > 0 ? d->freq : kSb16DefaultRate;
      int samples = DspGetLohi(d) + 1;
      int bytes = samples << d->fmt_stereo << (d->fmt_bits == 16);
      int64_t ticks = (int64_t)bytes * kSb16NsPerSecond / freq;
      if (ticks < kSb16SilenceMinNs) {
        d->mixer_regs[0x82] |= d->use_hdma ? kSb16Irq16 : kSb16Irq8;
        UpdateIrq(d);
      } else {
        d->silence_pending = 1;
        d->silence_at_ns = HostTimerNowNs() + ticks;
      }
      break;
    }
    case 0xe0: {
      int d0 = DspGet(d);
      DspOut(d, (uint8_t)~d0);
      break;
    }
    case 0xe2:
      DspGet(d);  // DSP identification data; the answer is not modelled (D43)
      break;
    case 0xe4:
      d->test_reg = DspGet(d);
      break;
    case 0xf9: {
      int d0 = DspGet(d);
      switch (d0) {
        case 0x0e:
          DspOut(d, 0xff);
          break;
        case 0x0f:
          DspOut(d, 0x07);
          break;
        case 0x37:
          DspOut(d, 0x38);
          break;
        default:
          DspOut(d, 0x00);
          break;
      }
      break;
    }
    default:
      break;
  }
  d->cmd = -1;
}

static void Command(Sb16Device* d, uint8_t cmd) {
  if (cmd > 0xaf && cmd < 0xd0) {
    // 0xB0-0xCF: the three-byte 16-bit/8-bit DMA formats (format, length low,
    // length high). Bit 3 selects the ADC (recording) direction; QEMU still
    // consumes its three parameter bytes before leaving the transfer inert,
    // and a guest that records would otherwise desynchronize the command
    // stream. Recording itself is out of scope (user ruling).
    switch (cmd >> 4) {
      case 11:
      case 12:
        d->needed_bytes = 3;
        d->cmd = cmd;
        return;
      default:
        d->cmd = -1;
        return;
    }
  }

  d->needed_bytes = 0;
  switch (cmd) {
    case 0x03:
      DspOut(d, 0x10);
      break;
    case 0x04:
      d->needed_bytes = 1;
      break;
    case 0x05:
      d->needed_bytes = 2;
      break;
    case 0x08:
      break;  // undocumented; QEMU ignores it
    case 0x09:
      DspOut(d, 0xf8);
      break;
    case 0x0e:
      d->needed_bytes = 2;
      break;
    case 0x0f:
      d->needed_bytes = 1;
      break;
    case 0x10:
      d->needed_bytes = 1;
      break;
    case 0x14:
      d->needed_bytes = 2;
      d->block_size = 0;
      break;
    case 0x1c:
      DmaCmd8(d, kSb16Dma8Auto, -1);
      break;
    case 0x20:
      DspOut(d, 0xff);
      break;
    case 0x35:
      break;  // MIDI: D43
    case 0x40:
      d->freq = -1;
      d->time_const = -1;
      d->needed_bytes = 1;
      break;
    case 0x41:
    case 0x42:
      d->freq = -1;
      d->time_const = -1;
      d->needed_bytes = 2;
      break;
    case 0x45:
      DspOut(d, 0xaa);
      break;
    case 0x47:
      break;  // continue auto-initialize 16-bit: nothing to restart
    case 0x48:
      d->needed_bytes = 2;
      break;
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x7d:
    case 0x7f:
      d->needed_bytes = 2;  // ADPCM: the parameters are consumed, data is D43
      break;
    case 0x80:
      d->needed_bytes = 2;
      break;
    case 0x90:
    case 0x91:
      // QEMU's DMA8_HIGH bit only fed a commented-out field; 0x90 is the
      // auto-initialize form and 0x91 the single-cycle one.
      DmaCmd8(d, ((cmd & 1) == 0) ? kSb16Dma8Auto : 0, -1);
      break;
    case 0xd0:
    case 0xd5:
      Control(d, 0);
      break;
    case 0xd1:
      Speaker(d, 1);
      break;
    case 0xd3:
      Speaker(d, 0);
      break;
    case 0xd4:
      ContinueDma8(d);
      break;
    case 0xd6:
      Control(d, 1);
      break;
    case 0xd9:
    case 0xda:
      d->dma_auto = 0;
      break;
    case 0xe0:
      d->needed_bytes = 1;
      break;
    case 0xe1:
      DspOut(d, (uint8_t)(kSb16Version & 0xff));
      DspOut(d, (uint8_t)(kSb16Version >> 8));
      break;
    case 0xe2:
      d->needed_bytes = 1;
      break;
    case 0xe3:
      // Include the string's terminating NUL: the driver reads the fixed
      // copyright text and then expects the zero byte (QEMU sb16.c pushes
      // e3[sizeof(e3)-1] first, i.e. the NUL, and the LIFO read buffer puts
      // it after the text). Without it the driver polls 0x22E forever.
      for (int i = (int)sizeof(kSb16Copyright) - 1; i >= 0; i--)
        DspOut(d, (uint8_t)kSb16Copyright[i]);
      break;
    case 0xe4:
      d->needed_bytes = 1;
      break;
    case 0xe7:
      break;  // ESS probe: not this card
    case 0xe8:
      DspOut(d, d->test_reg);
      break;
    case 0xf2:
    case 0xf3:
      DspOut(d, 0xaa);
      d->mixer_regs[0x82] |= (cmd == 0xf2) ? kSb16Irq8 : kSb16Irq16;
      UpdateIrq(d);
      break;
    case 0xf9:
      d->needed_bytes = 1;
      break;
    case 0xfa:
    case 0xfc:
      DspOut(d, 0x00);
      break;
    default:
      break;
  }
  d->cmd = d->needed_bytes ? cmd : -1;
}

static void MixerWrite(Sb16Device* d, uint8_t val) {
  switch (d->mixer_nreg) {
    case 0x00:
      ResetMixer(d);
      break;
    case 0x80:
      // IRQ select: the machine wires IRQ5, so the stored identity byte is
      // kept for readback but the line does not move (QEMU's s->pic is fixed
      // the same way).
      d->mixer_regs[0x80] = val;
      break;
    case 0x81:
      // DMA select: channels 1 and 5 are soldered to the board.
      d->mixer_regs[0x81] = val;
      break;
    case 0x82:
      break;  // IRQ status is read-only
    default:
      d->mixer_regs[d->mixer_nreg] = val;
      break;
  }
}

static void DspResetPortWrite(Sb16Device* d, uint8_t val) {
  switch (val) {
    case 0x00:
      if (d->v2x6 == 1) DspReset(d);
      d->v2x6 = 0;
      break;
    case 0x01:
    case 0x03:  // FreeBSD's reset sequence also writes 3
      d->v2x6 = 1;
      break;
    case 0xb8:
      DspReset(d);
      break;
    case 0xc6:
      d->v2x6 = 0;
      break;
    case 0x39:
      DspOut(d, 0x38);
      DspReset(d);
      d->v2x6 = 0x39;
      break;
    default:
      d->v2x6 = val;
      break;
  }
}

static void DspWriteData(Sb16Device* d, uint8_t val) {
  if (d->needed_bytes == 0) {
    Command(d, val);
    return;
  }
  if (d->in_index < (int)sizeof(d->in_data)) d->in_data[d->in_index++] = val;
  if (d->in_index == d->needed_bytes) {
    d->needed_bytes = 0;
    Complete(d);
  }
}

static uint8_t ReadPort(Sb16Device* d, int off) {
  switch (off) {
    case 0x04:
      return 0xff;  // mixer index is write-only
    case 0x05:
      return d->mixer_regs[d->mixer_nreg];
    case 0x06:
      return 0xff;
    case 0x0a:
      if (d->out_data_len) {
        uint8_t v = d->out_data[--d->out_data_len];
        d->last_read_byte = v;
        return v;
      }
      return d->last_read_byte;
    case 0x0c:
      return 0x00;  // status: a command byte may be written
    case 0x0d:
      return 0x00;  // timer interrupt acknowledge (no timer is modelled)
    case 0x0e: {
      uint8_t v = d->out_data_len ? 0x80 : 0x00;
      if (d->mixer_regs[0x82] & kSb16Irq8) {
        d->mixer_regs[0x82] &= (uint8_t)~kSb16Irq8;
        UpdateIrq(d);
      }
      return v;
    }
    case 0x0f:
      if (d->mixer_regs[0x82] & kSb16Irq16) {
        d->mixer_regs[0x82] &= (uint8_t)~kSb16Irq16;
        UpdateIrq(d);
      }
      return 0xff;
    default:
      return 0xff;
  }
}

static void WritePort(Sb16Device* d, int off, uint8_t val) {
  switch (off) {
    case 0x04:
      d->mixer_nreg = val;
      break;
    case 0x05:
      MixerWrite(d, val);
      break;
    case 0x06:
      DspResetPortWrite(d, val);
      break;
    case 0x0c:
      DspWriteData(d, val);
      break;
    default:
      break;
  }
}

void Sb16Poll(Sb16Device* d) {
  int64_t now = HostTimerNowNs();
  if (d->silence_pending && now >= d->silence_at_ns) {
    d->silence_pending = 0;
    d->mixer_regs[0x82] |= kSb16Irq8;
    UpdateIrq(d);
  }
  if (!d->dma_running || !d->clock_on || d->block_size <= 0 || d->bytes_per_second <= 0) return;
  int64_t elapsed = now - d->clock_start_ns;
  if (elapsed <= 0) return;
  uint64_t due = (uint64_t)elapsed * (uint64_t)d->bytes_per_second / kSb16NsPerSecond;
  if (due <= d->clock_bytes) return;
  uint64_t want = due - d->clock_bytes;
  if (want > kSb16ChunkBytes) want = kSb16ChunkBytes;
  // A single-cycle transfer ends exactly at the DSP block size; reading past
  // it would consume bytes the guest intended for the next command (QEMU's
  // SB_read_DMA clamps `copy` to `left_till_irq` when dma_auto is false).
  if (!d->dma_auto && want > (uint64_t)d->left_till_irq) want = (uint64_t)d->left_till_irq;
  int frame = d->align + 1;
  want -= want % (uint64_t)frame;
  if (want == 0) return;
  // A muted speaker still consumes DMA at the sample rate; only an audible
  // block is limited by the host queue. Gating the muted path on host room
  // would stall a guest that turned the speaker off and then started a block.
  if (d->audio && d->speaker) {
    int room = HostAudioFree(d->audio);
    if (room <= 0) return;
    uint64_t frames = (uint64_t)room;
    frames -= frames % (uint64_t)frame;
    if (frames < want) want = frames;
    if (want == 0) return;
  }
  uint8_t buf[kSb16ChunkBytes];
  int terminal = 0;
  int moved = d->dma ? d->dma(d->dma_ctx, d->use_hdma ? kSb16Dma16Channel : kSb16Dma8Channel,
                              buf, (int)want, &terminal)
                     : 0;
  if (moved <= 0) return;
  d->clock_bytes += (uint64_t)moved;
  if (d->audio && d->speaker) HostAudioWrite(d->audio, buf, moved);
  d->left_till_irq -= moved;
  if (d->left_till_irq <= 0) {
    d->mixer_regs[0x82] |= d->use_hdma ? kSb16Irq16 : kSb16Irq8;
    UpdateIrq(d);
    if (!d->dma_auto) {
      Control(d, 0);
      d->speaker = 0;
    } else {
      while (d->left_till_irq <= 0) d->left_till_irq += d->block_size;
    }
  } else if (terminal && !d->dma_auto) {
    // The controller reached terminal count before the DSP's block did: the
    // guest programmed a shorter DMA count than the SB16 length. Report the
    // block end instead of leaving the handler waiting for an IRQ that can
    // never arrive.
    d->left_till_irq = 0;
    d->mixer_regs[0x82] |= d->use_hdma ? kSb16Irq16 : kSb16Irq8;
    UpdateIrq(d);
    Control(d, 0);
    d->speaker = 0;
  }
}

void Sb16Init(Sb16Device* d) {
  memset(d, 0, sizeof(*d));
  d->cmd = -1;
  d->time_const = -1;
  d->block_size = -1;
  Sb16Reset(d);
}

void Sb16Reset(Sb16Device* d) {
  SetIrqLow(d);
  CloseAudio(d);
  ResetMixer(d);
  d->mixer_regs[0x80] = 2;  // QEMU's magic_of_irq(5)
  d->mixer_regs[0x81] = (uint8_t)((1 << kSb16Dma8Channel) | (1 << kSb16Dma16Channel));
  d->mixer_regs[0x82] = 0;
  d->mixer_nreg = 0;
  memset(d->csp_regs, 0, sizeof(d->csp_regs));
  d->csp_regs[5] = 1;
  d->csp_regs[9] = 0xf8;
  memset(d->csp_reg83, 0, sizeof(d->csp_reg83));
  d->csp_reg83r = 0;
  d->csp_reg83w = 0;
  d->csp_param = 0;
  d->csp_value = 0;
  d->csp_mode = 0;
  d->test_reg = 0;
  d->last_read_byte = 0;
  d->v2x6 = 0;
  DspResetState(d);
}

void Sb16Destroy(Sb16Device* d) {
  if (!d) return;
  SetIrqLow(d);
  CloseAudio(d);
}

void Sb16SetIrqSink(Sb16Device* d, void (*set_irq)(void* ctx, int line, int level), void* ctx) {
  d->set_irq = set_irq;
  d->irq_ctx = ctx;
}

void Sb16SetDmaSink(Sb16Device* d,
                    int (*dma)(void* ctx, int channel, uint8_t* buf, int len, int* terminal),
                    void* ctx) {
  d->dma = dma;
  d->dma_ctx = ctx;
}

static uint64_t Sb16Read(void* dev, uint64_t addr, int size) {
  (void)size;
  return ReadPort((Sb16Device*)dev, (int)(addr - kSb16Base));
}

static void Sb16Write(void* dev, uint64_t addr, int size, uint64_t val) {
  (void)size;
  WritePort((Sb16Device*)dev, (int)(addr - kSb16Base), (uint8_t)val);
}

const DeviceOps kSb16Ops = {"sb16", Sb16Read, Sb16Write};

void Sb16Register(Bus* io, Sb16Device* d) {
  BusAddRegion(io, kSb16Base, kSb16PortCount, &kSb16Ops, d);
}
