;*
;* Syscall handlers
;*

	;* must be included by syscall0.asm or syscall1.asm

        .text

        .global syscall
syscall:
        .asg r14, nr

	LDI R31, SYSCALL_VALUE
	HALT
        JMP R3.w0

        .global syscall1
syscall1:
        .asg r14, nr
        .asg r15, arg0

	LDI R31, SYSCALL_VALUE
	HALT
        JMP R3.w0

        .global syscall2
syscall2:
        .asg r14, nr
        .asg r15, arg0
        .asg r16, arg1

	LDI R31, SYSCALL_VALUE
	HALT
        JMP R3.w0

        .global syscall3
syscall3:
        .asg r14, nr
        .asg r15, arg0
        .asg r16, arg1
        .asg r17, arg2

	LDI R31, SYSCALL_VALUE
	HALT
        JMP R3.w0
