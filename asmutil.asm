;*
;* Various assembly utils
;*

        .text

	;* Save on call: R0-R1, R14-R29
	;* Save on entry: R3.w2-R13


        .global delay_cycles

delay_cycles:
        .asg r14, nr

	SUB R14, R14, 5		;* function entry + overhead
	LSR R14, R14, 1		;* loop is by two
$L1:	SUB R14, R14, 1
	QBNE $L1, R14, 0
        JMP R3.w0

	.global pwm_loop_asm

	;* argument hi, lo

pwm_loop_asm:

$L4:	LSR R0, R14, 1		;* divide by two
$L2:	SUB R0, R0, 1		;* loop high
	QBNE $L2, R0, 0

	SET R30, R30, 13	;* set high

	LSR R1, R15, 1		;* divide by two
$L3:	SUB R1, R1, 1		;* loop low
	QBNE $L3, R1, 0

	CLR R30, R30, 13	;* set low

	QBBC $L4, R31, 31	;* test PRU1 event, if not loop
        JMP R3.w0		;*

	.global delay_cycles_accurate
delay_cycles_accurate:
	SUB R14, R14, 3
	QBEQ $L10, R14.w2, 0
	LDI R0, 0		;* R0 = 0
$L12:	SUB R0.w0, R0.w0, 1	;* 0 - 1 = 0xffffffff
$L11:	LOOP $L11, R0.w0	;* loop 65535 times + 1 65536
	SUB R14.w2, R14.w2, 1
	QBNE $L12, R14.w2, 0
$L10:	LOOP $L10, R14.w0
	JMP R3.w0

	.global delay_cycles_accurate2
delay_cycles_accurate2:
	SUB R14, R14, 5		;* subtract overhead
	LSR R0, R14, 1		;* divide by two
	QBBC $L10, R14, 0	;* test low bit
	NOP			;* add one cycle for odd
$L20:	SUB R0, R0, 1		;* loop high
	QBNE $L2, R0, 0
	JMP R3.w0

	.global pwm_loop_asm2
pwm_loop_asm2:
	SUB R16, R15, 7

	SUB R17, R15, 1
	SUB R18, R14, 1

$L34:	LSR R0, R16, 1		;* divide by two
$L32:	SUB R0, R0, 1		;* loop high
	QBNE $L32, R0, 0

	SET R30, R30, 13	;* set high

$L38:	LSR R0, R18, 1		;* divide by two
$L33:	SUB R0, R0, 1		;* loop low
	QBNE $L33, R0, 0

	CLR R30, R30, 13	;* set low

	LSR R0, R17, 1		;* divide by two
$L36:	SUB R0, R0, 1		;* loop high
	QBNE $L36, R0, 0

	SET R30, R30, 13	;* set high

	QBBC $L38, R31, 31	;* test PRU1 event, if not loop

$L37:	JMP R3.w0		;*

PWM	.macro	nr,enmsk,delta,deltamin,cnt,stmsk,clrmsk,next,cxtstmsk,hi,lo
	QBBC	m1?,enmsk,nr			;* if (cfg.enmask & (1U << (_i))) {
	SUB	delta, next, cnt		;* delta = next - cnt
	QBBC	m1?, delta, 31			;* if ((delta & (1U << 31)) != 0) {
	QBBC	m2?, cxtstmsk, nr		;* if (cxt.stmask & (1U << (_i))) {
	CLR	cxtstmsk, cxtstmsk, nr		;* cxt.stmask &= ~(1U << (_i));
	CLR	clrmsk, clrmsk, nr		;* clrmsk &= ~(1U << (_i));
	ADD	next, next, hi			;* cxt.next[(_i)] += hi
	JMP	m3?
m2?:	SET	cxtstmsk, cxtstmsk, nr		;* cxt.stmask |= (1U << (_i));
	CLR	stmsk, stmsk, nr		;* stmsk |= (1U << (_i));
	ADD	next, next, lo			;* cxt.next[(_i)] += lo
m3?	ADD	delta, next, cnt		;* delta = cxt.next[(_i)] - cnt;
	MIN	deltamin, delta, deltamin	;* deltamin = min(delta, deltamin)
m1?:
	.endm

	;* update_gpo R14=clrmsk, R15=setmsk
	.global update_gpo

update_gpo:
	;* first update the local R30 (with the low order bits)
	AND R30.w0, R30.w0, R14.w0
	OR R30.w0, R30.w0, R15.w0
	JMP R3.w0		;*
