; Protected-mode smoke probe for the x86_min machine (smoke-probe nature per
; guideline 6: acceptance is kvm-unit-tests, this validates stage-3 items 1-4
; under QEMU arbitration). A multiboot ELF enters flat protected mode (CS=0x08
; data=0x10, PE=1 — the QEMU -kernel contract), rebuilds GDT/IDT/TSS, and
; walks the stage-3 segment, gate and paging semantics:
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
;   t9  ring-0 CALL through a DPL0 call gate: return EIP/CS on the stack,
;       ESP restored by RETF (runs 4th, right after t3)
;   t10 ring-3 CALL through a DPL3 call gate with 2 stack params: params
;       copied between the return address and the saved SS:ESP, handler on
;       the TSS ring-0 stack, RETF imm drops the params (runs 8th, after t7)
;   t11 CALL through a TSS descriptor: full state save/restore round trip,
;       NT forced in the new task, IRET task-return through the back-link
;   t12 JMP through a TSS descriptor (no NT, state round trip) and back;
;       then a CALL to the busy current TSS -> #GP(TSS selector)
;   t13 INT through an IDT task gate (NT forced), then a real #GP delivered
;       through a task gate: the error code lands on the task's stack and
;       the task resumes main past the faulting instruction
;   t14 identity map 0-1MB through one page table, CR3 then CR0.PG|WP on:
;       fetch and data accesses walk the two-level tables
;   t15 ring-0 write to a linear whose PDE is absent -> #PF(ec=2, W only),
;       CR2 == the faulting linear (runs paged)
;   t16 PTE flipped read-only: ring-0 read succeeds, write -> #PF(ec=3,
;       P|W — CR0.WP makes the supervisor honor R/O), then restored
;   t17 A/D bits: a read sets A in PDE and PTE (D stays 0), a write sets
;       PTE.D; every PTE change is followed by a CR3 reload (the QEMU side
;       caches translations, a real 386 caches them too)
;   t18 ring 3 paged: U/RW user page write succeeds; write to a
;       supervisor-only page -> #PF(ec=7, P|W|U), exit through gate 0x24
;   t19 build an LDT (GDT slot 0x58 + six entries), LLDT it, SLDT round
;       trip, then load DS through a TI=1 selector and use it
;   t20 store beyond the LDT entry's limit -> #GP(0) (segment limits bind
;       LDT segments the way they bind GDT ones); LLDT 0 clears the LDTR
;       (SLDT reads 0) and the next TI=1 load lookup-fails #GP(ec=sel&~3)
;   t21 VERR/VERW matrix: writable data / read-only data / readable code /
;       null selector / out-of-table selector (SDM: only ZF answers)
;   t22 LAR/LSL values and failures: rights byte 0x00cf9b00 for code,
;       effective limits with and without G, DPL3 data from CPL 0, and an
;       out-of-table selector leaves the destination untouched with ZF=0
;   t23 ARPL raises a selector's RPL (ZF=1 on change, ZF=0 when already
;       there) and leaves the index alone
;   t24 a task switch loads the incoming TSS's LDT (+0x60): the task body
;       SLDTs it, reads through an LDT segment and VERRs an LDT code entry
; The tN numbers are report slots, not run order: t9 runs after t3 and t10
; runs after t7 (both need the ring levels already established); t14-t18 run
; at the end, inside the ring-0 resume flow after t13, and t19-t24 (the LDT
; set) run right before them.
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
%define SEL_GATE9   0x40    ; DPL0 call gate -> gate9_entry, 0 params
%define SEL_GATE10  0x48    ; DPL3 call gate -> gate10_entry, 2 params
%define SEL_TSS2    0x50    ; 32-bit TSS #2 at TSS2_LIN
%define SEL_LDT     0x58    ; the LDT descriptor itself (GDT-only, SDM 3.5)
%define SEL_LDT0    0x0c    ; LDT entry 1: data DPL0 writable, limit 0x1ff
%define SEL_LDTBIG  0x14    ; LDT entry 2: data DPL0 writable, 4 GiB
%define SEL_LDTCODE 0x1c    ; LDT entry 3: code DPL0 readable, 4 GiB
%define SEL_LDTRO   0x24    ; LDT entry 4: data DPL0 read-only
%define SEL_LDT3    0x2c    ; LDT entry 5: data DPL3 writable, 4 GiB

%define IDT_LIN    0x2100      ; gates built at runtime, 0x2a slots
%define TSS_LIN    0x2400      ; TSS image: ESP0 at +4, SS0 at +8
%define TSS2_LIN   0x2600      ; second TSS image (filled at runtime)
%define RES_LIN    0x2500      ; result bytes r1..r24 (1 = ok, RAM starts 0)
%define OBS_LIN    0x2700      ; handler observation slots (dwords)
%define LDT_LIN    0x2a00      ; six LDT entries, filled at runtime
%define PD_LIN     0x60000     ; page directory (4 KiB aligned)
%define PT0_LIN    0x61000     ; page table covering 0-4 MiB, identity
%define RING0_TOP  0x4000
%define TSS_ESP0   0x4800
%define RING3_TOP  0x5000
%define SCRATCH    0x5100

%macro RELOAD_CR3 0
  mov eax, PD_LIN
  mov cr3, eax
%endmacro

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
  mov cx, SEL_CODE0
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x20 * 8
  mov eax, int20_handler
  mov cx, SEL_CODE0
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x21 * 8
  mov eax, int21_handler
  mov cx, SEL_CODE0
  mov bl, 0xee
  call mk_gate
  mov edi, IDT_LIN + 0x22 * 8
  mov eax, gp_handler
  mov cx, SEL_CODE0
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x23 * 8
  mov eax, int23_handler
  mov cx, SEL_CODE0
  mov bl, 0xee
  call mk_gate
  mov edi, IDT_LIN + 14 * 8
  mov eax, pf_handler
  mov cx, SEL_CODE0
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x24 * 8
  mov eax, int24_handler
  mov cx, SEL_CODE0
  mov bl, 0xee
  call mk_gate
  lidt [idtdesc]
  mov ax, SEL_TSS
  ltr ax
  ; GDT call gates (same 8-byte layout as IDT gates) and the second TSS
  ; image, both filled at runtime.
  mov edi, gdt + SEL_GATE9
  mov eax, gate9_entry
  mov cx, SEL_CODE0
  mov bl, 0x8c                 ; present, DPL0, 32-bit call gate
  call mk_gate
  mov edi, gdt + SEL_GATE10
  mov eax, gate10_entry
  mov cx, SEL_CODE0
  mov bl, 0xec                 ; present, DPL3, 32-bit call gate
  mov dl, 2                    ; copy two stack parameters
  call mk_gate
  mov dword [TSS2_LIN + 4], TSS_ESP0
  mov word [TSS2_LIN + 8], SEL_DATA0
  ; the task-entry context (eip/eflags/eax are rewritten per test)
  mov dword [TSS2_LIN + 0x38], 0x6000      ; ESP
  mov word [TSS2_LIN + 0x48], SEL_DATA0    ; ES
  mov word [TSS2_LIN + 0x4c], SEL_CODE0    ; CS
  mov word [TSS2_LIN + 0x50], SEL_DATA0    ; SS
  mov word [TSS2_LIN + 0x54], SEL_DATA0    ; DS
  mov word [TSS2_LIN + 0x58], SEL_DATA0    ; FS
  mov word [TSS2_LIN + 0x5c], SEL_DATA0    ; GS
  mov word [TSS2_LIN + 0x64], 0x68         ; I/O map base (minimal TSS)

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

  ; ---- t9: same-privilege CALL through the DPL0 call gate 0x40 -------------
  mov dword [OBS_LIN + 0x38], esp
  call far [ptr_gate9]
t9_ret:
  cmp dword [OBS_LIN + 0x3c], t9_ret      ; the gate pushed the exact return EIP
  jne .t9_done
  cmp dword [OBS_LIN + 0x40], SEL_CODE0   ; and the inner CS
  jne .t9_done
  cmp esp, [OBS_LIN + 0x38]               ; RETF put ESP back
  jne .t9_done
  mov byte [RES_LIN + 8], 1
.t9_done:

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

  ; ---- t10: ring-3 CALL through the DPL3 call gate 0x48 with 2 params ------
  push dword 0x11110002
  push dword 0x11110001
  mov dword [OBS_LIN + 0x44], esp
  call far [ptr_gate10]
t10_ret:
  cmp dword [OBS_LIN + 0x48], 0x11110001  ; param0 copied to the ring-0 stack
  jne .t10_done
  cmp dword [OBS_LIN + 0x4c], 0x11110002  ; param1 after it
  jne .t10_done
  mov eax, [OBS_LIN + 0x50]               ; saved old ESP == ESP at the call
  cmp eax, [OBS_LIN + 0x44]
  jne .t10_done
  cmp dword [OBS_LIN + 0x54], 3           ; saved old SS RPL 3
  jne .t10_done
  cmp dword [OBS_LIN + 0x58], SEL_CODE0   ; handler CS committed at RPL 0
  jne .t10_done
  mov eax, [OBS_LIN + 0x44]               ; RETF 8: ESP loaded from the frame
  add eax, 8                              ; then the imm dropped the params
  cmp esp, eax
  jne .t10_done
  mov byte [RES_LIN + 9], 1
.t10_done:

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

mk_gate:                       ; EAX = offset, BL = attr, DL = param count,
                               ; CX = selector, EDI = gate slot
  mov [edi], ax                ; offset 15:0
  mov [edi + 2], cx            ; selector (code for int/trap/call gates)
  mov [edi + 4], dl            ; call-gate parameter count
  mov [edi + 5], bl            ; P DPL type
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

int24_handler:                 ; t18 exit: same frame rewrite as int23, but
  mov dword [esp], pg_resume   ; back into the paged half of the probe
  mov word [esp + 4], SEL_CODE0
  mov dword [esp + 8], 0x2
  iretd

pf_handler:                    ; #PF from any ring: record the error code and
  push eax                     ; CR2, drop the error code, skip the 2-byte
  push ds                      ; faulting MOV
  mov ax, SEL_DATA0
  mov ds, ax
  mov eax, [esp + 8]           ; [esp]=ds, [esp+4]=eax, [esp+8]=error code
  mov dword [OBS_LIN + 0x7c], eax
  mov eax, cr2
  mov dword [OBS_LIN + 0x80], eax
  pop ds
  pop eax
  add esp, 4                   ; the error code is not part of the IRET frame
  add dword [esp], 2
  iretd

ring3_paging_entry:            ; t18 body: the user-page write must succeed,
  mov ax, SEL_DATA3R3          ; the supervisor-page write must #PF(ec=7)
  mov ds, ax
  mov ecx, 0x5800
  mov [ecx], ecx
  mov eax, [ecx]
  cmp eax, 0x5800
  jne .fault
  mov byte [OBS_LIN + 0x84], 1
.fault:
  mov ecx, 0x9000
  mov [ecx], ecx               ; pf_handler records ec + CR2 and skips 2
  cmp byte [OBS_LIN + 0x84], 1
  jne .done
  cmp dword [OBS_LIN + 0x7c], 7
  jne .done
  cmp dword [OBS_LIN + 0x80], 0x9000
  jne .done
  mov byte [RES_LIN + 17], 1
.done:
  int 0x24                     ; exit vector back to ring 0
.hang4:
  hlt
  jmp .hang4

gate9_entry:                   ; t9: record the return frame, RETF back
  push eax
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  mov eax, [esp + 8]           ; [esp]=ds [esp+4]=saved eax [esp+8]=return EIP
  mov dword [OBS_LIN + 0x3c], eax
  mov eax, [esp + 12]          ; the inner CS
  mov dword [OBS_LIN + 0x40], eax
  pop ds
  pop eax
  retf

gate10_entry:                  ; t10: inward gate entry — params copied, on
  push eax                     ; the TSS ring-0 stack
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  mov eax, [esp + 16]          ; +8=ret EIP +12=CS +16=param0 +20=param1
  mov dword [OBS_LIN + 0x48], eax
  mov eax, [esp + 20]
  mov dword [OBS_LIN + 0x4c], eax
  mov eax, [esp + 24]          ; +24=saved old ESP, +28=saved old SS
  mov dword [OBS_LIN + 0x50], eax
  mov eax, [esp + 28]
  and eax, 3
  mov dword [OBS_LIN + 0x54], eax
  mov ax, cs                   ; the handler's CS register: RPL forced 0
  mov dword [OBS_LIN + 0x58], eax
  pop ds
  pop eax
  retf 8                       ; outward: frame SS:ESP loaded, then ESP += 8

task2_entry:                   ; t11: the TSS2 task body
  push eax
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  mov edx, [esp + 4]           ; the saved EAX (the TSS2 image value)
  mov dword [OBS_LIN + 0x5c], edx
  pushfd
  pop eax
  and eax, 0x4000              ; NT forced by the CALL switch
  mov dword [OBS_LIN + 0x60], eax
  mov eax, cs
  and eax, 3
  mov dword [OBS_LIN + 0x64], eax
  pop ds
  pop eax
  iretd                        ; NT=1: nested-task return to main

task3_entry:                   ; t12: the JMP-switched task body
  push eax
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  pushfd
  pop eax
  and eax, 0x4000
  mov dword [OBS_LIN + 0x6c], eax
  pop ds
  pop eax
  jmp far [ptr_maintss]        ; JMP back: main's TSS is available again

task4_entry:                   ; t13a: arrived through the IDT task gate
  push eax
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  pushfd
  pop eax
  and eax, 0x4000
  mov dword [OBS_LIN + 0x74], eax
  pop ds
  pop eax
  iretd                        ; NT=1: nested-task return

task5_entry:                   ; t24: the switch loaded this TSS's LDT — the
  push eax                     ; body runs with it and reports through OBS
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  sldt eax                     ; zero-extended (cemu and QEMU both)
  mov dword [OBS_LIN + 0x88], eax
  mov ax, SEL_LDT0
  mov ds, ax                   ; the task's own LDT serves TI=1 loads
  mov eax, [0x100]             ; t19's value through the LDT segment
  mov edx, eax                 ; the DS reload clobbers ax — park the value
  mov ax, SEL_DATA0            ; back to flat: OBS lives past the 0x1ff limit
  mov ds, ax
  mov dword [OBS_LIN + 0x8c], edx
  mov ax, SEL_LDTCODE
  verr ax
  pushfd
  pop eax
  and eax, 0x40                ; ZF: the LDT code entry verifies readable
  mov dword [OBS_LIN + 0x90], eax
  pop ds
  pop eax
  iretd                        ; nested-task return to main

task4_ec:                      ; t13b: a real #GP through the task gate: the
  push eax                     ; error code sits on this task's stack; fix
  push ds                      ; main's saved EIP past the faulting MOV and
  mov ax, SEL_DATA0            ; return
  mov ds, ax
  mov eax, [esp + 8]           ; [esp]=ds, [esp+4]=eax, [esp+8]=error code
  mov dword [OBS_LIN + 0x78], eax
  add dword [TSS_LIN + 0x20], 2   ; skip the 2-byte faulting MOV es,ax
  pop ds
  pop eax
  iretd

busy_handler:                  ; t12b: #GP from the busy-TSS CALL — record
  push eax                     ; the error code, skip the 6-byte far call
  push ds
  mov ax, SEL_DATA0
  mov ds, ax
  mov eax, [esp + 8]           ; [esp]=ds, [esp+4]=eax, [esp+8]=error code
  mov dword [OBS_LIN + 0x70], eax
  pop ds
  pop eax
  add esp, 4
  add dword [esp], 6
  iretd

resume0:
  mov eax, cs
  and eax, 3
  mov dword [OBS_LIN + 0x34], eax  ; == 0 (DS is still the ring-3 flat segment)
  mov byte [RES_LIN + 7], 1
  mov esp, RING0_TOP
  mov ax, SEL_DATA0
  mov ds, ax

  ; ---- t11: CALL through the TSS2 descriptor -> task switch round trip -----
  mov dword [TSS2_LIN + 0x20], task2_entry
  mov dword [TSS2_LIN + 0x24], 0x2
  mov dword [TSS2_LIN + 0x28], 0x22220001  ; the task's EAX comes from here
  mov eax, 0xAAAA0001
  mov ebx, 0xAAAA0002
  mov dword [OBS_LIN + 0x68], esp
  call far [ptr_tss2]
t11_ret:
  cmp eax, 0xAAAA0001              ; the task return restored main's GPRs
  jne .t11_done
  cmp ebx, 0xAAAA0002
  jne .t11_done
  cmp esp, [OBS_LIN + 0x68]        ; and its ESP
  jne .t11_done
  cmp dword [OBS_LIN + 0x5c], 0x22220001   ; task2 saw the TSS2 image EAX
  jne .t11_done
  cmp dword [OBS_LIN + 0x60], 0x4000       ; the CALL switch forced NT
  jne .t11_done
  cmp dword [OBS_LIN + 0x64], 0            ; task2 CS RPL 0
  jne .t11_done
  mov byte [RES_LIN + 10], 1
.t11_done:

  ; ---- t12: JMP through the TSS2 (no NT, state round trip) and back, then
  ; a CALL to the busy current TSS -> #GP(TSS selector) through gate 0x26 ----
  mov dword [TSS2_LIN + 0x20], task3_entry
  mov dword [TSS2_LIN + 0x24], 0x2
  jmp far [ptr_tss2]
t12_ret:
  cmp dword [OBS_LIN + 0x6c], 0    ; the JMP switch left NT clear
  jne t12_done
  ; the busy-TSS #GP comes in on vector 13 — repoint it at a handler that
  ; skips the 6-byte far call, then touch the current task's own TSS
  mov edi, IDT_LIN + 13 * 8
  mov eax, busy_handler
  mov cx, SEL_CODE0
  mov bl, 0x8e
  call mk_gate
  call far [ptr_maintss]           ; the current task's TSS is busy
t12b_ret:
  cmp dword [OBS_LIN + 0x70], SEL_TSS
  jne t12_done
  mov byte [RES_LIN + 11], 1
t12_done:

  ; ---- t13: INT through the IDT task gate 0x25, then a real #GP delivered
  ; through a task gate at vector 13 -----------------------------------------
  mov dword [TSS2_LIN + 0x20], task4_entry
  mov dword [TSS2_LIN + 0x24], 0x2
  mov edi, IDT_LIN + 0x25 * 8
  mov eax, 0                       ; task gates carry no offset
  mov cx, SEL_TSS2
  mov bl, 0x85                     ; present, DPL0, task gate
  call mk_gate
  int 0x25
t13a_ret:
  cmp dword [OBS_LIN + 0x74], 0x4000   ; the INT switch forced NT
  jne t13_done
  mov dword [TSS2_LIN + 0x20], task4_ec
  mov edi, IDT_LIN + 13 * 8
  mov eax, 0
  mov cx, SEL_TSS2
  mov bl, 0x85
  call mk_gate
  mov ax, 0x1230                      ; TI=0: a TI=1 selector with no LDT is
  mov es, ax                          ; #TS now (t20); this one is #GP(0x1230)
t13_ret:
  cmp dword [OBS_LIN + 0x78], 0x1230  ; the error code reached the task stack
  jne t13_done
  mov byte [RES_LIN + 12], 1
t13_done:

  ; t12 repointed gate 13 at busy_handler (6-byte skipper); the LDT tests
  ; fault on 2-byte MOVs, so restore the plain #GP handler first.
  mov edi, IDT_LIN + 13 * 8
  mov eax, gp_handler
  mov cx, SEL_CODE0
  mov bl, 0x8e
  call mk_gate

  ; ---- t19: build an LDT, load it, resolve a TI=1 selector ------------------
  ; The LDT descriptor lives in GDT slot 0x58 (system, type 2, present);
  ; six data/code entries follow at LDT_LIN, filled inline.
  mov dword [LDT_LIN + 0x08], 0x000001ff   ; e1: data DPL0 W, limit 0x1ff
  mov dword [LDT_LIN + 0x0c], 0x00009200
  mov dword [LDT_LIN + 0x10], 0x0000ffff   ; e2: data DPL0 W, 4 GiB (G=1)
  mov dword [LDT_LIN + 0x14], 0x00cf9200
  mov dword [LDT_LIN + 0x18], 0x0000ffff   ; e3: code DPL0 readable, 4 GiB
  mov dword [LDT_LIN + 0x1c], 0x00cf9b00
  mov dword [LDT_LIN + 0x20], 0x0000ffff   ; e4: data DPL0 read-only
  mov dword [LDT_LIN + 0x24], 0x00009100
  mov dword [LDT_LIN + 0x28], 0x0000ffff   ; e5: data DPL3 W, 4 GiB
  mov dword [LDT_LIN + 0x2c], 0x00cff200
  mov dword [gdt + 0x58], 0x2a00002f       ; limit 0x2f, base LDT_LIN
  mov dword [gdt + 0x5c], 0x00008200       ; system, type 2, present
  mov ax, SEL_LDT
  lldt ax
  sldt ax
  cmp ax, SEL_LDT
  jne .t19_done
  mov ax, SEL_LDT0
  mov ds, ax                               ; a TI=1 selector names the LDT
  mov dword [0x100], 0xC0DE0019
  mov eax, [0x100]
  mov edx, eax                ; the DS reload clobbers ax — park the value
  mov ax, SEL_DATA0           ; back to flat before any RES write:
  mov ds, ax                  ; the LDT segment's limit is 0x1ff
  cmp edx, 0xC0DE0019
  jne .t19_done
  mov byte [RES_LIN + 18], 1
.t19_done:
  mov ax, SEL_DATA0
  mov ds, ax

  ; ---- t20: LDT limit violation, then a cleared LDTR -------------------------
  ; Two distinct defect classes: a store past the LDT segment's own limit is
  ; a plain #GP(0) (t2 semantics), while a selector lookup with no LDT is a
  ; table-limit failure carrying the selector (#GP(sel&~3)).
  mov dword [OBS_LIN + 0], 0x55
  mov ax, SEL_LDT0
  mov ds, ax
  mov ecx, 0x200
  mov [ecx], ecx                           ; beyond limit 0x1ff -> #GP(0)
  mov ax, SEL_DATA0
  mov ds, ax
  cmp dword [OBS_LIN + 0], 0
  jne .t20_restore
  xor ax, ax
  lldt ax                                  ; null: the LDTR cache empties
  sldt ax
  test ax, ax
  jnz .t20_restore
  mov ax, SEL_LDT0
  mov ds, ax                               ; no LDT: the lookup is #GP(0x0c)
  mov ax, SEL_DATA0
  mov ds, ax
  cmp dword [OBS_LIN + 0], 0x0c
  jne .t20_restore
  mov byte [RES_LIN + 19], 1
.t20_restore:
  mov ax, SEL_LDT
  lldt ax

  ; ---- t21: VERR/VERW across the descriptor matrix ---------------------------
  mov ax, SEL_LDT0                         ; writable data
  verr ax
  pushfd
  pop edx
  test edx, 0x40
  jz .t21_done
  verw ax
  pushfd
  pop edx
  test edx, 0x40
  jz .t21_done
  mov ax, SEL_LDTRO                        ; read-only data: readable, not writable
  verw ax
  pushfd
  pop edx
  test edx, 0x40
  jnz .t21_done
  verr ax
  pushfd
  pop edx
  test edx, 0x40
  jz .t21_done
  mov ax, SEL_LDTCODE                      ; readable code: verr yes, verw no
  verr ax
  pushfd
  pop edx
  test edx, 0x40
  jz .t21_done
  verw ax
  pushfd
  pop edx
  test edx, 0x40
  jnz .t21_done
  xor ax, ax                               ; null selector clears ZF
  verr ax
  pushfd
  pop edx
  test edx, 0x40
  jnz .t21_done
  mov ax, 0x34                             ; beyond the LDT limit: ZF=0
  verr ax
  pushfd
  pop edx
  test edx, 0x40
  jnz .t21_done
  mov byte [RES_LIN + 20], 1
.t21_done:

  ; ---- t22: LAR/LSL values and failures --------------------------------------
  mov eax, 0xDEADBEEF
  mov ax, SEL_LDTCODE
  lar eax, ax                              ; rights byte of the 4 GiB code seg:
  cmp eax, 0x00c09b00                      ; mask 00FxFF00 zeroes the limit nibble
  jne .t22_done
  mov ax, SEL_LDT0
  lsl eax, ax                              ; the raw 0x1ff limit (no G)
  cmp eax, 0x1ff
  jne .t22_done
  mov ax, SEL_LDTBIG
  lsl eax, ax                              ; G-expanded 4 GiB
  cmp eax, 0xffffffff
  jne .t22_done
  mov ax, SEL_LDT3                         ; DPL3 data from CPL 0: dpl >= cpl
  lar eax, ax
  cmp eax, 0x00c0f200
  jne .t22_done
  mov eax, 0xDEADBEEF
  mov ecx, 0x34                            ; out of table: ZF=0, dest kept
  lar eax, cx                              ; (cx: eax's low half is the marker)
  pushfd
  pop edx
  test edx, 0x40
  jnz .t22_done
  cmp eax, 0xDEADBEEF
  jne .t22_done
  mov byte [RES_LIN + 21], 1
.t22_done:

  ; ---- t23: ARPL raises the RPL, ZF reports the change -----------------------
  mov ax, SEL_LDT0                         ; RPL 0
  mov bx, 0x0003                           ; RPL 3
  arpl ax, bx
  pushfd
  pop edx
  test edx, 0x40
  jz .t23_done
  cmp ax, 0x0f                             ; index kept, RPL raised
  jne .t23_done
  arpl ax, bx                              ; already 3: ZF=0, unchanged
  pushfd
  pop edx
  test edx, 0x40
  jnz .t23_done
  cmp ax, 0x0f
  jne .t23_done
  mov byte [RES_LIN + 22], 1
.t23_done:

  ; ---- t24: the task switch loads the incoming TSS's LDT ---------------------
  mov dword [TSS2_LIN + 0x20], task5_entry
  mov dword [TSS2_LIN + 0x24], 0x2
  mov word [TSS2_LIN + 0x60], SEL_LDT
  call far [ptr_tss2]
t24_ret:
  cmp dword [OBS_LIN + 0x88], SEL_LDT      ; the task SLDTs its own LDT
  jne t24_done
  cmp dword [OBS_LIN + 0x8c], 0xC0DE0019   ; read through an LDT segment
  jne t24_done
  cmp dword [OBS_LIN + 0x90], 0x40         ; VERR of an LDT code entry
  jne t24_done
  mov byte [RES_LIN + 23], 1
t24_done:

  ; ---- t14: identity map 0-1MB, switch on paging (CR3, then PG|WP) ---------
  mov dword [PD_LIN], PT0_LIN | 7     ; PDE[0]: the one 4 MiB table, P|R/W
  mov esi, PT0_LIN
  xor eax, eax                        ; PT0[i] = (i<<12) | P|R/W, 256 pages
.pt0_fill:
  mov edx, eax
  shl edx, 12
  or edx, 7
  mov [esi + eax * 4], edx
  inc eax
  cmp eax, 256
  jl .pt0_fill
  RELOAD_CR3
  mov eax, cr0
  or eax, 0x80010000                  ; PG | WP (WP: the R/O test needs a
  mov cr0, eax                        ; supervisor that honors read-only)
  mov dword [0x20000], 0xC0DE0014     ; data access and instruction fetch
  mov eax, [0x20000]                  ; both walk the tables now
  cmp eax, 0xC0DE0014
  jne pg_done
  mov byte [RES_LIN + 13], 1

  ; ---- t15: PDE[1] absent -> ring-0 write to 0x400000 is #PF(ec=2) ---------
  mov ecx, 0x400000
  mov [ecx], ecx                      ; pf_handler records ec + CR2, skips 2
  cmp dword [OBS_LIN + 0x7c], 2       ; W set, P and U clear
  jne pg_done
  cmp dword [OBS_LIN + 0x80], 0x400000
  jne pg_done
  mov byte [RES_LIN + 14], 1

  ; ---- t16: PTE for 0x7000 flipped read-only --------------------------------
  and dword [PT0_LIN + 7 * 4], ~2     ; clear R/W; CR3 reload so a cached
  RELOAD_CR3                          ; TLB (QEMU, real silicon) notices
  mov eax, [0x7000]                   ; the read succeeds
  mov ecx, 0x7000
  mov [ecx], ecx                      ; -> #PF(ec=3, P|W at CPL 0)
  cmp dword [OBS_LIN + 0x7c], 3
  jne .pg_restore
  cmp dword [OBS_LIN + 0x80], 0x7000
  jne .pg_restore
  mov byte [RES_LIN + 15], 1
.pg_restore:
  or dword [PT0_LIN + 7 * 4], 2
  RELOAD_CR3

  ; ---- t17: A/D bits on successful translations -----------------------------
  and dword [PT0_LIN + 5 * 4], ~0x60  ; clear A|D in the SCRATCH page's PTE
  and dword [PD_LIN], ~0x60           ; and in PDE[0] (the walk of THIS store
                                      ; sets A first, the store then clears
                                      ; it — the net content is A=0)
  RELOAD_CR3
  mov eax, [SCRATCH]                  ; a read: A in both levels, D untouched
  mov edx, [PD_LIN]
  and edx, 0x60
  cmp edx, 0x20                       ; PDE: A set, D does not exist there
  jne pg_done
  mov edx, [PT0_LIN + 5 * 4]
  and edx, 0x60
  cmp edx, 0x20                       ; PTE: A set, D still clear
  jne pg_done
  and dword [PT0_LIN + 5 * 4], ~0x60  ; clear again for the write pass
  RELOAD_CR3
  mov dword [SCRATCH], 0xC0DE0017     ; a write sets PTE.D as well
  mov edx, [PT0_LIN + 5 * 4]
  and edx, 0x60
  cmp edx, 0x60
  jne pg_done
  mov byte [RES_LIN + 16], 1

  ; ---- t18: ring 3 under paging — user page ok, supervisor page faults ------
  or dword [PD_LIN], 4                ; U on the directory entry: ring 3 can
  mov eax, 255                        ; reach the table at all; U on every
.pte_user:                            ; page (the image's code pages are in
  or dword [PT0_LIN + eax * 4], 4     ; entries 16-18, well past the stacks)
  dec eax
  jns .pte_user
  and dword [PT0_LIN + 9 * 4], ~4     ; 0x9000: the supervisor-only page
  RELOAD_CR3                          ; a cached TLB must see the new rights
  push dword SEL_DATA3R3
  push dword RING3_TOP
  push dword 0x2
  push dword SEL_CODE3R3
  push dword ring3_paging_entry
  iretd

pg_done:
pg_resume:
  mov esp, RING0_TOP
  mov ax, SEL_DATA0
  mov ds, ax

  ; ---- report ---------------------------------------------------------------------
  xor ebx, ebx
.loop:
  mov al, 't'
  call putc
  mov eax, ebx
  inc eax                       ; 1-based test number, two digits for 10+
  cmp eax, 10
  jl .single
  xor edx, edx                ; two digits: divide out the tens
  mov ecx, 10
  div ecx
  push edx
  add al, '0'
  call putc                   ; the tens digit
  pop eax
  jmp .digit
.single:
.digit:
  add al, '0'
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
  cmp ebx, 24
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
  dq 0                         ; 40: DPL0 call gate, built at runtime
  dq 0                         ; 48: DPL3 call gate, count 2, built at runtime
  dq 0x0000890026000067        ; 50: 32-bit TSS #2 at TSS2_LIN, limit 0x67
  dq 0                         ; 58: the LDT descriptor, filled at runtime
gdt_end:

gdtdesc:
  dw gdt_end - gdt - 1
  dd gdt

idtdesc:
  dw 0x14f                     ; 0x2a gate slots (through the 0x25 task gate)
  dd IDT_LIN

msg_ok:   db " ok", 10, 0
msg_bad:  db " BAD", 10, 0
msg_done: db "pm-smoke done", 10, 0

; far-call/far-jmp memory operands (offset then selector, m16:32)
ptr_gate9:   dd gate9_entry
             dw SEL_GATE9
ptr_gate10:  dd 0
             dw SEL_GATE10
ptr_tss2:    dd 0
             dw SEL_TSS2
ptr_maintss: dd 0
             dw SEL_TSS
