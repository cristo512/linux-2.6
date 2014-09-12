/*
 *  linux/drivers/char/asm9260_serial.c
 *
 *  Driver for ALPSCALE ASM9260 Serial ports
 *  Copyright (C) 2013
 *
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>

#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#define SERIAL_ASM9260_MAJOR	204
#define MINOR_START		64
#define ASM9260_DEVICENAME	"ttyS"
#define DRIVER_NAME		"asm9260_uart"
#define ASM9260_UART_FIFOSIZE	16
#define ASM9260_BUS_RATE	100000000
#define ASM9260_MAX_UART	10

#define UART_BAUD_DIVINT_MASK		((unsigned int)0x003FFFC0)
#define UART_BAUD_DIVFRAC_MASK		((unsigned int)0x0000003F)
#define	UART_BAUD_DIV_MAX		0x3FFFFF

#define SET_REG 0x4
#define CLR_REG 0x8

#define HW_CTRL0			0x00
#define BM_CTRL0_RXTO_SOURCE_STATUS	BIT(25)
#define BM_CTRL0_RXTO_ENABLE		BIT(24)
#define BM_CTRL0_RXTO_MASK		(0xFF<<16)
#define BM_CTRL0_DEFAULT_RXTIMEOUT	(20<<16) /* TIMEOUT = (100*7+1)*(1/BAUD) */

#define HW_CTRL1			0x10

#define HW_CTRL2			0x20
#define BM_CTRL2_CTSE			BIT(15)
#define BM_CTRL2_RTSE			BIT(14)
#define BM_CTRL2_RXE			BIT(9)
#define BM_CTRL2_TXE			BIT(8)
#define BM_CTRL2_LBE			BIT(7)
#define BM_CTRL2_PORT_ENABLE		BIT(0)
#define BM_CTRL2_TXIFLSEL		(7<<16)
#define BM_CTRL2_RXIFLSEL		(7<<20)
#define BM_CTRL2_DEFAULT_TXIFLSEL	(2<<16)
#define BM_CTRL2_DEFAULT_RXIFLSEL	(3<<20)

#define HW_LINECTRL			0x30
#define ASM9260_UART_BREAK		BIT(0)
#define ASM9260_UART_PEN		BIT(1)
#define ASM9260_UART_EPS		BIT(2)
#define ASM9260_UART_STP2		BIT(3)
#define ASM9260_UART_FEN		BIT(4)
#define ASM9260_UART_WLEN		(3<<5)
#define ASM9260_UART_SPS		BIT(7)
#define ASM9260_UART_BAUD_DIVFRA	(0x3F<<8)
#define ASM9260_UART_BAUD_DIVINT	(0xFFFF<<16)
#define ASM9260_US_CHRL_5		(0<<5)
#define ASM9260_US_CHRL_6		(1<<5)
#define ASM9260_US_CHRL_7		(2<<5)
#define ASM9260_US_CHRL_8		(3<<5)
#define ASM9260_US_NBSTOP_1		(0<<3)
#define ASM9260_US_NBSTOP_2		(1<<3)
#define ASM9260_US_PAR_MARK		((3<<1) | (1<<7))
#define ASM9260_US_PAR_SPACE		((1<<1) | (1<<7))
#define ASM9260_US_PAR_ODD		((1<<1) | (0<<7))
#define ASM9260_US_PAR_EVEN		((3<<1) | (0<<7))
#define ASM9260_US_PAR_NONE		(0<<1)

/* Interrupt register.
 * contains the interrupt enables and the interrupt status bits */
#define HW_INTR			0x40
/* Tx FIFO EMPTY Raw Interrupt enable */
#define BM_INTR_TFEIEN		BIT(27)
/* Overrun Error Interrupt Enable. */
#define BM_INTR_OEIEN		BIT(26)
/* Break Error Interrupt Enable. */
#define BM_INTR_BEIEN		BIT(25)
/* Parity Error Interrupt Enable. */
#define BM_INTR_PEIEN		BIT(24)
/* Framing Error Interrupt Enable. */
#define BM_INTR_FEIEN		BIT(23)
/* Receive Timeout Interrupt Enable.
 * If not set and FIFO is enabled, then RX will be triggered only
 * if FIFO is full. */
#define BM_INTR_RTIEN		BIT(22)
/* Transmit Interrupt Enable. */
#define BM_INTR_TXIEN		BIT(21)
/* Receive Interrupt Enable. */
#define BM_INTR_RXIEN		BIT(20)
/* nUARTDSR Modem Interrupt Enable. */
#define BM_INTR_DSRMIEN		BIT(19)
/* nUARTDCD Modem Interrupt Enable. */
#define BM_INTR_DCDMIEN		BIT(18)
/* nUARTCTS Modem Interrupt Enable. */
#define BM_INTR_CTSMIEN		BIT(17)
/* nUARTRI Modem Interrupt Enable. */
#define BM_INTR_RIMIEN		BIT(16)
/* Auto-Boud Timeout */
#define BM_INTR_ABTO		BIT(13)
#define BM_INTR_ABEO		BIT(12)
/* Tx FIFO EMPTY Raw Interrupt state */
#define BM_INTR_TFEIS		BIT(11)
/* Overrun Error */
#define BM_INTR_OEIS		BIT(10)
/* Break Error */
#define BM_INTR_BEIS		BIT(9)
/* Parity Error */
#define BM_INTR_PEIS		BIT(8)
/* Framing Error */
#define BM_INTR_FEIS		BIT(7)
/* Receive Timeout */
#define BM_INTR_RTIS		BIT(6)
/* Transmit done */
#define BM_INTR_TXIS		BIT(5)
/* Receive done */
#define BM_INTR_RXIS		BIT(4)
#define BM_INTR_DSRMIS		BIT(3)
#define BM_INTR_DCDMIS		BIT(2)
#define BM_INTR_CTSMIS		BIT(1)
#define BM_INTR_RIMIS		BIT(0)


#define BM_INTR_DEF_MASK	(BM_INTR_RXIEN | BM_INTR_TXIEN | BM_INTR_RTIEN \
		| BM_INTR_FEIEN | BM_INTR_PEIEN | BM_INTR_BEIEN | BM_INTR_OEIEN)

#define BM_INTR_EN_MASK		(0x3fff0000)
#define BM_INTR_IS_MASK		(0x00003fff)

#define HW_DATA				0x50

#define HW_STAT				0x60
#define BM_STAT_BUSY			BIT(29)
#define BM_STAT_CTS			BIT(28)
#define BM_STAT_TXEMPTY			BIT(27)
#define BM_STAT_RXFULL			BIT(26)
#define BM_STAT_TXFULL			BIT(25)
#define BM_STAT_RXEMPTY			BIT(24)
#define BM_STAT_OVERRUNERR		BIT(19)
#define BM_STAT_BREAKERR		BIT(18)
#define BM_STAT_PARITYERR		BIT(17)
#define BM_STAT_FRAMEERR		BIT(16)


#define HW_ILPR					0x80
#define HW_RS485CTRL				0x90
#define HW_RS485ADRMATCH			0xA0
#define HW_RS485DLY				0xB0
#define HW_AUTOBAUD				0xC0
#define HW_CTRL3				0xD0

#define	ASM9260_UART_RS485EN			BIT(0)
#define	ASM9260_UART_RS485_RXDIS		BIT(1)
#define	ASM9260_UART_RS485_AADEN		BIT(2)
#define	ASM9260_UART_RS485_PINSEL		BIT(3)
#define	ASM9260_UART_RS485_DIR_CTRL		BIT(4)
#define	ASM9260_UART_RS485_ONIV			BIT(5)

/*
 * We wrap our port structure around the generic uart_port.
 */
struct asm9260_uart_port {
	struct uart_port	uart;		/* uart */
	struct device_node	*np;
	struct clk		*clk;		/* uart clock */
	struct clk		*clk_ahb;
	int			clk_on;

	struct serial_rs485	rs485;		/* rs485 settings */

	uint32_t intmask;
	int init_ok;
};

static void asm9260_start_rx(struct uart_port *port);
static struct asm9260_uart_port *asm9260_ports;
static int asm9260_ports_num;


static inline struct asm9260_uart_port *
to_asm9260_uart_port(struct uart_port *uart)
{
	return container_of(uart, struct asm9260_uart_port, uart);
}

static void asm9260_intr_mask_clr(struct uart_port *uport, uint32_t val)
{
	WARN_ON(val & ~BM_INTR_EN_MASK);

	iowrite32(val, uport->membase + HW_INTR + CLR_REG);
}

static inline void asm9260_intr_mask(struct uart_port *uport)
{
	iowrite32(BM_INTR_DEF_MASK,
			uport->membase + HW_INTR + CLR_REG);
}

static inline void asm9260_intr_unmask(struct uart_port *uport)
{
	iowrite32(BM_INTR_DEF_MASK,
			uport->membase + HW_INTR + SET_REG);
}

/*
 * Return TIOCSER_TEMT when transmitter FIFO and Shift register is empty.
 */
static u_int asm9260_tx_empty(struct uart_port *uport)
{
	return (ioread32(uport->membase + HW_STAT)
			& BM_STAT_TXEMPTY) ? TIOCSER_TEMT : 0;
}

static void asm9260_set_mctrl(struct uart_port *uport, u_int mctrl)
{
}

static u_int asm9260_get_mctrl(struct uart_port *uport)
{
	return 0;
}

/*
 * Stop transmitting.
 */
static void asm9260_stop_tx(struct uart_port *uport)
{
	struct asm9260_uart_port *asm9260_port = to_asm9260_uart_port(uport);

	/* TODO we should use here TXE on line ctrl */
	asm9260_intr_mask_clr(uport, BM_INTR_TXIEN);

	if ((asm9260_port->rs485.flags & SER_RS485_ENABLED) &&
	    !(asm9260_port->rs485.flags & SER_RS485_RX_DURING_TX))
		asm9260_start_rx(uport);
}

static void asm9260_tx_chars(struct uart_port *uport);
/*
 * Start transmitting.
 */
static void asm9260_start_tx(struct uart_port *uport)
{
	/* TODO we should use hear TXE on line ctrl */
	asm9260_intr_unmask(uport);
	asm9260_tx_chars(uport);
}

/*
 * start receiving - port is in process of being opened.
 */
static void asm9260_start_rx(struct uart_port *uport)
{
	/* enable receive */
	iowrite32(BM_CTRL2_RXE,
			uport->membase + HW_CTRL2 + SET_REG);
}

/*
 * Stop receiving - port is in process of being closed.
 */
static void asm9260_stop_rx(struct uart_port *uport)
{
	/* disable receive */
	iowrite32(BM_CTRL2_RXE,
			uport->membase + HW_CTRL2 + CLR_REG);
}

/*
 * Enable modem status interrupts
 */
static void asm9260_enable_ms(struct uart_port *uport)
{
	/*The driver doesn't support modem control*/
}

/*
 * Control the transmission of a break signal
 */
static void asm9260_break_ctl(struct uart_port *uport, int break_state)
{
	if (break_state != 0)
		iowrite32(ASM9260_UART_BREAK,
				uport->membase + HW_LINECTRL + SET_REG);
	else
		iowrite32(ASM9260_UART_BREAK,
				uport->membase + HW_LINECTRL + CLR_REG);
}

/*
 * Characters received (called from interrupt handler)
 */
static void asm9260_rx_chars(struct uart_port *uport, unsigned int intr)
{
	unsigned int status, ch;

	status = ioread32(uport->membase + HW_STAT);
	while (!(status & BM_STAT_RXEMPTY)) {
		unsigned int flg;
		ch = ioread32(uport->membase + HW_DATA);

		uport->icount.rx++;
		flg = TTY_NORMAL;

		if (unlikely(intr & (BM_INTR_PEIS | BM_INTR_FEIS
				       | BM_INTR_OEIS | BM_INTR_BEIS))) {

			/* clear error */
			iowrite32(0, uport->membase + HW_STAT);

			if (intr & BM_INTR_BEIS) {
				uport->icount.brk++;
				if (uart_handle_break(uport))
					continue;
			} else if (intr & BM_INTR_PEIS)
				uport->icount.parity++;
			else if (intr & BM_INTR_FEIS)
				uport->icount.frame++;

			if (intr & BM_INTR_OEIS)
				uport->icount.overrun++;

			intr &= uport->read_status_mask;

			if (intr & BM_INTR_BEIS)
				flg = TTY_BREAK;
			else if (intr & BM_INTR_PEIS)
				flg = TTY_PARITY;
			else if (intr & BM_INTR_FEIS)
				flg = TTY_FRAME;

		}

		if (uart_handle_sysrq_char(uport, ch))
			continue;

		uart_insert_char(uport, intr, BM_INTR_OEIS, ch, flg);
		status = ioread32(uport->membase + HW_STAT);
	}

	tty_flip_buffer_push(&uport->state->port);
}

static void asm9260_tx_chars(struct uart_port *uport)
{
	struct circ_buf *xmit = &uport->state->xmit;

	if (uport->x_char && !(ioread32(uport->membase + HW_STAT)
				& BM_STAT_TXFULL)) {
		iowrite32(uport->x_char, uport->membase + HW_DATA);
		uport->icount.tx++;
		uport->x_char = 0;
	}
	if (uart_circ_empty(xmit) || uart_tx_stopped(uport))
		return;

	while (!uart_circ_empty(xmit)) {
		if (ioread32(uport->membase + HW_STAT)
				& BM_STAT_TXFULL) {
			break;;
		}
		iowrite32(xmit->buf[xmit->tail],
				uport->membase + HW_DATA);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		uport->icount.tx++;
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(uport);

	if (uart_circ_empty(xmit))
		asm9260_intr_mask_clr(uport, BM_INTR_TXIEN);
}

/*
 * receive interrupt handler.
 */
static void
asm9260_handle_receive(struct uart_port *uport, unsigned int pending)
{
	/* Interrupt receive */
	if ((pending & BM_INTR_RXIS) || (pending & BM_INTR_RTIS))
		asm9260_rx_chars(uport, pending);
	else if (pending & BM_INTR_BEIS) {
		/*
		 * End of break detected. If it came along with a
		 * character, asm9260_rx_chars will handle it.
		 */
		iowrite32(0, uport->membase + HW_STAT);
	}
}

/*
 * transmit interrupt handler. (Transmit is IRQF_NODELAY safe)
 */
static void
asm9260_handle_transmit(struct uart_port *uport, unsigned int pending)
{
	if (pending & BM_INTR_TXIS)
		asm9260_tx_chars(uport);
}

/*
 * Interrupt handler
 */
static irqreturn_t asm9260_interrupt(int irq, void *dev_id)
{
	struct uart_port *uport = dev_id;
	unsigned int status, pending;

	/* TODO: need rework */
	status = ioread32(uport->membase + HW_INTR);
	pending = status & 0xFFF;

	asm9260_handle_receive(uport, pending);
	asm9260_handle_transmit(uport, pending);

	iowrite32(pending,
			uport->membase + HW_INTR + CLR_REG);

	asm9260_intr_unmask(uport);
	return IRQ_HANDLED;
}

static irqreturn_t asm9260_fast_int(int irq, void *dev_id)
{
	struct uart_port *uport = dev_id;
	unsigned int status, pending;

	status = ioread32(uport->membase + HW_INTR);
	pending = (status & (status >> 16)) & 0xFFF;
	if (!pending)
		return IRQ_NONE;

	asm9260_intr_mask(uport);
	return IRQ_WAKE_THREAD;
}

/*
 * Perform initialization and enable port for reception
 */
static int asm9260_startup(struct uart_port *uport)
{
	int retval;

	/*
	 * Ensure that no interrupts are enabled otherwise when
	 * request_irq() is called we could get stuck trying to
	 * handle an unexpected interrupt
	 */
	iowrite32(0, uport->membase + HW_INTR);

	retval = devm_request_threaded_irq(uport->dev, uport->irq,
			asm9260_fast_int, asm9260_interrupt, IRQF_SHARED,
			dev_name(uport->dev), uport);
	if (retval) {
		dev_err(uport->dev, "Can't get irq\n");
		return retval;
	}

	/* enable rx timeout */
	iowrite32(BM_CTRL0_RXTO_MASK | BM_CTRL0_RXTO_SOURCE_STATUS,
			uport->membase + HW_CTRL0 + CLR_REG);
	iowrite32(BM_CTRL0_DEFAULT_RXTIMEOUT | BM_CTRL0_RXTO_ENABLE,
			uport->membase + HW_CTRL0 + SET_REG);


	/*
	 * Finally, enable the serial port
	 * enable tx & rx
	 */
	iowrite32(BM_CTRL2_RXIFLSEL | BM_CTRL2_TXIFLSEL,
			uport->membase + HW_CTRL2 + CLR_REG);
	iowrite32(BM_CTRL2_PORT_ENABLE | BM_CTRL2_TXE | BM_CTRL2_RXE |
			BM_CTRL2_DEFAULT_TXIFLSEL |
			BM_CTRL2_DEFAULT_RXIFLSEL,
			uport->membase + HW_CTRL2);

	asm9260_intr_unmask(uport);
	return 0;
}

/*
 * Disable the port
 */
static void asm9260_shutdown(struct uart_port *uport)
{
	int timeout = 10000;

	/*wait for controller finish tx*/
	while (!(ioread32(uport->membase + HW_STAT)
				& BM_STAT_TXEMPTY)) {
		if (--timeout < 0)
			break;
	}

	/*
	 * Ensure everything is stopped.
	 */
	asm9260_stop_tx(uport);
	asm9260_stop_rx(uport);
}

/*
 * Flush any TX data submitted for DMA. Called when the TX circular
 * buffer is reset.
 */
static void asm9260_flush_buffer(struct uart_port *uport)
{
}

/*
 * Power / Clock management.
 */
static void asm9260_serial_pm(struct uart_port *uport, unsigned int state,
			    unsigned int oldstate)
{
}

static void asm9260_set_rs485(struct uart_port *uport)
{
	struct asm9260_uart_port *port = to_asm9260_uart_port(uport);
	unsigned int rs485_ctrl;
	/* set RS485 */
	rs485_ctrl = ioread32(uport->membase + HW_RS485CTRL);

	/* Resetting serial mode to RS232 (0x0) */
	rs485_ctrl &= ~ASM9260_UART_RS485EN;

	if (port->rs485.flags & SER_RS485_ENABLED) {
		dev_dbg(uport->dev, "Setting UART to RS485\n");
		if ((port->rs485.delay_rts_after_send) > 0) {
			/* delay is (rs485conf->delay_rts_after_send * Bit Period * 1/16) */
			iowrite32(port->rs485.delay_rts_after_send,
					uport->membase + HW_RS485DLY);
		}

		if ((port->rs485.flags & SER_RS485_RTS_ON_SEND) &&
			!(port->rs485.flags & SER_RS485_RTS_AFTER_SEND)) {
			/*
			 * Set logical level for RTS pin equal to 1 when sending,
			 * and set logical level for RTS pin equal to 0 after sending
			*/
			rs485_ctrl |= ASM9260_UART_RS485_ONIV;
		} else if (!(port->rs485.flags & SER_RS485_RTS_ON_SEND) &&
			(port->rs485.flags & SER_RS485_RTS_AFTER_SEND)) {
			/*
			 * Set logical level for RTS pin equal to 0 when sending,
			 * and set logical level for RTS pin equal to 1 after sending
			*/
			rs485_ctrl &= ~ASM9260_UART_RS485_ONIV;
		} else{
			printk("Please view RS485CTRL register in datasheet for more details.\n");
		}

		/* Enable RS485 and RTS is used to control direction automatically,  */
		rs485_ctrl |= ASM9260_UART_RS485EN | ASM9260_UART_RS485_DIR_CTRL;
		rs485_ctrl &= ~ASM9260_UART_RS485_PINSEL;

		if (port->rs485.flags & SER_RS485_RX_DURING_TX)
			dev_dbg(uport->dev, "hardware should support SER_RS485_RX_DURING_TX.\n");
	} else {
		dev_dbg(uport->dev, "Setting UART to RS232\n");
	}

	iowrite32(rs485_ctrl, uport->membase + HW_RS485CTRL);
}
/*
 * Change the port parameters
 */
static void asm9260_set_termios(struct uart_port *uport, struct ktermios *termios,
			      struct ktermios *old)
{
	unsigned int mode, baud;
	unsigned int bauddivint, bauddivfrac;


	asm9260_intr_mask(uport);

	/*
	 * We don't support modem control lines.
	*/
	termios->c_cflag &= ~(HUPCL | CMSPAR);
	termios->c_cflag |= CLOCAL;

	/* Get current mode register */
	mode = ioread32(uport->membase + HW_LINECTRL);
	mode &= ~(ASM9260_UART_PEN | ASM9260_UART_EPS
			| ASM9260_UART_STP2 | ASM9260_UART_FEN
			| ASM9260_UART_WLEN | ASM9260_UART_SPS
			| ASM9260_UART_BAUD_DIVFRA | ASM9260_UART_BAUD_DIVINT);

	baud = uart_get_baud_rate(uport, termios, old,
			uport->uartclk * 4 / UART_BAUD_DIV_MAX, uport->uartclk / 16);
	bauddivint =
		(((uport->uartclk << 2) / baud) & UART_BAUD_DIVINT_MASK) << 10;
	bauddivfrac =
		(((uport->uartclk << 2) / baud) & UART_BAUD_DIVFRAC_MASK) << 8;
	/* byte size */
	switch (termios->c_cflag & CSIZE) {
	case CS5:
		mode |= ASM9260_US_CHRL_5;
		break;
	case CS6:
		mode |= ASM9260_US_CHRL_6;
		break;
	case CS7:
		mode |= ASM9260_US_CHRL_7;
		break;
	default:
		mode |= ASM9260_US_CHRL_8;
		break;
	}

	/* enable fifo */
	mode |= ASM9260_UART_FEN;

	/* stop bits */
	if (termios->c_cflag & CSTOPB)
		mode |= ASM9260_US_NBSTOP_2;
	else
		mode |= ASM9260_US_NBSTOP_1;

	/* parity */
	if (termios->c_cflag & PARENB) {
		/* Mark or Space parity */
		if (termios->c_cflag & CMSPAR) {
			if (termios->c_cflag & PARODD)
				mode |= ASM9260_US_PAR_MARK;
			else
				mode |= ASM9260_US_PAR_SPACE;
		} else if (termios->c_cflag & PARODD)
			mode |= ASM9260_US_PAR_ODD;
		else
			mode |= ASM9260_US_PAR_EVEN;
	} else
		mode |= ASM9260_US_PAR_NONE;

	spin_lock(&uport->lock);

	uport->read_status_mask = BM_INTR_OEIS;
	if (termios->c_iflag & INPCK)
		uport->read_status_mask |= (BM_INTR_FEIS | BM_INTR_PEIS);
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		uport->read_status_mask |= BM_INTR_BEIS;

	/*
	 * Characters to ignore
	 */
	uport->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		uport->ignore_status_mask |=
			(BM_INTR_FEIS  | BM_INTR_PEIS);
	if (termios->c_iflag & IGNBRK) {
		uport->ignore_status_mask |= BM_INTR_BEIS;
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			uport->ignore_status_mask |= BM_INTR_OEIS;
	}

	/* update the per-port timeout */
	uart_update_timeout(uport, termios->c_cflag, baud);

	/* drain transmitter */
	while (!(ioread32(uport->membase + HW_STAT)
				& BM_STAT_TXEMPTY))
		cpu_relax();

	while (!(ioread32(uport->membase + HW_STAT)
				& BM_STAT_RXEMPTY))
		ioread32(uport->membase + HW_DATA);

	asm9260_set_rs485(uport);

	/* set hardware flow control */
	if (termios->c_cflag & CRTSCTS)
		iowrite32(BM_CTRL2_CTSE | BM_CTRL2_RTSE,
				uport->membase + HW_CTRL2 + SET_REG);
	else
		iowrite32(BM_CTRL2_CTSE | BM_CTRL2_RTSE,
				uport->membase + HW_CTRL2 + CLR_REG);

	/* set the parity, stop bits, data size and baud rate*/
	iowrite32(mode | bauddivint | bauddivfrac,
			uport->membase + HW_LINECTRL);

	/* CTS flow-control and modem-status interrupts */
	if (UART_ENABLE_MS(uport, termios->c_cflag))
		uport->ops->enable_ms(uport);

	spin_unlock(&uport->lock);

	dev_dbg(uport->dev, "mode:0x%x, baud:%d, bauddivint:0x%x, bauddivfrac:0x%x, ctrl2:0x%x\n",
			mode, baud, bauddivint, bauddivfrac,
			ioread32(uport->membase + HW_CTRL2));

	asm9260_intr_unmask(uport);
}

/*
 * Return string describing the specified port
 */
static const char *asm9260_type(struct uart_port *uport)
{
	return (uport->type == PORT_ASM9260) ? DRIVER_NAME : NULL;
}

/*
 * Release the memory region(s) being used by 'port'.
 */
static void asm9260_release_port(struct uart_port *uport)
{
}

/*
 * Request the memory region(s) being used by 'port'.
 */
static int asm9260_request_port(struct uart_port *uport)
{
	return 0;
}

/*
 * Configure/autoconfigure the port.
 */
static void asm9260_config_port(struct uart_port *uport, int flags)
{
	if (flags & UART_CONFIG_TYPE) {
		uport->type = PORT_ASM9260;
		asm9260_request_port(uport);
	}
}

/*
 * Verify the new serial_struct (for TIOCSSERIAL).
 */
static int asm9260_verify_port(struct uart_port *uport, struct serial_struct *ser)
{
	int ret = 0;

	if (ser->type != PORT_UNKNOWN && ser->type != PORT_ASM9260)
		ret = -EINVAL;
	if (uport->irq != ser->irq)
		ret = -EINVAL;
	if (ser->io_type != SERIAL_IO_MEM)
		ret = -EINVAL;
	if (uport->uartclk / 16 != ser->baud_base)
		ret = -EINVAL;
	if ((void *)uport->mapbase != ser->iomem_base)
		ret = -EINVAL;
	if (uport->iobase != ser->port)
		ret = -EINVAL;
	if (ser->hub6 != 0)
		ret = -EINVAL;
	return ret;
}

/* Enable or disable the rs485 support */
void asm9260_config_rs485(struct uart_port *uport, struct serial_rs485 *rs485conf)
{
	struct asm9260_uart_port *port = to_asm9260_uart_port(uport);

	asm9260_intr_mask(uport);
	spin_lock(&uport->lock);

	/* Disable interrupts */

	port->rs485 = *rs485conf;

	asm9260_set_rs485(uport);

	/* Enable tx interrupts */
	spin_unlock(&uport->lock);
	asm9260_intr_unmask(uport);

}

static int asm9260_ioctl(struct uart_port *uport,
		unsigned int cmd, unsigned long arg)
{
	struct serial_rs485 rs485conf;

	switch (cmd) {
	case TIOCSRS485:
		if (copy_from_user(&rs485conf, (struct serial_rs485 *) arg,
					sizeof(rs485conf)))
			return -EFAULT;

		asm9260_config_rs485(uport, &rs485conf);
		break;

	case TIOCGRS485:
		if (copy_to_user((struct serial_rs485 *) arg,
					&(to_asm9260_uart_port(uport)->rs485),
					sizeof(rs485conf)))
			return -EFAULT;
		break;

	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

static struct uart_ops asm9260_pops = {
	.tx_empty	= asm9260_tx_empty,
	.set_mctrl	= asm9260_set_mctrl,
	.get_mctrl	= asm9260_get_mctrl,
	.stop_tx	= asm9260_stop_tx,
	.start_tx	= asm9260_start_tx,
	.stop_rx	= asm9260_stop_rx,
	.enable_ms	= asm9260_enable_ms,
	.break_ctl	= asm9260_break_ctl,
	.startup	= asm9260_startup,
	.shutdown	= asm9260_shutdown,
	.flush_buffer	= asm9260_flush_buffer,
	.set_termios	= asm9260_set_termios,
	.type		= asm9260_type,
	.release_port	= asm9260_release_port,
	.request_port	= asm9260_request_port,
	.config_port	= asm9260_config_port,
	.verify_port	= asm9260_verify_port,
	.pm		= asm9260_serial_pm,
	.ioctl	= asm9260_ioctl,
};

#ifdef CONFIG_SERIAL_ASM9260_CONSOLE

static struct asm9260_uart_port *get_asm9260_uart_port(int line);
static struct console asm9260_console;

static void asm9260_console_putchar(struct uart_port *uport, int ch)
{
	while (ioread32(uport->membase + HW_STAT)
			& BM_STAT_TXFULL)
		cpu_relax();
	iowrite32(ch, uport->membase + HW_DATA);
}

/*
 * Interrupts are disabled on entering
 */
static void asm9260_console_write(struct console *co, const char *s, u_int count)
{
	struct uart_port *uport;
	struct asm9260_uart_port *port;
	unsigned int status;
	int locked = 1;

	port = get_asm9260_uart_port(co->index);
	uport = &port->uart;

	asm9260_intr_mask(uport);

	if (oops_in_progress)
		locked = spin_trylock(&uport->lock);
	else
		spin_lock(&uport->lock);


	uart_console_write(uport, s, count, asm9260_console_putchar);

	/*
	 * Finally, wait for transmitter to become empty
	 * and restore IMR
	 */
	do {
		status = ioread32(uport->membase + HW_STAT);
	} while (!(status & BM_STAT_TXEMPTY));

	if (locked)
		spin_unlock(&uport->lock);

	asm9260_intr_unmask(uport);
}

/*
 * If the port was already initialised (eg, by a boot loader),
 * try to determine the current setup.
 */
static void __init asm9260_console_get_options(struct uart_port *port, int *baud,
					     int *parity, int *bits)
{
	unsigned int mr, quot, linectrl, bauddivint, bauddivfrc;

	/*
	 * If the baud rate generator isn't running, the port wasn't
	 * initialized by the boot loader.
	 */
	linectrl = ioread32(port->membase + HW_LINECTRL);
	bauddivint = (linectrl & ASM9260_UART_BAUD_DIVINT) >> 16;
	bauddivfrc = (linectrl & ASM9260_UART_BAUD_DIVFRA) >> 8;
	quot = (bauddivint << 6) | bauddivfrc;

	if (!quot)
		return;

	mr = linectrl & ASM9260_UART_WLEN;
	if (mr == ASM9260_US_CHRL_8)
		*bits = 8;
	else
		*bits = 7;

	mr = linectrl &
		(ASM9260_UART_PEN | ASM9260_UART_EPS | ASM9260_UART_SPS);
	if (mr == ASM9260_US_PAR_EVEN)
		*parity = 'e';
	else if (mr == ASM9260_US_PAR_ODD)
		*parity = 'o';

	/*
	 * The serial core only rounds down when matching this to a
	 * supported baud rate. Make sure we don't end up slightly
	 * lower than one of those, as it would make us fall through
	 * to a much lower baud rate than we really want.
	 */
	*baud = (port->uartclk * 4) / quot;
}

static int asm9260_get_of_clks(struct asm9260_uart_port *port,
		struct device_node *np);
static void asm9260_enable_clks(struct asm9260_uart_port *port);
static void asm9260_uart_of_enumerate(void);
static int __init asm9260_console_setup(struct console *co, char *options)
{
	struct uart_port *uport;
	struct asm9260_uart_port *port;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	asm9260_uart_of_enumerate();

	port = get_asm9260_uart_port(co->index);
	uport = &port->uart;

	asm9260_enable_clks(port);

	iowrite32(BM_CTRL2_TXE | BM_CTRL2_RXE | BM_CTRL2_PORT_ENABLE,
			uport->membase + HW_CTRL2 + SET_REG);

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	else
		asm9260_console_get_options(uport, &baud, &parity, &bits);

	return uart_set_options(uport, co, baud, parity, bits, flow);
}

static struct uart_driver asm9260_uart;

static struct console asm9260_console = {
	.name		= ASM9260_DEVICENAME,
	.write		= asm9260_console_write,
	.device		= uart_console_device,
	.setup		= asm9260_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &asm9260_uart,
};

#define ASM9260_CONSOLE_DEVICE	(&asm9260_console)

/*
 * Early console initialization (before VM subsystem initialized).
 */
static int __init asm9260_console_init(void)
{
	register_console(&asm9260_console);
	return 0;
}

console_initcall(asm9260_console_init);
#else
#define ASM9260_CONSOLE_DEVICE	NULL
#endif

static struct uart_driver asm9260_uart = {
	.owner			= THIS_MODULE,
	.driver_name		= DRIVER_NAME,
	.dev_name		= ASM9260_DEVICENAME,
	.nr			= ASM9260_MAX_UART,
	.cons			= ASM9260_CONSOLE_DEVICE,
};

/* Match table for of_platform binding */
static struct of_device_id asm9260_of_match[] = {
	{ .compatible = "alpscale,asm9260-uart", },
	{}
};
MODULE_DEVICE_TABLE(of, asm9260_of_match);

static void asm9260_enable_clks(struct asm9260_uart_port *port)
{
	struct uart_port *uport = &port->uart;
	int err;

	if (port->clk_on)
		return;

	err = clk_set_rate(port->clk, ASM9260_BUS_RATE);
	if (err) {
		dev_err(uport->dev, "Failed to set rate!\n");
	}

	err = clk_prepare_enable(port->clk);
	if (err) {
		dev_err(uport->dev, "Failed to enable clk!\n");
	}

	err = clk_prepare_enable(port->clk_ahb);
	if (err) {
		dev_err(uport->dev, "Failed to enable ahb_clk!\n");
	}

	uport->uartclk = clk_get_rate(port->clk);
	port->clk_on = 1;
}


/* get devicetree clocks, if some thing wrong, warn about it */
static int asm9260_get_of_clks(struct asm9260_uart_port *port,
		struct device_node *np)
{
	int clk_idx = 0;

	port->clk = of_clk_get(np, clk_idx);
	if (IS_ERR(port->clk))
		goto out_err;

	/* configure AHB clock */
	clk_idx = 1;
	port->clk_ahb = of_clk_get(np, clk_idx);
	if (IS_ERR(port->clk_ahb))
		goto out_err;

	return 0;
out_err:
	pr_err("%s: Failed to get clk (%i)\n", __func__, clk_idx);
	return 1;
}

static int asm9260_get_count_of_nodes(const struct of_device_id *matches)
{
	int count = 0;
	struct device_node *np;

	for_each_matching_node(np, matches)
		count++;

	return count;
}

static struct asm9260_uart_port *get_asm9260_uart_port(int line)
{
	if (line >= asm9260_ports_num) {
		pr_err("%s: Line number overflow. Check DeviceTree!!",
				__func__);
		return NULL;
	}

	return &asm9260_ports[line];
}

static void asm9260_uart_of_enumerate(void)
{
	static int enum_done;
	struct device_node *np;

	if (enum_done)
		return;

	asm9260_ports_num = asm9260_get_count_of_nodes(asm9260_of_match);
	asm9260_ports = kcalloc(asm9260_ports_num,
				sizeof(struct asm9260_uart_port), GFP_KERNEL);

	for_each_matching_node(np, asm9260_of_match) {
		struct uart_port *uport;
		struct asm9260_uart_port *port;
		int line;

		line = of_alias_get_id(np, "serial");
		if (line < 0) {
			pr_err("Error! Devicetree has no \"serial\" aliases\n");
			continue;
		}

		port = get_asm9260_uart_port(line);
		if (!port)
			continue;

		uport = &port->uart;
		if(asm9260_get_of_clks(port, np))
			return;

		uport->iotype	= UPIO_MEM;
		uport->flags	= UPF_BOOT_AUTOCONF;
		uport->ops	= &asm9260_pops;
		uport->fifosize	= ASM9260_UART_FIFOSIZE;
		uport->line	= line;

		/* Since of_map don't do actual request of memory region,
		 * it is save to use it for all, enabled and disabled uarts. */
		uport->membase = of_iomap(np, 0);
		if (!uport->membase) {
			pr_err("Unable to map registers\n");
			continue;
		}
		port->np = np;
		port->init_ok = 1;
	}

	enum_done = 1;
}

/*
 * Configure the port from the platform device resource info.
 */
static void asm9260_init_port(struct asm9260_uart_port *asm9260_port,
				      struct platform_device *pdev)
{
	struct uart_port *uport = &asm9260_port->uart;
	struct device_node *np = pdev->dev.of_node;
	struct resource res;

	uport->dev = &pdev->dev;

	uport->irq = irq_of_parse_and_map(np, 0);

	of_address_to_resource(np, 0, &res);
	if (!devm_request_mem_region(uport->dev, res.start,
				resource_size(&res), dev_name(uport->dev)))
		panic("%s: unable to request mem region", dev_name(uport->dev));

	uport->mapbase	= res.start;

	asm9260_enable_clks(asm9260_port);
}

static int asm9260_serial_probe(struct platform_device *pdev)
{
	struct asm9260_uart_port *port;
	struct device_node *np = pdev->dev.of_node;
	int ret, line;

	asm9260_uart_of_enumerate();

	if (!np) {
		dev_err(&pdev->dev, "Error! We support only DeviceTree!\n");
		return -EPERM;
	}

	line = of_alias_get_id(np, "serial");
	if (line < 0) {
		dev_err(&pdev->dev,
				"Error! Devicetree has no \"serial\" aliases\n");
		return -EPERM;
	}

	port = get_asm9260_uart_port(line);

	if (!port->init_ok) {
		dev_err(&pdev->dev, "Bad init!\n");
	}

	asm9260_init_port(port, pdev);

	ret = uart_add_one_port(&asm9260_uart, &port->uart);
	if (ret) {
		dev_err(&pdev->dev, "Filed to add uart port\n");
		goto err_add_port;
	}

	platform_set_drvdata(pdev, port);

	return 0;

err_add_port:
	if (!uart_console(&port->uart)) {
		clk_put(port->clk);
		port->clk = NULL;
	}
	dev_err(&pdev->dev, "Filed to probe device\n");
	return ret;
}

static int asm9260_serial_remove(struct platform_device *pdev)
{
	struct uart_port *port = platform_get_drvdata(pdev);
	struct asm9260_uart_port *asm9260_port = to_asm9260_uart_port(port);
	int ret = 0;

	uart_remove_one_port(&asm9260_uart, port);
	uart_unregister_driver(&asm9260_uart);

	/* TODO: how should we handle clks here */
	clk_put(asm9260_port->clk);

	return ret;
}

static struct platform_driver asm9260_serial_driver = {
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(asm9260_of_match),
	},
	.probe		= asm9260_serial_probe,
	.remove		= asm9260_serial_remove,
};

static int __init asm9260_serial_init(void)
{
	int ret;
	ret = uart_register_driver(&asm9260_uart);
	if (ret)
		return ret;

	ret = platform_driver_register(&asm9260_serial_driver);
	if (ret)
		uart_unregister_driver(&asm9260_uart);

	return ret;
}

static void __exit asm9260_serial_exit(void)
{
	platform_driver_unregister(&asm9260_serial_driver);
	uart_unregister_driver(&asm9260_uart);
}

module_init(asm9260_serial_init);
module_exit(asm9260_serial_exit);

MODULE_DESCRIPTION("ASM9260 serial port driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
