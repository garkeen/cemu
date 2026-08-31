	.file	"realmode.c"
	.text
/APP
	.code16gcc
	test_function: 
	mov $0x1234, %eax 
	ret
/NO_APP
	.def	_strlen;	.scl	3;	.type	32;	.endef
_strlen:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$16, %esp
	movl	$0, -4(%ebp)
	jmp	L2
L3:
	addl	$1, -4(%ebp)
	addl	$1, 8(%ebp)
L2:
	movl	8(%ebp), %eax
	movzbl	(%eax), %eax
	testb	%al, %al
	jne	L3
	movl	-4(%ebp), %eax
	leave
	ret
	.def	_outb;	.scl	3;	.type	32;	.endef
_outb:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$8, %esp
	movl	8(%ebp), %edx
	movl	12(%ebp), %eax
	movb	%dl, -4(%ebp)
	movw	%ax, -8(%ebp)
	movzbl	-4(%ebp), %eax
	movzwl	-8(%ebp), %edx
/APP
 # 38 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	out %al, %dx
 # 0 "" 2
/NO_APP
	nop
	leave
	ret
	.data
	.align 4
_serial_iobase:
	.long	1016
.lcomm _serial_inited,4,4
	.text
	.def	_inb;	.scl	3;	.type	32;	.endef
_inb:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$20, %esp
	movl	8(%ebp), %eax
	movw	%ax, -20(%ebp)
	movzwl	-20(%ebp), %eax
	movl	%eax, %edx
/APP
 # 48 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	in %dx, %al
 # 0 "" 2
/NO_APP
	movb	%al, -1(%ebp)
	movzbl	-1(%ebp), %eax
	leave
	ret
	.def	_serial_outb;	.scl	3;	.type	32;	.endef
_serial_outb:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$28, %esp
	movl	8(%ebp), %eax
	movb	%al, -20(%ebp)
L9:
	movl	_serial_iobase, %eax
	addl	$5, %eax
	movzwl	%ax, %eax
	movl	%eax, (%esp)
	call	_inb
	movb	%al, -1(%ebp)
	movzbl	-1(%ebp), %eax
	andl	$32, %eax
	testl	%eax, %eax
	je	L9
	movl	_serial_iobase, %eax
	movzwl	%ax, %edx
	movzbl	-20(%ebp), %eax
	movzbl	%al, %eax
	movl	%edx, 4(%esp)
	movl	%eax, (%esp)
	call	_outb
	nop
	leave
	ret
	.def	_serial_init;	.scl	3;	.type	32;	.endef
_serial_init:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
	movl	_serial_iobase, %eax
	addl	$3, %eax
	movzwl	%ax, %eax
	movl	%eax, (%esp)
	call	_inb
	movb	%al, -1(%ebp)
	orb	$-128, -1(%ebp)
	movl	_serial_iobase, %eax
	addl	$3, %eax
	movzwl	%ax, %edx
	movzbl	-1(%ebp), %eax
	movl	%edx, 4(%esp)
	movl	%eax, (%esp)
	call	_outb
	movl	_serial_iobase, %eax
	movzwl	%ax, %eax
	movl	%eax, 4(%esp)
	movl	$1, (%esp)
	call	_outb
	movl	_serial_iobase, %eax
	addl	$1, %eax
	movzwl	%ax, %eax
	movl	%eax, 4(%esp)
	movl	$0, (%esp)
	call	_outb
	movl	_serial_iobase, %eax
	addl	$3, %eax
	movzwl	%ax, %eax
	movl	%eax, (%esp)
	call	_inb
	movb	%al, -1(%ebp)
	andb	$127, -1(%ebp)
	movl	_serial_iobase, %eax
	addl	$3, %eax
	movzwl	%ax, %edx
	movzbl	-1(%ebp), %eax
	movl	%edx, 4(%esp)
	movl	%eax, (%esp)
	call	_outb
	movl	_serial_iobase, %eax
	addl	$1, %eax
	movzwl	%ax, %eax
	movl	%eax, 4(%esp)
	movl	$0, (%esp)
	call	_outb
	movl	_serial_iobase, %eax
	addl	$3, %eax
	movzwl	%ax, %eax
	movl	%eax, 4(%esp)
	movl	$3, (%esp)
	call	_outb
	movl	_serial_iobase, %eax
	addl	$2, %eax
	movzwl	%ax, %eax
	movl	%eax, 4(%esp)
	movl	$0, (%esp)
	call	_outb
	movl	_serial_iobase, %eax
	addl	$4, %eax
	movzwl	%ax, %eax
	movl	%eax, 4(%esp)
	movl	$3, (%esp)
	call	_outb
	nop
	leave
	ret
	.def	_print_serial;	.scl	3;	.type	32;	.endef
_print_serial:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$20, %esp
	movl	8(%ebp), %eax
	movl	%eax, (%esp)
	call	_strlen
	movl	%eax, -8(%ebp)
	movl	_serial_inited, %eax
	testl	%eax, %eax
	jne	L12
	call	_serial_init
	movl	$1, _serial_inited
L12:
	movl	$0, -4(%ebp)
	jmp	L13
L14:
	movl	8(%ebp), %edx
	movl	-4(%ebp), %eax
	addl	%edx, %eax
	movzbl	(%eax), %eax
	movsbl	%al, %eax
	movl	%eax, (%esp)
	call	_serial_outb
	addl	$1, -4(%ebp)
L13:
	movl	-4(%ebp), %eax
	cmpl	-8(%ebp), %eax
	jb	L14
	nop
	leave
	ret
	.def	_print_serial_u32;	.scl	3;	.type	32;	.endef
_print_serial_u32:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$20, %esp
	leal	-16(%ebp), %eax
	addl	$11, %eax
	movl	%eax, -4(%ebp)
	movl	-4(%ebp), %eax
	movb	$0, (%eax)
L16:
	movl	8(%ebp), %ecx
	movl	$-858993459, %edx
	movl	%ecx, %eax
	mull	%edx
	shrl	$3, %edx
	movl	%edx, %eax
	sall	$2, %eax
	addl	%edx, %eax
	addl	%eax, %eax
	subl	%eax, %ecx
	movl	%ecx, %edx
	movl	%edx, %eax
	addl	$48, %eax
	subl	$1, -4(%ebp)
	movl	%eax, %edx
	movl	-4(%ebp), %eax
	movb	%dl, (%eax)
	movl	8(%ebp), %eax
	movl	$-858993459, %edx
	mull	%edx
	movl	%edx, %eax
	shrl	$3, %eax
	movl	%eax, 8(%ebp)
	cmpl	$0, 8(%ebp)
	jne	L16
	movl	-4(%ebp), %eax
	movl	%eax, (%esp)
	call	_print_serial
	nop
	leave
	ret
.lcomm _failed,4,4
	.section .rdata,"dr"
LC0:
	.ascii "--- DONE: 0 ---\12\0"
LC1:
	.ascii "--- DONE: 1 ---\12\0"
	.text
	.def	_exit;	.scl	3;	.type	32;	.endef
_exit:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$8, %esp
	movl	8(%ebp), %eax
	movzbl	%al, %eax
	movl	$244, 4(%esp)
	movl	%eax, (%esp)
	call	_outb
	cmpl	$0, 8(%ebp)
	jne	L18
	movl	$LC0, (%esp)
	call	_print_serial
	jmp	L20
L18:
	movl	$LC1, (%esp)
	call	_print_serial
L20:
/APP
 # 134 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	hlt
 # 0 "" 2
/NO_APP
	jmp	L20
	.data
	.align 8
_gdt:
	.long	0
	.long	0
	.long	65535
	.long	13605632
	.long	65535
	.long	13603584
	.align 4
_gdt_descr:
	.word	23
	.long	_gdt
	.comm	_tmp_stack, 512, 5
.lcomm _inregs,40,32
.lcomm _outregs,40,32
	.text
	.def	_init_inregs;	.scl	3;	.type	32;	.endef
_init_inregs:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%ebx
	movl	$_inregs, %ebx
	movl	$0, %eax
	movl	$10, %edx
	movl	%ebx, %edi
	movl	%edx, %ecx
	rep stosl
	cmpl	$0, 8(%ebp)
	je	L22
	movl	8(%ebp), %eax
	movl	(%eax), %edx
	movl	%edx, _inregs
	movl	4(%eax), %edx
	movl	%edx, _inregs+4
	movl	8(%eax), %edx
	movl	%edx, _inregs+8
	movl	12(%eax), %edx
	movl	%edx, _inregs+12
	movl	16(%eax), %edx
	movl	%edx, _inregs+16
	movl	20(%eax), %edx
	movl	%edx, _inregs+20
	movl	24(%eax), %edx
	movl	%edx, _inregs+24
	movl	28(%eax), %edx
	movl	%edx, _inregs+28
	movl	32(%eax), %edx
	movl	%edx, _inregs+32
	movl	36(%eax), %eax
	movl	%eax, _inregs+36
L22:
	movl	_inregs+24, %eax
	testl	%eax, %eax
	jne	L24
	movl	$_tmp_stack+512, %eax
	movl	%eax, _inregs+24
L24:
	nop
	popl	%ebx
	popl	%edi
	popl	%ebp
	ret
	.def	_exec_in_big_real_mode;	.scl	3;	.type	32;	.endef
_exec_in_big_real_mode:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$16, %esp
	movl	$0, -4(%ebp)
	jmp	L26
L27:
	movl	8(%ebp), %eax
	movzwl	(%eax), %eax
	movzwl	%ax, %eax
	movl	-4(%ebp), %edx
	addl	%edx, %eax
	movzbl	(%eax), %eax
	movl	-4(%ebp), %edx
	addl	$_test_insn, %edx
	movb	%al, (%edx)
	addl	$1, -4(%ebp)
L26:
	movl	8(%ebp), %eax
	movzwl	2(%eax), %eax
	movzwl	%ax, %eax
	cmpl	%eax, -4(%ebp)
	jl	L27
	jmp	L28
L29:
	movl	-4(%ebp), %eax
	addl	$_test_insn, %eax
	movb	$-112, (%eax)
	addl	$1, -4(%ebp)
L28:
	movl	$_test_insn_end, %eax
	subl	$_test_insn, %eax
	cmpl	%eax, -4(%ebp)
	jl	L29
	movl	_inregs, %eax
	movl	%eax, _save.1441
	movl	_inregs+4, %eax
	movl	%eax, _save.1441+4
	movl	_inregs+8, %eax
	movl	%eax, _save.1441+8
	movl	_inregs+12, %eax
	movl	%eax, _save.1441+12
	movl	_inregs+16, %eax
	movl	%eax, _save.1441+16
	movl	_inregs+20, %eax
	movl	%eax, _save.1441+20
	movl	_inregs+24, %eax
	movl	%eax, _save.1441+24
	movl	_inregs+28, %eax
	movl	%eax, _save.1441+28
	movl	_inregs+32, %eax
	movl	%eax, _save.1441+32
	movl	_inregs+36, %eax
	movl	%eax, _save.1441+36
	movl	$16, %edx
/APP
 # 194 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	lgdtl _gdt_descr 
	mov %cr0, %eax 
	or $1, %eax 
	mov %eax, %cr0 
	mov %dx, %gs 
	and $-2, %eax 
	mov %eax, %cr0 
	pushw %es 
	pushw _save.1441+36; popfw 
	xchg %eax, _save.1441+0 
	xchg %ebx, _save.1441+4 
	xchg %ecx, _save.1441+8 
	xchg %edx, _save.1441+12 
	xchg %esi, _save.1441+16 
	xchg %edi, _save.1441+20 
	xchg %esp, _save.1441+24 
	xchg %ebp, _save.1441+28 
	test_insn: . = . + 32
	test_insn_end: 
	xchg %eax, _save.1441+0 
	xchg %ebx, _save.1441+4 
	xchg %ecx, _save.1441+8 
	xchg %edx, _save.1441+12 
	xchg %esi, _save.1441+16 
	xchg %edi, _save.1441+20 
	xchg %esp, _save.1441+24 
	xchg %ebp, _save.1441+28 
	pushfl 
	popl _save.1441+36 
	popw %es 
	cld
	xor %eax, %eax 
	mov %eax, %gs 
	
 # 0 "" 2
/NO_APP
	movl	%eax, -8(%ebp)
	movl	_save.1441, %eax
	movl	%eax, _outregs
	movl	_save.1441+4, %eax
	movl	%eax, _outregs+4
	movl	_save.1441+8, %eax
	movl	%eax, _outregs+8
	movl	_save.1441+12, %eax
	movl	%eax, _outregs+12
	movl	_save.1441+16, %eax
	movl	%eax, _outregs+16
	movl	_save.1441+20, %eax
	movl	%eax, _outregs+20
	movl	_save.1441+24, %eax
	movl	%eax, _outregs+24
	movl	_save.1441+28, %eax
	movl	%eax, _outregs+28
	movl	_save.1441+32, %eax
	movl	%eax, _outregs+32
	movl	_save.1441+36, %eax
	movl	%eax, _outregs+36
	nop
	leave
	ret
	.def	_regs_equal;	.scl	3;	.type	32;	.endef
_regs_equal:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$16, %esp
	movl	$_inregs, -8(%ebp)
	movl	$_outregs, -12(%ebp)
	movl	$0, -4(%ebp)
	jmp	L31
L34:
	movl	-4(%ebp), %eax
	movl	8(%ebp), %edx
	movl	%eax, %ecx
	sarl	%cl, %edx
	movl	%edx, %eax
	andl	$1, %eax
	testl	%eax, %eax
	jne	L32
	movl	-4(%ebp), %eax
	leal	0(,%eax,4), %edx
	movl	-8(%ebp), %eax
	addl	%edx, %eax
	movl	(%eax), %edx
	movl	-4(%ebp), %eax
	leal	0(,%eax,4), %ecx
	movl	-12(%ebp), %eax
	addl	%ecx, %eax
	movl	(%eax), %eax
	cmpl	%eax, %edx
	je	L32
	movl	$0, %eax
	jmp	L33
L32:
	addl	$1, -4(%ebp)
L31:
	cmpl	$7, -4(%ebp)
	jle	L34
	movl	$1, %eax
L33:
	leave
	ret
	.section .rdata,"dr"
LC2:
	.ascii "PASS: \0"
LC3:
	.ascii "FAIL: \0"
LC4:
	.ascii "\12\0"
	.text
	.def	_report;	.scl	3;	.type	32;	.endef
_report:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$12, %esp
	movl	12(%ebp), %eax
	movl	16(%ebp), %edx
	movw	%ax, -4(%ebp)
	movl	%edx, %eax
	movb	%al, -8(%ebp)
	movzwl	-4(%ebp), %eax
	movl	%eax, (%esp)
	call	_regs_equal
	testl	%eax, %eax
	jne	L36
	movb	$0, -8(%ebp)
L36:
	cmpb	$0, -8(%ebp)
	je	L37
	movl	$LC2, %eax
	jmp	L38
L37:
	movl	$LC3, %eax
L38:
	movl	%eax, (%esp)
	call	_print_serial
	movl	8(%ebp), %eax
	movl	%eax, (%esp)
	call	_print_serial
	movl	$LC4, (%esp)
	call	_print_serial
	movzbl	-8(%ebp), %eax
	xorl	$1, %eax
	testb	%al, %al
	je	L40
	movl	$1, _failed
L40:
	nop
	leave
	ret
	.section .rdata,"dr"
LC5:
	.ascii "xchg 1\0"
LC6:
	.ascii "xchg 2\0"
LC7:
	.ascii "xchg 3\0"
LC8:
	.ascii "xchg 4\0"
LC9:
	.ascii "xchg 5\0"
LC10:
	.ascii "xchg 6\0"
LC11:
	.ascii "xchg 7\0"
LC12:
	.ascii "xchg 8\0"
	.text
	.def	_test_xchg;	.scl	3;	.type	32;	.endef
_test_xchg:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%ebx
	subl	$12, %esp
/APP
 # 294 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test1: xchg %eax,%eax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 295 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test2: xchg %eax,%ebx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 296 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test3: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test3: xchg %eax,%ecx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 297 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test4: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test4: xchg %eax,%edx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 298 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test5: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test5: xchg %eax,%esi
	 
	1002: 
	.popsection
 # 0 "" 2
 # 299 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test6: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test6: xchg %eax,%edi
	 
	1002: 
	.popsection
 # 0 "" 2
 # 300 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test7: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test7: xchg %eax,%ebp
	 
	1002: 
	.popsection
 # 0 "" 2
 # 301 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xchg_test8: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xchg_test8: xchg %eax,%esp
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_inregs, %ebx
	movl	$0, %eax
	movl	$10, %edx
	movl	%ebx, %edi
	movl	%edx, %ecx
	rep stosl
	movl	$1, _inregs+4
	movl	$2, _inregs+8
	movl	$3, _inregs+12
	movl	$4, _inregs+16
	movl	$5, _inregs+20
	movl	$7, _inregs+24
	movl	$6, _inregs+28
	movl	$_insn_xchg_test1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC5, (%esp)
	call	_report
	movl	$_insn_xchg_test2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs+4, %eax
	cmpl	%eax, %edx
	jne	L42
	movl	_outregs+4, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	jne	L42
	movl	$1, %eax
	jmp	L43
L42:
	movl	$0, %eax
L43:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC6, (%esp)
	call	_report
	movl	$_insn_xchg_test3, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs+8, %eax
	cmpl	%eax, %edx
	jne	L44
	movl	_outregs+8, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	jne	L44
	movl	$1, %eax
	jmp	L45
L44:
	movl	$0, %eax
L45:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$5, 4(%esp)
	movl	$LC7, (%esp)
	call	_report
	movl	$_insn_xchg_test4, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs+12, %eax
	cmpl	%eax, %edx
	jne	L46
	movl	_outregs+12, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	jne	L46
	movl	$1, %eax
	jmp	L47
L46:
	movl	$0, %eax
L47:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC8, (%esp)
	call	_report
	movl	$_insn_xchg_test5, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs+16, %eax
	cmpl	%eax, %edx
	jne	L48
	movl	_outregs+16, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	jne	L48
	movl	$1, %eax
	jmp	L49
L48:
	movl	$0, %eax
L49:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$17, 4(%esp)
	movl	$LC9, (%esp)
	call	_report
	movl	$_insn_xchg_test6, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs+20, %eax
	cmpl	%eax, %edx
	jne	L50
	movl	_outregs+20, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	jne	L50
	movl	$1, %eax
	jmp	L51
L50:
	movl	$0, %eax
L51:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$33, 4(%esp)
	movl	$LC10, (%esp)
	call	_report
	movl	$_insn_xchg_test7, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs+28, %eax
	cmpl	%eax, %edx
	jne	L52
	movl	_outregs+28, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	jne	L52
	movl	$1, %eax
	jmp	L53
L52:
	movl	$0, %eax
L53:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$129, 4(%esp)
	movl	$LC11, (%esp)
	call	_report
	movl	$_insn_xchg_test8, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs+24, %eax
	cmpl	%eax, %edx
	jne	L54
	movl	_outregs+24, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	jne	L54
	movl	$1, %eax
	jmp	L55
L54:
	movl	$0, %eax
L55:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65, 4(%esp)
	movl	$LC12, (%esp)
	call	_report
	nop
	addl	$12, %esp
	popl	%ebx
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC13:
	.ascii "shld\0"
	.text
	.def	_test_shld;	.scl	3;	.type	32;	.endef
_test_shld:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 339 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_shld_test: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_shld_test: shld $8,%edx,%eax
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$190, -48(%ebp)
	movl	$-285212672, -36(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_shld_test, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$48879, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC13, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC14:
	.ascii "mov 1\0"
LC15:
	.ascii "mov 2\0"
LC16:
	.ascii "mov 3\0"
LC17:
	.ascii "mov 4\0"
LC18:
	.ascii "mov 5\0"
	.text
	.def	_test_mov_imm;	.scl	3;	.type	32;	.endef
_test_mov_imm:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 349 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mov_r32_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mov_r32_imm_1: mov $1234567890, %eax 
	1002: 
	.popsection
 # 0 "" 2
 # 350 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mov_r16_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mov_r16_imm_1: mov $1234, %ax 
	1002: 
	.popsection
 # 0 "" 2
 # 351 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mov_r8_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mov_r8_imm_1: mov $0x12, %ah 
	1002: 
	.popsection
 # 0 "" 2
 # 352 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mov_r8_imm_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mov_r8_imm_2: mov $0x34, %al 
	1002: 
	.popsection
 # 0 "" 2
 # 353 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mov_r8_imm_3: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mov_r8_imm_3: mov $0x12, %ah
	mov $0x34, %al
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_mov_r16_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$1234, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC14, (%esp)
	call	_report
	movl	$_insn_mov_r32_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$1234567890, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC15, (%esp)
	call	_report
	movl	$_insn_mov_r8_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4608, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC16, (%esp)
	call	_report
	movl	$_insn_mov_r8_imm_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$52, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC17, (%esp)
	call	_report
	movl	$_insn_mov_r8_imm_3, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4660, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC18, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC19:
	.ascii "sub 1\0"
LC20:
	.ascii "sub 2\0"
LC21:
	.ascii "sub 3\0"
LC22:
	.ascii "sub 4\0"
	.text
	.def	_test_sub_imm;	.scl	3;	.type	32;	.endef
_test_sub_imm:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 377 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sub_r32_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sub_r32_imm_1: mov $1234567890, %eax
	sub $10, %eax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 378 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sub_r16_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sub_r16_imm_1: mov $1234, %ax
	sub $10, %ax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 379 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sub_r8_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sub_r8_imm_1: mov $0x12, %ah
	sub $0x10, %ah
	 
	1002: 
	.popsection
 # 0 "" 2
 # 380 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sub_r8_imm_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sub_r8_imm_2: mov $0x34, %al
	sub $0x10, %al
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_sub_r16_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$1224, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC19, (%esp)
	call	_report
	movl	$_insn_sub_r32_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$1234567880, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC20, (%esp)
	call	_report
	movl	$_insn_sub_r8_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$512, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC21, (%esp)
	call	_report
	movl	$_insn_sub_r8_imm_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$36, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC22, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC23:
	.ascii "xor 1\0"
LC24:
	.ascii "xor 2\0"
LC25:
	.ascii "xor 3\0"
LC26:
	.ascii "xor 4\0"
	.text
	.def	_test_xor_imm;	.scl	3;	.type	32;	.endef
_test_xor_imm:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 401 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xor_r32_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xor_r32_imm_1: mov $1234567890, %eax
	xor $1234567890, %eax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 402 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xor_r16_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xor_r16_imm_1: mov $1234, %ax
	xor $1234, %ax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 403 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xor_r8_imm_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xor_r8_imm_1: mov $0x12, %ah
	xor $0x12, %ah
	 
	1002: 
	.popsection
 # 0 "" 2
 # 404 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xor_r8_imm_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xor_r8_imm_2: mov $0x34, %al
	xor $0x34, %al
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_xor_r16_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC23, (%esp)
	call	_report
	movl	$_insn_xor_r32_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC24, (%esp)
	call	_report
	movl	$_insn_xor_r8_imm_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC25, (%esp)
	call	_report
	movl	$_insn_xor_r8_imm_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC26, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC27:
	.ascii "cmp 1\0"
LC28:
	.ascii "cmp 2\0"
LC29:
	.ascii "cmp 3\0"
	.text
	.def	_test_cmp_imm;	.scl	3;	.type	32;	.endef
_test_cmp_imm:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 425 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cmp_test1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cmp_test1: mov $0x34, %al
	cmp $0x34, %al
	 
	1002: 
	.popsection
 # 0 "" 2
 # 427 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cmp_test2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cmp_test2: mov $0x34, %al
	cmp $0x39, %al
	 
	1002: 
	.popsection
 # 0 "" 2
 # 429 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cmp_test3: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cmp_test3: mov $0x34, %al
	cmp $0x24, %al
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_cmp_test1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$64, %eax
	testl	%eax, %eax
	setne	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC27, (%esp)
	call	_report
	movl	$_insn_cmp_test2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$64, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC28, (%esp)
	call	_report
	movl	$_insn_cmp_test3, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$64, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC29, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC30:
	.ascii "add 1\0"
LC31:
	.ascii "add 2\0"
	.text
	.def	_test_add_imm;	.scl	3;	.type	32;	.endef
_test_add_imm:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 450 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_add_test1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_add_test1: mov $0x43211234, %eax 
	add $0x12344321, %eax 
	 
	1002: 
	.popsection
 # 0 "" 2
 # 452 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_add_test2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_add_test2: mov $0x12, %eax 
	add $0x21, %al
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_add_test1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$1431655765, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC30, (%esp)
	call	_report
	movl	$_insn_add_test2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$51, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC31, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC32:
	.ascii "clc\0"
LC33:
	.ascii "stc\0"
LC34:
	.ascii "cli\0"
LC35:
	.ascii "sti\0"
LC36:
	.ascii "cld\0"
LC37:
	.ascii "std\0"
	.text
	.def	_test_eflags_insn;	.scl	3;	.type	32;	.endef
_test_eflags_insn:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 466 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_clc: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_clc: clc 
	1002: 
	.popsection
 # 0 "" 2
 # 467 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_stc: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_stc: stc 
	1002: 
	.popsection
 # 0 "" 2
 # 468 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cli: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cli: cli 
	1002: 
	.popsection
 # 0 "" 2
 # 469 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sti: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sti: sti 
	1002: 
	.popsection
 # 0 "" 2
 # 470 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cld: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cld: cld 
	1002: 
	.popsection
 # 0 "" 2
 # 471 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_std: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_std: std 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_clc, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$1, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC32, (%esp)
	call	_report
	movl	$_insn_stc, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$1, %eax
	testl	%eax, %eax
	setne	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC33, (%esp)
	call	_report
	movl	$_insn_cli, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$512, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC34, (%esp)
	call	_report
	movl	$_insn_sti, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$512, %eax
	testl	%eax, %eax
	setne	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC35, (%esp)
	call	_report
	movl	$_insn_cld, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$1024, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC36, (%esp)
	call	_report
	movl	$_insn_std, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$1024, %eax
	testl	%eax, %eax
	setne	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC37, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC38:
	.ascii "pio 1\0"
LC39:
	.ascii "pio 2\0"
LC40:
	.ascii "pio 3\0"
LC41:
	.ascii "pio 4\0"
LC42:
	.ascii "pio 5\0"
LC43:
	.ascii "pio 6\0"
	.text
	.def	_test_io;	.scl	3;	.type	32;	.endef
_test_io:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 496 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_io_test1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_io_test1: mov $0xff, %al 
	out %al, $0xe0 
	mov $0x00, %al 
	in $0xe0, %al 
	 
	1002: 
	.popsection
 # 0 "" 2
 # 500 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_io_test2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_io_test2: mov $0xffff, %ax 
	out %ax, $0xe0 
	mov $0x0000, %ax 
	in $0xe0, %ax 
	 
	1002: 
	.popsection
 # 0 "" 2
 # 504 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_io_test3: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_io_test3: mov $0xffffffff, %eax 
	out %eax, $0xe0 
	mov $0x000000, %eax 
	in $0xe0, %eax 
	 
	1002: 
	.popsection
 # 0 "" 2
 # 508 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_io_test4: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_io_test4: mov $0xe0, %dx 
	mov $0xff, %al 
	out %al, %dx 
	mov $0x00, %al 
	in %dx, %al 
	 
	1002: 
	.popsection
 # 0 "" 2
 # 513 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_io_test5: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_io_test5: mov $0xe0, %dx 
	mov $0xffff, %ax 
	out %ax, %dx 
	mov $0x0000, %ax 
	in %dx, %ax 
	 
	1002: 
	.popsection
 # 0 "" 2
 # 518 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_io_test6: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_io_test6: mov $0xe0, %dx 
	mov $0xffffffff, %eax 
	out %eax, %dx 
	mov $0x00000000, %eax 
	in %dx, %eax 
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_io_test1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$255, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC38, (%esp)
	call	_report
	movl	$_insn_io_test2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$65535, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC39, (%esp)
	call	_report
	movl	$_insn_io_test3, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-1, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC40, (%esp)
	call	_report
	movl	$_insn_io_test4, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$255, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC41, (%esp)
	call	_report
	movl	$_insn_io_test5, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$65535, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC42, (%esp)
	call	_report
	movl	$_insn_io_test6, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-1, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC43, (%esp)
	call	_report
	nop
	leave
	ret
/APP
	retf: lretw
	retf_imm: lretw $10
	.section .rdata,"dr"
LC44:
	.ascii "call 1\0"
LC45:
	.ascii "call near 1\0"
LC46:
	.ascii "call near 2\0"
LC47:
	.ascii "call far 1\0"
LC48:
	.ascii "call far 2\0"
LC49:
	.ascii "ret imm 1\0"
LC50:
	.ascii "retf imm 1\0"
/NO_APP
	.text
	.def	_test_call;	.scl	3;	.type	32;	.endef
_test_call:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$40, %esp
/APP
 # 555 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_call1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_call1: mov $test_function, %eax 
	call *%eax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 557 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_call_near1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_call_near1: jmp 2f
	1: mov $0x1234, %eax
	ret
	2: call 1b	 
	1002: 
	.popsection
 # 0 "" 2
 # 561 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_call_near2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_call_near2: call 1f
	jmp 2f
	1: mov $0x1234, %eax
	ret
	2:	 
	1002: 
	.popsection
 # 0 "" 2
 # 566 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_call_far1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_call_far1: lcallw *(%ebx)
	 
	1002: 
	.popsection
 # 0 "" 2
 # 567 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_call_far2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_call_far2: lcallw $0, $retf
	 
	1002: 
	.popsection
 # 0 "" 2
 # 568 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_ret_imm: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_ret_imm: sub $10, %sp; jmp 2f; 1: retw $10; 2: callw 1b 
	1002: 
	.popsection
 # 0 "" 2
 # 569 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_retf_imm: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_retf_imm: sub $10, %sp; lcallw $0, $retf_imm 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_call1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4660, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC44, (%esp)
	call	_report
	movl	$_insn_call_near1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4660, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC45, (%esp)
	call	_report
	movl	$_insn_call_near2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4660, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC46, (%esp)
	call	_report
	movl	$_retf, %eax
	shrl	$4, %eax
	sall	$16, %eax
	movl	%eax, %edx
	movl	$_retf, %eax
	andl	$15, %eax
	orl	%edx, %eax
	movl	%eax, -12(%ebp)
	leal	-12(%ebp), %eax
	movl	%eax, _inregs+4
	movl	$_insn_call_far1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC47, (%esp)
	call	_report
	movl	$_insn_call_far2, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC48, (%esp)
	call	_report
	movl	$_insn_ret_imm, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC49, (%esp)
	call	_report
	movl	$_insn_retf_imm, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC50, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC51:
	.ascii "jnz short 1\0"
LC52:
	.ascii "jnz short 2\0"
LC53:
	.ascii "jmp short 1\0"
	.text
	.def	_test_jcc_short;	.scl	3;	.type	32;	.endef
_test_jcc_short:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 599 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jnz_short1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jnz_short1: jnz 1f
	mov $0x1234, %eax
	1:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 602 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jnz_short2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jnz_short2: 1:
	cmp $0x1234, %eax
	mov $0x1234, %eax
	jnz 1b
	 
	1002: 
	.popsection
 # 0 "" 2
 # 606 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jmp_short1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jmp_short1: jmp 1f
	mov $0x1234, %eax
	1:
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_jnz_short1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC51, (%esp)
	call	_report
	movl	$_insn_jnz_short2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$64, %eax
	testl	%eax, %eax
	setne	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC52, (%esp)
	call	_report
	movl	$_insn_jmp_short1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC53, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC54:
	.ascii "jnz near 1\0"
LC55:
	.ascii "jnz near 2\0"
LC56:
	.ascii "jmp near 1\0"
	.text
	.def	_test_jcc_near;	.scl	3;	.type	32;	.endef
_test_jcc_near:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 625 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jnz_near1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jnz_near1: .byte 0x0f, 0x85, 0x06, 0x00
	mov $0x1234, %eax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 627 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jnz_near2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jnz_near2: cmp $0x1234, %eax
	mov $0x1234, %eax
	.byte 0x0f, 0x85, 0xf0, 0xff
	 
	1002: 
	.popsection
 # 0 "" 2
 # 630 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jmp_near1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jmp_near1: .byte 0xE9, 0x06, 0x00
	mov $0x1234, %eax
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_jnz_near1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC54, (%esp)
	call	_report
	movl	$_insn_jnz_near2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+36, %eax
	andl	$64, %eax
	testl	%eax, %eax
	setne	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC55, (%esp)
	call	_report
	movl	$_insn_jmp_near1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC56, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC57:
	.ascii "jmp far 1\0"
	.text
	.def	_test_long_jmp;	.scl	3;	.type	32;	.endef
_test_long_jmp:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 647 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_long_jmp: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_long_jmp: calll 1f
	jmp 2f
	1: jmp $0, $test_function
	2:
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_long_jmp, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4660, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC57, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC58:
	.ascii "push/pop 1\0"
LC59:
	.ascii "push/pop 2\0"
LC60:
	.ascii "push/pop 3\0"
LC61:
	.ascii "push/pop 4\0"
LC62:
	.ascii "push/pop 5\0"
LC63:
	.ascii "push/pop 6\0"
	.align 4
LC64:
	.ascii "push/pop with high bits set in %esp\0"
	.text
	.def	_test_push_pop;	.scl	3;	.type	32;	.endef
_test_push_pop:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 660 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_push32: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_push32: mov $0x12345678, %eax
	push %eax
	pop %ebx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 663 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_push16: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_push16: mov $0x1234, %ax
	push %ax
	pop %bx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 667 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_push_es: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_push_es: mov $0x231, %bx
	mov $0x123, %ax
	mov %ax, %es
	pushw %es
	pop %bx 
	 
	1002: 
	.popsection
 # 0 "" 2
 # 673 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_pop_es: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_pop_es: push %ax
	popw %es
	mov %es, %bx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 677 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_push_pop_ss: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_push_pop_ss: pushw %ss
	pushw %ax
	popw %ss
	mov %ss, %bx
	popw %ss
	 
	1002: 
	.popsection
 # 0 "" 2
 # 683 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_push_pop_fs: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_push_pop_fs: pushl %fs
	pushl %eax
	popl %fs
	mov %fs, %ebx
	popl %fs
	 
	1002: 
	.popsection
 # 0 "" 2
 # 689 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_push_pop_high_esp_bits: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_push_pop_high_esp_bits: xor $0x12340000, %esp 
	push %ax; 
	xor $0x12340000, %esp 
	pop %bx 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_push32, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_outregs+4, %eax
	cmpl	%eax, %edx
	jne	L69
	movl	_outregs, %eax
	cmpl	$305419896, %eax
	jne	L69
	movl	$1, %eax
	jmp	L70
L69:
	movl	$0, %eax
L70:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC58, (%esp)
	call	_report
	movl	$_insn_push16, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_outregs+4, %eax
	cmpl	%eax, %edx
	jne	L71
	movl	_outregs, %eax
	cmpl	$4660, %eax
	jne	L71
	movl	$1, %eax
	jmp	L72
L71:
	movl	$0, %eax
L72:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC59, (%esp)
	call	_report
	movl	$_insn_push_es, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_outregs, %eax
	cmpl	%eax, %edx
	jne	L73
	movl	_outregs, %eax
	cmpl	$291, %eax
	jne	L73
	movl	$1, %eax
	jmp	L74
L73:
	movl	$0, %eax
L74:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC60, (%esp)
	call	_report
	movl	$_insn_pop_es, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_outregs, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC61, (%esp)
	call	_report
	movl	$_insn_push_pop_ss, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_outregs, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC62, (%esp)
	call	_report
	movl	$_insn_push_pop_fs, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_outregs, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC63, (%esp)
	call	_report
	movl	$39287, _inregs
	movl	$30617, _inregs+4
	movl	$_insn_push_pop_high_esp_bits, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %eax
	cmpl	$39287, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$2, 4(%esp)
	movl	$LC64, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC65:
	.ascii "null\0"
	.text
	.def	_test_null;	.scl	3;	.type	32;	.endef
_test_null:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 726 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_null: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_null:  
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_null, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC65, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC66:
	.ascii "pusha/popa 1\0"
	.text
	.def	_test_pusha_popa;	.scl	3;	.type	32;	.endef
_test_pusha_popa:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 736 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_pusha: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_pusha: pushal
	popl %edi
	popl %esi
	popl %ebp
	addl $4, %esp
	popl %ebx
	popl %edx
	popl %ecx
	popl %eax
	 
	1002: 
	.popsection
 # 0 "" 2
 # 747 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_popa: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_popa: pushl %eax
	pushl %ecx
	pushl %edx
	pushl %ebx
	pushl %esp
	pushl %ebp
	pushl %esi
	pushl %edi
	popal
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$1, -44(%ebp)
	movl	$2, -40(%ebp)
	movl	$3, -36(%ebp)
	movl	$4, -32(%ebp)
	movl	$5, -28(%ebp)
	movl	$6, -20(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_pusha, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC66, (%esp)
	call	_report
	movl	$_insn_popa, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC66, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC67:
	.ascii "iret 1\0"
LC68:
	.ascii "iret 2\0"
LC69:
	.ascii "iret 3\0"
LC70:
	.ascii "rflags.rf\0"
LC71:
	.ascii "iret 4\0"
	.text
	.def	_test_iret;	.scl	3;	.type	32;	.endef
_test_iret:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 769 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_iret32: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_iret32: pushfl
	pushl %cs
	calll 1f
	jmp 2f
	1: iretl
	2:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 777 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_iret16: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_iret16: pushfw
	pushw %cs
	callw 1f
	jmp 2f
	1: iretw
	2:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 784 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_iret_flags32: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_iret_flags32: pushfl
	popl %eax
	andl $~0x2, %eax
	orl $0xffc18028, %eax
	pushl %eax
	pushl %cs
	calll 1f
	jmp 2f
	1: iretl
	2:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 795 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_iret_flags16: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_iret_flags16: pushfw
	popw %ax
	and $~0x2, %ax
	or $0x8028, %ax
	pushw %ax
	pushw %cs
	callw 1f
	jmp 2f
	1: iretw
	2:
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_iret32, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC67, (%esp)
	call	_report
	movl	$_insn_iret16, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC68, (%esp)
	call	_report
	movl	$_insn_iret_flags32, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC69, (%esp)
	call	_report
	movl	_outregs+36, %eax
	andl	$65536, %eax
	testl	%eax, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC70, (%esp)
	call	_report
	movl	$_insn_iret_flags16, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC71, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC72:
	.ascii "int 1\0"
	.text
	.def	_test_int;	.scl	3;	.type	32;	.endef
_test_int:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
	movl	$0, (%esp)
	call	_init_inregs
	movl	$68, %eax
	movl	$4096, (%eax)
	movl	$4096, %eax
	movb	$-49, (%eax)
/APP
 # 829 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_int11: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_int11: int $0x11
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_int11, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC72, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC73:
	.ascii "sti inhibit\0"
	.text
	.def	_test_sti_inhibit;	.scl	3;	.type	32;	.endef
_test_sti_inhibit:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
	movl	$0, (%esp)
	call	_init_inregs
	movl	$460, %eax
	movl	$4096, (%eax)
	movl	$4096, %eax
	movb	$-49, (%eax)
/APP
 # 842 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sti_inhibit: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sti_inhibit: cli
	movw $0x200b, %dx
	movl $1, %eax
	outl %eax, %dx
	movl $0, %eax
	outl %eax, %dx
	sti
	hlt
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_sti_inhibit, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC73, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC74:
	.ascii "imul 1\0"
LC75:
	.ascii "imul 2\0"
LC76:
	.ascii "imul 3\0"
LC77:
	.ascii "imul 4\0"
LC78:
	.ascii "imul 5\0"
LC79:
	.ascii "imul 6\0"
	.text
	.def	_test_imul;	.scl	3;	.type	32;	.endef
_test_imul:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 857 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_imul8_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_imul8_1: mov $2, %al
	mov $-4, %cx
	imul %cl
	 
	1002: 
	.popsection
 # 0 "" 2
 # 861 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_imul16_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_imul16_1: mov $2, %ax
	mov $-4, %cx
	imul %cx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 865 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_imul32_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_imul32_1: mov $2, %eax
	mov $-4, %ecx
	imul %ecx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 869 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_imul8_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_imul8_2: mov $0x12340002, %eax
	mov $4, %cx
	imul %cl
	 
	1002: 
	.popsection
 # 0 "" 2
 # 873 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_imul16_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_imul16_2: mov $2, %ax
	mov $4, %cx
	imul %cx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 877 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_imul32_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_imul32_2: mov $2, %eax
	mov $4, %ecx
	imul %ecx
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_imul8_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movzbl	%al, %eax
	cmpl	$248, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC74, (%esp)
	call	_report
	movl	$_insn_imul16_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$65528, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC75, (%esp)
	call	_report
	movl	$_insn_imul32_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-8, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC76, (%esp)
	call	_report
	movl	$_insn_imul8_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movzwl	%ax, %eax
	cmpl	$8, %eax
	jne	L81
	movl	_outregs, %eax
	movw	$0, %ax
	cmpl	$305397760, %eax
	jne	L81
	movl	$1, %eax
	jmp	L82
L81:
	movl	$0, %eax
L82:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC77, (%esp)
	call	_report
	movl	$_insn_imul16_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$8, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC78, (%esp)
	call	_report
	movl	$_insn_imul32_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$8, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC79, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC80:
	.ascii "mul 1\0"
LC81:
	.ascii "mul 2\0"
LC82:
	.ascii "mul 3\0"
	.text
	.def	_test_mul;	.scl	3;	.type	32;	.endef
_test_mul:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 906 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mul8: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mul8: mov $2, %al
	mov $4, %cx
	imul %cl
	 
	1002: 
	.popsection
 # 0 "" 2
 # 910 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mul16: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mul16: mov $2, %ax
	mov $4, %cx
	imul %cx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 914 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_mul32: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_mul32: mov $2, %eax
	mov $4, %ecx
	imul %ecx
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_mul8, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movzbl	%al, %eax
	cmpl	$8, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC80, (%esp)
	call	_report
	movl	$_insn_mul16, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$8, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC81, (%esp)
	call	_report
	movl	$_insn_mul32, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$8, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC82, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC83:
	.ascii "div 1\0"
LC84:
	.ascii "div 2\0"
LC85:
	.ascii "div 3\0"
	.text
	.def	_test_div;	.scl	3;	.type	32;	.endef
_test_div:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 932 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_div8: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_div8: mov $257, %ax
	mov $2, %cl
	div %cl
	 
	1002: 
	.popsection
 # 0 "" 2
 # 936 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_div16: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_div16: mov $512, %ax
	mov $5, %cx
	div %cx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 940 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_div32: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_div32: mov $512, %eax
	mov $5, %ecx
	div %ecx
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_div8, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$384, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC83, (%esp)
	call	_report
	movl	$_insn_div16, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$102, %eax
	jne	L85
	movl	_outregs+12, %eax
	cmpl	$2, %eax
	jne	L85
	movl	$1, %eax
	jmp	L86
L85:
	movl	$0, %eax
L86:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC84, (%esp)
	call	_report
	movl	$_insn_div32, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$102, %eax
	jne	L87
	movl	_outregs+12, %eax
	cmpl	$2, %eax
	jne	L87
	movl	$1, %eax
	jmp	L88
L87:
	movl	$0, %eax
L88:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC85, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC86:
	.ascii "idiv 1\0"
LC87:
	.ascii "idiv 2\0"
LC88:
	.ascii "idiv 3\0"
	.text
	.def	_test_idiv;	.scl	3;	.type	32;	.endef
_test_idiv:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 960 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_idiv8: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_idiv8: mov $256, %ax
	mov $-2, %cl
	idiv %cl
	 
	1002: 
	.popsection
 # 0 "" 2
 # 964 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_idiv16: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_idiv16: mov $512, %ax
	mov $-2, %cx
	idiv %cx
	 
	1002: 
	.popsection
 # 0 "" 2
 # 968 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_idiv32: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_idiv32: mov $512, %eax
	mov $-2, %ecx
	idiv %ecx
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_idiv8, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$128, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC86, (%esp)
	call	_report
	movl	$_insn_idiv16, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$65280, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC87, (%esp)
	call	_report
	movl	$_insn_idiv32, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-256, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$13, 4(%esp)
	movl	$LC88, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC89:
	.ascii "cbq 1\0"
LC90:
	.ascii "cwde 1\0"
	.text
	.def	_test_cbw;	.scl	3;	.type	32;	.endef
_test_cbw:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 986 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cbw: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cbw: mov $0xFE, %eax 
	cbw
	 
	1002: 
	.popsection
 # 0 "" 2
 # 988 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cwde: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cwde: mov $0xFFFE, %eax 
	cwde
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_cbw, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$65534, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC89, (%esp)
	call	_report
	movl	$_insn_cwde, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-2, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC90, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC91:
	.ascii "LOOPcc short 1\0"
LC92:
	.ascii "LOOPcc short 2\0"
LC93:
	.ascii "LOOPcc short 3\0"
	.text
	.def	_test_loopcc;	.scl	3;	.type	32;	.endef
_test_loopcc:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 1002 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_loop: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_loop: mov $10, %ecx
	1: inc %eax
	loop 1b
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1006 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_loope: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_loope: mov $10, %ecx
	mov $1, %eax
	1: dec %eax
	loope 1b
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1011 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_loopne: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_loopne: mov $10, %ecx
	mov $5, %eax
	1: dec %eax
	loopne 1b
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_loop, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$10, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC91, (%esp)
	call	_report
	movl	$_insn_loope, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-1, %eax
	jne	L92
	movl	_outregs+8, %eax
	cmpl	$8, %eax
	jne	L92
	movl	$1, %eax
	jmp	L93
L92:
	movl	$0, %eax
L93:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$5, 4(%esp)
	movl	$LC92, (%esp)
	call	_report
	movl	$_insn_loopne, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	testl	%eax, %eax
	jne	L94
	movl	_outregs+8, %eax
	cmpl	$5, %eax
	jne	L94
	movl	$1, %eax
	jmp	L95
L94:
	movl	$0, %eax
L95:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$5, 4(%esp)
	movl	$LC93, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC94:
	.ascii "DAS\0"
	.text
	.def	_test_das;	.scl	3;	.type	32;	.endef
_test_das:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$40, %esp
	movw	$0, -12(%ebp)
/APP
 # 1293 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_das: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_das: das 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movw	$0, -10(%ebp)
	jmp	L97
L101:
	movswl	-10(%ebp), %eax
	movl	_test_cases.1733(,%eax,4), %eax
	movl	%eax, -16(%ebp)
	movl	-16(%ebp), %eax
	movzbl	%al, %eax
	movl	%eax, _inregs
	movl	-16(%ebp), %eax
	shrl	$16, %eax
	movzbl	%al, %eax
	movl	%eax, _inregs+36
	movl	$_insn_das, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, (%esp)
	call	_regs_equal
	testl	%eax, %eax
	je	L98
	movl	_outregs, %edx
	movl	-16(%ebp), %eax
	shrl	$8, %eax
	movzbl	%al, %eax
	cmpl	%eax, %edx
	jne	L98
	movl	_outregs+36, %eax
	movzbl	%al, %eax
	movl	-16(%ebp), %edx
	shrl	$24, %edx
	cmpl	%edx, %eax
	je	L99
L98:
	addw	$1, -12(%ebp)
	jmp	L100
L99:
	movzwl	-10(%ebp), %eax
	addl	$1, %eax
	movw	%ax, -10(%ebp)
L97:
	cmpw	$1023, -10(%ebp)
	jle	L101
L100:
	cmpw	$0, -12(%ebp)
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$65535, 4(%esp)
	movl	$LC94, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC95:
	.ascii "cwd 1\0"
LC96:
	.ascii "cwd 2\0"
LC97:
	.ascii "cdq 1\0"
LC98:
	.ascii "cdq 2\0"
	.text
	.def	_test_cwd_cdq;	.scl	3;	.type	32;	.endef
_test_cwd_cdq:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 1315 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cwd_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cwd_1: mov $0x8000, %ax
	cwd
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1319 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cwd_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cwd_2: mov $0x1000, %ax
	cwd
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1323 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cdq_1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cdq_1: mov $0x80000000, %eax
	cdq
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1327 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cdq_2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cdq_2: mov $0x10000000, %eax
	cdq
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_cwd_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$32768, %eax
	jne	L103
	movl	_outregs+12, %eax
	cmpl	$65535, %eax
	jne	L103
	movl	$1, %eax
	jmp	L104
L103:
	movl	$0, %eax
L104:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC95, (%esp)
	call	_report
	movl	$_insn_cwd_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4096, %eax
	jne	L105
	movl	_outregs+12, %eax
	testl	%eax, %eax
	jne	L105
	movl	$1, %eax
	jmp	L106
L105:
	movl	$0, %eax
L106:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC96, (%esp)
	call	_report
	movl	$_insn_cdq_1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-2147483648, %eax
	jne	L107
	movl	_outregs+12, %eax
	cmpl	$-1, %eax
	jne	L107
	movl	$1, %eax
	jmp	L108
L107:
	movl	$0, %eax
L108:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC97, (%esp)
	call	_report
	movl	$_insn_cdq_2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$268435456, %eax
	jne	L109
	movl	_outregs+12, %eax
	testl	%eax, %eax
	jne	L109
	movl	$1, %eax
	jmp	L110
L109:
	movl	$0, %eax
L110:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$9, 4(%esp)
	movl	$LC98, (%esp)
	call	_report
	nop
	leave
	ret
	.data
	.align 4
_desc:
	.long	4660
	.word	16
	.section .rdata,"dr"
LC99:
	.ascii "lds\0"
LC100:
	.ascii "les\0"
LC101:
	.ascii "lfs\0"
LC102:
	.ascii "lgs\0"
LC103:
	.ascii "lss\0"
	.text
	.def	_test_lds_lss;	.scl	3;	.type	32;	.endef
_test_lds_lss:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$_desc, %eax
	movl	%eax, -44(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
/APP
 # 1361 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_lds: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_lds: pushl %ds
	lds (%ebx), %eax
	mov %ds, %ebx
	popl %ds
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_lds, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movl	_desc, %edx
	cmpl	%edx, %eax
	jne	L112
	movl	_outregs+4, %edx
	movzwl	_desc+4, %eax
	movzwl	%ax, %eax
	cmpl	%eax, %edx
	jne	L112
	movl	$1, %eax
	jmp	L113
L112:
	movl	$0, %eax
L113:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC99, (%esp)
	call	_report
/APP
 # 1370 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_les: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_les: les (%ebx), %eax
	mov %es, %ebx
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_les, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movl	_desc, %edx
	cmpl	%edx, %eax
	jne	L114
	movl	_outregs+4, %edx
	movzwl	_desc+4, %eax
	movzwl	%ax, %eax
	cmpl	%eax, %edx
	jne	L114
	movl	$1, %eax
	jmp	L115
L114:
	movl	$0, %eax
L115:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC100, (%esp)
	call	_report
/APP
 # 1377 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_lfs: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_lfs: pushl %fs
	lfs (%ebx), %eax
	mov %fs, %ebx
	popl %fs
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_lfs, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movl	_desc, %edx
	cmpl	%edx, %eax
	jne	L116
	movl	_outregs+4, %edx
	movzwl	_desc+4, %eax
	movzwl	%ax, %eax
	cmpl	%eax, %edx
	jne	L116
	movl	$1, %eax
	jmp	L117
L116:
	movl	$0, %eax
L117:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC101, (%esp)
	call	_report
/APP
 # 1386 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_lgs: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_lgs: pushl %gs
	lgs (%ebx), %eax
	mov %gs, %ebx
	popl %gs
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_lgs, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movl	_desc, %edx
	cmpl	%edx, %eax
	jne	L118
	movl	_outregs+4, %edx
	movzwl	_desc+4, %eax
	movzwl	%ax, %eax
	cmpl	%eax, %edx
	jne	L118
	movl	$1, %eax
	jmp	L119
L118:
	movl	$0, %eax
L119:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC102, (%esp)
	call	_report
/APP
 # 1395 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_lss: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_lss: mov %ss, %dx
	lss (%ebx), %eax
	mov %ss, %ebx
	mov %dx, %ss
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_lss, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	movl	_desc, %edx
	cmpl	%edx, %eax
	jne	L120
	movl	_outregs+4, %edx
	movzwl	_desc+4, %eax
	movzwl	%ax, %eax
	cmpl	%eax, %edx
	jne	L120
	movl	$1, %eax
	jmp	L121
L120:
	movl	$0, %eax
L121:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC103, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC104:
	.ascii "jcxz short 1\0"
LC105:
	.ascii "jcxz short 2\0"
LC106:
	.ascii "jcxz short 3\0"
LC107:
	.ascii "jecxz short 1\0"
LC108:
	.ascii "jecxz short 2\0"
	.text
	.def	_test_jcxz;	.scl	3;	.type	32;	.endef
_test_jcxz:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
/APP
 # 1407 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jcxz1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jcxz1: jcxz 1f
	mov $0x1234, %eax
	1:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1410 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jcxz2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jcxz2: mov $0x100, %ecx
	jcxz 1f
	mov $0x1234, %eax
	mov $0, %ecx
	1:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1415 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jcxz3: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jcxz3: mov $0x10000, %ecx
	jcxz 1f
	mov $0x1234, %eax
	1:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1419 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jecxz1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jecxz1: jecxz 1f
	mov $0x1234, %eax
	1:
	 
	1002: 
	.popsection
 # 0 "" 2
 # 1422 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_jecxz2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_jecxz2: mov $0x10000, %ecx
	jecxz 1f
	mov $0x1234, %eax
	mov $0, %ecx
	1:
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, (%esp)
	call	_init_inregs
	movl	$_insn_jcxz1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC104, (%esp)
	call	_report
	movl	$_insn_jcxz2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4660, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC105, (%esp)
	call	_report
	movl	$_insn_jcxz3, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+8, %eax
	cmpl	$65536, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$4, 4(%esp)
	movl	$LC106, (%esp)
	call	_report
	movl	$_insn_jecxz1, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC107, (%esp)
	call	_report
	movl	$_insn_jecxz2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$4660, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC108, (%esp)
	call	_report
	nop
	leave
	ret
	.section .rdata,"dr"
LC109:
	.ascii "cpuid\0"
	.text
	.def	_test_cpuid;	.scl	3;	.type	32;	.endef
_test_cpuid:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%ebx
	subl	$80, %esp
/APP
 # 1448 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_cpuid: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_cpuid: cpuid 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$4660, -12(%ebp)
	leal	-68(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	-12(%ebp), %eax
	movl	%eax, -68(%ebp)
	leal	-68(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	_inregs, %eax
	movl	%eax, -16(%ebp)
	movl	_inregs+8, %eax
	movl	%eax, -20(%ebp)
	movl	-16(%ebp), %eax
	movl	-20(%ebp), %edx
	movl	%edx, %ecx
/APP
 # 1456 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	cpuid
 # 0 "" 2
/NO_APP
	movl	%eax, -16(%ebp)
	movl	%ebx, -24(%ebp)
	movl	%ecx, -20(%ebp)
	movl	%edx, -28(%ebp)
	movl	$_insn_cpuid, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	%eax, -16(%ebp)
	jne	L124
	movl	_outregs+4, %eax
	cmpl	%eax, -24(%ebp)
	jne	L124
	movl	_outregs+8, %eax
	cmpl	%eax, -20(%ebp)
	jne	L124
	movl	_outregs+12, %eax
	cmpl	%eax, -28(%ebp)
	jne	L124
	movl	$1, %eax
	jmp	L125
L124:
	movl	$0, %eax
L125:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$15, 4(%esp)
	movl	$LC109, (%esp)
	call	_report
	nop
	addl	$80, %esp
	popl	%ebx
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC110:
	.ascii "ss relative addressing (1)\0"
LC111:
	.ascii "ss relative addressing (2)\0"
	.text
	.def	_test_ss_base_for_esp_ebp;	.scl	3;	.type	32;	.endef
_test_ss_base_for_esp_ebp:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1465 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_ssrel1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_ssrel1: mov %ss, %ax; mov %bx, %ss; movl (%ebp), %ebx; mov %ax, %ss 
	1002: 
	.popsection
 # 0 "" 2
 # 1466 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_ssrel2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_ssrel2: mov %ss, %ax; mov %bx, %ss; movl (%ebp,%edi,8), %ebx; mov %ax, %ss 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$1, -44(%ebp)
	movl	$_array.1798, %eax
	movl	%eax, -20(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_ssrel1, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %eax
	cmpl	$-2023406815, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC110, (%esp)
	call	_report
	movl	$1, _inregs+4
	movl	$_array.1798, %eax
	movl	%eax, _inregs+28
	movl	$0, _inregs+20
	movl	$_insn_ssrel2, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %eax
	cmpl	$-2023406815, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC111, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC112:
	.ascii "sgdt\0"
LC113:
	.ascii "sidt\0"
	.text
	.def	_test_sgdt_sidt;	.scl	3;	.type	32;	.endef
_test_sgdt_sidt:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$84, %esp
/APP
 # 1485 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sgdt: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sgdt: sgdtw (%eax) 
	1002: 
	.popsection
 # 0 "" 2
 # 1486 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sidt: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sidt: sidtw (%eax) 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	leal	-60(%ebp), %eax
	movl	%eax, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
/APP
 # 1491 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	sgdtw -54(%ebp)
 # 0 "" 2
/NO_APP
	movl	$_insn_sgdt, (%esp)
	call	_exec_in_big_real_mode
	movzwl	-54(%ebp), %edx
	movzwl	-60(%ebp), %eax
	cmpw	%ax, %dx
	jne	L128
	movl	-52(%ebp), %edx
	movl	-58(%ebp), %eax
	cmpl	%eax, %edx
	jne	L128
	movl	$1, %eax
	jmp	L129
L128:
	movl	$0, %eax
L129:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC112, (%esp)
	call	_report
	leal	-60(%ebp), %eax
	movl	%eax, _inregs
/APP
 # 1496 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	sidtw -54(%ebp)
 # 0 "" 2
/NO_APP
	movl	$_insn_sidt, (%esp)
	call	_exec_in_big_real_mode
	movzwl	-54(%ebp), %edx
	movzwl	-60(%ebp), %eax
	cmpw	%ax, %dx
	jne	L130
	movl	-52(%ebp), %edx
	movl	-58(%ebp), %eax
	cmpl	%eax, %edx
	jne	L130
	movl	$1, %eax
	jmp	L131
L130:
	movl	$0, %eax
L131:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC113, (%esp)
	call	_report
	nop
	addl	$84, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC114:
	.ascii "sahf\0"
	.text
	.def	_test_sahf;	.scl	3;	.type	32;	.endef
_test_sahf:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1503 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_sahf: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_sahf: sahf; pushfw; mov (%esp), %al; popfw 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$64768, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_sahf, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs, %eax
	orb	$-41, %al
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC114, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC115:
	.ascii "lahf\0"
	.text
	.def	_test_lahf;	.scl	3;	.type	32;	.endef
_test_lahf:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1513 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_lahf: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_lahf: pushfw; mov %al, (%esp); popfw; lahf 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$199, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_lahf, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	shrl	$8, %eax
	movl	%eax, %edx
	movl	_inregs, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC115, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC116:
	.ascii "movsx\0"
LC117:
	.ascii "movzx\0"
LC118:
	.ascii "movsx ah\0"
LC119:
	.ascii "movzx ah\0"
	.text
	.def	_test_movzx_movsx;	.scl	3;	.type	32;	.endef
_test_movzx_movsx:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1523 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_movsx: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_movsx: movsx %al, %ebx 
	1002: 
	.popsection
 # 0 "" 2
 # 1524 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_movzx: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_movzx: movzx %al, %ebx 
	1002: 
	.popsection
 # 0 "" 2
 # 1525 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_movzsah: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_movzsah: movsx %ah, %ebx 
	1002: 
	.popsection
 # 0 "" 2
 # 1526 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_movzxah: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_movzxah: movzx %ah, %ebx 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$305419932, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_movsx, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_inregs, %eax
	movsbl	%al, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$2, 4(%esp)
	movl	$LC116, (%esp)
	call	_report
	movl	$_insn_movzx, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_inregs, %eax
	movzbl	%al, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$2, 4(%esp)
	movl	$LC117, (%esp)
	call	_report
	movl	$_insn_movzsah, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_inregs, %eax
	shrl	$8, %eax
	movsbl	%al, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$2, 4(%esp)
	movl	$LC118, (%esp)
	call	_report
	movl	$_insn_movzxah, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+4, %edx
	movl	_inregs, %eax
	shrl	$8, %eax
	movzbl	%al, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$2, 4(%esp)
	movl	$LC119, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC120:
	.ascii "bswap\0"
	.text
	.def	_test_bswap;	.scl	3;	.type	32;	.endef
_test_bswap:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1542 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_bswap: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_bswap: bswap %ecx 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$305419896, -40(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_bswap, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+8, %eax
	cmpl	$2018915346, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$4, 4(%esp)
	movl	$LC120, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC121:
	.ascii "aad\0"
	.text
	.def	_test_aad;	.scl	3;	.type	32;	.endef
_test_aad:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1552 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_aad: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_aad: aad 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$305419896, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_aad, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$305397972, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC121, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC122:
	.ascii "aam\0"
	.text
	.def	_test_aam;	.scl	3;	.type	32;	.endef
_test_aam:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1562 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_aam: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_aam: aam 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$1985229328, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_aam, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$1985216774, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC122, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC123:
	.ascii "xlat\0"
	.text
	.def	_test_xlat;	.scl	3;	.type	32;	.endef
_test_xlat:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$324, %esp
/APP
 # 1572 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xlat: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xlat: xlat 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$0, -12(%ebp)
	jmp	L139
L140:
	movl	-12(%ebp), %eax
	addl	$1, %eax
	leal	-308(%ebp), %edx
	movl	-12(%ebp), %ecx
	addl	%ecx, %edx
	movb	%al, (%edx)
	addl	$1, -12(%ebp)
L139:
	cmpl	$255, -12(%ebp)
	jle	L140
	leal	-52(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$-1985229329, -52(%ebp)
	leal	-308(%ebp), %eax
	movl	%eax, -48(%ebp)
	leal	-52(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_xlat, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$-1985229328, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC123, (%esp)
	call	_report
	nop
	addl	$324, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC124:
	.ascii "salc (1)\0"
LC125:
	.ascii "salc (2)\0"
	.text
	.def	_test_salc;	.scl	3;	.type	32;	.endef
_test_salc:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1588 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_clc_salc: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_clc_salc: clc; .byte 0xd6 
	1002: 
	.popsection
 # 0 "" 2
 # 1589 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_stc_salc: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_stc_salc: stc; .byte 0xd6 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$305419896, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_clc_salc, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$305419776, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC124, (%esp)
	call	_report
	movl	$_insn_stc_salc, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$305420031, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC125, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC126:
	.ascii "fninit\0"
	.text
	.def	_test_fninit;	.scl	3;	.type	32;	.endef
_test_fninit:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
	movw	$-1, -50(%ebp)
	movw	$-1, -52(%ebp)
/APP
 # 1602 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_fninit: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_fninit: fninit ; fnstsw (%eax) ; fnstcw (%ebx) 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	leal	-52(%ebp), %eax
	movl	%eax, -48(%ebp)
	leal	-50(%ebp), %eax
	movl	%eax, -44(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_fninit, (%esp)
	call	_exec_in_big_real_mode
	movzwl	-52(%ebp), %eax
	testw	%ax, %ax
	jne	L143
	movzwl	-50(%ebp), %eax
	movzwl	%ax, %eax
	andl	$4159, %eax
	cmpl	$63, %eax
	jne	L143
	movl	$1, %eax
	jmp	L144
L143:
	movl	$0, %eax
L144:
	andl	$1, %eax
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC126, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC127:
	.ascii "nopl\0"
	.text
	.def	_test_nopl;	.scl	3;	.type	32;	.endef
_test_nopl:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$12, %esp
/APP
 # 1612 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_nopl1: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_nopl1: .byte 0x90
 
	1002: 
	.popsection
 # 0 "" 2
 # 1613 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_nopl2: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_nopl2: .byte 0x66, 0x90
 
	1002: 
	.popsection
 # 0 "" 2
 # 1614 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_nopl3: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_nopl3: .byte 0x0f, 0x1f, 0x00
 
	1002: 
	.popsection
 # 0 "" 2
 # 1615 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_nopl4: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_nopl4: .byte 0x0f, 0x1f, 0x40, 0x00
 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_nopl1, (%esp)
	call	_exec_in_big_real_mode
	movl	$_insn_nopl2, (%esp)
	call	_exec_in_big_real_mode
	movl	$_insn_nopl3, (%esp)
	call	_exec_in_big_real_mode
	movl	$_insn_nopl4, (%esp)
	call	_exec_in_big_real_mode
	movl	$1, 8(%esp)
	movl	$0, 4(%esp)
	movl	$LC127, (%esp)
	call	_report
	nop
	leave
	ret
.lcomm _perf_baseline,8,8
	.def	_cycles_in_big_real_mode;	.scl	3;	.type	32;	.endef
_cycles_in_big_real_mode:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	subl	$92, %esp
	leal	-80(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$1073741824, -72(%ebp)
	leal	-80(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	8(%ebp), %eax
	movl	%eax, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs+16, %eax
	movl	$0, %edx
	movl	%eax, %edx
	movl	$0, %eax
	movl	_outregs+4, %ecx
	movl	$0, %ebx
	movl	%eax, %esi
	orl	%ecx, %esi
	movl	%esi, -32(%ebp)
	movl	%edx, %eax
	orl	%ebx, %eax
	movl	%eax, -28(%ebp)
	movl	_outregs+12, %eax
	movl	$0, %edx
	movl	%eax, %edx
	movl	$0, %eax
	movl	_outregs, %ecx
	movl	$0, %ebx
	movl	%eax, %esi
	orl	%ecx, %esi
	movl	%esi, -40(%ebp)
	movl	%edx, %eax
	orl	%ebx, %eax
	movl	%eax, -36(%ebp)
	movl	-40(%ebp), %eax
	movl	-36(%ebp), %edx
	subl	-32(%ebp), %eax
	sbbl	-28(%ebp), %edx
	addl	$92, %esp
	popl	%ebx
	popl	%esi
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC128:
	.ascii " millicycles/emulated jump instruction\12\0"
	.text
	.def	_test_perf_loop;	.scl	3;	.type	32;	.endef
_test_perf_loop:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%esi
	pushl	%ebx
	subl	$16, %esp
/APP
 # 1657 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_loop: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_loop: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_perf_loop, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, _perf_baseline
	movl	%edx, _perf_baseline+4
	movl	_perf_baseline, %eax
	movl	_perf_baseline+4, %edx
	imull	$1000, %edx, %ebx
	imull	$0, %eax, %ecx
	addl	%ebx, %ecx
	movl	$1000, %ebx
	mull	%ebx
	movl	%eax, %ebx
	movl	%edx, %esi
	addl	%esi, %ecx
	movl	%ecx, %esi
	movl	%ebx, %eax
	movl	%esi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC128, (%esp)
	call	_print_serial
	nop
	addl	$16, %esp
	popl	%ebx
	popl	%esi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC129:
	.ascii " millicycles/emulated move instruction\12\0"
	.text
	.def	_test_perf_mov;	.scl	3;	.type	32;	.endef
_test_perf_mov:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%esi
	pushl	%ebx
	subl	$32, %esp
/APP
 # 1667 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_move: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_move: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:mov %esi, %edi
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_perf_move, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, -16(%ebp)
	movl	%edx, -12(%ebp)
	movl	-12(%ebp), %eax
	imull	$1000, %eax, %edx
	movl	-16(%ebp), %eax
	imull	$0, %eax, %eax
	leal	(%edx,%eax), %ecx
	movl	$1000, %esi
	movl	%esi, %eax
	mull	-16(%ebp)
	movl	%eax, %ebx
	movl	%edx, %esi
	addl	%esi, %ecx
	movl	%ecx, %esi
	movl	%ebx, %eax
	movl	%esi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC129, (%esp)
	call	_print_serial
	nop
	addl	$32, %esp
	popl	%ebx
	popl	%esi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC130:
	.ascii " millicycles/emulated arithmetic instruction\12\0"
	.text
	.def	_test_perf_arith;	.scl	3;	.type	32;	.endef
_test_perf_arith:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%esi
	pushl	%ebx
	subl	$32, %esp
/APP
 # 1677 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_arith: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_arith: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:add $4, %edi
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	movl	$_insn_perf_arith, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, -16(%ebp)
	movl	%edx, -12(%ebp)
	movl	-12(%ebp), %eax
	imull	$1000, %eax, %edx
	movl	-16(%ebp), %eax
	imull	$0, %eax, %eax
	leal	(%edx,%eax), %ecx
	movl	$1000, %esi
	movl	%esi, %eax
	mull	-16(%ebp)
	movl	%eax, %ebx
	movl	%edx, %esi
	addl	%esi, %ecx
	movl	%ecx, %esi
	movl	%ebx, %eax
	movl	%esi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC130, (%esp)
	call	_print_serial
	nop
	addl	$32, %esp
	popl	%ebx
	popl	%esi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC131:
	.ascii " millicycles/emulated memory load instruction\12\0"
	.text
	.def	_test_perf_memory_load;	.scl	3;	.type	32;	.endef
_test_perf_memory_load:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi
	subl	$80, %esp
/APP
 # 1687 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_memory_load: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_memory_load: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:cmp $0, (%edi)
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-56(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	leal	-64(%ebp), %eax
	movl	%eax, -36(%ebp)
	leal	-56(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_perf_memory_load, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, -16(%ebp)
	movl	%edx, -12(%ebp)
	movl	-12(%ebp), %eax
	imull	$1000, %eax, %edx
	movl	-16(%ebp), %eax
	imull	$0, %eax, %eax
	leal	(%edx,%eax), %ecx
	movl	$1000, %edi
	movl	%edi, %eax
	mull	-16(%ebp)
	movl	%eax, %esi
	movl	%edx, %edi
	addl	%edi, %ecx
	movl	%ecx, %edi
	movl	%esi, %eax
	movl	%edi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC131, (%esp)
	call	_print_serial
	nop
	addl	$80, %esp
	popl	%esi
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC132:
	.ascii " millicycles/emulated memory store instruction\12\0"
	.text
	.def	_test_perf_memory_store;	.scl	3;	.type	32;	.endef
_test_perf_memory_store:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi
	subl	$80, %esp
/APP
 # 1700 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_memory_store: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_memory_store: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:mov %ax, (%edi)
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-56(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	leal	-64(%ebp), %eax
	movl	%eax, -36(%ebp)
	leal	-56(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_perf_memory_store, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, -16(%ebp)
	movl	%edx, -12(%ebp)
	movl	-12(%ebp), %eax
	imull	$1000, %eax, %edx
	movl	-16(%ebp), %eax
	imull	$0, %eax, %eax
	leal	(%edx,%eax), %ecx
	movl	$1000, %edi
	movl	%edi, %eax
	mull	-16(%ebp)
	movl	%eax, %esi
	movl	%edx, %edi
	addl	%edi, %ecx
	movl	%ecx, %edi
	movl	%esi, %eax
	movl	%edi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC132, (%esp)
	call	_print_serial
	nop
	addl	$80, %esp
	popl	%esi
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC133:
	.ascii " millicycles/emulated memory RMW instruction\12\0"
	.text
	.def	_test_perf_memory_rmw;	.scl	3;	.type	32;	.endef
_test_perf_memory_rmw:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi
	subl	$80, %esp
/APP
 # 1712 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_memory_rmw: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_memory_rmw: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:add $1, (%edi)
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-56(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	leal	-64(%ebp), %eax
	movl	%eax, -36(%ebp)
	leal	-56(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_perf_memory_rmw, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, -16(%ebp)
	movl	%edx, -12(%ebp)
	movl	-12(%ebp), %eax
	imull	$1000, %eax, %edx
	movl	-16(%ebp), %eax
	imull	$0, %eax, %eax
	leal	(%edx,%eax), %ecx
	movl	$1000, %edi
	movl	%edi, %eax
	mull	-16(%ebp)
	movl	%eax, %esi
	movl	%edx, %edi
	addl	%edi, %ecx
	movl	%ecx, %edi
	movl	%esi, %eax
	movl	%edi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC133, (%esp)
	call	_print_serial
	nop
	addl	$80, %esp
	popl	%esi
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC134:
	.ascii " millicycles/emulated SHL instruction\12\0"
	.text
	.def	_test_perf_memory_shl;	.scl	3;	.type	32;	.endef
_test_perf_memory_shl:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi
	subl	$80, %esp
/APP
 # 1723 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_memory_shl: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_memory_shl: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:shl $1, %edi
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-56(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	leal	-64(%ebp), %eax
	movl	%eax, -36(%ebp)
	leal	-56(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_perf_memory_shl, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, -16(%ebp)
	movl	%edx, -12(%ebp)
	movl	-12(%ebp), %eax
	imull	$1000, %eax, %edx
	movl	-16(%ebp), %eax
	imull	$0, %eax, %eax
	leal	(%edx,%eax), %ecx
	movl	$1000, %edi
	movl	%edi, %eax
	mull	-16(%ebp)
	movl	%eax, %esi
	movl	%edx, %edi
	addl	%edi, %ecx
	movl	%ecx, %edi
	movl	%esi, %eax
	movl	%edi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC134, (%esp)
	call	_print_serial
	nop
	addl	$80, %esp
	popl	%esi
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
	.align 4
LC135:
	.ascii " millicycles/emulated ADC instruction\12\0"
	.text
	.def	_test_perf_memory_adc;	.scl	3;	.type	32;	.endef
_test_perf_memory_adc:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi
	subl	$80, %esp
/APP
 # 1734 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_perf_memory_adc: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_perf_memory_adc: rdtsc; mov %eax, %ebx; mov %edx, %esi
1:adc $1, %edi
.byte 0x67; loop 1b
rdtsc 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-56(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	leal	-64(%ebp), %eax
	movl	%eax, -36(%ebp)
	leal	-56(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_perf_memory_adc, (%esp)
	call	_cycles_in_big_real_mode
	movl	%eax, -16(%ebp)
	movl	%edx, -12(%ebp)
	movl	-12(%ebp), %eax
	imull	$1000, %eax, %edx
	movl	-16(%ebp), %eax
	imull	$0, %eax, %eax
	leal	(%edx,%eax), %ecx
	movl	$1000, %edi
	movl	%edi, %eax
	mull	-16(%ebp)
	movl	%eax, %esi
	movl	%edx, %edi
	addl	%edi, %ecx
	movl	%ecx, %edi
	movl	%esi, %eax
	movl	%edi, %edx
	shrdl	$30, %edx, %eax
	shrl	$30, %edx
	movl	%eax, (%esp)
	call	_print_serial_u32
	movl	$LC135, (%esp)
	call	_print_serial
	nop
	addl	$80, %esp
	popl	%esi
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC136:
	.ascii "mov dr with mod bits\0"
	.text
	.def	_test_dr_mod;	.scl	3;	.type	32;	.endef
_test_dr_mod:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1743 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_drmod: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_drmod: movl %ebx, %dr0
	.byte 0x0f 
	 .byte 0x21 
	 .byte 0x0
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$57005, -48(%ebp)
	movl	$44269, -44(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_drmod, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %eax
	cmpl	$44269, %eax
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$3, 4(%esp)
	movl	$LC136, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC137:
	.ascii "smsw\0"
	.text
	.def	_test_smsw;	.scl	3;	.type	32;	.endef
_test_smsw:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1754 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_smsw: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_smsw: movl %cr0, %ebx
	movl %ebx, %ecx
	or $0x40000000, %ebx
	movl %ebx, %cr0
	smswl %eax
	movl %ecx, %cr0
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$305419896, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_smsw, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_outregs+4, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$7, 4(%esp)
	movl	$LC137, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.section .rdata,"dr"
LC138:
	.ascii "xadd\0"
	.text
	.def	_test_xadd;	.scl	3;	.type	32;	.endef
_test_xadd:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	subl	$68, %esp
/APP
 # 1769 "D:/code/c/TinyEMU/v86/tests/kvm-unit-tests/x86/realmode.c" 1
	.pushsection .data.insn  
	insn_xadd: 
	.word 1001f, 1002f - 1001f 
	.popsection 
	.pushsection .text.insn, "ax" 
	1001: 
	insn_code_xadd: xaddl %eax, %eax
	 
	1002: 
	.popsection
 # 0 "" 2
/NO_APP
	leal	-48(%ebp), %edx
	movl	$0, %eax
	movl	$10, %ecx
	movl	%edx, %edi
	rep stosl
	movl	$305419896, -48(%ebp)
	leal	-48(%ebp), %eax
	movl	%eax, (%esp)
	call	_init_inregs
	movl	$_insn_xadd, (%esp)
	call	_exec_in_big_real_mode
	movl	_outregs, %edx
	movl	_inregs, %eax
	addl	%eax, %eax
	cmpl	%eax, %edx
	sete	%al
	movzbl	%al, %eax
	movl	%eax, 8(%esp)
	movl	$1, 4(%esp)
	movl	$LC138, (%esp)
	call	_report
	nop
	addl	$68, %esp
	popl	%edi
	popl	%ebp
	ret
	.globl	_realmode_start
	.def	_realmode_start;	.scl	2;	.type	32;	.endef
_realmode_start:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$24, %esp
	movl	$33, 4(%esp)
	movl	$0, (%esp)
	call	_outb
	movl	$161, 4(%esp)
	movl	$0, (%esp)
	call	_outb
	call	_test_null
	call	_test_shld
	call	_test_push_pop
	call	_test_pusha_popa
	call	_test_mov_imm
	call	_test_cmp_imm
	call	_test_add_imm
	call	_test_sub_imm
	call	_test_xor_imm
	call	_test_io
	call	_test_eflags_insn
	call	_test_jcc_short
	call	_test_jcc_near
	call	_test_call
	call	_test_long_jmp
	call	_test_xchg
	call	_test_iret
	call	_test_int
	call	_test_sti_inhibit
	call	_test_imul
	call	_test_mul
	call	_test_div
	call	_test_idiv
	call	_test_loopcc
	call	_test_cbw
	call	_test_cwd_cdq
	call	_test_das
	call	_test_lds_lss
	call	_test_jcxz
	call	_test_cpuid
	call	_test_ss_base_for_esp_ebp
	call	_test_sgdt_sidt
	call	_test_lahf
	call	_test_sahf
	call	_test_movzx_movsx
	call	_test_bswap
	call	_test_aad
	call	_test_aam
	call	_test_xlat
	call	_test_salc
	call	_test_fninit
	call	_test_dr_mod
	call	_test_smsw
	call	_test_nopl
	call	_test_xadd
	call	_test_perf_loop
	call	_test_perf_mov
	call	_test_perf_arith
	call	_test_perf_memory_shl
	call	_test_perf_memory_adc
	call	_test_perf_memory_load
	call	_test_perf_memory_store
	call	_test_perf_memory_rmw
	movl	_failed, %eax
	movl	%eax, (%esp)
	call	_exit
	nop
	leave
	ret
	.globl	_r_gdt
	.data
	.align 8
_r_gdt:
	.long	0
	.long	0
	.long	65535
	.long	39680
	.long	65535
	.long	37632
	.globl	_r_gdt_descr
	.align 4
_r_gdt_descr:
	.word	23
	.long	_r_gdt
	.globl	_r_idt_descr
	.align 4
_r_idt_descr:
	.word	1023
	.long	0
/APP
	.section .init 
	.code32 
	mb_magic = 0x1BADB002 
	mb_flags = 0x0 
	# multiboot header 
	.long mb_magic, mb_flags, 0 - (mb_magic + mb_flags) 
	.globl start 
	.data 
	. = . + 4096 
	stacktop: 
	.text 
	start: 
	lgdt r_gdt_descr 
	lidt r_idt_descr 
	ljmp $8, $1f; 1: 
	.code16gcc 
	mov $16, %eax 
	mov %ax, %ds 
	mov %ax, %es 
	mov %ax, %fs 
	mov %ax, %gs 
	mov %ax, %ss 
	mov %cr0, %eax 
	btc $0, %eax 
	mov %eax, %cr0 
	ljmp $0, $realmode_entry 
	realmode_entry: 
	xor %ax, %ax 
	mov %ax, %ds 
	mov %ax, %es 
	mov %ax, %ss 
	mov %ax, %fs 
	mov %ax, %gs 
	mov $stacktop, %esp
	ljmp $0, $realmode_start 
	.code16gcc 
	
/NO_APP
.lcomm _save.1441,40,32
	.align 32
_test_cases.1733:
	.long	1174405120
	.long	-2029936640
	.long	-1760495104
	.long	-1760454144
	.long	33554689
	.long	-2097045247
	.long	-1827603711
	.long	-1827562751
	.long	33554946
	.long	-2097044990
	.long	-1760494590
	.long	-1760453630
	.long	100664067
	.long	-2029935869
	.long	-1827603197
	.long	-1827562237
	.long	33555460
	.long	-2097044476
	.long	-1827602940
	.long	-1827561980
	.long	100664581
	.long	-2029935355
	.long	-1760493819
	.long	-1760452859
	.long	100664838
	.long	-2029935098
	.long	1443889158
	.long	-1760452602
	.long	33556231
	.long	-2097043705
	.long	303038727
	.long	-1827561209
	.long	33556488
	.long	-2097043448
	.long	303038984
	.long	-1827560952
	.long	100665609
	.long	-2029934327
	.long	370148105
	.long	-1760451831
	.long	301990922
	.long	-1828609014
	.long	303039498
	.long	-1827560438
	.long	369100043
	.long	-1761499893
	.long	370148619
	.long	-1760451317
	.long	369100300
	.long	-1761499636
	.long	370148876
	.long	-1760451060
	.long	301991693
	.long	-1828608243
	.long	303040269
	.long	-1827559667
	.long	301991950
	.long	-1828607986
	.long	303040526
	.long	-1827559410
	.long	369101071
	.long	-1761498865
	.long	370149647
	.long	-1760450289
	.long	33558544
	.long	-2097041392
	.long	370149904
	.long	-1760450032
	.long	100667665
	.long	-2029932271
	.long	303041297
	.long	-1827558639
	.long	100667922
	.long	-2029932014
	.long	370150418
	.long	-1760449518
	.long	33559315
	.long	-2097040621
	.long	303041811
	.long	-1827558125
	.long	100668436
	.long	-2029931500
	.long	303042068
	.long	-1827557868
	.long	33559829
	.long	-2097040107
	.long	370151189
	.long	-1760448747
	.long	33560086
	.long	-2097039850
	.long	303042582
	.long	-1827557354
	.long	100669207
	.long	-2029930729
	.long	370151703
	.long	-1760448233
	.long	100669464
	.long	-2029930472
	.long	370151960
	.long	-1760447976
	.long	33560857
	.long	-2097039079
	.long	303043353
	.long	-1827556583
	.long	369103898
	.long	-1761496038
	.long	370152474
	.long	-1760447462
	.long	301995291
	.long	-1828604645
	.long	303043867
	.long	-1827556069
	.long	301995548
	.long	-1828604388
	.long	303044124
	.long	-1827555812
	.long	369104669
	.long	-1761495267
	.long	370153245
	.long	-1760446691
	.long	369104926
	.long	-1761495010
	.long	370153502
	.long	-1760446434
	.long	301996319
	.long	-1828603617
	.long	303044895
	.long	-1827555041
	.long	33562656
	.long	-2029928416
	.long	303045152
	.long	-1827554784
	.long	100671777
	.long	-2097037023
	.long	370154273
	.long	-1760445663
	.long	100672034
	.long	-2097036766
	.long	303045666
	.long	-1827554270
	.long	33563427
	.long	-2029927645
	.long	370154787
	.long	-1760445149
	.long	100672548
	.long	-2097036252
	.long	370155044
	.long	-1760444892
	.long	33563941
	.long	-2029927131
	.long	303046437
	.long	-1827553499
	.long	33564198
	.long	-2029926874
	.long	303046694
	.long	-1760444378
	.long	100673319
	.long	-2097035481
	.long	370155815
	.long	-1827552985
	.long	100673576
	.long	-2097035224
	.long	370156072
	.long	-1827552728
	.long	33564969
	.long	-2029926103
	.long	303047465
	.long	-1760443607
	.long	369108010
	.long	-1828600790
	.long	370156586
	.long	-1827552214
	.long	301999403
	.long	-1761491669
	.long	303047979
	.long	-1760443093
	.long	301999660
	.long	-1761491412
	.long	303048236
	.long	-1760442836
	.long	369108781
	.long	-1828600019
	.long	370157357
	.long	-1827551443
	.long	369109038
	.long	-1828599762
	.long	370157614
	.long	-1827551186
	.long	302000431
	.long	-1761490641
	.long	303049007
	.long	-1760442065
	.long	100675632
	.long	-2097033168
	.long	303049264
	.long	-1760441808
	.long	33567025
	.long	-2029924047
	.long	370158385
	.long	-1827550415
	.long	33567282
	.long	-2029923790
	.long	303049778
	.long	-1760441294
	.long	100676403
	.long	-2097032397
	.long	370158899
	.long	-1827549901
	.long	33567796
	.long	-2029923276
	.long	370159156
	.long	-1827549644
	.long	100676917
	.long	-2097031883
	.long	303050549
	.long	-1760440523
	.long	100677174
	.long	-2097031626
	.long	370159670
	.long	-1827549130
	.long	33568567
	.long	-2029922505
	.long	303051063
	.long	-1760440009
	.long	33568824
	.long	-2029922248
	.long	303051320
	.long	-1760439752
	.long	100677945
	.long	-2097030855
	.long	370160441
	.long	-1827548359
	.long	302003258
	.long	-1761487814
	.long	303051834
	.long	-1760439238
	.long	369112379
	.long	-1828596421
	.long	370160955
	.long	-1827547845
	.long	369112636
	.long	-1828596164
	.long	370161212
	.long	-1827547588
	.long	302004029
	.long	-1761487043
	.long	303052605
	.long	-1760438467
	.long	302004286
	.long	-1761486786
	.long	303052862
	.long	-1760438210
	.long	369113407
	.long	-1828595393
	.long	370161983
	.long	-1827546817
	.long	33570880
	.long	-2097029056
	.long	370162240
	.long	-1827546560
	.long	100680001
	.long	-2029919935
	.long	303053633
	.long	-1760437439
	.long	100680258
	.long	-2029919678
	.long	370162754
	.long	-1827546046
	.long	33571651
	.long	-2097028285
	.long	303054147
	.long	-1760436925
	.long	100680772
	.long	-2029919164
	.long	303054404
	.long	-1760436668
	.long	33572165
	.long	-2097027771
	.long	370163525
	.long	-1827545275
	.long	33572422
	.long	-2097027514
	.long	303054918
	.long	-1827545018
	.long	100681543
	.long	-2029918393
	.long	370164039
	.long	-1760435897
	.long	100681800
	.long	-2029918136
	.long	370164296
	.long	-1760435640
	.long	33573193
	.long	-2097026743
	.long	303055689
	.long	-1827544247
	.long	369116234
	.long	-1761483702
	.long	370164810
	.long	-1760435126
	.long	302007627
	.long	-1828592309
	.long	303056203
	.long	-1827543733
	.long	302007884
	.long	-1828592052
	.long	303056460
	.long	-1827543476
	.long	369117005
	.long	-1761482931
	.long	370165581
	.long	-1760434355
	.long	369117262
	.long	-1761482674
	.long	370165838
	.long	-1760434098
	.long	302008655
	.long	-1828591281
	.long	303057231
	.long	-1827542705
	.long	100683856
	.long	-2029916080
	.long	303057488
	.long	-1827542448
	.long	33575249
	.long	-2097024687
	.long	370166609
	.long	-1760433327
	.long	33575506
	.long	-2097024430
	.long	303058002
	.long	-1827541934
	.long	100684627
	.long	-2029915309
	.long	370167123
	.long	-1760432813
	.long	33576020
	.long	-2097023916
	.long	370167380
	.long	-1760432556
	.long	100685141
	.long	-2029914795
	.long	303058773
	.long	-1827541163
	.long	100685398
	.long	-2029914538
	.long	370167894
	.long	-1760432042
	.long	33576791
	.long	-2097023145
	.long	303059287
	.long	-1827540649
	.long	33577048
	.long	-2097022888
	.long	303059544
	.long	-1827540392
	.long	100686169
	.long	-2029913767
	.long	370168665
	.long	-1760431271
	.long	302011482
	.long	-1828588454
	.long	303060058
	.long	-1827539878
	.long	369120603
	.long	-1761479333
	.long	370169179
	.long	-1760430757
	.long	369120860
	.long	-1761479076
	.long	370169436
	.long	-1760430500
	.long	302012253
	.long	-1828587683
	.long	303060829
	.long	-1827539107
	.long	302012510
	.long	-1828587426
	.long	303061086
	.long	-1827538850
	.long	369121631
	.long	-1761478305
	.long	370170207
	.long	-1760429729
	.long	100687968
	.long	1191247968
	.long	370170464
	.long	-1760429472
	.long	33579361
	.long	50397537
	.long	303061857
	.long	-1827538079
	.long	33579618
	.long	50397794
	.long	370170978
	.long	-1760428958
	.long	100688739
	.long	117506915
	.long	303062371
	.long	-1827537565
	.long	33580132
	.long	50398308
	.long	303062628
	.long	-1827537308
	.long	100689253
	.long	117507429
	.long	370171749
	.long	-1760428187
	.long	100689510
	.long	117507686
	.long	370172006
	.long	1460732006
	.long	33580903
	.long	50399079
	.long	303063399
	.long	319881575
	.long	33581160
	.long	50399336
	.long	303063656
	.long	319881832
	.long	100690281
	.long	117508457
	.long	370172777
	.long	386990953
	.long	302015594
	.long	318833770
	.long	303064170
	.long	319882346
	.long	369124715
	.long	385942891
	.long	370173291
	.long	386991467
	.long	369124972
	.long	385943148
	.long	370173548
	.long	386991724
	.long	302016365
	.long	318834541
	.long	303064941
	.long	319883117
	.long	302016622
	.long	318834798
	.long	303065198
	.long	319883374
	.long	369125743
	.long	385943919
	.long	370174319
	.long	386992495
	.long	33583216
	.long	50401392
	.long	370174576
	.long	386992752
	.long	100692337
	.long	117510513
	.long	303065969
	.long	319884145
	.long	100692594
	.long	117510770
	.long	370175090
	.long	386993266
	.long	33583987
	.long	50402163
	.long	303066483
	.long	319884659
	.long	100693108
	.long	117511284
	.long	303066740
	.long	319884916
	.long	33584501
	.long	50402677
	.long	370175861
	.long	386994037
	.long	33584758
	.long	50402934
	.long	303067254
	.long	319885430
	.long	100693879
	.long	117512055
	.long	370176375
	.long	386994551
	.long	100694136
	.long	117512312
	.long	370176632
	.long	386994808
	.long	33585529
	.long	50403705
	.long	303068025
	.long	319886201
	.long	369128570
	.long	385946746
	.long	370177146
	.long	386995322
	.long	302019963
	.long	318838139
	.long	303068539
	.long	319886715
	.long	302020220
	.long	318838396
	.long	303068796
	.long	319886972
	.long	369129341
	.long	385947517
	.long	370177917
	.long	386996093
	.long	369129598
	.long	385947774
	.long	370178174
	.long	386996350
	.long	302020991
	.long	318839167
	.long	303069567
	.long	319887743
	.long	-2113896320
	.long	50405504
	.long	303069824
	.long	319888000
	.long	-2046787199
	.long	117514625
	.long	370178945
	.long	386997121
	.long	-2046786942
	.long	117514882
	.long	303070338
	.long	319888514
	.long	-2113895549
	.long	50406275
	.long	370179459
	.long	386997635
	.long	-2046786428
	.long	117515396
	.long	370179716
	.long	386997892
	.long	-2113895035
	.long	50406789
	.long	303071109
	.long	319889285
	.long	-2113894778
	.long	50407046
	.long	-1844412282
	.long	319889542
	.long	-2046785657
	.long	117516167
	.long	-1777303161
	.long	386998663
	.long	-2046785400
	.long	117516424
	.long	-1777302904
	.long	386998920
	.long	-2113894007
	.long	50407817
	.long	-1844411511
	.long	319890313
	.long	-1778350966
	.long	385950858
	.long	-1777302390
	.long	386999434
	.long	-1845459573
	.long	318842251
	.long	-1844410997
	.long	319890827
	.long	-1845459316
	.long	318842508
	.long	-1844410740
	.long	319891084
	.long	-1778350195
	.long	385951629
	.long	-1777301619
	.long	387000205
	.long	-1778349938
	.long	385951886
	.long	-1777301362
	.long	387000462
	.long	-1845458545
	.long	318843279
	.long	-1844409969
	.long	319891855
	.long	-2046783344
	.long	117518480
	.long	-1844409712
	.long	319892112
	.long	-2113891951
	.long	50409873
	.long	-1777300591
	.long	387001233
	.long	-2113891694
	.long	50410130
	.long	-1844409198
	.long	319892626
	.long	-2046782573
	.long	117519251
	.long	-1777300077
	.long	387001747
	.long	-2113891180
	.long	50410644
	.long	-1777299820
	.long	387002004
	.long	-2046782059
	.long	117519765
	.long	-1844408427
	.long	319893397
	.long	-2046781802
	.long	117520022
	.long	-1777299306
	.long	387002518
	.long	-2113890409
	.long	50411415
	.long	-1844407913
	.long	319893911
	.long	-2113890152
	.long	50411672
	.long	-1844407656
	.long	319894168
	.long	-2046781031
	.long	117520793
	.long	-1777298535
	.long	387003289
	.long	318780570
	.long	318846106
	.long	319829146
	.long	319894682
	.long	385889691
	.long	385955227
	.long	386938267
	.long	387003803
	.long	385889948
	.long	385955484
	.long	386938524
	.long	387004060
	.long	318781341
	.long	318846877
	.long	319829917
	.long	319895453
	.long	318781598
	.long	318847134
	.long	319830174
	.long	319895710
	.long	385890719
	.long	385956255
	.long	386939295
	.long	387004831
	.long	50348192
	.long	50413728
	.long	386939552
	.long	387005088
	.long	117457313
	.long	117522849
	.long	319830945
	.long	319896481
	.long	117457570
	.long	117523106
	.long	386940066
	.long	387005602
	.long	50348963
	.long	50414499
	.long	319831459
	.long	319896995
	.long	117458084
	.long	117523620
	.long	319831716
	.long	319897252
	.long	50349477
	.long	50415013
	.long	386940837
	.long	387006373
	.long	50349734
	.long	50415270
	.long	319832230
	.long	319897766
	.long	117458855
	.long	117524391
	.long	386941351
	.long	387006887
	.long	117459112
	.long	117524648
	.long	386941608
	.long	387007144
	.long	50350505
	.long	50416041
	.long	319833001
	.long	319898537
	.long	385893546
	.long	385959082
	.long	386942122
	.long	387007658
	.long	318784939
	.long	318850475
	.long	319833515
	.long	319899051
	.long	318785196
	.long	318850732
	.long	319833772
	.long	319899308
	.long	385894317
	.long	385959853
	.long	386942893
	.long	387008429
	.long	385894574
	.long	385960110
	.long	386943150
	.long	387008686
	.long	318785967
	.long	318851503
	.long	319834543
	.long	319900079
	.long	117461168
	.long	117526704
	.long	319834800
	.long	319900336
	.long	50352561
	.long	50418097
	.long	386943921
	.long	387009457
	.long	50352818
	.long	50418354
	.long	319835314
	.long	319900850
	.long	117461939
	.long	117527475
	.long	386944435
	.long	387009971
	.long	50353332
	.long	50418868
	.long	386944692
	.long	387010228
	.long	117462453
	.long	117527989
	.long	319836085
	.long	319901621
	.long	117462710
	.long	117528246
	.long	386945206
	.long	387010742
	.long	50354103
	.long	50419639
	.long	319836599
	.long	319902135
	.long	50354360
	.long	50419896
	.long	319836856
	.long	319902392
	.long	117463481
	.long	117529017
	.long	386945977
	.long	387011513
	.long	318788794
	.long	318854330
	.long	319837370
	.long	319902906
	.long	385897915
	.long	385963451
	.long	386946491
	.long	387012027
	.long	385898172
	.long	385963708
	.long	386946748
	.long	387012284
	.long	318789565
	.long	318855101
	.long	319838141
	.long	319903677
	.long	318789822
	.long	318855358
	.long	319838398
	.long	319903934
	.long	385898943
	.long	385964479
	.long	386947519
	.long	387013055
	.long	117465280
	.long	117530816
	.long	386947776
	.long	387013312
	.long	50356673
	.long	50422209
	.long	319839169
	.long	319904705
	.long	50356930
	.long	50422466
	.long	386948290
	.long	387013826
	.long	117466051
	.long	117531587
	.long	319839683
	.long	319905219
	.long	50357444
	.long	50422980
	.long	319839940
	.long	319905476
	.long	117466565
	.long	117532101
	.long	386949061
	.long	387014597
	.long	117466822
	.long	117532358
	.long	386949318
	.long	387014854
	.long	50358215
	.long	50423751
	.long	319840711
	.long	319906247
	.long	50358472
	.long	50424008
	.long	319840968
	.long	319906504
	.long	117467593
	.long	117533129
	.long	386950089
	.long	387015625
	.long	318792906
	.long	318858442
	.long	319841482
	.long	319907018
	.long	385902027
	.long	385967563
	.long	386950603
	.long	387016139
	.long	385902284
	.long	385967820
	.long	386950860
	.long	387016396
	.long	318793677
	.long	318859213
	.long	319842253
	.long	319907789
	.long	318793934
	.long	318859470
	.long	319842510
	.long	319908046
	.long	385903055
	.long	385968591
	.long	386951631
	.long	387017167
	.long	50360528
	.long	50426064
	.long	386951888
	.long	387017424
	.long	117469649
	.long	117535185
	.long	319843281
	.long	319908817
	.long	117469906
	.long	117535442
	.long	386952402
	.long	387017938
	.long	50361299
	.long	50426835
	.long	319843795
	.long	319909331
	.long	117470420
	.long	117535956
	.long	319844052
	.long	319909588
	.long	50361813
	.long	50427349
	.long	386953173
	.long	387018709
	.long	50362070
	.long	50427606
	.long	319844566
	.long	319910102
	.long	117471191
	.long	117536727
	.long	386953687
	.long	387019223
	.long	117471448
	.long	117536984
	.long	386953944
	.long	387019480
	.long	50362841
	.long	50428377
	.long	319845337
	.long	319910873
	.long	385905882
	.long	385971418
	.long	386954458
	.long	387019994
	.long	318797275
	.long	318862811
	.long	319845851
	.long	319911387
	.long	318797532
	.long	318863068
	.long	319846108
	.long	319911644
	.long	385906653
	.long	385972189
	.long	386955229
	.long	387020765
	.long	385906910
	.long	385972446
	.long	386955486
	.long	387021022
	.long	318798303
	.long	318863839
	.long	319846879
	.long	319912415
	.long	-2097119008
	.long	-2097053472
	.long	319847136
	.long	319912672
	.long	-2030009887
	.long	-2029944351
	.long	386956257
	.long	387021793
	.long	-2030009630
	.long	-2029944094
	.long	319847650
	.long	319913186
	.long	-2097118237
	.long	-2097052701
	.long	386956771
	.long	387022307
	.long	-2030009116
	.long	-2029943580
	.long	386957028
	.long	387022564
	.long	-2097117723
	.long	-2097052187
	.long	319848421
	.long	319913957
	.long	-2097117466
	.long	-2097051930
	.long	-1827634970
	.long	-1827569434
	.long	-2030008345
	.long	-2029942809
	.long	-1760525849
	.long	-1760460313
	.long	-2030008088
	.long	-2029942552
	.long	-1760525592
	.long	-1760460056
	.long	-2097116695
	.long	-2097051159
	.long	-1827634199
	.long	-1827568663
	.long	-1761573654
	.long	-1761508118
	.long	-1760525078
	.long	-1760459542
	.long	-1828682261
	.long	-1828616725
	.long	-1827633685
	.long	-1827568149
	.long	-1828682004
	.long	-1828616468
	.long	-1827633428
	.long	-1827567892
	.long	-1761572883
	.long	-1761507347
	.long	-1760524307
	.long	-1760458771
	.long	-1761572626
	.long	-1761507090
	.long	-1760524050
	.long	-1760458514
	.long	-1828681233
	.long	-1828615697
	.long	-1827632657
	.long	-1827567121
	.long	-2030006032
	.long	-2029940496
	.long	-1827632400
	.long	-1827566864
	.long	-2097114639
	.long	-2097049103
	.long	-1760523279
	.long	-1760457743
	.long	-2097114382
	.long	-2097048846
	.long	-1827631886
	.long	-1827566350
	.long	-2030005261
	.long	-2029939725
	.long	-1760522765
	.long	-1760457229
	.long	-2097113868
	.long	-2097048332
	.long	-1760522508
	.long	-1760456972
	.long	-2030004747
	.long	-2029939211
	.long	-1827631115
	.long	-1827565579
	.long	-2030004490
	.long	-2029938954
	.long	-1760521994
	.long	-1760456458
	.long	-2097113097
	.long	-2097047561
	.long	-1827630601
	.long	-1827565065
	.long	-2097112840
	.long	-2097047304
	.long	-1827630344
	.long	-1827564808
	.long	-2030003719
	.long	-2029938183
	.long	-1760521223
	.long	-1760455687
	.long	-1828678406
	.long	-1828612870
	.long	-1827629830
	.long	-1827564294
	.long	-1761569285
	.long	-1761503749
	.long	-1760520709
	.long	-1760455173
	.long	-1761569028
	.long	-1761503492
	.long	-1760520452
	.long	-1760454916
	.long	-1828677635
	.long	-1828612099
	.long	-1827629059
	.long	-1827563523
	.long	-1828677378
	.long	-1828611842
	.long	-1827628802
	.long	-1827563266
	.long	-1761568257
	.long	-1761502721
	.long	-1760519681
	.long	-1760454145
	.align 4
_array.1798:
	.long	305419896
	.long	0
	.long	0
	.long	0
	.long	-2023406815
	.ident	"GCC: (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 8.1.0"
	.def	_retf;	.scl	2;	.type	32;	.endef
