/*
 * Texas Instruments DA8xx/OMAP-L1x "glue layer"
 *
 * Copyright (c) 2008-2009 MontaVista Software, Inc. <source@mvista.com>
 *
 * Based on the DaVinci "glue layer" code.
 * Copyright (C) 2005-2006 by Texas Instruments
 *
 * This file is part of the Inventra Controller Driver for Linux.
 *
 * The Inventra Controller Driver for Linux is free software; you
 * can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * The Inventra Controller Driver for Linux is distributed in
 * the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with The Inventra Controller Driver for Linux ; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>

#include <mach/da8xx.h>
#include <mach/usb.h>

#include "musb_core.h"
#include "cppi41.h"
#include "cppi41_dma.h"

/*
 * DA8XX specific definitions
 */

/* USB 2.0 OTG module registers */
#define DA8XX_USB_REVISION_REG	0x00
#define DA8XX_USB_CTRL_REG	0x04
#define DA8XX_USB_STAT_REG	0x08
#define DA8XX_USB_EMULATION_REG 0x0c
#define DA8XX_USB_MODE_REG	0x10	/* Transparent, CDC, [Generic] RNDIS */
#define DA8XX_USB_AUTOREQ_REG	0x14
#define DA8XX_USB_SRP_FIX_TIME_REG 0x18
#define DA8XX_USB_TEARDOWN_REG	0x1c
#define DA8XX_USB_INTR_SRC_REG	0x20
#define DA8XX_USB_INTR_SRC_SET_REG 0x24
#define DA8XX_USB_INTR_SRC_CLEAR_REG 0x28
#define DA8XX_USB_INTR_MASK_REG 0x2c
#define DA8XX_USB_INTR_MASK_SET_REG 0x30
#define DA8XX_USB_INTR_MASK_CLEAR_REG 0x34
#define DA8XX_USB_INTR_SRC_MASKED_REG 0x38
#define DA8XX_USB_END_OF_INTR_REG 0x3c
#define DA8XX_USB_GENERIC_RNDIS_EP_SIZE_REG(n) (0x50 + (((n) - 1) << 2))

/* Control register bits */
#define DA8XX_SOFT_RESET_MASK	1

#define DA8XX_USB_TX_EP_MASK	0x1f		/* EP0 + 4 Tx EPs */
#define DA8XX_USB_RX_EP_MASK	0x1e		/* 4 Rx EPs */

/* USB interrupt register bits */
#define DA8XX_INTR_USB_SHIFT	16
#define DA8XX_INTR_USB_MASK	(0x1ff << DA8XX_INTR_USB_SHIFT) /* 8 Mentor */
					/* interrupts and DRVVBUS interrupt */
#define DA8XX_INTR_DRVVBUS	0x100
#define DA8XX_INTR_RX_SHIFT	8
#define DA8XX_INTR_RX_MASK	(DA8XX_USB_RX_EP_MASK << DA8XX_INTR_RX_SHIFT)
#define DA8XX_INTR_TX_SHIFT	0
#define DA8XX_INTR_TX_MASK	(DA8XX_USB_TX_EP_MASK << DA8XX_INTR_TX_SHIFT)
#define A_WAIT_BCON_TIMEOUT     1100            /* in ms */
#define DA8XX_MENTOR_CORE_OFFSET 0x400

#define CFGCHIP2	IO_ADDRESS(DA8XX_SYSCFG0_BASE + DA8XX_CFGCHIP2_REG)

struct da8xx_glue {
	struct device		*dev;
	struct platform_device	*musb;
	struct clk		*clk;
};

#ifdef CONFIG_USB_TI_CPPI41_DMA
#define CPPI41_QMGR_REG0SIZE	0x3fff
/*
 * CPPI 4.1 resources used for USB OTG controller module:
 *
 * USB   DMA  DMA  QMgr  Tx     Src
 *       Tx   Rx         QNum   Port
 * ---------------------------------
 * EP0   0    0    0     16,17  1
 * ---------------------------------
 * EP1   1    1    0     18,19  2
 * ---------------------------------
 * EP2   2    2    0     20,21  3
 * ---------------------------------
 * EP3   3    3    0     22,23  4
 * ---------------------------------
 */

static u16 tx_comp_q[] = { 24, 24, 24, 24 };
static u16 rx_comp_q[] = { 26, 26, 26, 26 };

/* DMA block configuration */
static const struct cppi41_tx_ch tx_ch_info[] = {
	[0] = {
		.port_num       = 1,
		.num_tx_queue   = 2,
		.tx_queue       = { { 0, 16 }, { 0, 17 } }
	},
	[1] = {
		.port_num       = 2,
		.num_tx_queue   = 2,
		.tx_queue       = { { 0, 18 }, { 0, 19 } }
	},
	[2] = {
		.port_num       = 3,
		.num_tx_queue   = 2,
		.tx_queue       = { { 0, 20 }, { 0, 21 } }
	},
	[3] = {
		.port_num       = 4,
		.num_tx_queue   = 2,
		.tx_queue       = { { 0, 22 }, { 0, 23 } }
	}
};

#define DA8XX_USB0_CFG_BASE IO_ADDRESS(DA8XX_USB0_BASE)

/* Queues 0 to 27 are pre-assigned, others are spare */
static const u32 assigned_queues[] = { 0x0fffffff, 0 };

/* Fair scheduling */
u32 dma_sched_table[] = {
	0x81018000, 0x83038202,
};

int __devinit cppi41_init(struct musb *musb)
{
	struct usb_cppi41_info *cppi_info = &usb_cppi41_info[0];
	u16 numch, blknum, order, i;

	/* init cppi info structure  */
	cppi_info->dma_block = 0;
	for (i = 0 ; i < USB_CPPI41_NUM_CH ; i++)
		cppi_info->ep_dma_ch[i] = i;

	cppi_info->q_mgr = 0;
	cppi_info->num_tx_comp_q = 2;
	cppi_info->num_rx_comp_q = 2;
	cppi_info->tx_comp_q = tx_comp_q;
	cppi_info->rx_comp_q = rx_comp_q;
#ifdef CONFIG_USB_TI_CPPI41_IN_TRANSPARENT
	cppi_info->grndis_for_host_rx  = 0;
#else
	cppi_info->grndis_for_host_rx  = 1;
#endif
	//cppi_info->bd_intr_ctrl = 0; /* am35x dont support bd interrupt */

	blknum = cppi_info->dma_block;

	/* Queue manager information */
	cppi41_queue_mgr[0].num_queue = 64;
	cppi41_queue_mgr[0].queue_types = CPPI41_FREE_DESC_BUF_QUEUE |
						CPPI41_UNASSIGNED_QUEUE;
	cppi41_queue_mgr[0].base_fdbq_num = 0;
	cppi41_queue_mgr[0].assigned = assigned_queues;

	/* init mappings */
	cppi41_queue_mgr[0].q_mgr_rgn_base = DA8XX_USB0_CFG_BASE + 0x4000;
	cppi41_queue_mgr[0].desc_mem_rgn_base = DA8XX_USB0_CFG_BASE + 0x5000;
	cppi41_queue_mgr[0].q_mgmt_rgn_base = DA8XX_USB0_CFG_BASE + 0x6000;
	cppi41_queue_mgr[0].q_stat_rgn_base = DA8XX_USB0_CFG_BASE + 0x6800;

	/* init DMA block */
	cppi41_dma_block[0].num_tx_ch = 4;
	cppi41_dma_block[0].num_rx_ch = 4;
	cppi41_dma_block[0].tx_ch_info = tx_ch_info;

	cppi41_dma_block[0].global_ctrl_base = DA8XX_USB0_CFG_BASE + 0x1000;
	cppi41_dma_block[0].ch_ctrl_stat_base = DA8XX_USB0_CFG_BASE + 0x1800;
	cppi41_dma_block[0].sched_ctrl_base = DA8XX_USB0_CFG_BASE + 0x2000;
	cppi41_dma_block[0].sched_table_base = DA8XX_USB0_CFG_BASE + 0x2800;

	/* Initialize for Linking RAM region 0 alone */
	cppi41_queue_mgr_init(cppi_info->q_mgr, 0,
			USB_CPPI41_QMGR_REG0_ALLOC_SIZE);

	numch =  USB_CPPI41_NUM_CH * 2;
	order = get_count_order(numch);
	cppi41_dma_block[0].num_max_ch = numch;

	/* TODO: check two teardown desc per channel (5 or 7 ?)*/
	if (order < 5)
		order = 5;

	cppi41_dma_block_init(blknum, cppi_info->q_mgr, order,
			dma_sched_table, numch);
	return 0;
}

EXPORT_SYMBOL(cppi41_init);
#endif /* CONFIG_USB_TI_CPPI41_DMA */

void da8xx_musb_enable_sof(struct musb *musb)
{
	void __iomem *reg_base = musb->ctrl_base;

	musb_writeb(musb->mregs, MUSB_INTRUSBE, MUSB_INTR_SOF |
	musb_readb(musb->mregs, MUSB_INTRUSBE));
	musb_writel(reg_base, DA8XX_USB_INTR_SRC_SET_REG, MUSB_INTR_SOF |
		musb_readl(reg_base, DA8XX_USB_INTR_SRC_SET_REG));
}

void da8xx_musb_disable_sof(struct musb *musb)
{
	void __iomem *reg_base = musb->ctrl_base;
	u8 intrusb;

	intrusb = musb_readb(musb->mregs, MUSB_INTRUSBE);
	intrusb &= ~MUSB_INTR_SOF;
	musb_writeb(musb->mregs, MUSB_INTRUSBE, intrusb);
	musb_writel(reg_base, DA8XX_USB_INTR_MASK_CLEAR_REG, MUSB_INTR_SOF);
}

/*
 * REVISIT (PM): we should be able to keep the PHY in low power mode most
 * of the time (24 MHz oscillator and PLL off, etc.) by setting POWER.D0
 * and, when in host mode, autosuspending idle root ports... PHY_PLLON
 * (overriding SUSPENDM?) then likely needs to stay off.
 */

static inline void phy_on(void)
{
	u32 cfgchip2 = readl(CFGCHIP2);

	/*
	 * Start the on-chip PHY and its PLL.
	 */
	cfgchip2 &= ~(CFGCHIP2_RESET | CFGCHIP2_PHYPWRDN | CFGCHIP2_OTGPWRDN);
	cfgchip2 |= CFGCHIP2_PHY_PLLON;
	writel(cfgchip2, CFGCHIP2);

	pr_info("Waiting for USB PHY clock good...\n");
	while (!(readl(CFGCHIP2) & CFGCHIP2_PHYCLKGD))
		cpu_relax();
}

static inline void phy_off(void)
{
	u32 cfgchip2 = readl(CFGCHIP2);

	/*
	 * Ensure that USB 1.1 reference clock is not being sourced from
	 * USB 2.0 PHY.  Otherwise do not power down the PHY.
	 */
	if (!(cfgchip2 & CFGCHIP2_USB1PHYCLKMUX) &&
	     (cfgchip2 & CFGCHIP2_USB1SUSPENDM)) {
		pr_warning("USB 1.1 clocked from USB 2.0 PHY -- "
			   "can't power it down\n");
		return;
	}

	/*
	 * Power down the on-chip PHY.
	 */
	cfgchip2 |= CFGCHIP2_PHYPWRDN | CFGCHIP2_OTGPWRDN;
	writel(cfgchip2, CFGCHIP2);
}

/*
 * Because we don't set CTRL.UINT, it's "important" to:
 *	- not read/write INTRUSB/INTRUSBE (except during
 *	  initial setup, as a workaround);
 *	- use INTSET/INTCLR instead.
 */

/**
 * da8xx_musb_enable - enable interrupts
 */
static void da8xx_musb_enable(struct musb *musb)
{
	void __iomem *reg_base = musb->ctrl_base;
	u32 mask;

	/* Workaround: setup IRQs through both register sets. */
	mask = ((musb->epmask & DA8XX_USB_TX_EP_MASK) << DA8XX_INTR_TX_SHIFT) |
	       ((musb->epmask & DA8XX_USB_RX_EP_MASK) << DA8XX_INTR_RX_SHIFT) |
	       DA8XX_INTR_USB_MASK;
	musb_writel(reg_base, DA8XX_USB_INTR_MASK_SET_REG, mask);

	/* Force the DRVVBUS IRQ so we can start polling for ID change. */
	if (is_otg_enabled(musb)) {
		musb_writel(reg_base, DA8XX_USB_INTR_SRC_SET_REG,
			    DA8XX_INTR_DRVVBUS << DA8XX_INTR_USB_SHIFT);
	}
}

/**
 * da8xx_musb_disable - disable HDRC and flush interrupts
 */
static void da8xx_musb_disable(struct musb *musb)
{
	void __iomem *reg_base = musb->ctrl_base;

	musb_writel(reg_base, DA8XX_USB_INTR_MASK_CLEAR_REG,
		    DA8XX_INTR_USB_MASK |
		    DA8XX_INTR_TX_MASK | DA8XX_INTR_RX_MASK);
	musb_writeb(musb->mregs, MUSB_DEVCTL, 0);
	musb_writel(reg_base, DA8XX_USB_END_OF_INTR_REG, 0);
}

#define portstate(stmt)		stmt

static void da8xx_musb_set_vbus(struct musb *musb, int is_on)
{
	WARN_ON(is_on && is_peripheral_enabled(musb));
}

#define	POLL_SECONDS	2

static struct timer_list otg_workaround;

static void otg_timer(unsigned long _musb)
{
	struct musb		*musb = (void *)_musb;
	void __iomem		*mregs = musb->mregs;
	u8			devctl;
	unsigned long		flags;

	/*
	 * We poll because DaVinci's won't expose several OTG-critical
	 * status change events (from the transceiver) otherwise.
	 */
	devctl = musb_readb(mregs, MUSB_DEVCTL);
	dev_dbg(musb->controller, "Poll devctl %02x (%s)\n", devctl,
		otg_state_string(musb->xceiv->state));

	spin_lock_irqsave(&musb->lock, flags);
	switch (musb->xceiv->state) {
	case OTG_STATE_A_WAIT_BCON:
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl & MUSB_DEVCTL_BDEVICE) {
			musb->xceiv->state = OTG_STATE_B_IDLE;
			MUSB_DEV_MODE(musb);
		} else {
			musb->xceiv->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
		}
		break;
	case OTG_STATE_A_WAIT_VFALL:
		/*
		 * Wait till VBUS falls below SessionEnd (~0.2 V); the 1.3
		 * RTL seems to mis-handle session "start" otherwise (or in
		 * our case "recover"), in routine "VBUS was valid by the time
		 * VBUSERR got reported during enumeration" cases.
		 */
		if (devctl & MUSB_DEVCTL_VBUS) {
			mod_timer(&otg_workaround, jiffies + POLL_SECONDS * HZ);
			break;
		}
		musb->xceiv->state = OTG_STATE_A_WAIT_VRISE;
		musb_writel(musb->ctrl_base, DA8XX_USB_INTR_SRC_SET_REG,
			    MUSB_INTR_VBUSERROR << DA8XX_INTR_USB_SHIFT);
		break;
	case OTG_STATE_B_IDLE:
		if (!is_peripheral_enabled(musb))
			break;

		/*
		 * There's no ID-changed IRQ, so we have no good way to tell
		 * when to switch to the A-Default state machine (by setting
		 * the DEVCTL.Session bit).
		 *
		 * Workaround:  whenever we're in B_IDLE, try setting the
		 * session flag every few seconds.  If it works, ID was
		 * grounded and we're now in the A-Default state machine.
		 *
		 * NOTE: setting the session flag is _supposed_ to trigger
		 * SRP but clearly it doesn't.
		 */
		musb_writeb(mregs, MUSB_DEVCTL, devctl | MUSB_DEVCTL_SESSION);
		devctl = musb_readb(mregs, MUSB_DEVCTL);
		if (devctl & MUSB_DEVCTL_BDEVICE)
			mod_timer(&otg_workaround, jiffies + POLL_SECONDS * HZ);
		else
			musb->xceiv->state = OTG_STATE_A_IDLE;
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&musb->lock, flags);
}

static void da8xx_musb_try_idle(struct musb *musb, unsigned long timeout)
{
	static unsigned long last_timer;

	if (!is_otg_enabled(musb))
		return;

	if (timeout == 0)
		timeout = jiffies + msecs_to_jiffies(3);

	/* Never idle if active, or when VBUS timeout is not set as host */
	if (musb->is_active || (musb->a_wait_bcon == 0 &&
				musb->xceiv->state == OTG_STATE_A_WAIT_BCON)) {
		dev_dbg(musb->controller, "%s active, deleting timer\n",
			otg_state_string(musb->xceiv->state));
		del_timer(&otg_workaround);
		last_timer = jiffies;
		return;
	}

	if (time_after(last_timer, timeout) && timer_pending(&otg_workaround)) {
		dev_dbg(musb->controller, "Longer idle timer already pending, ignoring...\n");
		return;
	}
	last_timer = timeout;

	dev_dbg(musb->controller, "%s inactive, starting idle timer for %u ms\n",
		otg_state_string(musb->xceiv->state),
		jiffies_to_msecs(timeout - jiffies));
	mod_timer(&otg_workaround, timeout);
}

static irqreturn_t da8xx_musb_interrupt(int irq, void *hci)
{
	struct musb		*musb = hci;
	void __iomem		*reg_base = musb->ctrl_base;
	unsigned long		flags;
	irqreturn_t		ret = IRQ_NONE;
	u32			status, pend0;

	spin_lock_irqsave(&musb->lock, flags);

#ifdef CONFIG_USB_TI_CPPI41_DMA
	if (is_cppi41_enabled(musb)) {
		/*
		 * Check for the interrupts from Tx/Rx completion queues; they
		 * are level-triggered and will stay asserted until the queues
		 * are emptied.  We're using the queue pending register 0 as a
		 * substitute for the interrupt status register and reading it
		 * directly for speed.
		 */
		pend0 = musb_readl(reg_base, 0x4000 +
				   QMGR_QUEUE_PENDING_REG(0));
		if (pend0 & (0xf << 24)) {		/* queues 24 to 27 */
			u32 tx = (pend0 >> 24) & 0x3;
			u32 rx = (pend0 >> 26) & 0x3;

			pr_debug("CPPI 4.1 IRQ: Tx %x, Rx %x\n", tx, rx);
			cppi41_completion(musb, rx, tx);
			ret = IRQ_HANDLED;
		}
	}
#endif
	/*
	 * NOTE: DA8XX shadows the Mentor IRQs.  Don't manage them through
	 * the Mentor registers (except for setup), use the TI ones and EOI.
	 */

	/* Acknowledge and handle non-CPPI interrupts */
	status = musb_readl(reg_base, DA8XX_USB_INTR_SRC_MASKED_REG);
	if (!status)
		goto eoi;

	musb_writel(reg_base, DA8XX_USB_INTR_SRC_CLEAR_REG, status);
	dev_dbg(musb->controller, "USB IRQ %08x\n", status);

	musb->int_rx = (status & DA8XX_INTR_RX_MASK) >> DA8XX_INTR_RX_SHIFT;
	musb->int_tx = (status & DA8XX_INTR_TX_MASK) >> DA8XX_INTR_TX_SHIFT;
	musb->int_usb = (status & DA8XX_INTR_USB_MASK) >> DA8XX_INTR_USB_SHIFT;

	/*
	 * DRVVBUS IRQs are the only proxy we have (a very poor one!) for
	 * DA8xx's missing ID change IRQ.  We need an ID change IRQ to
	 * switch appropriately between halves of the OTG state machine.
	 * Managing DEVCTL.Session per Mentor docs requires that we know its
	 * value but DEVCTL.BDevice is invalid without DEVCTL.Session set.
	 * Also, DRVVBUS pulses for SRP (but not at 5 V)...
	 */
	if (status & (DA8XX_INTR_DRVVBUS << DA8XX_INTR_USB_SHIFT)) {
		int drvvbus = musb_readl(reg_base, DA8XX_USB_STAT_REG);
		void __iomem *mregs = musb->mregs;
		u8 devctl = musb_readb(mregs, MUSB_DEVCTL);
		int err;

		err = is_host_enabled(musb) && (musb->int_usb &
						MUSB_INTR_VBUSERROR);
		if (err) {
			/*
			 * The Mentor core doesn't debounce VBUS as needed
			 * to cope with device connect current spikes. This
			 * means it's not uncommon for bus-powered devices
			 * to get VBUS errors during enumeration.
			 *
			 * This is a workaround, but newer RTL from Mentor
			 * seems to allow a better one: "re"-starting sessions
			 * without waiting for VBUS to stop registering in
			 * devctl.
			 */
			musb->int_usb &= ~MUSB_INTR_VBUSERROR;
			musb->xceiv->state = OTG_STATE_A_WAIT_VFALL;
			mod_timer(&otg_workaround, jiffies + POLL_SECONDS * HZ);
			WARNING("VBUS error workaround (delay coming)\n");
		} else if (is_host_enabled(musb) && drvvbus) {
			musb->is_active = 1;
			MUSB_HST_MODE(musb);
			musb->xceiv->default_a = 1;
			musb->xceiv->state = OTG_STATE_A_WAIT_VRISE;
			portstate(musb->port1_status |= USB_PORT_STAT_POWER);
			del_timer(&otg_workaround);
		} else {
			musb->is_active = 0;
			MUSB_DEV_MODE(musb);
			musb->xceiv->default_a = 0;
			musb->xceiv->state = OTG_STATE_B_IDLE;
			portstate(musb->port1_status &= ~USB_PORT_STAT_POWER);
		}

		dev_dbg(musb->controller, "VBUS %s (%s)%s, devctl %02x\n",
				drvvbus ? "on" : "off",
				otg_state_string(musb->xceiv->state),
				err ? " ERROR" : "",
				devctl);
		ret = IRQ_HANDLED;
	}

	if (musb->int_tx || musb->int_rx || musb->int_usb)
		ret |= musb_interrupt(musb);

 eoi:
	/* EOI needs to be written for the IRQ to be re-asserted. */
	if (ret == IRQ_HANDLED || status)
		musb_writel(reg_base, DA8XX_USB_END_OF_INTR_REG, 0);

	/* Poll for ID change */
	if (is_otg_enabled(musb) && musb->xceiv->state == OTG_STATE_B_IDLE)
		mod_timer(&otg_workaround, jiffies + POLL_SECONDS * HZ);

	spin_unlock_irqrestore(&musb->lock, flags);

	return ret;
}

static int da8xx_musb_set_mode(struct musb *musb, u8 musb_mode)
{
	u32 cfgchip2 = readl(CFGCHIP2);

	cfgchip2 &= ~CFGCHIP2_OTGMODE;
	switch (musb_mode) {
	case MUSB_HOST:		/* Force VBUS valid, ID = 0 */
		cfgchip2 |= CFGCHIP2_FORCE_HOST;
		break;
	case MUSB_PERIPHERAL:	/* Force VBUS valid, ID = 1 */
		cfgchip2 |= CFGCHIP2_FORCE_DEVICE;
		break;
	case MUSB_OTG:		/* Don't override the VBUS/ID comparators */
		cfgchip2 |= CFGCHIP2_NO_OVERRIDE;
		break;
	default:
		dev_dbg(musb->controller, "Trying to set unsupported mode %u\n", musb_mode);
	}

	writel(cfgchip2, CFGCHIP2);
	return 0;
}

static int __devinit da8xx_musb_init(struct musb *musb)
{
	void __iomem *reg_base = musb->ctrl_base;
	u32 rev;

	musb->mregs += DA8XX_MENTOR_CORE_OFFSET;

	/* Returns zero if e.g. not clocked */
	rev = musb_readl(reg_base, DA8XX_USB_REVISION_REG);
	if (!rev)
		goto fail;

	usb_nop_xceiv_register(0);
	musb->xceiv = otg_get_transceiver(0);
	if (!musb->xceiv)
		goto fail;

        pr_info("MUSB%d controller's USBSS revision = %08x\n", musb->id, rev);

	if (is_host_enabled(musb))
		setup_timer(&otg_workaround, otg_timer, (unsigned long)musb);

	/* Reset the controller */
	musb_writel(reg_base, DA8XX_USB_CTRL_REG, DA8XX_SOFT_RESET_MASK);

	/* Start the on-chip PHY and its PLL. */
	phy_on();

	msleep(5);

#ifdef CONFIG_USB_TI_CPPI41_DMA
	cppi41_init(musb);
#endif

	/* NOTE: IRQs are in mixed mode, not bypass to pure MUSB */
	pr_debug("DA8xx OTG revision %08x, PHY %03x, control %02x\n",
		 rev, readl(CFGCHIP2),
		 musb_readb(reg_base, DA8XX_USB_CTRL_REG));

	musb->a_wait_bcon = A_WAIT_BCON_TIMEOUT;
	musb->isr = da8xx_musb_interrupt;
	return 0;
fail:
	return -ENODEV;
}

#ifdef CONFIG_USB_TI_CPPI41_DMA
void cppi41_free(void)
{
	u32 numch, blknum, order;
	struct usb_cppi41_info *cppi_info = &usb_cppi41_info[0];

	numch =  USB_CPPI41_NUM_CH * 2;
	order = get_count_order(numch);
	blknum = cppi_info->dma_block;

	cppi41_dma_block_uninit(blknum, cppi_info->q_mgr, order,
			dma_sched_table, numch);
	cppi41_queue_mgr_uninit(cppi_info->q_mgr);
}
#endif

static int da8xx_musb_exit(struct musb *musb)
{
	if (is_host_enabled(musb))
		del_timer_sync(&otg_workaround);

	phy_off();

	otg_put_transceiver(musb->xceiv);
	usb_nop_xceiv_unregister(0);

#ifdef CONFIG_USB_TI_CPPI41_DMA
	cppi41_free();
#endif
	return 0;
}

static const struct musb_platform_ops da8xx_ops = {
	.fifo_mode	= 2,
	.flags		= MUSB_GLUE_EP_ADDR_FLAT_MAPPING | MUSB_GLUE_DMA_CPPI41,
	.init		= da8xx_musb_init,
	.exit		= da8xx_musb_exit,

	.enable		= da8xx_musb_enable,
	.disable	= da8xx_musb_disable,

	.set_mode	= da8xx_musb_set_mode,
	.try_idle	= da8xx_musb_try_idle,

	.set_vbus	= da8xx_musb_set_vbus,

	.read_fifo	= musb_read_fifo,
	.write_fifo	= musb_write_fifo,

	.dma_controller_create = cppi41_dma_controller_create,
	.dma_controller_destroy = cppi41_dma_controller_destroy,
	.en_sof = da8xx_musb_enable_sof,
	.dis_sof = da8xx_musb_disable_sof,
};

static u64 da8xx_dmamask = DMA_BIT_MASK(32);

static int __init da8xx_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data	*pdata = pdev->dev.platform_data;
	struct platform_device		*musb;
	struct da8xx_glue		*glue;

	struct clk			*clk;

	int				ret = -ENOMEM;

	glue = kzalloc(sizeof(*glue), GFP_KERNEL);
	if (!glue) {
		dev_err(&pdev->dev, "failed to allocate glue context\n");
		goto err0;
	}

	musb = platform_device_alloc("musb-hdrc", -1);
	if (!musb) {
		dev_err(&pdev->dev, "failed to allocate musb device\n");
		goto err1;
	}

	clk = clk_get(&pdev->dev, "usb20");
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "failed to get clock\n");
		ret = PTR_ERR(clk);
		goto err2;
	}

	ret = clk_enable(clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable clock\n");
		goto err3;
	}

	musb->dev.parent		= &pdev->dev;
	musb->dev.dma_mask		= &da8xx_dmamask;
	musb->dev.coherent_dma_mask	= da8xx_dmamask;

	glue->dev			= &pdev->dev;
	glue->musb			= musb;
	glue->clk			= clk;

	pdata->platform_ops		= &da8xx_ops;

	platform_set_drvdata(pdev, glue);

	pdev->resource->parent = NULL;

	ret = platform_device_add_resources(musb, pdev->resource,
			pdev->num_resources);
	if (ret) {
		dev_err(&pdev->dev, "failed to add resources\n");
		goto err4;
	}

	ret = platform_device_add_data(musb, pdata, sizeof(*pdata));
	if (ret) {
		dev_err(&pdev->dev, "failed to add platform_data\n");
		goto err4;
	}

	ret = platform_device_add(musb);
	if (ret) {
		dev_err(&pdev->dev, "failed to register musb device\n");
		goto err4;
	}

	return 0;

err4:
	clk_disable(clk);

err3:
	clk_put(clk);

err2:
	platform_device_put(musb);

err1:
	kfree(glue);

err0:
	return ret;
}

static int __exit da8xx_remove(struct platform_device *pdev)
{
	struct da8xx_glue		*glue = platform_get_drvdata(pdev);

	platform_device_del(glue->musb);
	/*platform_device_put(glue->musb);*/
	clk_disable(glue->clk);
	clk_put(glue->clk);
	kfree(glue);

	return 0;
}

#ifdef CONFIG_PM
static int da8xx_suspend(struct device *dev)
{
	struct da8xx_glue	*glue = dev_get_drvdata(dev);

	/* Shutdown the on-chip PHY and its PLL. */
	phy_off();

	clk_disable(glue->clk);

	return 0;
}

static int da8xx_resume(struct device *dev)
{
	struct da8xx_glue	*glue = dev_get_drvdata(dev);
	int			ret;

	/* Start the on-chip PHY and its PLL. */
	phy_on();

	ret = clk_enable(glue->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops da8xx_pm_ops = {
	.suspend	= da8xx_suspend,
	.resume		= da8xx_resume,
};

#define DEV_PM_OPS	(&da8xx_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif

static struct platform_driver da8xx_driver = {
	.remove		= __exit_p(da8xx_remove),
	.driver		= {
		.name	= "musb-da8xx",
		.pm     = DEV_PM_OPS,
	},
};

MODULE_DESCRIPTION("DA8xx/OMAP-L1x MUSB Glue Layer");
MODULE_AUTHOR("Sergei Shtylyov <sshtylyov@ru.mvista.com>");
MODULE_LICENSE("GPL v2");

static int __init da8xx_init(void)
{
	return platform_driver_probe(&da8xx_driver, da8xx_probe);
}
subsys_initcall(da8xx_init);

static void __exit da8xx_exit(void)
{
	platform_driver_unregister(&da8xx_driver);
}
module_exit(da8xx_exit);
