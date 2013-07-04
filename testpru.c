/*
 * testpru
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#include "debug.h"
#include "pru_defs.h"

#include "linux_types.h"
#include "remoteproc.h"
#include "syscall.h"

#include "pru_vring.h"

#include "pt.h"

struct pru_vring tx_ring;
struct pru_vring rx_ring;

#define DELAY_CYCLES(x) \
	do { \
		unsigned int t = (x); \
		while (--t) { \
			__asm(" "); \
		} \
	} while(0)

/* event definitions */
#define SYSEV_ARM_TO_PRU0	21
#define SYSEV_ARM_TO_PRU1	22
#define SYSEV_PRU0_TO_ARM	19
#define SYSEV_PRU0_TO_PRU1	17
#define SYSEV_PRU1_TO_ARM	20
#define SYSEV_PRU1_TO_PRU0	19

void *resource_get_type(struct resource_table *res, int type, int idx)
{
	struct fw_rsc_hdr *rsc_hdr;
	int i, j;

	j = 0;
	for (i = 0; i < res->num; i++) {
		rsc_hdr = (void *)((char *)res + res->offset[i]);
		if (type >= 0 && rsc_hdr->type != type)
			continue;
		if (j == idx)
			return &rsc_hdr->data[0];
	}

	return NULL;
}

static void handle_event_startup(void)
{
	int i;
	struct resource_table *res;
	struct fw_rsc_vdev *rsc_vdev;
	struct fw_rsc_vdev_vring *rsc_vring;

	res = sc_get_cfg_resource_table();
	BUG_ON(res == NULL);

	/* get first VDEV resource */
	rsc_vdev = resource_get_type(res, RSC_VDEV, 0);
	BUG_ON(rsc_vdev == NULL);

	BUG_ON(rsc_vdev->num_of_vrings < 2);
	for (i = 0, rsc_vring = rsc_vdev->vring; i < 2; i++, rsc_vring++) {
		sc_printf("VR#%d: da=0x%x align=0x%x num=0x%x notifyid=0x%x",
				i, rsc_vring->da, rsc_vring->align,
				rsc_vring->num, rsc_vring->notifyid);
	}

	pru_vring_init(&tx_ring, "tx", &rsc_vdev->vring[0]);
	pru_vring_init(&rx_ring, "rx", &rsc_vdev->vring[1]);
}

#define event_condition() \
	((__R31 & (1 << 30)) != 0)

#define RX_SIZE	32
#define RX_SIZE_MASK (RX_SIZE - 1)
#define TX_SIZE	64
#define TX_SIZE_MASK (TX_SIZE - 1)

static u8 rx_in;
static u8 rx_out;
static u8 rx_cnt;
static char rx_buf[RX_SIZE];

static u8 tx_in;
static u8 tx_out;
static u8 tx_cnt;
static char tx_buf[TX_SIZE];

/* might yield, make sure _ch is static */
#define RX_IN(_ch) \
	do { \
		/* yield while there's no space */ \
		PT_YIELD_UNTIL(pt, rx_cnt < RX_SIZE); \
		rx_buf[rx_in++ & RX_SIZE_MASK] = (_ch); \
		rx_cnt++; \
	} while (0)

/* waits until input (bah - no gcc makes me a sad panda) */
#define RX_OUT(_ch) \
	do { \
		PT_WAIT_UNTIL(pt, rx_cnt > 0); \
	 	rx_cnt--; \
		(_ch) = rx_buf[rx_out++ & RX_SIZE_MASK]; \
	} while (0)

/* might yield, make sure _ch is static */
#define TX_IN(_ch) \
	do { \
		/* yield while there's no space */ \
		PT_YIELD_UNTIL(pt, tx_cnt < TX_SIZE); \
		tx_buf[tx_in++ & TX_SIZE_MASK] = (_ch); \
		tx_cnt++; \
	} while (0)

/* waits until input (bah - no gcc makes me a sad panda) */
#define TX_OUT(_ch) \
	do { \
		PT_WAIT_UNTIL(pt, tx_cnt > 0); \
	 	tx_cnt--; \
		(_ch) = tx_buf[tx_out++ & TX_SIZE_MASK]; \
	} while (0)

/* non blocking version */
#define TX_OUT_NB(_ch) \
	do { \
	 	tx_cnt--; \
		(_ch) = tx_buf[tx_out++ & TX_SIZE_MASK]; \
	} while (0)

static int event_thread(struct pt *pt)
{
	static struct pru_vring_elem pvre;
	static u16 idx, count;
	static u32 rx_len, len;
	struct vring_desc *vrd;
	static char *ptr;

	PT_BEGIN(pt);

	for (;;) {
		/* wait until we get the indication */
		PT_WAIT_UNTIL(pt, event_condition());

		if (PINTC_SRSR0 & (1 << SYSEV_ARM_TO_PRU0)) {
			PINTC_SICR = SYSEV_ARM_TO_PRU0;

			while (pru_vring_pop(&rx_ring, &pvre)) {

				/* process the incoming buffers (??? not used) */
				if ((count = pvre.in_num) > 0) {
					idx = pvre.in_idx;
					while (count-- > 0) {
						vrd = pru_vring_desc(&rx_ring, idx);
						ptr = pa_to_da(vrd->addr);
						idx++;
					}
				}

				rx_len = 0;

				/* process the outgoing buffers (this is our RX) */
				if (pvre.out_num > 0) {

					idx = pvre.out_idx;
					count = pvre.out_num;
					while (count-- > 0) {
						vrd = pru_vring_desc(&rx_ring, idx);
						ptr = pa_to_da(vrd->addr);
						len = vrd->len;

						/* put it in the rx buffer (can yield) */
						while (len-- > 0)
							RX_IN(*ptr++);

						rx_len += vrd->len;

						idx++;
					}
				}

				pru_vring_push(&rx_ring, &pvre, rx_len);
				/* VRING0 PRU0 -> ARM */
				SIGNAL_EVENT(25);
			}
		}

		if (PINTC_SRSR0 & (1 << SYSEV_PRU1_TO_PRU0)) {
			PINTC_SICR = SYSEV_PRU1_TO_PRU0;
		}
	}

	/* get around warning */
	PT_YIELD(pt);

	PT_END(pt);
}

static void handle_led_startup(void)
{
	/* IEP timer is incrementing by one */
	PIEP_GLOBAL_CFG = GLOBAL_CFG_CNT_ENABLE	|
			  GLOBAL_CFG_DEFAULT_INC(1) |
			  GLOBAL_CFG_CMP_INC(1);

	/* delay with the timer */
	PIEP_CMP_CMP0 = PIEP_COUNT + 10;
	PIEP_CMP_STATUS = CMD_STATUS_CMP_HIT(0); /* clear the interrupt */
	PIEP_CMP_CFG |= CMP_CFG_CMP_EN(0);

}

static u32 led_cycles = PRU_us(100);

static void handle_led_event(void)
{
	/* toggle led */
	__R30 ^= (1 << 5);

	PIEP_CMP_STATUS = CMD_STATUS_CMP_HIT(0);
	PIEP_CMP_CMP0 += led_cycles;
}

#define led_condition() \
	((PIEP_CMP_STATUS & CMD_STATUS_CMP_HIT(0)) != 0)

static int led_thread(struct pt *pt)
{
	PT_BEGIN(pt);

	for (;;) {
		/* wait until we get the indication */
		PT_WAIT_UNTIL(pt, led_condition());

		handle_led_event();
	}

	PT_YIELD(pt);

	PT_END(pt);
}

#define v_puts(_p, _ch, _str) \
	do { \
		_p = (_str); \
		while ((_ch = *_p++) != '\0') \
			TX_IN(_ch); \
	} while(0)

#define v_putc(_ch) TX_IN(_ch)
#define v_getc(_ch) RX_OUT(_ch)

static int prompt_thread(struct pt *pt)
{
	static char ch, ch1;
	static char *p;
	static char *pp;

	PT_BEGIN(pt);

	for (;;) {
		v_puts(p, ch, "PRU> ");
		v_getc(ch1);
		v_putc(ch1);

		if (ch1 == '?') {
			pp = "\r\nHelp\r\n";
		} else if (ch1 == '1') {
			led_cycles = PRU_ms(500);
			pp = "\r\n500ms\r\n";
		} else if (ch1 == '2') {
			led_cycles = PRU_ms(100);
			pp = "\r\n100ms\r\n";
		} else if (ch1 == '3') {
			led_cycles = PRU_ms(1);
			pp = "\r\n1ms\r\n";
		} else if (ch1 == '4') {
			led_cycles = PRU_us(500);
			pp = "\r\n500us\r\n";
		} else if (ch1 == '5') {
			led_cycles = PRU_us(250);
			pp = "\r\n500us\r\n";
		} else if (ch1 == '6') {
			led_cycles = PRU_us(100);
			pp = "\r\n100us\r\n";
		} else if (ch1 == '7') {
			led_cycles = PRU_us(10);
			pp = "\r\n10us\r\n";
		} else if (ch1 == '8') {
			led_cycles = PRU_us(1);
			pp = "\r\n1us\r\n";
		} else if (ch1 == '9') {
			led_cycles = PRU_ns(500);
			pp = "\r\n500ns\r\n";
		} else {
			pp = "\r\n*BAD*\r\n";
		}
		v_puts(p, ch, pp);
	}

	PT_YIELD(pt);

	PT_END(pt);
}

static int tx_thread(struct pt *pt)
{
	struct vring_desc *vrd = NULL;
	u32 chunk, i;
	char *ptr, ch;

	PT_BEGIN(pt);

	for (;;) {

		/* wait until we get the indication (and there's a buffer) */
		PT_WAIT_UNTIL(pt, tx_cnt && pru_vring_buf_is_avail(&tx_ring));

		vrd = pru_vring_get_next_avail_desc(&tx_ring);

		/* we don't support VRING_DESC_F_INDIRECT */
		BUG_ON(vrd->flags & VRING_DESC_F_INDIRECT);

		chunk = tx_cnt;
		if (chunk > vrd->len)
			chunk = vrd->len;

		ptr = pa_to_da(vrd->addr);

		for (i = 0; i < chunk; i++) {
			ch = tx_buf[tx_out++ & TX_SIZE_MASK];
			*ptr++ = ch;
		}
		tx_cnt -= chunk;
		vrd->len = chunk;
		vrd->flags &= ~VRING_DESC_F_NEXT;	/* last */

		pru_vring_push_one(&tx_ring, chunk);

		/* VRING0 PRU0 -> ARM */
		SIGNAL_EVENT(25);
	}

	PT_YIELD(pt);

	PT_END(pt);
}

static struct pt pt_event;
static struct pt pt_led;
static struct pt pt_prompt;
static struct pt pt_tx;

int main(int argc, char *argv[])
{
	/* enable OCP master port */
	PRUCFG_SYSCFG &= ~SYSCFG_STANDBY_INIT;

	sc_printf("Using protothreads");

	PT_INIT(&pt_event);
	PT_INIT(&pt_led);
	PT_INIT(&pt_prompt);
	PT_INIT(&pt_tx);

	handle_event_startup();
	rx_in = rx_out = rx_cnt = 0;
	tx_in = tx_out = tx_cnt = 0;

	handle_led_startup();

	for (;;) {
		event_thread(&pt_event);
		tx_thread(&pt_tx);
		led_thread(&pt_led);
		prompt_thread(&pt_prompt);
	}
}
