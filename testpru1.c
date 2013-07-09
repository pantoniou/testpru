/*
 * testpru
 *
 */

#define PRU1

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#include "debug.h"
#include "pru_defs.h"
#include "prucomm.h"

#include "syscall.h"

extern void delay_cycles(u32 delay);
extern void delay_cycles_accurate(u32 delay);
extern void delay_cycles_accurate2(u32 delay);
extern void pwm_loop_asm(u32 hi, u32 lo);
extern void pwm_loop_asm2(u32 hi, u32 lo);
extern void fast_pwm(void);

void pwm_loop(u32 hi, u32 lo)
{
	while (!pru_signal()) {
		__R30 |=  (1 << 13);
		delay_cycles(hi);
		__R30 &= ~(1 << 13);
		delay_cycles(lo);
	}
}

void pwm_loop2(u32 hi, u32 lo)
{
	while (!pru_signal()) {
		__R30 |=  (1 << 13);
		delay_cycles_accurate2(hi);
		__R30 &= ~(1 << 13);
		delay_cycles_accurate2(lo);
	}
}

#define T1 asm (" .global T1\nT1:");
#define T2 asm (" .global T2\nT2:");

#if 0
int main(int argc, char *argv[])
{
	u32 hi, lo;
	struct pwm_config *pwmc = (void *)DPRAM_SHARED;

	/* enable OCP master port */
	PRUCFG_SYSCFG &= ~SYSCFG_STANDBY_INIT;

#if 0
	sc_puts("PRU1; waiting");
	for (;;) {
		while (!pru_signal())
			;

		if (PINTC_SRSR0 & (1 << SYSEV_PRU0_TO_PRU1)) {
			PINTC_SICR = SYSEV_PRU0_TO_PRU1;
			break;
		}
	}
#endif

	sc_puts("PRU1; go");

	hi = PRU_us(100);
	lo = PRU_us(100);

	for (;;) {
#if 0
		// pwm_loop(hi, lo);
		T1
		PCTRL_CONTROL &= ~CONTROL_COUNTER_ENABLE;
		PCTRL_CONTROL |= CONTROL_COUNTER_ENABLE;
		T2
		// pwm_loop_asm(hi, lo);
		// pwm_loop2(hi, lo);
#endif
		pwm_loop_asm2(hi, lo);

		/* signalled interrupt from either PRU0 or host */
		if (!pru_signal())
			continue;

		if (PINTC_SRSR0 & (1 << SYSEV_OTHER_PRU_TO_THIS_PRU)) {
			PINTC_SICR = SYSEV_OTHER_PRU_TO_THIS_PRU;

			hi = pwmc->hi_cycles;
			lo = pwmc->lo_cycles;

			sc_printf("hi=%d lo=%d", hi, lo);
		}
		if (PINTC_SRSR0 & (1 << SYSEV_ARM_TO_THIS_PRU)) {
			PINTC_SICR = SYSEV_ARM_TO_THIS_PRU;
			/* sc_puts("PRU1 signalled from ARM"); */
		}
	}
}

#else

struct pwm_multi_config cfg;

static void pwm_setup(void)
{
	u8 i;

	for (i = 0; i < MAX_PWMS; i++)
		cfg.hilo[i][0] = cfg.hilo[i][1] = PRU_us(200);

	cfg.enmask = BIT(13) | BIT(12);
	cfg.hilo[12][0] = PRU_us(333);
	cfg.hilo[12][1] = PRU_us(333);
	cfg.hilo[13][0] = PRU_us(100);
	cfg.hilo[13][1] = PRU_us(100);
}

#define USE_PWM_LOOP
#undef USE_PWM_MACRO

struct cxt {
	u32 cnt;
	u32 next;
	u32 enmask;
	u32 stmask;
	u32 setmsk;
	u32 clrmsk;
	u32 deltamin;
	u32 *next_hi_lo;
};

static inline u32 read_PIEP_COUNT(void)
{
	return PIEP_COUNT;

}

static void handle_pwm_cmd(struct cxt *cxt)
{
	u8 i;
	u32 msk, setmsk, clrmsk;
	u32 enmask, stmask, cnt, deltamin, next;
	struct pwm_cmd *pwmc;
	u32 *nextp;
	u32 *next_hi_lop;
	const u32 *hilop;

	cnt = cxt->cnt;
	next = cxt->next;
	enmask = cxt->enmask;
	stmask = cxt->stmask;
	setmsk = cxt->setmsk;
	clrmsk = cxt->clrmsk;
	deltamin = cxt->deltamin;
	next_hi_lop = cxt->next_hi_lo;

	// sc_printf("cnt=%x next=%x deltamin=%x", cnt, next, deltamin);

	pwmc = PWM_CMD;
	if (pwmc->cmd == PWM_CMD_CONFIG) {

		enmask = pwmc->u.cfg.enmask;
		stmask = 0;		/* starting all low */

		clrmsk = 0;
		for (i = 0, msk = 1, nextp = next_hi_lop, hilop = &pwmc->u.cfg.hilo[0][0];
				i < MAX_PWMS;
				i++, msk <<= 1, nextp += 3, hilop += 2) {
			if ((enmask & msk) == 0) {
				nextp[1] = PRU_us(100);	/* default */
				nextp[2] = PRU_us(100);
				continue;
			}
			nextp[0] = cnt;		/* next */
			nextp[1] = hilop[0];	/* hi */
			nextp[2] = hilop[1];	/* lo */
		}

		clrmsk = enmask;
		setmsk = 0;
		/* guaranteed to be immediate */
		deltamin = 0;
	} else if (pwmc->cmd == PWM_CMD_ENABLE) {
		msk = BIT(pwmc->pwm_nr);
		if (pwmc->pwm_nr < MAX_PWMS && (PWM_EN_MASK & msk) && (enmask & msk) == 0) {
			enmask |= msk;
			if (pwmc->pwm_nr < 16)
				__R30 |= msk;
			nextp = &next_hi_lop[pwmc->pwm_nr * 3];

			nextp[0] = cnt;	/* since we start high, wait this amount */

			/* first enable */
			if (enmask == msk)
				cnt = read_PIEP_COUNT();
			deltamin = 0;
			next = cnt;
			// sc_printf("e %d %d %d - next=%x cnt=%x", pwmc->pwm_nr, nextp[1], nextp[2], next, cnt);
		}
	} else if (pwmc->cmd == PWM_CMD_DISABLE) {
		msk = BIT(pwmc->pwm_nr);
		if (pwmc->pwm_nr < MAX_PWMS && (PWM_EN_MASK & msk) && (enmask & msk) != 0) {
			enmask &= ~msk;
			/* clear bit */
			if (pwmc->pwm_nr < 16)
				__R30 &= ~msk;

			// sc_printf("d %d", pwmc->pwm_nr);
		}
	} else if (pwmc->cmd == PWM_CMD_MODIFY) {
		msk = BIT(pwmc->pwm_nr);
		if (pwmc->pwm_nr < MAX_PWMS && (PWM_EN_MASK & msk)) {
			nextp = &next_hi_lop[pwmc->pwm_nr * 3];

			/* only allow sane values */
			if (pwmc->u.hilo[0] >= MIN_PWM_PULSE &&
				pwmc->u.hilo[1] >= MIN_PWM_PULSE) {
				nextp[1] = pwmc->u.hilo[0];
				nextp[2] = pwmc->u.hilo[1];
			}

			// sc_printf("m %d %d %d", pwmc->pwm_nr, nextp[1], nextp[2]);
		}
	} else if (pwmc->cmd == PWM_CMD_SET || pwmc->cmd == PWM_CMD_CLR) {
		msk = BIT(pwmc->pwm_nr);
		if (pwmc->pwm_nr < MAX_PWMS) {
			/* set bit (if the pwm is running it will be temporary) */
			if (pwmc->pwm_nr < 16) {
				if (pwmc->cmd == PWM_CMD_SET)
					__R30 |= msk;
				else
					__R30 &= ~msk;
			}
		}

	}
	pwmc->magic = PWM_REPLY_MAGIC;
	SIGNAL_EVENT(SYSEV_THIS_PRU_TO_OTHER_PRU);

	// sc_printf("cnt=%x next=%x deltamin=%x", cnt, next, deltamin);

	cxt->cnt = cnt;
	cxt->next = next;
	cxt->enmask = enmask;
	cxt->stmask = stmask;
	cxt->setmsk = setmsk;
	cxt->clrmsk = clrmsk;
	cxt->deltamin = deltamin;
}

static void handle_pru_signal(struct cxt *cxt)
{
	if (PINTC_SRSR0 & (1 << SYSEV_OTHER_PRU_TO_THIS_PRU)) {
		PINTC_SICR = SYSEV_OTHER_PRU_TO_THIS_PRU;

		// sc_printf("PRU signal");

		if (PWM_CMD->magic == PWM_CMD_MAGIC) 
			handle_pwm_cmd(cxt);

	}
	if (PINTC_SRSR0 & (1 << SYSEV_ARM_TO_THIS_PRU)) {
		PINTC_SICR = SYSEV_ARM_TO_THIS_PRU;
		// sc_puts("ARM signal");
	}
}

int main(int argc, char *argv[])
{
	u8 i;
	u32 cnt, next;
	u32 msk, setmsk, clrmsk;
	u32 delta, deltamin, tnext, hi, lo;
	u32 *nextp;
	const u32 *hilop;
	u32 enmask;	/* enable mask */
	u32 stmask;	/* state mask */
	static u32 next_hi_lo[MAX_PWMS][3];
	static struct cxt cxt;

	/* enable OCP master port */
	PRUCFG_SYSCFG &= ~SYSCFG_STANDBY_INIT;

	PRUCFG_SYSCFG = (PRUCFG_SYSCFG &
			~(SYSCFG_IDLE_MODE_M | SYSCFG_STANDBY_MODE_M)) | 
			SYSCFG_IDLE_MODE_NO | SYSCFG_STANDBY_MODE_NO;

	/* our PRU wins arbitration */
#if defined(PRU0)
	PRUCFG_SPP &= ~SPP_PRU1_PAD_HP_EN;
#elif defined(PRU1)
	PRUCFG_SPP |=  SPP_PRU1_PAD_HP_EN;
#endif

	pwm_setup();

	sc_puts("PRU1; go");

	/* configure timer */
	PIEP_GLOBAL_CFG = GLOBAL_CFG_CNT_ENABLE	|
			  GLOBAL_CFG_DEFAULT_INC(1) |
			  GLOBAL_CFG_CMP_INC(1);
	PIEP_CMP_STATUS = CMD_STATUS_CMP_HIT(1); /* clear the interrupt */
	PIEP_CMP_CFG |= CMP_CFG_CMP_EN(1);

	/* copy from cfg to cxt */

	/* initialize */
	cnt = read_PIEP_COUNT();

	enmask = cfg.enmask;
	stmask = 0;		/* starting all low */

	clrmsk = 0;
	for (i = 0, msk = 1, nextp = &next_hi_lo[0][0], hilop = &cfg.hilo[0][0];
			i < MAX_PWMS;
			i++, msk <<= 1, nextp += 3, hilop += 2) {
		if ((enmask & msk) == 0) {
			nextp[1] = PRU_us(100);	/* default */
			nextp[2] = PRU_us(100);
			continue;
		}
		nextp[0] = cnt;		/* next */
		nextp[1] = hilop[0];	/* hi */
		nextp[2] = hilop[1];	/* lo */
	}

	clrmsk = enmask;
	setmsk = 0;
	/* guaranteed to be immediate */
	deltamin = 0;
	next = cnt + deltamin;

	for (;;) {

		/* signalled interrupt from either PRU0 or host */
		if (pru_signal()) {
			cxt.cnt = cnt;
			cxt.next = next;
			cxt.enmask = enmask;
			cxt.stmask = stmask;
			cxt.setmsk = setmsk;
			cxt.clrmsk = clrmsk;
			cxt.deltamin = deltamin;
			cxt.next_hi_lo = &next_hi_lo[0][0];

			handle_pru_signal(&cxt);

			cnt = cxt.cnt;
			next = cxt.next;
			enmask = cxt.enmask;
			stmask = cxt.stmask;
			setmsk = cxt.setmsk;
			clrmsk = cxt.clrmsk;
			deltamin = cxt.deltamin;

			// sc_printf("next=%x", next);
		}

		/* if nothing is enabled just skip it all */
		if (enmask == 0)
			continue;

		setmsk = 0;
		clrmsk = (u32)-1;
		deltamin = PRU_ms(100); /* (1U << 31) - 1; */
		next = cnt + deltamin;

#define SINGLE_PWM(_i) \
	do { \
		if (enmask & (1U << (_i))) { \
			nextp = &next_hi_lo[(_i)][0]; \
			tnext = nextp[0]; \
			hi = nextp[1]; \
			lo = nextp[2]; \
			/* avoid signed arithmetic */ \
			while (((delta = (tnext - cnt)) & (1U << 31)) != 0) { \
				/* toggle the state */ \
				if (stmask & (1U << (_i))) { \
					stmask &= ~(1U << (_i)); \
					clrmsk &= ~(1U << (_i)); \
					tnext += hi; \
				} else { \
					stmask |= (1U << (_i)); \
					setmsk |= (1U << (_i)); \
					tnext += lo; \
				} \
			} \
			if (delta <= deltamin) { \
				deltamin = delta; \
				next = tnext; \
			} \
			nextp[0] = tnext; \
		} \
	} while (0)

#ifdef USE_PWM_LOOP
		for (i = 0, msk = 1, nextp = &next_hi_lo[0][0]; i < MAX_PWMS; i++, msk <<= 1, nextp += 3) {

			if ((enmask & msk) == 0)
				continue;

			tnext = nextp[0];
			hi = nextp[1];
			lo = nextp[2];

			/* avoid signed arithmetic */
			while (((delta = (tnext - cnt)) & (1U << 31)) != 0) {
				/* toggle the state */
				if (stmask & msk) {
					stmask &= ~msk;
					clrmsk &= ~msk;
					tnext += hi;
				} else {
					stmask |= msk;
					setmsk |= msk;
					tnext += lo;
				}
			}
			if (delta <= deltamin) {
				deltamin = delta;
				next = tnext;
			}
			nextp[0] = tnext;
		}
#endif

#ifdef USE_PWM_MACRO

#if MAX_PWMS > 0 && (PWM_EN_MASK & BIT(0))
		SINGLE_PWM(0);
#endif
#if MAX_PWMS > 1 && (PWM_EN_MASK & BIT(1))
		SINGLE_PWM(1);
#endif
#if MAX_PWMS > 2 && (PWM_EN_MASK & BIT(2))
		SINGLE_PWM(2);
#endif
#if MAX_PWMS > 3 && (PWM_EN_MASK & BIT(3))
		SINGLE_PWM(3);
#endif
#if MAX_PWMS > 4 && (PWM_EN_MASK & BIT(4))
		SINGLE_PWM(4);
#endif
#if MAX_PWMS > 5 && (PWM_EN_MASK & BIT(5))
		SINGLE_PWM(5);
#endif
#if MAX_PWMS > 6 && (PWM_EN_MASK & BIT(6))
		SINGLE_PWM(6);
#endif
#if MAX_PWMS > 7 && (PWM_EN_MASK & BIT(7))
		SINGLE_PWM(7);
#endif
#if MAX_PWMS > 8 && (PWM_EN_MASK & BIT(8))
		SINGLE_PWM(8);
#endif
#if MAX_PWMS > 9 && (PWM_EN_MASK & BIT(9))
		SINGLE_PWM(9);
#endif
#if MAX_PWMS > 10 && (PWM_EN_MASK & BIT(10))
		SINGLE_PWM(10);
#endif
#if MAX_PWMS > 11 && (PWM_EN_MASK & BIT(11))
		SINGLE_PWM(11);
#endif
#if MAX_PWMS > 12 && (PWM_EN_MASK & BIT(12))
		SINGLE_PWM(12);
#endif
#if MAX_PWMS > 13 && (PWM_EN_MASK & BIT(13))
		SINGLE_PWM(13);
#endif
#if MAX_PWMS > 14 && (PWM_EN_MASK & BIT(14))
		SINGLE_PWM(14);
#endif
#if MAX_PWMS > 15 && (PWM_EN_MASK & BIT(15))
		SINGLE_PWM(15);
#endif
#endif

		__R30 = (__R30 & (clrmsk & 0xffff)) | (setmsk & 0xffff);

#if 1
		cnt = read_PIEP_COUNT();
		if (((next - cnt) & (1U << 31)) == 0 && delta > PRU_ms(1)) {
			sc_printf("bad next=%x cnt=%x", next, cnt);
		}
#endif
		/* loop while nothing changes */
		do {
			cnt = read_PIEP_COUNT();
			if (pru_signal())
				break;
		} while (((next - cnt) & (1U << 31)) == 0);

	}
}

#endif
