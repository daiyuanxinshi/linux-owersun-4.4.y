/*******************************************************************************
  This contains the functions to handle the normal descriptors.

  Copyright (C) 2007-2009  STMicroelectronics Ltd

  This program is free software; you can redistribute it and/or modify it
  under the terms and conditions of the GNU General Public License,
  version 2, as published by the Free Software Foundation.

  This program is distributed in the hope it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  more details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.

  The full GNU General Public License is included in this distribution in
  the file called "COPYING".

  Author: Giuseppe Cavallaro <peppe.cavallaro@st.com>
*******************************************************************************/

#include <linux/stmmac.h>
#include "common.h"
#include "descs_com.h"

static int ndesc_get_tx_status(void *data, struct stmmac_extra_stats *x,
			       struct dma_desc *p, void __iomem *ioaddr)
{
	struct net_device_stats *stats = (struct net_device_stats *)data;
	unsigned int tdes0 = p->des0;
	int ret = 0;

	if (unlikely(tdes0 & TDES0_ERROR_SUMMARY)) {
		if (unlikely(tdes0 & TDES0_UNDERFLOW_ERROR)) {
			x->tx_underflow++;
			stats->tx_fifo_errors++;
		}
		if (unlikely(tdes0 & TDES0_NO_CARRIER)) {
			x->tx_carrier++;
			stats->tx_carrier_errors++;
		}
		if (unlikely(tdes0 & TDES0_LOSS_CARRIER)) {
			x->tx_losscarrier++;
			stats->tx_carrier_errors++;
		}
		if (unlikely((tdes0 & TDES0_EXCESSIVE_DEFERRAL) ||
			     (tdes0 & TDES0_EXCESSIVE_COLLISIONS) ||
			     (tdes0 & TDES0_LATE_COLLISION))) {
			unsigned int collisions;

			collisions = (tdes0 & TDES0_COLLISION_COUNT_MASK) >> 3;
			stats->collisions += collisions;
		}
		ret = -1;
	}

	if (tdes0 & TDES0_VLAN_FRAME)
		x->tx_vlan++;

	if (unlikely(tdes0 & TDES0_DEFERRED))
		x->tx_deferred++;

	return ret;
}

static int ndesc_get_tx_len(struct dma_desc *p)
{
	return (p->des1 & RDES1_BUFFER1_SIZE_MASK);
}

/* This function verifies if each incoming frame has some errors
 * and, if required, updates the multicast statistics.
 * In case of success, it returns good_frame because the GMAC device
 * is supposed to be able to compute the csum in HW. */
static int ndesc_get_rx_status(void *data, struct stmmac_extra_stats *x,
			       struct dma_desc *p)
{
	int ret = good_frame;
	unsigned int rdes0 = p->des0;
	struct net_device_stats *stats = (struct net_device_stats *)data;

	if (unlikely(rdes0 & RDES0_OWN))
		return dma_own;

	if (unlikely(!(rdes0 & RDES0_LAST_DESCRIPTOR))) {
		pr_warn("%s: Oversized frame spanned multiple buffers\n",
			__func__);
		stats->rx_length_errors++;
		return discard_frame;
	}

	if (unlikely(rdes0 & RDES0_ERROR_SUMMARY)) {
		if (unlikely(rdes0 & RDES0_DESCRIPTOR_ERROR))
			x->rx_desc++;
		if (unlikely(rdes0 & RDES0_SA_FILTER_FAIL))
			x->sa_filter_fail++;
		if (unlikely(rdes0 & RDES0_OVERFLOW_ERROR))
			x->overflow_error++;
		if (unlikely(rdes0 & RDES0_IPC_CSUM_ERROR))
			x->ipc_csum_error++;
		if (unlikely(rdes0 & RDES0_COLLISION)) {
			x->rx_collision++;
			stats->collisions++;
		}
		if (unlikely(rdes0 & RDES0_CRC_ERROR)) {
			x->rx_crc++;
			stats->rx_crc_errors++;
		}
		ret = discard_frame;
	}
	if (unlikely(rdes0 & RDES0_DRIBBLING))
		x->dribbling_bit++;

	if (unlikely(rdes0 & RDES0_LENGTH_ERROR)) {
		x->rx_length++;
		ret = discard_frame;
	}
	if (unlikely(rdes0 & RDES0_MII_ERROR)) {
		x->rx_mii++;
		ret = discard_frame;
	}
#ifdef STMMAC_VLAN_TAG_USED
	if (rdes0 & RDES0_VLAN_TAG)
		x->vlan_tag++;
#endif
	return ret;
}

static void ndesc_init_rx_desc(struct dma_desc *p, int disable_rx_ic, int mode,
			       int end)
{
	p->des0 |= RDES0_OWN;
	p->des1 |= (BUF_SIZE_2KiB - 1) & RDES1_BUFFER1_SIZE_MASK;

	if (mode == STMMAC_CHAIN_MODE)
		ndesc_rx_set_on_chain(p, end);
	else
		ndesc_rx_set_on_ring(p, end);

	if (disable_rx_ic)
		p->des1 |= RDES1_DISABLE_IC;
}

static void ndesc_init_tx_desc(struct dma_desc *p, int mode, int end)
{
	p->des0 &= ~TDES0_OWN;
	if (mode == STMMAC_CHAIN_MODE)
		ndesc_tx_set_on_chain(p);
	else
		ndesc_end_tx_desc_on_ring(p, end);
}

static int ndesc_get_tx_owner(struct dma_desc *p)
{
	return (p->des0 & TDES0_OWN) >> 31;
}

static void ndesc_set_tx_owner(struct dma_desc *p)
{
	p->des0 |= TDES0_OWN;
}

static void ndesc_set_rx_owner(struct dma_desc *p)
{
	p->des0 |= RDES0_OWN;
}

static int ndesc_get_tx_ls(struct dma_desc *p)
{
	return (p->des1 & TDES1_LAST_SEGMENT) >> 30;
}

static void ndesc_release_tx_desc(struct dma_desc *p, int mode)
{
	int ter = (p->des1 & TDES1_END_RING) >> 25;

	memset(p, 0, offsetof(struct dma_desc, des2));
	if (mode == STMMAC_CHAIN_MODE)
		ndesc_tx_set_on_chain(p);
	else
		ndesc_end_tx_desc_on_ring(p, ter);
}

static void ndesc_prepare_tx_desc(struct dma_desc *p, int is_fs, int len,
				  int csum_flag, int mode)
{
	unsigned int tdes1 = p->des1;

	if (is_fs)
		tdes1 |= TDES1_FIRST_SEGMENT;
	else
		tdes1 &= ~TDES1_FIRST_SEGMENT;

	if (likely(csum_flag))
		tdes1 |= (TX_CIC_FULL) << TDES1_CHECKSUM_INSERTION_SHIFT;
	else
		tdes1 &= ~(TX_CIC_FULL << TDES1_CHECKSUM_INSERTION_SHIFT);

	p->des1 = tdes1;

	if (mode == STMMAC_CHAIN_MODE)
		norm_set_tx_desc_len_on_chain(p, len);
	else
		norm_set_tx_desc_len_on_ring(p, len);
}

static void ndesc_clear_tx_ic(struct dma_desc *p)
{
	p->des1 &= ~TDES1_INTERRUPT;
}

static void ndesc_close_tx_desc(struct dma_desc *p)
{
	p->des1 |= TDES1_LAST_SEGMENT | TDES1_INTERRUPT;
}

static int ndesc_get_rx_frame_len(struct dma_desc *p, int rx_coe_type)
{
	unsigned int csum = 0;

	/* The type-1 checksum offload engines append the checksum at
	 * the end of frame and the two bytes of checksum are added in
	 * the length.
	 * Adjust for that in the framelen for type-1 checksum offload
	 * engines
	 */
	if (rx_coe_type == STMMAC_RX_COE_TYPE1)
		csum = 2;

	return (((p->des0 & RDES0_FRAME_LEN_MASK) >> RDES0_FRAME_LEN_SHIFT) -
		csum);

}

static void ndesc_enable_tx_timestamp(struct dma_desc *p)
{
	p->des1 |= TDES1_TIME_STAMP_ENABLE;
}

static int ndesc_get_tx_timestamp_status(struct dma_desc *p)
{
	return (p->des0 & TDES0_TIME_STAMP_STATUS) >> 17;
}

static u64 ndesc_get_timestamp(void *desc, u32 ats)
{
	struct dma_desc *p = (struct dma_desc *)desc;
	u64 ns;

	ns = p->des2;
	/* convert high/sec time stamp value to nanosecond */
	ns += p->des3 * 1000000000ULL;

	return ns;
}

static int ndesc_get_rx_timestamp_status(void *desc, u32 ats)
{
	struct dma_desc *p = (struct dma_desc *)desc;

	if ((p->des2 == 0xffffffff) && (p->des3 == 0xffffffff))
		/* timestamp is corrupted, hence don't store it */
		return 0;
	else
		return 1;
}

const struct stmmac_desc_ops ndesc_ops = {
	.tx_status = ndesc_get_tx_status,
	.rx_status = ndesc_get_rx_status,
	.get_tx_len = ndesc_get_tx_len,
	.init_rx_desc = ndesc_init_rx_desc,
	.init_tx_desc = ndesc_init_tx_desc,
	.get_tx_owner = ndesc_get_tx_owner,
	.release_tx_desc = ndesc_release_tx_desc,
	.prepare_tx_desc = ndesc_prepare_tx_desc,
	.clear_tx_ic = ndesc_clear_tx_ic,
	.close_tx_desc = ndesc_close_tx_desc,
	.get_tx_ls = ndesc_get_tx_ls,
	.set_tx_owner = ndesc_set_tx_owner,
	.set_rx_owner = ndesc_set_rx_owner,
	.get_rx_frame_len = ndesc_get_rx_frame_len,
	.enable_tx_timestamp = ndesc_enable_tx_timestamp,
	.get_tx_timestamp_status = ndesc_get_tx_timestamp_status,
	.get_timestamp = ndesc_get_timestamp,
	.get_rx_timestamp_status = ndesc_get_rx_timestamp_status,
};
