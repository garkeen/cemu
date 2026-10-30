; V86 interrupt delivery / reflection / nesting probe (self-contained
; multiboot ELF at 0x10000). The transcript is the judgement: run the same
; ELF under cemu and qemu-system-i386 -kernel and compare the F20/F22/F21
; lines plus the markers.
;
;   F20  V86 -> ring0 INT 0x20 through a 32-bit DPL3 interrupt gate. The
;        handler dumps the frame the CPU pushed: EIP, CS, EFLAGS, ESP, SS,
;        ES, DS, FS, GS (32-bit fields, SDM vol.3 17.3.3). The saved SS:ESP
;        must be the V86 stack; GS/FS/DS/ES must be cleared in the handler.
;   reflect  The handler copies the interrupted CS:EIP:FLAGS onto the V86
;        stack at [saved SS:saved ESP-6] and rewrites its frame to enter a
;        V86 handler at CS=0:EIP=0x8100 with ESP-6 -- the shape EMM386's
;        0x12edc9 reflector builds.
;   F22  The V86 handler runs INT 0x22 while the outer 3-word frame is live;
;        the nested delivery's saved SS:ESP must be 0x1800:0x6ffa.
;   F21  After the V86 handler IRETs through the outer frame, the main stub
;        reports via INT 0x21. The final frame must show the original
;        interrupted ESP 0x7000 and the restored V86 segment values.
;
; Build: nasm -f elf32 -o v86_int.o v86_int.asm
;        ld.lld -m elf_i386 -T v86_int.ld -o v86_int.elf v86_int.o
; Run:   cemu --machine x86 --isa x86 v86_int.elf
;        qemu-system-i386 -kernel v86_int.elf -display none \
;            -serial file:v86_int_qemu.txt -no-reboot \
;            -device isa-debug-exit,iobase=0xf4,iosize=0x4
; Both must print the same transcript and exit with status 11 (payload 5).
bits 32

%define SEL_CODE0 0x08
%define SEL_DATA0 0x10
%define SEL_TSS   0x18

%define IDT_LIN   0x2100
%define TSS_LIN   0x2400
%define TSS_ESP0  0x4800
%define F20       0x2080
%define F21       0x2084
%define F22       0x2088
%define FGP       0x208c

%define V86_STUB  0x8000
%define V86_HAND  0x8100
%define V86_GP    0x8300
%define V86_SS    0x1800
%define V86_SP    0x7000
%define MARK      0x9100

section .multiboot
align 4
  dd 0x1BADB002
  dd 0x3
  dd -(0x1BADB002 + 0x3)

section .text
global _start

_start:
  cli
  ; mask every PIC line: no IRQ may perturb the stack checks
  mov al, 0xff
  out 0x21, al
  out 0xa1, al

  ; COM1: divisor 1, 8N1, interrupts off
  mov dx, 0x3fb
  mov al, 0x80
  out dx, al
  mov dx, 0x3f8
  mov al, 1
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al
  mov dx, 0x3fb
  mov al, 3
  out dx, al
  mov dx, 0x3f9
  xor al, al
  out dx, al

  ; TSS: ring-0 stack for V86 gate delivery; no I/O bitmap reachable
  mov dword [TSS_LIN + 4], TSS_ESP0
  mov word [TSS_LIN + 8], SEL_DATA0
  mov word [TSS_LIN + 0x66], 0x68

  lgdt [gdtdesc]
  mov ax, SEL_DATA0
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  mov esp, 0x4000
  jmp SEL_CODE0:.flush
.flush:
  mov edi, IDT_LIN + 6 * 8
  mov eax, ud_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 13 * 8
  mov eax, gp_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 12 * 8
  mov eax, ss_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0x8e
  call mk_gate
  mov edi, IDT_LIN + 0x20 * 8
  mov eax, int20_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0xee
  call mk_gate
  mov edi, IDT_LIN + 0x21 * 8
  mov eax, int21_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0xee
  call mk_gate
  mov edi, IDT_LIN + 0x22 * 8
  mov eax, int22_handler
  mov cx, SEL_CODE0
  xor dl, dl
  mov bl, 0xee
  call mk_gate
  lidt [idtdesc]
  mov ax, SEL_TSS
  ltr ax

  ; Identity-map 0-4 MiB with U|RW|P and enable paging: EMM386 runs its V86
  ; contexts with paging on, and the V86 stack/frame accesses then walk the
  ; page tables as user accesses.
  mov edi, 0x31000
  xor eax, eax
  mov ecx, 1024
.pt:
  lea edx, [eax + 7]
  mov [edi], edx
  add eax, 0x1000
  add edi, 4
  loop .pt
  mov dword [0x30000], 0x31007
  mov eax, 0x30000
  mov cr3, eax
  mov eax, cr0
  or eax, 0x80000000
  mov cr0, eax

  ; deploy the V86 stub below 1 MiB
  mov esi, v86_stub_start
  mov edi, V86_STUB
  mov ecx, v86_stub_end - v86_stub_start
  cld
  rep movsb

  ; Enter V86: CS=0:EIP=0x8000, SS=0x1800:ESP=0x7000. IOPL must be 3:
  ; IRET, CLI/STI, PUSHF/POPF and INT n are sensitive in V86 and #GP when
  ; IOPL<3 (SDM vol.3 19.3; the EMM386 monitor runs its V86 contexts the
  ; same way).
  push dword V86_SS
  push dword V86_SP
  push dword 0x00023202
  push dword 0
  push dword V86_STUB
  iretd

; ---- handlers ---------------------------------------------------------------
; A V86 delivery clears GS/FS/DS/ES (SDM vol.3 17.3.3); every handler
; reloads the flat data selector before touching memory.
reload_flat:
  push ax
  mov ax, SEL_DATA0
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  pop ax
  ret

int20_handler:
  call reload_flat
  mov [F20], esp
  mov ebp, esp
  mov esi, s_f20
  call pframe

  ; reflect: copy the interrupted CS:EIP:FLAGS onto the V86 stack ...
  mov eax, [ebp + 0x0c]
  and eax, 0xffff
  sub eax, 6
  mov ecx, [ebp + 0x10]
  and ecx, 0xffff
  shl ecx, 4
  add ecx, eax
  mov edx, [ebp + 0x00]
  mov [ecx], dx
  mov edx, [ebp + 0x04]
  mov [ecx + 2], dx
  mov edx, [ebp + 0x08]
  mov [ecx + 4], dx
  ; ... and redirect this IRET into the V86 handler with ESP-6
  mov dword [ebp + 0x00], V86_HAND
  mov dword [ebp + 0x04], 0
  mov dword [ebp + 0x0c], eax
  mov esp, [F20]
  iretd

int22_handler:
  call reload_flat
  mov [F22], esp
  mov ebp, esp
  mov esi, s_f22
  call pframe
  mov esp, [F22]
  iretd

int21_handler:
  call reload_flat
  mov [F21], esp
  mov ebp, esp
  mov esi, s_f21
  call pframe
  mov esi, s_mark
  call puts
  mov esi, s_m0
  movzx eax, word [MARK + 0]
  call field
  mov esi, s_m1
  movzx eax, word [MARK + 2]
  call field
  mov esi, s_m2
  movzx eax, word [MARK + 4]
  call field
  mov esi, s_m3
  movzx eax, word [MARK + 6]
  call field
  mov esi, s_m4
  movzx eax, word [MARK + 8]
  call field
  mov esi, s_m5
  movzx eax, word [MARK + 10]
  call field
  mov esi, s_m6
  movzx eax, word [MARK + 12]
  call field
  mov al, 10
  call putc
  mov esi, s_done
  call puts
  mov al, 5
  jmp exit_port

ud_handler:
  call reload_flat
  mov [FGP], esp
  mov ebp, esp
  mov esi, s_ud
  call pframe
  jmp fail_exit

; #GP carries an error code on top of the frame: [ec][EIP][CS][EFLAGS]...
gp_handler:
  call reload_flat
  mov [FGP], esp
  mov ebp, esp
  cmp word [0x910e], 0x1111
  je gp_rep
  mov esi, s_gp
  call puts
  mov esi, s_ec
  mov eax, [ebp + 0x00]
  call field
  mov esi, s_eip
  mov eax, [ebp + 0x04]
  call field
  mov esi, s_cs
  mov eax, [ebp + 0x08]
  call field
  mov esi, s_fl
  mov eax, [ebp + 0x0c]
  call field
  mov esi, s_esp
  mov eax, [ebp + 0x10]
  call field
  mov esi, s_ss
  mov eax, [ebp + 0x14]
  call field
  mov esi, s_es
  mov eax, [ebp + 0x18]
  call field
  mov esi, s_ds
  mov eax, [ebp + 0x1c]
  call field
  mov esi, s_fs
  mov eax, [ebp + 0x20]
  call field
  mov esi, s_gs
  mov eax, [ebp + 0x24]
  call field
  mov al, 10
  call putc
  ; EMM386's error-code reflector shape: write [ec][EIP][CS][FLAGS] to the
  ; V86 stack and enter the V86 #GP handler with SP-8. The handler skips the
  ; faulting one-byte OUT and drops the error code before its IRET.
  mov eax, [ebp + 0x10]
  and eax, 0xffff
  sub eax, 8
  mov ecx, [ebp + 0x14]
  and ecx, 0xffff
  shl ecx, 4
  add ecx, eax
  mov edx, [ebp + 0x00]
  mov [ecx], dx
  mov edx, [ebp + 0x04]
  mov [ecx + 2], dx
  mov edx, [ebp + 0x08]
  mov [ecx + 4], dx
  mov edx, [ebp + 0x0c]
  mov [ecx + 6], dx
  ; The ring-0 frame has the error code on top: [ec][EIP][CS][EFLAGS]
  ; [ESP][SS][ES][DS][FS][GS]. Redirect the frame to the V86 #GP handler
  ; and drop the error code before the IRET.
  mov dword [ebp + 0x04], V86_GP
  mov dword [ebp + 0x08], 0
  mov dword [ebp + 0x10], eax
  mov esp, [FGP]
  add esp, 4
  iretd
gp_rep:
  mov [0x2380], ecx
  mov [0x2384], esi
  mov [0x2388], edi
  mov [0x238c], edx
  mov esi, s_rep
  call puts
  mov esi, s_ec
  mov eax, [ebp + 0x00]
  call field
  mov esi, s_eip
  mov eax, [ebp + 0x04]
  call field
  mov esi, s_ecx
  mov eax, [0x2380]
  call field
  mov esi, s_esi
  mov eax, [0x2384]
  call field
  mov esi, s_edi
  mov eax, [0x2388]
  call field
  mov esi, s_edx
  mov eax, [0x238c]
  call field
  mov al, 10
  call putc
  ; The REP tail is an expected path (the header's fourth marker), so it exits
  ; like the pass tail; only ud_handler/ss_handler keep fail_exit.
  mov esi, s_done
  call puts
  mov al, 5
  jmp exit_port
ss_handler:
  call reload_flat
  mov [FGP], esp
  mov ebp, esp
  mov esi, s_ssv
  call puts
  mov esi, s_ec
  mov eax, [ebp + 0x00]
  call field
  mov esi, s_eip
  mov eax, [ebp + 0x04]
  call field
  mov esi, s_cs
  mov eax, [ebp + 0x08]
  call field
  mov esi, s_fl
  mov eax, [ebp + 0x0c]
  call field
  mov esi, s_esp
  mov eax, [ebp + 0x10]
  call field
  mov esi, s_ss
  mov eax, [ebp + 0x14]
  call field
  mov al, 10
  call putc
  jmp fail_exit
fail_exit:
  mov al, 1
exit_port:
  mov dx, 0xf4
  out dx, al
.hang:
  hlt
  jmp .hang

; ---- output helpers ---------------------------------------------------------
putc:                            ; AL = character; waits for COM1 THRE
  push eax
  push edx
  mov ah, al
.wait:
  mov dx, 0x3fd
  in al, dx
  test al, 0x20
  jz .wait
  mov dx, 0x3f8
  mov al, ah
  out dx, al
  pop edx
  pop eax
  ret

puts:                            ; ESI = NUL-terminated string
  push eax
  push esi
.next:
  mov al, [esi]
  test al, al
  jz .done
  call putc
  inc esi
  jmp .next
.done:
  pop esi
  pop eax
  ret

hex32:                           ; EAX = value, prints 8 lowercase digits
  push eax
  push ebx
  push ecx
  mov ebx, eax
  mov ecx, 8
.loop:
  rol ebx, 4
  mov eax, ebx
  and eax, 0xf
  cmp eax, 10
  jb .digit
  add eax, 'a' - 10 - '0'
.digit:
  add eax, '0'
  call putc
  dec ecx
  jnz .loop
  pop ecx
  pop ebx
  pop eax
  ret

field:                           ; ESI = "name=", EAX = value; prints both
  call puts
  call hex32
  mov al, ' '
  call putc
  ret

pframe:                          ; EBP = frame, ESI = prefix
  call puts
  mov esi, s_eip
  mov eax, [ebp + 0x00]
  call field
  mov esi, s_cs
  mov eax, [ebp + 0x04]
  call field
  mov esi, s_fl
  mov eax, [ebp + 0x08]
  call field
  mov esi, s_esp
  mov eax, [ebp + 0x0c]
  call field
  mov esi, s_ss
  mov eax, [ebp + 0x10]
  call field
  mov esi, s_es
  mov eax, [ebp + 0x14]
  call field
  mov esi, s_ds
  mov eax, [ebp + 0x18]
  call field
  mov esi, s_fs
  mov eax, [ebp + 0x1c]
  call field
  mov esi, s_gs
  mov eax, [ebp + 0x20]
  call field
  mov al, 10
  call putc
  ret

mk_gate:                         ; EAX offset, CX selector, BL attr, EDI slot
  mov [edi], ax
  mov [edi + 2], cx
  mov [edi + 4], dl
  mov [edi + 5], bl
  shr eax, 16
  mov [edi + 6], ax
  ret

; ---- V86 stub (copied to 0x8000; position independent) ----------------------
bits 16
v86_stub_start:
  mov ax, 0
  mov ds, ax
  mov es, ax
  mov ax, 0x3000
  mov fs, ax
  mov ax, 0x4000
  mov gs, ax
  mov ax, V86_SS
  mov ss, ax
  mov sp, V86_SP
  mov word [MARK + 0], 0x1111
  mov ax, 0x1111
  int 0x20
  mov word [MARK + 2], 0x2222
  mov word [MARK + 4], 0x3333
  ; I/O bitmap denies every port (the TSS has no bitmap bytes): the OUT
  ; faults with #GP(0) even though IOPL=3, because VM=1 always consults the
  ; bitmap (SDM vol.2 OUT Operation).
  mov dx, 0x3f8
  out dx, al
  mov word [MARK + 10], 0x6666
  ; REP OUTSW to the denied COM1 port: the first iteration faults #GP(0).
  ; The architectural state at the fault (EIP at the REP, CX/SI/DI/DX) is
  ; what EMM386's I/O emulation reads to resume the transfer.
  mov word [MARK + 14], 0x1111
  mov cx, 3
  mov si, 0x9200
  mov dx, 0x3f8
  mov ax, 0x2222
  rep outsw
  mov word [MARK + 16], 0x3333
  mov esp, 0x00017000
  mov eax, 0x55667788
  push eax
  mov word [MARK + 12], 0x9999
  mov ax, 0x3333
  int 0x21
  hlt
  jmp $
times (0x100 - ($ - v86_stub_start)) db 0x90
v86_handler:
  mov word [MARK + 6], 0x4444
  int 0x22
  mov word [MARK + 8], 0x5555
  iret
times (0x300 - ($ - v86_stub_start)) db 0x90
v86_gp_handler:
  ; Stack: [ec][EIP][CS][FLAGS]; skip the one-byte OUT, drop the error code.
  mov bp, sp
  add word [bp + 2], 1
  add sp, 2
  iret
v86_stub_end:
bits 32

; ---- data -------------------------------------------------------------------
gdt:
  dq 0
  dq 0x00cf9b000000ffff          ; 08: code, base 0, 4 GiB, DPL0, 32-bit
  dq 0x00cf93000000ffff          ; 10: data, base 0, 4 GiB, DPL0
  dq 0x0000890024000067          ; 18: 32-bit TSS at TSS_LIN
gdt_end:

gdtdesc:
  dw gdt_end - gdt - 1
  dd gdt

idtdesc:
  dw 0x17f
  dd IDT_LIN

s_f20:  db "F20 ", 0
s_f21:  db "F21 ", 0
s_f22:  db "F22 ", 0
s_gp:   db "GP  ", 0
s_rep:  db "REP ", 0
s_ecx:  db "ecx=", 0
s_esi:  db "esi=", 0
s_edi:  db "edi=", 0
s_edx:  db "edx=", 0
s_ud:   db "UD  ", 0
s_ssv:  db "SS  ", 0
s_eip:  db "eip=", 0
s_cs:   db "cs=", 0
s_fl:   db "fl=", 0
s_esp:  db "esp=", 0
s_ss:   db "ss=", 0
s_es:   db "es=", 0
s_ds:   db "ds=", 0
s_fs:   db "fs=", 0
s_gs:   db "gs=", 0
s_ec:   db "ec=", 0
s_mark: db "mark ", 0
s_m0:   db "m0=", 0
s_m1:   db "m1=", 0
s_m2:   db "m2=", 0
s_m3:   db "m3=", 0
s_m4:   db "m4=", 0
s_m5:   db "m5=", 0
s_m6:   db "m6=", 0
s_done: db "v86-int done", 10, 0
