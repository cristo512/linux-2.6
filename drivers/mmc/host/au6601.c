/*
 * Copyright (C) 2014 Oleksij Rempel.
 *
 * Authors: Oleksij Rempel <linux@rempel-privat.de>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */


#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/pm.h>
#include <linux/irq.h>
#include <linux/interrupt.h>

#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>

#define DRVNAME			"au6601-pci"
#define PCI_ID_ALCOR_MICRO	0x1aea
#define PCI_ID_AU6601		0x6601

#define MHZ_TO_HZ(freq)	((freq) * 1000 * 1000)

#define AU6601_MIN_CLOCK		(150 * 1000)
#define AU6601_MAX_CLOCK		MHZ_TO_HZ(208)
#define AU6601_MAX_SEGMENTS		512
#define AU6601_MAX_BLOCK_LENGTH		512
#define AU6601_MAX_DMA_BLOCKS		8
#define AU6601_DMA_LOCAL_SEGMENTS	3
#define AU6601_MAX_BLOCK_COUNT		65536

/* SDMA phy address. Higer then 0x0800.0000? */
#define AU6601_REG_SDMA_ADDR	0x00
 #define AU6601_SDMA_MASK	0xfffff000
/* ADMA block count? AU6621 only. */
#define REG_05	0x05
/* PIO */
#define AU6601_REG_BUFFER	0x08
/* ADMA ctrl? AU6621 only. */
#define REG_0C	0x0c
/* ADMA phy address. AU6621 only. */
#define REG_10	0x10
/* CMD index */
#define AU6601_REG_CMD_OPCODE	0x23
/* CMD parametr */
#define AU6601_REG_CMD_ARG	0x24
/* CMD response 4x4 Bytes */
#define AU6601_REG_CMD_RSP0	0x30
#define AU6601_REG_CMD_RSP1	0x34
#define AU6601_REG_CMD_RSP2	0x38
#define AU6601_REG_CMD_RSP3	0x3C
/* LED ctrl? */
#define REG_51	0x51
/* ??? */
#define REG_52	0x52
/* LED related? Always toggled BIT0 */
#define REG_61	0x61
/* Same as REG_61? */
#define REG_63	0x63
/* ??? */
#define REG_69	0x69
/* Block size for SDMA or PIO */
#define AU6601_REG_BLOCK_SIZE	0x6c
/* Some power related reg, used together with REG_7A */
#define REG_70	0x70
/* PLL ctrl */
#define AU6601_REG_PLL_CTRL	0x72
/* ??? */
#define REG_74	0x74
/* ??? */
#define REG_75	0x75
/* card slot state? */
#define REG_76	0x76
/* ??? */
#define REG_77	0x77
/* looks like soft reset? */
#define AU6601_REG_SW_RESET	0x79
 #define AU6601_RESET_UNK	BIT(7)	/* unknown bit */
 #define AU6601_RESET_DATA	BIT(3)
 #define AU6601_RESET_CMD	BIT(0)
/* see REG_70 */
#define REG_7A	0x7a
/* ??? Padding? Timeing? */
#define REG_7B	0x7b
/* ??? Padding? Timeing? */
#define REG_7C	0x7c
/* ??? Padding? Timeing? */
#define REG_7D	0x7d
/* read EEPROM? */
#define REG_7F	0x7f

#define AU6601_REG_CMD_CTRL	0x81
#define AU6601_REG_BUS_CTRL	0x82
 #define AU6601_BUS_WIDTH_4BIT	BIT(5)
#define REG_83	0x83

#define AU6601_REG_BUS_STATUS	0x84
 #define AU6601_BUS_STAT_CMD	BIT(15)
/* BIT(4) - BIT(7) are permanently 1.
 * May be reseved or not attached DAT4-DAT7 */
 #define AU6601_BUS_STAT_DAT3		BIT(3)
 #define AU6601_BUS_STAT_DAT2		BIT(2)
 #define AU6601_BUS_STAT_DAT1		BIT(1)
 #define AU6601_BUS_STAT_DAT0		BIT(0)
 #define AU6601_BUS_STAT_DAT_MASK	0xf
#define REG_85	0x85
/* ??? */
#define REG_86	0x86
#define AU6601_REG_INT_STATUS	0x90 /* IRQ intmask */
#define AU6601_REG_INT_ENABLE	0x94
/* ??? */
#define REG_A1	0xa1
/* ??? */
#define REG_A2	0xa2
/* ??? */
#define REG_A3	0xa3
/* ??? */
#define REG_B0	0xb0
/* ??? */
#define REG_B4	0xb4

 /* AU6601_REG_INT_STATUS is identical or almost identical with sdhci.h */
 /* OK - are tested and confirmed bits */
 #define  AU6601_INT_RESPONSE		0x00000001	/* ok */
 #define  AU6601_INT_DATA_END		0x00000002	/* fifo, ok */
 #define  AU6601_INT_BLK_GAP		0x00000004
 #define  AU6601_INT_DMA_END		0x00000008
 #define  AU6601_INT_SPACE_AVAIL	0x00000010	/* fifo, ok */
 #define  AU6601_INT_DATA_AVAIL		0x00000020	/* fifo, ok */
 #define  AU6601_INT_CARD_REMOVE	0x00000040
 #define  AU6601_INT_CARD_INSERT	0x00000080	/* 0x40 and 0x80 flip */
 #define  AU6601_INT_CARD_INT		0x00000100
 #define  AU6601_INT_ERROR		0x00008000	/* ok */
 #define  AU6601_INT_TIMEOUT		0x00010000	/* seems to be ok */
 #define  AU6601_INT_CRC		0x00020000	/* seems to be ok */
 #define  AU6601_INT_END_BIT		0x00040000
 #define  AU6601_INT_INDEX		0x00080000
 #define  AU6601_INT_DATA_TIMEOUT	0x00100000
 #define  AU6601_INT_DATA_CRC		0x00200000
 #define  AU6601_INT_DATA_END_BIT	0x00400000
 #define  AU6601_INT_BUS_POWER		0x00800000
 #define  AU6601_INT_ACMD12ERR		0x01000000
 #define  AU6601_INT_ADMA_ERROR		0x02000000

 #define  AU6601_INT_NORMAL_MASK	0x00007FFF
 #define  AU6601_INT_ERROR_MASK		0xFFFF8000

/* magic 0xF0001 */
 #define  AU6601_INT_CMD_MASK	(AU6601_INT_RESPONSE | AU6601_INT_TIMEOUT | \
		AU6601_INT_CRC | AU6601_INT_END_BIT | AU6601_INT_INDEX)
/* magic 0x70003A */
 #define  AU6601_INT_DATA_MASK	(AU6601_INT_DATA_END | AU6601_INT_DMA_END | \
		AU6601_INT_DATA_AVAIL | AU6601_INT_SPACE_AVAIL | \
		AU6601_INT_DATA_TIMEOUT | AU6601_INT_DATA_CRC | \
		AU6601_INT_DATA_END_BIT)
 #define AU6601_INT_ALL_MASK	((uint32_t)-1)

bool disable_dma = 0;

struct au6601_host {
	struct pci_dev *pdev;
	struct  device *dev;
	void __iomem *iobase;
	void __iomem *virt_base;
	dma_addr_t phys_base;

	struct mmc_host *mmc;
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	struct mmc_data *data;
	unsigned int data_early:1;      /* Data finished before cmd */
	unsigned int dma_on:1;
	unsigned int trigger_dma_dac:1;	/* Trigger Data after Command.
					 * In some cases data ragister
					 * should be triggered after
					 * command was done */

	struct mutex cmd_mutex;

	struct timer_list timer;

	struct sg_mapping_iter sg_miter;	/* SG state for PIO */
	unsigned int blocks;		/* remaining PIO blocks */
	unsigned int requested_blocks;		/* count of requested */
	int sg_count;	   /* Mapped sg entries */
};

static void au6601_send_cmd(struct au6601_host *host,
			    struct mmc_command *cmd);

static void au6601_prepare_data(struct au6601_host *host,
				struct mmc_command *cmd);
static void au6601_finish_data(struct au6601_host *host);
static void au6601_tasklet_finish(struct au6601_host *host);

static const struct pci_device_id pci_ids[] = {
	{
		.vendor	 = PCI_ID_ALCOR_MICRO,
		.device	 = PCI_ID_AU6601,
		.subvendor      = PCI_ANY_ID,
		.subdevice      = PCI_ANY_ID,
	},
	{ /* end: all zeroes */ },
};
MODULE_DEVICE_TABLE(pci, pci_ids);

static inline void au6601_rmw(void __iomem *reg, u32 clear, u32 set)
{
	u32 var;

	var = ioread32(reg);
	var &= ~clear;
	var |= set;
	iowrite32(var, reg);
}

static inline void au6601_mask_irqs(struct au6601_host *host)
{
	iowrite32(0, host->iobase + AU6601_REG_INT_ENABLE);
}

static inline void au6601_unmask_irqs(struct au6601_host *host)
{
	iowrite32(AU6601_INT_CMD_MASK | AU6601_INT_DATA_MASK |
		  AU6601_INT_CARD_INSERT | AU6601_INT_CARD_REMOVE |
		  AU6601_INT_CARD_INT | AU6601_INT_BUS_POWER,
		  host->iobase + AU6601_REG_INT_ENABLE);
}

static void au6601_clear_set_reg86(struct au6601_host *host, u32 clear, u32 set)
{
	au6601_rmw(host->iobase + REG_86, clear, set);
}

/*
 * check if one of data line is pulled down
 */
static inline int au6601_card_busy(struct au6601_host *host)
{
	u8 status;

	status = (ioread8(host->iobase + AU6601_REG_BUS_STATUS) &
		AU6601_BUS_STAT_DAT_MASK);
	/* If all data lines are up, then card is not busy */
	if (status == (AU6601_BUS_STAT_DAT0 | AU6601_BUS_STAT_DAT1 |
		       AU6601_BUS_STAT_DAT2 | AU6601_BUS_STAT_DAT3))
		return 0;

	return 1;
}

/* val = 0x1 abort command; 0x8 abort data? */
static void au6601_reset(struct au6601_host *host, u8 val)
{
	int i;
	iowrite8(val | AU6601_RESET_UNK, host->iobase + AU6601_REG_SW_RESET);
	for (i = 0; i < 100; i++) {
		if (!(ioread8(host->iobase + AU6601_REG_SW_RESET) & val))
			return;
		udelay(50);
	}
	dev_err(host->dev, "%s: timeout\n", __func__);
}

/*
 * - 0x8	only Vcc is on
 * - 0x1	Vcc and other pins are on
 * - 0x1 | 0x8	like 0x1, but DAT2 is off
 */
static void au6601_set_power(struct au6601_host *host,
			     unsigned int value, unsigned int set)
{
	u8 tmp1, tmp2;

	tmp1 = ioread8(host->iobase + REG_70);
	tmp2 = ioread8(host->iobase + REG_7A);
	if (set) {
		iowrite8(tmp1 | value, host->iobase + REG_70);
		iowrite8(tmp2 | value, host->iobase + REG_7A);
	} else {
		iowrite8(tmp2 & ~value, host->iobase + REG_7A);
		iowrite8(tmp1 & ~value, host->iobase + REG_70);
	}
}

static void au6601_trigger_data_transfer(struct au6601_host *host,
		unsigned int dma)
{
	struct mmc_data *data = host->data;
	u8 ctrl = 0;

	if (data->flags & MMC_DATA_WRITE)
		ctrl |= 0x80;

	if (dma) {
		iowrite32(host->phys_base, host->iobase + AU6601_REG_SDMA_ADDR);
		ctrl |= 0x40;
		host->dma_on = 1;

		if (data->flags & MMC_DATA_WRITE)
			goto done;
		/* prepare first DMA buffer for write operation */
		if (host->blocks > AU6601_MAX_DMA_BLOCKS)
			host->requested_blocks = AU6601_MAX_DMA_BLOCKS;
		else
			host->requested_blocks = host->blocks;

	}

done:
	iowrite32(data->blksz * host->requested_blocks,
		host->iobase + AU6601_REG_BLOCK_SIZE);
	iowrite8(ctrl | 0x1, host->iobase + REG_83);
}

/*****************************************************************************\
 *									     *
 * Core functions							     *
 *									     *
\*****************************************************************************/

static void au6601_read_block(struct au6601_host *host)
{
	size_t blksize, len, chunk;
	u32 uninitialized_var(scratch);
	void __iomem *virt_base = host->virt_base;
	u8 *buf;

	dev_dbg(host->dev, "PIO reading\n");

	blksize = host->data->blksz * host->requested_blocks;
	chunk = 0;

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		if (host->dma_on) {
			memcpy_fromio(buf, virt_base, len);
			virt_base += len;
			len = 0;
		} else {
			while (len) {
				if (chunk == 0) {
					scratch = ioread32(host->iobase +
							AU6601_REG_BUFFER);
					chunk = 4;
				}

				*buf = scratch & 0xFF;

				buf++;
				scratch >>= 8;
				chunk--;
				len--;
			}
		}
	}

	sg_miter_stop(&host->sg_miter);
}

static void au6601_write_block(struct au6601_host *host)
{
	size_t blksize, len, chunk;
	void __iomem *virt_base = host->virt_base;
	u32 scratch;
	u8 *buf;

	dev_dbg(host->dev, "PIO writing\n");

	blksize = host->data->blksz * host->requested_blocks;
	chunk = 0;
	scratch = 0;

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		if (host->dma_on) {
			memcpy_toio(virt_base, buf, len);
			virt_base += len;
			len = 0;
		} else {
			while (len) {
				scratch |= (u32)*buf << (chunk * 8);

				buf++;
				chunk++;
				len--;

				if ((chunk == 4) || ((len == 0)
						&& (blksize == 0))) {
					iowrite32(scratch, host->iobase +
						AU6601_REG_BUFFER);
					chunk = 0;
					scratch = 0;
				}
			}
		}
	}

	sg_miter_stop(&host->sg_miter);
}

static void au6601_transfer_data(struct au6601_host *host)
{
	if (host->blocks == 0)
		return;

	if (host->data->flags & MMC_DATA_READ)
		au6601_read_block(host);
	else
		au6601_write_block(host);

	host->blocks -= host->requested_blocks;
	if (host->dma_on) {
		host->dma_on = 0;
		if (host->blocks || (!host->blocks &&
				(host->data->flags & MMC_DATA_WRITE)))
			au6601_trigger_data_transfer(host, 1);
		else
			au6601_finish_data(host);
	}

	dev_dbg(host->dev, "PIO transfer complete.\n");
}

static void au6601_finish_command(struct au6601_host *host)
{
	struct mmc_command *cmd = host->cmd;

	if (host->cmd->flags & MMC_RSP_PRESENT) {
		cmd->resp[0] = ioread32be(host->iobase + AU6601_REG_CMD_RSP0);
		if (host->cmd->flags & MMC_RSP_136) {
			cmd->resp[1] =
				ioread32be(host->iobase + AU6601_REG_CMD_RSP1);
			cmd->resp[2] =
				ioread32be(host->iobase + AU6601_REG_CMD_RSP2);
			cmd->resp[3] =
				ioread32be(host->iobase + AU6601_REG_CMD_RSP3);
		}

	}

	host->cmd->error = 0;

	/* Finished CMD23, now send actual command. */
	if (host->cmd == host->mrq->sbc) {
		host->cmd = NULL;
		au6601_send_cmd(host, host->mrq->cmd);
	} else {
		/* Processed actual command. */
		if (!host->data)
			au6601_tasklet_finish(host);
		else if (host->data_early)
			au6601_finish_data(host);
		else if (host->trigger_dma_dac) {
			host->dma_on = 1;
			au6601_transfer_data(host);
		}

		host->cmd = NULL;
	}
}

static void au6601_finish_data(struct au6601_host *host)
{
	struct mmc_data *data;

	data = host->data;
	host->data = NULL;
	host->dma_on = 0;
	host->trigger_dma_dac = 0;

	/*
	 * The specification states that the block count register must
	 * be updated, but it does not specify at what point in the
	 * data flow. That makes the register entirely useless to read
	 * back so we have to assume that nothing made it to the card
	 * in the event of an error.
	 */
	if (data->error)
		data->bytes_xfered = 0;
	else
		data->bytes_xfered = data->blksz * data->blocks;

	/*
	 * Need to send CMD12 if -
	 * a) open-ended multiblock transfer (no CMD23)
	 * b) error in multiblock transfer
	 */
	if (data->stop &&
	    (data->error ||
	     !host->mrq->sbc)) {

		/*
		 * The controller needs a reset of internal state machines
		 * upon error conditions.
		 */
		if (data->error) {
			au6601_reset(host, AU6601_RESET_CMD);
			au6601_reset(host, AU6601_RESET_DATA);
		}
		au6601_send_cmd(host, data->stop);
	} else
		au6601_tasklet_finish(host);
}

static void au6601_prepare_sg_miter(struct au6601_host *host)
{
	unsigned int flags = SG_MITER_ATOMIC;
	struct mmc_data *data = host->data;

	if (data->flags & MMC_DATA_READ)
		flags |= SG_MITER_TO_SG;
	else
		flags |= SG_MITER_FROM_SG;
	sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);
}

static void au6601_prepare_data(struct au6601_host *host,
				struct mmc_command *cmd)
{
	unsigned int dma = 0;
	struct mmc_data *data = cmd->data;

	if (!data)
		return;

	/* Sanity checks */
	BUG_ON(data->blksz * data->blocks > 524288);
	BUG_ON(data->blksz > host->mmc->max_blk_size);
	BUG_ON(data->blocks > AU6601_MAX_BLOCK_COUNT);

	host->data = data;
	host->data_early = 0;
	host->data->bytes_xfered = 0;
	host->requested_blocks = 1;

	au6601_prepare_sg_miter(host);
	host->blocks = data->blocks;

	if (!disable_dma &&
			host->blocks > 1 &&
			data->blksz == host->mmc->max_blk_size) {
		dma = 1;

		if (data->flags & MMC_DATA_WRITE) {
			/* prepare first write buffer */
			/* Don't trigger data transfer now.
			 * DMA may start it too eraly */
			host->trigger_dma_dac = 1;
			return;
		}
	}

	au6601_trigger_data_transfer(host, dma);
}

static void au6601_send_cmd(struct au6601_host *host,
			    struct mmc_command *cmd)
{
	u8 ctrl; /*some mysterious flags and control */
	unsigned long timeout;

	timeout = jiffies;
	if (!cmd->data && cmd->busy_timeout > 9000)
		timeout += DIV_ROUND_UP(cmd->busy_timeout, 1000) * HZ + HZ;
	else
		timeout += 10 * HZ;
	mod_timer(&host->timer, timeout);

	host->cmd = cmd;
	au6601_prepare_data(host, cmd);

	iowrite8(cmd->opcode | 0x40, host->iobase + AU6601_REG_CMD_OPCODE);
	iowrite32be(cmd->arg, host->iobase + AU6601_REG_CMD_ARG);

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		ctrl = 0;
		break;
	case MMC_RSP_R1:
		ctrl = 0x40;
		break;
	case MMC_RSP_R1B:
		ctrl = 0x40 | 0x10;
		break;
	case MMC_RSP_R2:
		ctrl = 0xc0;
		break;
	case MMC_RSP_PRESENT | MMC_RSP_OPCODE:
	case MMC_RSP_R3:
		ctrl = 0x80;
		break;
	default:
		dev_err(host->dev, "%s: cmd->flag (0x%02x) is not valid\n",
			mmc_hostname(host->mmc), mmc_resp_type(cmd));
		break;
	}

	iowrite8(ctrl | 0x20, host->iobase + AU6601_REG_CMD_CTRL);
}

/*****************************************************************************\
 *									     *
 * Interrupt handling							     *
 *									     *
\*****************************************************************************/

static void au6601_cmd_irq(struct au6601_host *host, u32 intmask)
{
	if (!host->cmd) {
		dev_err(host->dev,
			"Got command interrupt 0x%08x even though no command operation was in progress.\n",
			intmask);
		return;
	}

	if (intmask & AU6601_INT_TIMEOUT)
		host->cmd->error = -ETIMEDOUT;
	else if (intmask & (AU6601_INT_CRC | AU6601_INT_END_BIT |
			AU6601_INT_INDEX))
		host->cmd->error = -EILSEQ;

	if (host->cmd->error) {
		au6601_tasklet_finish(host);
		return;
	}

	/*
	 * The host can send and interrupt when the busy state has
	 * ended, allowing us to wait without wasting CPU cycles.
	 * Unfortunately this is overloaded on the "data complete"
	 * interrupt, so we need to take some care when handling
	 * it.
	 *
	 * Note: The 1.0 specification is a bit ambiguous about this
	 *       feature so there might be some problems with older
	 *       controllers.
	 */
	if (host->cmd->flags & MMC_RSP_BUSY) {
		if (host->cmd->data)
			dev_warn(host->dev,
				 "Cannot wait for busy signal when also doing a data transfer");
	}

	if (intmask & AU6601_INT_RESPONSE)
		au6601_finish_command(host);
}

static void au6601_data_irq(struct au6601_host *host, u32 intmask)
{
	if (!host->data) {
		/* FIXME: Ist is same for AU6601
		 * The "data complete" interrupt is also used to
		 * indicate that a busy state has ended. See comment
		 * above in au6601_cmd_irq().
		 */
		if (host->cmd && (host->cmd->flags & MMC_RSP_BUSY)) {
			if (intmask & AU6601_INT_DATA_END) {
				au6601_finish_command(host);
				return;
			}
		}

		dev_err(host->dev,
			"Got data interrupt 0x%08x even though no data operation was in progress.\n",
			(unsigned)intmask);

		if (intmask & AU6601_INT_ERROR_MASK) {
			host->cmd->error = -ETIMEDOUT;
			au6601_tasklet_finish(host);
		}
		return;
	}

	if (intmask & AU6601_INT_DATA_TIMEOUT)
		host->data->error = -ETIMEDOUT;
	else if (intmask & AU6601_INT_DATA_END_BIT)
		host->data->error = -EILSEQ;
	else if (intmask & AU6601_INT_DATA_CRC)
		host->data->error = -EILSEQ;

	if (host->data->error)
		au6601_finish_data(host);
	else {
		if (intmask & (AU6601_INT_DATA_AVAIL | AU6601_INT_SPACE_AVAIL))
			au6601_transfer_data(host);

		if (intmask & AU6601_INT_DATA_END) {
			if (host->cmd) {
				/*
				 * Data managed to finish before the
				 * command completed. Make sure we do
				 * things in the proper order.
				 */
				host->data_early = 1;
			} else if (host->blocks && !host->dma_on) {
				/*
				 * Probably we do multi block operation.
				 * Prepare PIO for next block.
				 */
				au6601_trigger_data_transfer(host, 0);
			} else if (host->blocks && host->dma_on) {
				au6601_transfer_data(host);
			} else {
				if (host->dma_on)
					au6601_transfer_data(host);
				au6601_finish_data(host);
			}
		}
	}
}

static irqreturn_t au6601_irq(int irq, void *d)
{
	struct au6601_host *host = d;
	irqreturn_t ret = IRQ_HANDLED;
	u32 intmask;

	mutex_lock(&host->cmd_mutex);

	intmask = ioread32(host->iobase + AU6601_REG_INT_STATUS);
	iowrite32(intmask, host->iobase + AU6601_REG_INT_STATUS);

	/* some thing bad */
	if (unlikely(!intmask || intmask == AU6601_INT_ALL_MASK)) {
		dev_warn(host->dev, "impossible IRQ %x\n", intmask);
		ret = IRQ_NONE;
		goto exit;
	}

	if (intmask & AU6601_INT_CMD_MASK) {
		dev_dbg(host->dev, "CMD IRQ %x\n", intmask);

		au6601_cmd_irq(host, intmask & AU6601_INT_CMD_MASK);
		intmask &= ~AU6601_INT_CMD_MASK;
	}

	if (intmask & AU6601_INT_DATA_MASK) {
		dev_dbg(host->dev, "DATA IRQ %x\n", intmask);
		au6601_data_irq(host, intmask & AU6601_INT_DATA_MASK);
		intmask &= ~AU6601_INT_DATA_MASK;
	}

	if (intmask & (AU6601_INT_CARD_INSERT | AU6601_INT_CARD_REMOVE)) {
		/* this check can be remove */
		if (intmask & AU6601_INT_CARD_REMOVE)
			dev_dbg(host->dev, "card removed\n");
		else
			dev_dbg(host->dev, "card inserted\n");

		intmask &= ~(AU6601_INT_CARD_INSERT | AU6601_INT_CARD_REMOVE);
		mmc_detect_change(host->mmc, msecs_to_jiffies(200));
	}

	if (intmask & 0x100) {
		dev_warn(host->dev,
			 "0x100 (card INT?) got unknown IRQ with %x\n",
			 intmask);
		intmask &= ~0x100;
	}

	if (intmask & 0xFFFF7FFF) {
		dev_warn(host->dev, "0xFFFF7FFF got unhandled IRQ with %x\n",
			 intmask);
	}

exit:
	mutex_unlock(&host->cmd_mutex);
	return ret;
}

static void au6601_sdc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct au6601_host *host;

	host = mmc_priv(mmc);
	mutex_lock(&host->cmd_mutex);

	host->mrq = mrq;

	/* check if card is present then send command and data */
	if (ioread8(host->iobase + REG_76) & 0x1)
		au6601_send_cmd(host, mrq->cmd);
	else {
		mrq->cmd->error = -ENOMEDIUM;
		au6601_tasklet_finish(host);
	}

	mutex_unlock(&host->cmd_mutex);
}

static void au6601_pre_req(struct mmc_host *mmc,
			   struct mmc_request *mrq,
			   bool is_first_req)
{
	//printk("pre %p\n", mrq);
}

static void au6601_post_req(struct mmc_host *mmc,
			    struct mmc_request *mrq,
			    int err)
{
	//printk("post %p\n", mrq);
}

static void au6601_set_clock(struct au6601_host *host, unsigned int clock)
{
	unsigned int div = 0, mult = 0, ctrl = 0x1;

	/* FIXME: mesuered and calculated values are different.
	 * the clock is unstable in some mult/div combinations.
	 */
	if (clock >= MHZ_TO_HZ(208)) {
		mult = 0xb0;	/* 30 * ? / 2 = ?MHz */
		div = 2;
	} else if (clock >= MHZ_TO_HZ(194)) {
		mult = 0x30;	/* 30 * 14 / 2 = 210MHz */
		div = 2;
	} else if (clock >= MHZ_TO_HZ(130)) {
		mult = 0x30;	/* 30 * 14 / 3 = 140MHz */
		div = 3;
	} else if (clock >= MHZ_TO_HZ(100)) {
		mult = 0x30;	/* 30 * 14 / 4 = 105MHz */
		div = 4;
	} else if (clock >= MHZ_TO_HZ(80)) {
		mult = 0x30;	/* 30 * 14 / 5 = 84MHz */
		div = 5;
	} else if (clock >= MHZ_TO_HZ(60)) {
		mult = 0x30;	/* 30 * 14 / 7 = 60MHz */
		div = 7;
	} else if (clock >= MHZ_TO_HZ(50)) {
		mult = 0x10;	/* 30 * 2 / 1 = 60MHz */
		div = 1;
	} else if (clock >= MHZ_TO_HZ(40)) {
		mult = 0x30;	/* 30 * 14 / 10 = 42MHz */
		div = 10;
	} else if (clock >= MHZ_TO_HZ(25)) {
		mult = 0x10;	/* 30 * 2 / 2 = 30MHz */
		div = 2;
	} else if (clock >= MHZ_TO_HZ(20)) {
		mult = 0x20;	/* 30 * 4 / 7 = 17MHz */
		div = 7;
	} else if (clock >= MHZ_TO_HZ(10)) {
		mult = 0x10;	/* 30 * 2 / 5 = 12MHz */
		div = 5;
	} else if (clock >= MHZ_TO_HZ(5)) {
		mult = 0x10;	/* 30 * 2 / 10 = 6MHz */
		div = 10;
	} else if (clock >= MHZ_TO_HZ(1)) {
		mult = 0x0;	/* 30 / 16 = 1,8 MHz */
		div = 16;
	} else if (clock == 0) {
		ctrl = 0;
	} else {
		mult = 0x0;	/* reversed 150 * 200 = 30MHz */
		div = 200;	/* 150 KHZ mesured */
	}
	dev_dbg(host->dev, "set freq %d, %x, %x\n", clock, div, mult);
	iowrite16((div - 1) << 8 | mult | ctrl,
		  host->iobase + AU6601_REG_PLL_CTRL);
}

static void au6601_sdc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct au6601_host *host;

	host = mmc_priv(mmc);
	mutex_lock(&host->cmd_mutex);

	iowrite8(0, host->iobase + REG_85);
	iowrite8(0x31, host->iobase + REG_7B);
	iowrite8(0x33, host->iobase + REG_7C);
	iowrite8(1, host->iobase + REG_75);
	iowrite8(0, host->iobase + REG_85);

	if (ios->bus_width == MMC_BUS_WIDTH_1) {
		iowrite8(0x0,
			 host->iobase + AU6601_REG_BUS_CTRL);
		au6601_clear_set_reg86(host, 0xc0, 0);
	} else if (ios->bus_width == MMC_BUS_WIDTH_4) {
		iowrite8(AU6601_BUS_WIDTH_4BIT,
			 host->iobase + AU6601_REG_BUS_CTRL);
		au6601_clear_set_reg86(host, 0, 0xc0);
	} else
		dev_err(host->dev, "Unknown BUS mode\n");

	au6601_set_clock(host, ios->clock);

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		au6601_set_power(host, 0x1 | 0x8, 0);
		break;
	case MMC_POWER_UP:
		au6601_set_power(host, 0x8, 1);
		break;
	case MMC_POWER_ON:
		au6601_set_power(host, 0x1, 1);
		au6601_set_power(host, 0x8, 0);
		break;
	default:
		dev_err(host->dev, "Unknown power parametr\n");
	}

	iowrite8(0x80, host->iobase + REG_83);
	iowrite8(0x7d, host->iobase + REG_69);
	ioread8(host->iobase + REG_74);
	mutex_unlock(&host->cmd_mutex);
}

static int au6601_ops_card_busy(struct mmc_host *mmc)
{
	struct au6601_host *host;
	host = mmc_priv(mmc);

	return au6601_card_busy(host);
}

static const struct mmc_host_ops au6601_sdc_ops = {
	.pre_req	= au6601_pre_req,
	.request	= au6601_sdc_request,
	.post_req	= au6601_post_req,
	.set_ios	= au6601_sdc_set_ios,

	.card_busy	= au6601_ops_card_busy,
};

static void au6601_tasklet_finish(struct au6601_host *host)
{
	struct mmc_request *mrq;

	/*
	 * If this tasklet gets rescheduled while running, it will
	 * be run again afterwards but without any active request.
	 */
	if (!host->mrq) {
		return;
	}

	del_timer(&host->timer);

	mrq = host->mrq;

	/*
	 * The controller needs a reset of internal state machines
	 * upon error conditions.
	 */
	if ((mrq->cmd && mrq->cmd->error) ||
		 (mrq->data && (mrq->data->error ||
		  (mrq->data->stop && mrq->data->stop->error)))) {

		au6601_reset(host, AU6601_RESET_CMD);
		au6601_reset(host, AU6601_RESET_DATA);
	}

	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;
	host->dma_on = 0;
	host->trigger_dma_dac = 0;

	mmc_request_done(host->mmc, mrq);
}

static void au6601_timeout_timer(unsigned long data)
{
	struct au6601_host *host;

	host = (struct au6601_host *)data;

	mutex_lock(&host->cmd_mutex);

	if (host->mrq) {
		dev_err(host->dev,
			"Timeout waiting for hardware interrupt.\n");

		if (host->data) {
			host->data->error = -ETIMEDOUT;
			au6601_finish_data(host);
		} else {
			if (host->cmd)
				host->cmd->error = -ETIMEDOUT;
			else
				host->mrq->cmd->error = -ETIMEDOUT;

			au6601_tasklet_finish(host);
		}
	}

	mmiowb();
	mutex_unlock(&host->cmd_mutex);
}



static void au6601_init_mmc(struct au6601_host *host)
{
	struct mmc_host *mmc = host->mmc;

	mmc->f_min = AU6601_MIN_CLOCK;
	mmc->f_max = AU6601_MAX_CLOCK;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_SD_HIGHSPEED;
	mmc->ops = &au6601_sdc_ops;

	/* Hardware cannot do scatter lists? */
	mmc->max_segs = AU6601_MAX_SEGMENTS;

	mmc->max_blk_size = AU6601_MAX_BLOCK_LENGTH;
	mmc->max_blk_count = AU6601_MAX_BLOCK_COUNT;

	mmc->max_seg_size = AU6601_MAX_BLOCK_LENGTH * AU6601_MAX_DMA_BLOCKS;
	mmc->max_req_size = mmc->max_seg_size * mmc->max_segs;
}

static void au6601_hw_init(struct au6601_host *host)
{

	iowrite8(0, host->iobase + REG_74);

	iowrite8(0, host->iobase + REG_76);
	/* disable DlinkMode? disabled by default. */
	iowrite8(0x80, host->iobase + REG_76);

	au6601_reset(host, AU6601_RESET_CMD);

	iowrite8(0x0, host->iobase + REG_05);
	iowrite8(0x1, host->iobase + REG_75);

	au6601_unmask_irqs(host);

	iowrite32(0x0, host->iobase + AU6601_REG_BUS_CTRL);

	au6601_reset(host, AU6601_RESET_DATA);

	iowrite8(0x0, host->iobase + REG_05);
	iowrite8(0x0, host->iobase + REG_85);
	iowrite8(0x8, host->iobase + REG_75);
	iowrite32(0x3d00fa, host->iobase + REG_B4);

	au6601_set_power(host, 0x1, 0);
	au6601_set_power(host, 0x8, 0);

	host->dma_on = 0;
}

static int __init au6601_dma_alloc(struct au6601_host *host)
{
	int ret;

	ret = pci_set_dma_mask(host->pdev, AU6601_SDMA_MASK);
	if (ret) {
		dev_err(host->dev, "Failed to set DMA mask\n");
		return ret;
	}

	host->virt_base = dmam_alloc_coherent(host->dev,
		AU6601_MAX_BLOCK_LENGTH * AU6601_MAX_DMA_BLOCKS * AU6601_DMA_LOCAL_SEGMENTS,
		&host->phys_base, GFP_KERNEL);

	if (!host->virt_base) {
		dev_err(host->dev, "Failed to alloc DMA\n");
		return -ENOMEM;
	}

	return 0;
}

static int __init au6601_pci_probe(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct mmc_host *mmc;
	struct au6601_host *host;
	int ret, bar = 0;

	dev_info(&pdev->dev, "AU6601 controller found [%04x:%04x] (rev %x)\n",
		 (int)pdev->vendor, (int)pdev->device, (int)pdev->revision);

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "BAR %d is not iomem. Aborting.\n", bar);
		return -ENODEV;
	}


	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	/* FIXME: create managed version of mmc_alloc_host and use it */
	mmc = mmc_alloc_host(sizeof(struct au6601_host *), &pdev->dev);
	if (!mmc) {
		dev_err(&pdev->dev, "Can't allocate MMC\n");
		return -ENOMEM;
	}

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->pdev = pdev;
	host->dev = &pdev->dev;

	ret = pci_request_region(pdev, bar, DRVNAME);
	if (ret) {
		dev_err(&pdev->dev, "Cannot request region\n");
		return -ENOMEM;
	}

	host->iobase = pcim_iomap(pdev, bar, 0);
	if (!host->iobase)
		return -ENOMEM;

	ret = devm_request_threaded_irq(&pdev->dev, pdev->irq, NULL, au6601_irq,
			IRQF_ONESHOT, "au6601 host",
			host);

	if (ret) {
		dev_err(&pdev->dev, "Failed to get irq for data line\n");
		return -ENOMEM;
	}

	ret = au6601_dma_alloc(host);
	if (ret)
		return ret;

	pci_set_master(pdev);
	pci_set_drvdata(pdev, host);

	mutex_init(&host->cmd_mutex);
	/*
	 * Init tasklets.
	 */
	setup_timer(&host->timer, au6601_timeout_timer, (unsigned long)host);

	au6601_init_mmc(host);
	au6601_hw_init(host);

	mmc_add_host(mmc);
	return 0;
}

static void au6601_hw_uninit(struct au6601_host *host)
{
	iowrite8(0x0, host->iobase + REG_76);
	au6601_mask_irqs(host);

	au6601_set_power(host, 0x1, 0);

	iowrite8(0x0, host->iobase + REG_85);
	iowrite8(0x0, host->iobase + REG_B4);

	au6601_set_power(host, 0x8, 0);
}

static void __exit au6601_pci_remove(struct pci_dev *pdev)
{
	struct au6601_host *host;

	host = pci_get_drvdata(pdev);

	au6601_hw_uninit(host);

	del_timer_sync(&host->timer);

	mmc_remove_host(host->mmc);
	mmc_free_host(host->mmc);
}

#ifdef CONFIG_PM_SLEEP
static int au6601_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct au6601_host *host = pci_get_drvdata(pdev);

	au6601_hw_uninit(host);
	return 0;
}

static int au6601_resume(struct device *dev)
{

	struct pci_dev *pdev = to_pci_dev(dev);
	struct au6601_host *host = pci_get_drvdata(pdev);

	au6601_hw_init(host);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(au6601_pm_ops, au6601_suspend, au6601_resume);

static struct pci_driver au6601_driver = {
	.name	=	 DRVNAME,
	.id_table =     pci_ids,
	.probe	=	au6601_pci_probe,
	.remove =       au6601_pci_remove,
	.driver	=	{
		.pm	= &au6601_pm_ops
	},
};

module_pci_driver(au6601_driver);

module_param(disable_dma, bool, S_IRUGO);
MODULE_PARM_DESC(disable_dma, "Disable DMA");

MODULE_AUTHOR("Oleksij Rempel <linux@rempel-privat.de>");
MODULE_DESCRIPTION("PCI driver for Alcor Micro AU6601 Secure Digital Host Controller Interface");
MODULE_LICENSE("GPL");
