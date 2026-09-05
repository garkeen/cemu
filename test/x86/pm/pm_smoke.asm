; Protected-mode smoke probe for the x86_min machine (smoke-probe nature per
; guideline 6: acceptance is kvm-unit-tests, this validates stage-3 items 1-2
; under QEMU arbitration). A multiboot ELF enters flat protected mode (CS=0x08
; data=0x10, PE=1 — the QEMU -kernel contract), rebuilds GDT/IDT/TSS, and
; walks the stage-3 segment and gate semantics:
;   t1  segment loads from the new GDT + data write/readback
;   t2  data-segment limit violation -> #GP(0) delivered through gate 13
;   t3  same-privilege INT 0x20 through a DPL0 interrupt gate: no stack
;       switch, IF cleared by the gate, pushed EFLAGS bit 1 set
;   t4  IRET trampoline into ring 3 (outer return loads SS:ESP)
;   t5  ring-3 INT 0x21 through a DPL3 gate -> TSS SS0:ESP0 stack switch ->
;       IRET back out (frame old SS/ESP checked in the handler)
;   t6  ring-3 INT 0x22 against a DPL0 gate -> #GP(ec = vec*8|2) through gate 13
;   t7  ring-3 write beyond a small data segment -> #GP(0)
;   t8  ring-3 INT 0x23 whose handler rewrites the gate frame to resume in
;       ring 0 (same-privilege return at the rewritten CS RPL)
; Reports one "tN ok" line per test plus "pm-smoke done", then exits through
; the QEMU isa-debug-exit port 0xF4 with payload 5 (status = (5<<1)|1 = 11).
;
; Calibration: serial output byte-identical to
;   qemu-system-i386 -kernel pm_smoke.elf -display none -no-reboot \
;       -serial file:pm_qemu.txt -device isa-debug-exit,iobase=0xf4,iosize=0x4
;
; Build: nasm -f elf32 -o pm_smoke.o pm_smoke.asm
;        ld.lld -m elf_i386 -T pm_smoke.ld -o pm_smoke.elf pm_smoke.o
; Run:   cemu --machine x86 --isa x86 pm_smoke.elf
  bits 32

%define SEL_CODE0   0x08
%define SEL_DATA0   0x10
%define SEL_DSMALL  0x18
%define SEL_CODE3   0x20
%define SEL_CODE3R3 (SEL_CODE3 | 3)
%define SEL_DATA3   0x28
%define SEL_DATA3R3 (SEL_DATA3 | 3)
%define SEL_DATA3S  0x30
%define SEL_DATA3SR3 (SEL_DATA3S | 3)
%define SEL_TSS     0x38

%define IDT_LIN    0x2100      ; gates built at runtime, 0x24 slots
%define TSS_LIN    0x2400      ; TSS image: ESP0 at +4, SS0 at +8
%define RES_LIN    0x2500      ; result bytes r1..r8 (1 = ok, RAM starts 0)
%define OBS_LIN    0x2510      ; handler observation slots (dwords)
%define RING0_TOP  0x4000
%define TSS_ESP0   0x4800
%define RING3_TOP  0x5000
%define SCRATCH    0x5100

section .multiboot
align 4
  dd 0x1BADB002                 ; magic
  dd 0x3                        ; PAGE_ALIGN | MEMORY_INFO
  dd -(0x1BADB002 + 0x3)        ; checksum

section .text
global _start

_start:
  cli
  ; mask every PIC line: no stray IRQ0 during the IF-sensitive checks
  mov al, 0xff
  out 0x21, al
  out 0xa1, al

  ; COM1: divisor 1 (115200 baud), 8N1, interrupts off (smoke's init)
  mov dx, 0x3fb
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8
  mov al, 0x01
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al
  mov dx, 0x3fb
  mov al, 0x03
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al

  ; the TSS image: ring-0 stack for gate delivery from ring 3
  mov dword [TSS_LIN + 4], TSS_ESP0
  mov word [TSS_LIN + 8], SEL_DATA0

  lgdt [gdtdesc]

  mov ax, SEL_DATA0
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  mov esp, RING0_TOP

  jmp SEL_CODE0:.flush          ; re-parse CS through the new GDT
.flush:
  ; the IDT is built before anything can fault — handler labels are plain
  ; immediates here, and gate 13 must exist to make any #GP observable
  mov edi, IDT_LIN + 13 * 8
  mov eax, gp_handler
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x20 * 8
  mov eax, int20_handler
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x21 * 8
  mov eax, int21_handler
  mov bl, 0xee
  call mk_gate
  mov edi, IDT_LIN + 0x22 * 8
  mov eax, gp_handler
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x23 * 8
  mov eax, int23_handler
  mov bl, 0xee
  call mk_gate
  lidt [idtdesc]
  mov ax, SEL_TSS
  ltr ax

  ; ---- t1: write/readback through the rebuilt segment set -------------------
  mov dword [SCRATCH], 0xC0DE0001
  mov eax, [SCRATCH]
  cmp eax, 0xC0DE0001
  jne .t1_done
  mov byte [RES_LIN + 0], 1
.t1_done:

  ; ---- t2: write beyond a 0x1ff-limit data segment -> #GP(0) ----------------
  mov ax, SEL_DSMALL
  mov ds, ax
  mov ecx, 0x100
  mov [ecx], ecx                ; in bounds: 0x100+3 <= 0x1ff
  mov ecx, 0x200
  mov [ecx], ecx                ; -> #GP(0); gp_handler skips the 2-byte MOV
  mov ax, SEL_DATA0
  mov ds, ax
  cmp dword [OBS_LIN + 0], 0    ; the recorded error code
  jne .t2_done
  mov byte [RES_LIN + 1], 1
.t2_done:

  ; ---- t3: same-privilege INT through the DPL0 gate 0x20 --------------------
  mov dword [OBS_LIN + 0x14], esp
  int 0x20
  mov eax, [OBS_LIN + 0x14]
  sub eax, 12                   ; flags + cs + eip pushed, no stack switch
  cmp eax, [OBS_LIN + 4]
  jne .t3_done
  cmp dword [OBS_LIN + 8], 0    ; handler CS RPL == CPL == 0
  jne .t3_done
  mov eax, [OBS_LIN + 0xc]      ; pushed EFLAGS: bit 1 set, IF clear
  and eax, 0x202
  cmp eax, 2
  jne .t3_done
  cmp dword [OBS_LIN + 0x10], 0 ; the interrupt gate cleared IF
  jne .t3_done
  mov byte [RES_LIN + 2], 1
.t3_done:

  ; ---- t4: IRET trampoline into ring 3 ---------------------------------------
  push dword SEL_DATA3R3        ; SS (RPL 3)
  push dword RING3_TOP          ; ESP
  push dword 0x2                ; EFLAGS: IF clear, bit 1 set
  push dword SEL_CODE3R3        ; CS (RPL 3)
  push dword ring3_entry
  iretd

ring3_entry:
  mov eax, cs
  and eax, 3
  mov dword [OBS_LIN + 0x28], eax          ; == 3 (DS is still flat SEL_DATA0)
  mov eax, ss
  and eax, 3
  mov dword [OBS_LIN + 0x2c], eax          ; == 3
  mov ax, SEL_DATA3R3
  mov ds, ax
  mov dword [SCRATCH + 4], 0xC0DE0002
  mov eax, [SCRATCH + 4]
  cmp eax, 0xC0DE0002
  jne .t4_done
  mov byte [RES_LIN + 3], 1                ; DS is the flat ring-3 data segment
.t4_done:

  ; ---- t5: ring-3 INT through the DPL3 gate 0x21 -----------------------------
  mov dword [OBS_LIN + 0x30], esp
  int 0x21
  cmp dword [OBS_LIN + 0x18], 0            ; handler CPL 0
  jne .t5_done
  cmp dword [OBS_LIN + 0x1c], 3            ; pushed old SS RPL 3
  jne .t5_done
  mov eax, [OBS_LIN + 0x20]
  cmp eax, [OBS_LIN + 0x30]                ; pushed old ESP == pre-INT ESP
  jne .t5_done
  cmp dword [OBS_LIN + 0x24], SEL_DATA0    ; handler SS == TSS SS0
  jne .t5_done
  mov byte [RES_LIN + 4], 1
.t5_done:

  ; ---- t6: ring-3 INT against the DPL0 gate 0x22 -> #GP(vec*8|2) -------------
  int 0x22                                 ; gp_handler skips the 2-byte INT
  cmp dword [OBS_LIN + 0], 0x22 * 8 | 2
  jne .t6_done
  mov byte [RES_LIN + 5], 1
.t6_done:

  ; ---- t7: ring-3 write beyond the 0x1ff-limit ring-3 segment -> #GP(0) ------
  mov ax, SEL_DATA3SR3
  mov ds, ax
  mov ecx, 0x200
  mov [ecx], ecx                           ; gp_handler skips the 2-byte MOV
  mov ax, SEL_DATA3R3
  mov ds, ax
  cmp dword [OBS_LIN + 0], 0
  jne .t7_done
  mov byte [RES_LIN + 6], 1
.t7_done:

  ; ---- t8: the exit vector — its handler rewrites the gate frame to resume
  ; at ring 0, so this INT never returns here ----------------------------------
  int 0x23
.hang3:
  hlt
  jmp .hang3

; ---- ring-0 helpers ------------------------------------------------------------

putc:                          ; AL = character, waits for COM1 THRE
  push edx
  push eax
  mov dx, 0x3fd
.wait:
  in al, dx
  test al, 0x20
  jz .wait
  pop eax
  mov dx, 0x3f8
  out dx, al
  pop edx
  ret

print:                         ; ESI = NUL-terminated string
  push eax
  push esi
.next:
  lodsb
  test al, al
  jz .done
  call putc
  jmp .next
.done:
  pop esi
  pop eax
  ret

mk_gate:                       ; EAX = handler, BL = attr, EDI = gate slot
  mov [edi], ax                ; offset 15:0
  mov word [edi + 2], SEL_CODE0
  mov [edi + 5], bl            ; P DPL type (byte 4 stays zero: RAM is clear)
  shr eax, 16
  mov [edi + 6], ax            ; offset 31:16
  ret

; ---- IDT handlers ---------------------------------------------------------------

gp_handler:                    ; #GP from any ring: record the error code,
  push eax                     ; drop it, skip the 2-byte faulting instruction
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  mov eax, [esp + 8]           ; [esp]=ds, [esp+4]=eax, [esp+8]=error code
  mov dword [OBS_LIN + 0], eax
  pop ds
  pop eax
  add esp, 4                   ; the error code is not part of the IRET frame
  add dword [esp], 2
  iretd

int20_handler:                 ; same-privilege delivery: no stack switch
  push eax
  mov eax, esp
  add eax, 4
  mov dword [OBS_LIN + 4], eax ; entry ESP (pre-INT esp minus 12)
  mov eax, [esp + 8]           ; CS
  and eax, 3
  mov dword [OBS_LIN + 8], eax
  mov eax, [esp + 12]          ; pushed EFLAGS
  mov dword [OBS_LIN + 0xc], eax
  pushfd
  pop eax
  and eax, 0x200
  mov dword [OBS_LIN + 0x10], eax  ; the interrupt gate cleared IF
  pop eax
  iretd

int21_handler:                 ; ring-3 delivery: the stack switched
  push eax
  push ds                      ; the ring-3 DS comes back on IRET
  mov ax, SEL_DATA0
  mov ds, ax
  mov ax, cs                   ; the delivered CS register: RPL = the new CPL
  and eax, 3                   ; (the frame holds the OLD cs at +12, not this)
  mov dword [OBS_LIN + 0x18], eax
  mov eax, [esp + 24]          ; +8=eip, +12=old cs, +16=flags, +20=old ESP,
  and eax, 3                   ; +24=old SS
  mov dword [OBS_LIN + 0x1c], eax
  mov eax, [esp + 20]
  mov dword [OBS_LIN + 0x20], eax
  mov ax, ss
  movzx eax, ax
  mov dword [OBS_LIN + 0x24], eax
  pop ds
  pop eax
  iretd

int23_handler:                 ; exit: rewrite the gate frame into a ring-0
  mov dword [esp], resume0     ; resume point and IRET back at the rewritten
  mov word [esp + 4], SEL_CODE0 ; CS RPL (same-privilege return at CPL 0)
  mov dword [esp + 8], 0x2
  iretd

resume0:
  mov eax, cs
  and eax, 3
  mov dword [OBS_LIN + 0x34], eax  ; == 0 (DS is still the ring-3 flat segment)
  mov byte [RES_LIN + 7], 1
  mov esp, RING0_TOP
  mov ax, SEL_DATA0
  mov ds, ax

  ; ---- report ---------------------------------------------------------------------
  xor ebx, ebx
.loop:
  mov al, 't'
  call putc
  mov al, bl
  add al, '1'
  call putc
  mov esi, RES_LIN
  add esi, ebx
  lodsb
  cmp al, 1
  je .ok
  mov esi, msg_bad
  jmp .emit
.ok:
  mov esi, msg_ok
.emit:
  call print
  inc ebx
  cmp ebx, 8
  jl .loop
  mov esi, msg_done
  call print

  mov al, 5                    ; emulator status (5<<1)|1 = 11
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

; ---- descriptor tables -----------------------------------------------------------

gdt:
  dq 0                         ; null
  dq 0x00cf9b000000ffff        ; 08: code, base 0, 4 GiB, DPL0, 32-bit
  dq 0x00cf93000000ffff        ; 10: data, base 0, 4 GiB, DPL0, writable
  dq 0x00409200000001ff        ; 18: data, base 0, limit 0x1ff, DPL0
  dq 0x00cffb000000ffff        ; 20: code, base 0, 4 GiB, DPL3, 32-bit
  dq 0x00cff3000000ffff        ; 28: data, base 0, 4 GiB, DPL3, writable
  dq 0x0040f200000001ff        ; 30: data, base 0, limit 0x1ff, DPL3
  dq 0x0000890024000067        ; 38: 32-bit TSS at TSS_LIN, limit 0x67
                               ; (bytes: 67 00 | 00 24 | 00 | 89 | 00 | 00)
gdt_end:

gdtdesc:
  dw gdt_end - gdt - 1
  dd gdt

idtdesc:
  dw 0x11f                     ; 0x24 gate slots
  dd IDT_LIN

msg_ok:   db " ok", 10, 0
msg_bad:  db " BAD", 10, 0
msg_done: db "pm-smoke done", 10, 0
