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

static struct pt pt_event;
static struct pt pt_led;
static struct pt pt_prompt;
static struct pt pt_tx;

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

static u32 hi_cycles = PRU_200MHz_us(100);
static u32 lo_cycles = PRU_200MHz_us(100);

static void led_kickstart(void)
{
	PIEP_CMP_STATUS = CMD_STATUS_CMP_HIT(0);
	PIEP_CMP_CMP0 += hi_cycles;
	__R30 |= (1 << 5);
}

static void update_hi_cycles(u32 new_cycles)
{
	hi_cycles = new_cycles;
}

static void update_lo_cycles(u32 new_cycles)
{
	lo_cycles = new_cycles;
}

#define USE_SW_COMPARE
#undef USE_HW_COMPARE

static int led_thread(struct pt *pt)
{
#ifdef USE_SW_COMPARE
	static u32 next;
#endif

	PT_BEGIN(pt);

	/* IEP timer is incrementing by one */
	PIEP_GLOBAL_CFG = GLOBAL_CFG_CNT_ENABLE	|
			  GLOBAL_CFG_DEFAULT_INC(1) |
			  GLOBAL_CFG_CMP_INC(1);

	/* lo */
	__R30 &= ~(1 << 5);

	/* delay with the timer */
	PIEP_CMP_CMP0 = PIEP_COUNT + lo_cycles;
	PIEP_CMP_STATUS = CMD_STATUS_CMP_HIT(0); /* clear the interrupt */
	PIEP_CMP_CFG |= CMP_CFG_CMP_EN(0);

#ifdef USE_HW_COMPARE
	for (;;) {
		/* wait until we get the indication */
		PT_WAIT_UNTIL(pt,
				(PIEP_CMP_STATUS & CMD_STATUS_CMP_HIT(0)) != 0);
		__R30 |= (1 << 5);
		PIEP_CMP_STATUS = CMD_STATUS_CMP_HIT(0);
		PIEP_CMP_CMP0 += hi_cycles;

		/* wait until we get the indication */
		PT_WAIT_UNTIL(pt,
				(PIEP_CMP_STATUS & CMD_STATUS_CMP_HIT(0)) != 0);
		__R30 &= ~(1 << 5);
		PIEP_CMP_STATUS = CMD_STATUS_CMP_HIT(0);
		PIEP_CMP_CMP0 += lo_cycles;

	}
#endif

#ifdef USE_SW_COMPARE
	next = PIEP_COUNT + lo_cycles;
	for (;;) {
		/* wait until we get the indication */
		PT_WAIT_UNTIL(pt, (int)(next - PIEP_COUNT) <= 0);
		__R30 |= (1 << 5);
		next += hi_cycles;

		/* wait until we get the indication */
		PT_WAIT_UNTIL(pt, (int)(next - PIEP_COUNT) <= 0);
		__R30 &= ~(1 << 5);
		next += lo_cycles;

	}
#endif

	PT_YIELD(pt);

	PT_END(pt);
}

/* context for console I/O */
#define CONSOLE_LINE_MAX	80

struct console_cxt {
	struct pt pt;
	char *buf;
	int size;
	int max_size;
	u8 flags;
#define S_WRITEMODE	0x01
#define S_CRLF		0x02
#define S_LINEMODE	0x04
#define S_ECHO		0x08
#define S_READLINE	0x10
};
/* pt must always! me the first member */
#define to_console_cxt(_pt)	((struct console_cxt *)(void *)(_pt))

static int console_thread(struct pt *pt)
{
	struct console_cxt *c = to_console_cxt(pt);	/* always called */
	static char ch;

	PT_BEGIN(pt);
	if (c->flags & S_WRITEMODE) {
		c->size = 0;
		while (c->size < c->max_size) {
			ch = *c->buf;
			if ((c->flags & S_LINEMODE) && ch == '\0')
				goto out;
			c->size++;
			c->buf++;
			/* '\n' -> '\r\n' */
			if ((c->flags & S_CRLF) && ch == '\n')
				TX_IN('\r');
			TX_IN(ch);
		}

	} else {
		c->size = 0;
		for (;;) {
rx_again:
			if ((c->flags & S_READLINE) == 0 &&
					c->size >= c->max_size)
				goto out;

			RX_OUT(ch);

			/* only support backspace (or del) */
			if ((c->flags & S_READLINE) &&
					(ch == '\b' || ch == 0x7f)) {
				if (c->size > 0) {
					c->size--;
					c->buf--;
					TX_IN('\b');
					TX_IN(' ');
					TX_IN('\b');
				}
				goto rx_again;
			}

			if ((c->flags & S_LINEMODE) &&
					(ch == '\r' || ch == '\n')) {
				if (c->size < c->max_size)
					*c->buf = '\0';
				goto out;
			}

			if ((c->flags & S_ECHO))
				TX_IN(ch);

			/* stop at max */
			if ((c->flags & S_READLINE) && c->size >= c->max_size)
				goto rx_again;

			*c->buf++ = ch;
			c->size++;
		}
	}
out:
	PT_YIELD(pt);

	PT_END(pt);
}

/* context unions (only one active at the time) */
static struct console_cxt console_cxt;

#define c_puts(_str) \
	do { \
		console_cxt.buf = (void *)(_str); \
		console_cxt.size = 0; \
		console_cxt.max_size = strlen(console_cxt.buf); \
		console_cxt.flags = S_CRLF | S_LINEMODE | S_WRITEMODE; \
		PT_SPAWN(pt, &console_cxt.pt, console_thread(&console_cxt.pt)); \
	} while(0)

#define c_write(_str, _sz) \
	do { \
		console_cxt.buf = (void *)(_str); \
		console_cxt.size = 0; \
		console_cxt.max_size = (_sz); \
		console_cxt.flags = S_CRLF | S_WRITEMODE; \
		PT_SPAWN(pt, &console_cxt.pt, console_thread(&console_cxt.pt)); \
	} while(0)

#define c_getc(_ch) \
	do { \
		console_cxt.buf = (void *) &(_ch); \
		console_cxt.size = 0; \
		console_cxt.max_size = 1; \
		console_cxt.flags = S_CRLF | S_LINEMODE; \
		PT_SPAWN(pt, &console_cxt.pt, console_thread(&console_cxt.pt)); \
	} while(0)

#define c_gets(_buf, _max) \
	do { \
		console_cxt.buf = (void *)(_buf); \
		console_cxt.size = 0; \
		console_cxt.max_size = _max; \
		console_cxt.flags = S_CRLF | S_LINEMODE; \
		PT_SPAWN(pt, &console_cxt.pt, console_thread(&console_cxt.pt)); \
	} while(0)

#define c_read(_buf, _sz) \
	do { \
		console_cxt.buf = (void *)(_buf); \
		console_cxt.size = 0; \
		console_cxt.max_size = (_sz); \
		console_cxt.flags = S_CRLF; \
		PT_SPAWN(pt, &console_cxt.pt, console_thread(&console_cxt.pt)); \
	} while(0)

#define c_readline(_buf, _sz) \
	do { \
		console_cxt.buf = (void *)(_buf); \
		console_cxt.size = 0; \
		console_cxt.max_size = (_sz); \
		console_cxt.flags = S_CRLF | S_LINEMODE | S_ECHO | S_READLINE; \
		PT_SPAWN(pt, &console_cxt.pt, console_thread(&console_cxt.pt)); \
		(_sz) = console_cxt.size; \
	} while(0)

static int simple_atoi(const char *str)
{
	int result;
	char c;

	result = 0;
	while (c = *str++) {
		result *= 10;
		result += c - '0';
	}
	return result;
}

static int prompt_thread(struct pt *pt)
{
	static char ch1;
	static char *pp;
	static char linebuf[80];
	char *p;
	static int linesz;
	int us;
	u32 val;

	PT_BEGIN(pt);

	for (;;) {
again:
		c_puts("PRU> ");
		linesz = sizeof(linebuf);
		c_readline(linebuf, linesz);
		c_puts("\n");
		if (linesz == 0)
			goto again;

		ch1 = linebuf[0];

		if (ch1 == '?') {
			c_puts("Help\n"
				" h <us> set high in us\n"
				" l <us> set low in us\n");
		} else if (ch1 == 'h' || ch1 == 'l') {
			p = linebuf + 1;
			while (*p == ' ')
				p++;
			us = simple_atoi(p);
			val = PRU_us(us);
			if (ch1 == 'h')
				update_hi_cycles(val);
			else
				update_lo_cycles(val);
			led_kickstart();
		} else {
			pp = "*BAD*\n";
		}

		c_puts(pp);
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

	for (;;) {
		event_thread(&pt_event);
		tx_thread(&pt_tx);
		led_thread(&pt_led);
		prompt_thread(&pt_prompt);
	}
}
