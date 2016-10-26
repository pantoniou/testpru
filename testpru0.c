/*
 * testpru
 *
 */

#define PRU0

#define DEBUG

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#include "debug.h"
#include "pru_defs.h"
#include "prucomm.h"
#include "linux_types.h"
#include "remoteproc.h"
#include "syscall.h"
#include "pru_vring.h"
#include "pt.h"
#include "virtio_ids.h"

#include "prurproc.h"

struct pru_vring tx_ring;
struct pru_vring rx_ring;

static struct pt pt_event;
static struct pt pt_prompt;
static struct pt pt_tx;

#define DELAY_CYCLES(x) \
	do { \
		unsigned int t = (x) >> 1; \
		do { \
			__asm(" "); \
		} while (--t); \
	} while(0)

extern void delay_cycles(u16 cycles);

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
		j++;
	}

	return NULL;
}

struct fw_rsc_vdev *
resource_get_rsc_vdev(struct resource_table *res, int id, int idx)
{
	struct fw_rsc_hdr *rsc_hdr;
	struct fw_rsc_vdev *rsc_vdev;
	int i, j;

	j = 0;
	for (i = 0; i < res->num; i++) {
		rsc_hdr = (void *)((char *)res + res->offset[i]);
		if (rsc_hdr->type != RSC_VDEV)
			continue;
		rsc_vdev = (struct fw_rsc_vdev *)&rsc_hdr->data[0];
		if (id >= 0 && id != rsc_vdev->id)
			continue;
		if (j == idx)
			return rsc_vdev;
		j++;
	}

	return NULL;
}

static void resource_setup(void)
{
	struct resource_table *res;
	struct fw_rsc_vdev *rsc_vdev;
#ifdef DEBUG
	int i;
	struct fw_rsc_vdev_vring *rsc_vring;
#endif

	res = sc_get_cfg_resource_table();
	BUG_ON(res == NULL);

	/* get first RPROC_SERIAL VDEV resource */
	rsc_vdev = resource_get_rsc_vdev(res, VIRTIO_ID_RPROC_SERIAL, 0);
	BUG_ON(rsc_vdev == NULL);

	BUG_ON(rsc_vdev->num_of_vrings < 2);
#ifdef DEBUG
	for (i = 0, rsc_vring = rsc_vdev->vring; i < 2; i++, rsc_vring++) {
		sc_printf("VR#%d: da=0x%x align=0x%x num=0x%x notifyid=0x%x",
				i, rsc_vring->da, rsc_vring->align,
				rsc_vring->num, rsc_vring->notifyid);
	}
#endif

	pru_vring_init(&tx_ring, "tx", &rsc_vdev->vring[0]);
	pru_vring_init(&rx_ring, "rx", &rsc_vdev->vring[1]);
}

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

static int handle_downcall(u32 id, u32 arg0, u32 arg1, u32 arg2,
		u32 arg3, u32 arg4)
{
	switch (id) {
		case DC_PWM_CONFIG:
			/* return -EINVAL */
			if (arg1 < MIN_PWM_PULSE || arg2 < MIN_PWM_PULSE)
				return -22;
			PWM_CMD->cmd = PWM_CMD_MODIFY;
			PWM_CMD->pwm_nr = arg0;
			PWM_CMD->u.hilo[0] = arg1;
			PWM_CMD->u.hilo[1] = arg2;
			break;
		case DC_PWM_ENABLE:
			PWM_CMD->cmd = PWM_CMD_ENABLE;
			PWM_CMD->pwm_nr = arg0;
			break;
		case DC_PWM_DISABLE:
			PWM_CMD->cmd = PWM_CMD_DISABLE;
			PWM_CMD->pwm_nr = arg0;
			break;
		default:
			sc_printf("bad downcall with id %d", id);
			/* error */
			return -1;
	}

	PWM_CMD->magic = PWM_CMD_MAGIC;
	SIGNAL_EVENT(SYSEV_THIS_PRU_TO_OTHER_PRU);

	return 0;
}

static int event_thread(struct pt *pt)
{
	static struct pru_vring_elem pvre;
	static u16 idx, count;
	static u32 rx_len, len;
	static struct vring_desc *vrd;
	static char *ptr;

	PT_BEGIN(pt);

	for (;;) {
		/* wait until we get the indication */
		PT_WAIT_UNTIL(pt,
			/* pru_signal() && */
			(PINTC_SRSR0 & SYSEV_THIS_PRU_INCOMING_MASK) != 0);

		/* downcall from the host */
		if (PINTC_SRSR0 & BIT(SYSEV_ARM_TO_THIS_PRU)) {
			PINTC_SICR = SYSEV_ARM_TO_THIS_PRU;

			/* wait until the PWM_CMD is clear */
			PT_WAIT_UNTIL(pt, PWM_CMD->magic == PWM_REPLY_MAGIC);

			sc_downcall(handle_downcall);
		}

		if (PINTC_SRSR0 & BIT(SYSEV_VR_ARM_TO_THIS_PRU)) {
			PINTC_SICR = SYSEV_VR_ARM_TO_THIS_PRU;

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

				/* VRING PRU -> ARM */
				SIGNAL_EVENT(SYSEV_VR_THIS_PRU_TO_ARM);
			}
		}

		if (PINTC_SRSR0 & BIT(SYSEV_OTHER_PRU_TO_THIS_PRU)) {
			PINTC_SICR = SYSEV_OTHER_PRU_TO_THIS_PRU;
		}
	}

	/* get around warning */
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

static char *parse_u32(char *str, u32 *valp)
{
	u32 result;
	u32 tval;
	char c;

	while (*str == ' ')
		str++;

	result = 0;
	for (;;) {
		c = *str;
		tval = (u32)(c - '0');
		if (tval >= 10)
			break;
		result *= 10;
		result += tval;
		str++;
	}
	*valp = result;
	return str;
}

static int prompt_thread(struct pt *pt)
{
	static char ch1;
	static char *pp;
	static char linebuf[80];
	char *p;
	static int linesz;
	u32 val;

	PT_BEGIN(pt);

	PWM_CMD->magic = PWM_REPLY_MAGIC;

	for (;;) {
again:
		c_puts("PRU> ");
		linesz = sizeof(linebuf);
		c_readline(linebuf, linesz);
		c_puts("\n");
		if (linesz == 0)
			goto again;

		ch1 = linebuf[0];
		pp = "";

		if (ch1 == '?') {
			c_puts("Help\n"
				" h <us> set high in us\n"
				" l <us> set low in us\n");
		} else if (ch1 == 'e' || ch1 == 'd') {

			/* wait until the command is processed */
			PT_WAIT_UNTIL(pt, PWM_CMD->magic == PWM_REPLY_MAGIC);

			if (ch1 == 'e') 
				PWM_CMD->cmd = PWM_CMD_ENABLE;
			else
				PWM_CMD->cmd = PWM_CMD_DISABLE;

			p = parse_u32(linebuf + 1, &val);
			PWM_CMD->pwm_nr = val;

			PWM_CMD->magic = PWM_CMD_MAGIC;
			SIGNAL_EVENT(SYSEV_THIS_PRU_TO_OTHER_PRU);

		} else if (ch1 == 'm') {

			/* wait until the command is processed */
			PT_WAIT_UNTIL(pt, PWM_CMD->magic == PWM_REPLY_MAGIC);

			PWM_CMD->cmd = PWM_CMD_MODIFY;

			p = parse_u32(linebuf + 1, &val);
			PWM_CMD->pwm_nr = val;

			p = parse_u32(p, &val);
			PWM_CMD->u.hilo[0] = val;

			p = parse_u32(p, &val);
			PWM_CMD->u.hilo[1] = val;

			PWM_CMD->magic = PWM_CMD_MAGIC;
			SIGNAL_EVENT(SYSEV_THIS_PRU_TO_OTHER_PRU);

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

		// SIGNAL_EVENT(SYSEV_THIS_PRU_TO_ARM);
		SIGNAL_EVENT(SYSEV_VR_THIS_PRU_TO_ARM);
	}

	PT_YIELD(pt);

	PT_END(pt);
}

int main(int argc, char *argv[])
{
	/* enable OCP master port */
	PRUCFG_SYSCFG &= ~SYSCFG_STANDBY_INIT;

	sc_printf("PRU0: Using protothreads");

	PT_INIT(&pt_event);
	PT_INIT(&pt_prompt);
	PT_INIT(&pt_tx);

	resource_setup();
	rx_in = rx_out = rx_cnt = 0;
	tx_in = tx_out = tx_cnt = 0;

	for (;;) {
		event_thread(&pt_event);
		tx_thread(&pt_tx);
		prompt_thread(&pt_prompt);
	}
}
