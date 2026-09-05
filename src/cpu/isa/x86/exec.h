#ifndef CEMU_CPU_ISA_X86_EXEC_H
#define CEMU_CPU_ISA_X86_EXEC_H

// Private to the x86 interpreter. exec.c (the instruction set: one case per
// opcode, in opcode order) and step.c (the step protocol: INTR sampling,
// prefixes, one commit) are one conceptual translation unit split for
// readability; this is the little they share.
//
// Nothing outside cpu/isa/x86/ includes this.
#include "cpu/isa/x86/x86.h"
#include "cpu/step.h"

// The per-step view. x86_step installs these before any helper runs; the
// register, flag and eip spellings below read them.
extern CpuState* cpu;
extern x86_state* s;
extern frame* fr;
extern eflags* fl;

#define eip cpu->pc  // the offset within CS; fetch reads base[cs] + eip

// Register names (SDM 3.4.1): the 16- and 8-bit names are the low parts of
// the 32-bit cells; AH..BH are bits 15:8 of the first four.
#define eax (s->r[eax_i].e)
#define ecx (s->r[ecx_i].e)
#define edx (s->r[edx_i].e)
#define ebx (s->r[ebx_i].e)
#define esp (s->r[esp_i].e)
#define ebp (s->r[ebp_i].e)
#define esi (s->r[esi_i].e)
#define edi (s->r[edi_i].e)
#define ax (s->r[eax_i].x)
#define cx (s->r[ecx_i].x)
#define dx (s->r[edx_i].x)
#define bx (s->r[ebx_i].x)
#define sp (s->r[esp_i].x)
#define bp (s->r[ebp_i].x)
#define si (s->r[esi_i].x)
#define di (s->r[edi_i].x)
#define al (s->r[eax_i].l)
#define cl (s->r[ecx_i].l)
#define dl (s->r[edx_i].l)
#define bl (s->r[ebx_i].l)
#define ah (s->r[eax_i].h)
#define ch (s->r[ecx_i].h)
#define dh (s->r[edx_i].h)
#define bh (s->r[ebx_i].h)

// ---- the decoder context ---------------------------------------------------

// Prefixes (SDM table 2-3) and one instruction's modrm/SIB decode, in the
// manual's vocabulary. `w32`/`a32` are the operand/address size the prefixes
// selected; `nxt` is the ip after the fully fetched instruction.
typedef struct dec {
  int w32, a32;  // operand / address size is 32-bit (else 16)
  int code16;    // CS is a 16-bit segment: EIP wraps at 64K
  int seg;       // segment override, -1 = default
  int rep;       // 0 none, 1 = F3 (repe), 2 = F2 (repne)
  int op2;       // the second byte for 0f opcodes, else 0
  uint32_t nxt;  // eip after the whole instruction, both arms know it

  // modrm decode (tables 2-1/2-3); valid after modrm().
  int mod, reg, rm;  // the three fields
  int is_mem;
  int mseg;       // the effective segment (override or default)
  uint64_t mlin;  // linear address of the memory operand
  uint32_t moff;  // the segment-relative offset (LEA's answer)
} dec;

// One decode context for the instruction being executed; `d` is the short
// spelling used throughout the interpreter.
extern dec g;
#define d g

// The opcode switches (exec.c) and the exception/interrupt dispatch they
// share with the step protocol. `soft` marks software-originated INTs (INT n,
// INT3, INTO) — only those check the gate DPL (SDM vol.2 INT Operation);
// `ec` is the raised exception's error code, pushed when the vector carries
// one.
void run_op(uint8_t op);
void do_int(int vec, uint32_t ret_eip, int soft, uint32_t ec);
// The instruction stream: the step consumes prefixes through this before the
// opcode dispatch takes over. It also records raw bytes for the trace.
uint8_t fetch8(void);

#endif
