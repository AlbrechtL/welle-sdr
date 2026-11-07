/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB device controller driver for the WCH USBHS controller (CH32V305/307/317).
 *
 * Each endpoint direction has one DMA buffer. IN endpoints other than EP0
 * point the DMA at the net_buf data instead when it is word-aligned and in
 * the DMA buffer's 64 KB window (the address registers hold 16 bits), so a
 * packet is not copied; otherwise the driver copies between the DMA buffer
 * and the net_buf. EP0 uses a single buffer for SETUP, OUT and IN.
 *
 * When a transfer on an IN endpoint other than EP0 ends, the ISR starts the
 * next queued one at once and leaves the finished buffer on a done list for
 * the thread: the endpoint does not wait for a thread to run between
 * transfers.
 *
 * Data toggles are handled in software. USBHS_UC_INT_BUSY makes the
 * controller NAK all traffic while an interrupt flag is pending, so only one
 * transfer completion is reported at a time.
 */

#define DT_DRV_COMPAT wch_usbhs

#include "udc_common.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/mem_blocks.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/drivers/clock_control.h>

#include <hal_ch32fun.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_wch_usbhs, CONFIG_UDC_DRIVER_LOG_LEVEL);

#define WCH_USBHS_EP0_MPS      64U
#define WCH_USBHS_MAX_EPS      16U
#define WCH_USBHS_BUF_BLK_SIZE 64U
/* Endpoint DMA addresses must be word-aligned */
#define WCH_USBHS_DMA_ALIGN_MASK 3U
/* The DMA address registers hold the low 16 bits of the address */
#define WCH_USBHS_DMA_WINDOW_MASK 0xFFFFU

/* TX and RX control registers share the bit layout */
#define EP_RES_MASK  USBHS_UEP_T_RES_MASK
#define EP_RES_ACK   USBHS_UEP_T_RES_ACK
#define EP_RES_NAK   USBHS_UEP_T_RES_NAK
#define EP_RES_STALL USBHS_UEP_T_RES_STALL
#define EP_TOG_MASK  USBHS_UEP_T_TOG_MASK
#define EP_TOG_DATA0 USBHS_UEP_T_TOG_DATA0
#define EP_TOG_DATA1 USBHS_UEP_T_TOG_DATA1

/* PHY PLL reference clock selection, RCC_CFGR2 USBHSCLK field */
#define WCH_USBHS_CKREF_3M 0U
#define WCH_USBHS_CKREF_4M 1U
#define WCH_USBHS_CKREF_8M 2U
#define WCH_USBHS_CKREF_5M 3U

struct wch_usbhs_config {
	USBHSD_TypeDef *regs;
	sys_mem_blocks_t *mem_block;
	uint8_t *ep0_buf;
	size_t num_of_eps;
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	const struct device *clk_dev;
	uint8_t clk_id;
	int speed_idx;
	void (*make_thread)(const struct device *dev);
	void (*irq_config_func)(const struct device *dev);
};

struct wch_usbhs_ep_data {
	uint8_t *buf;
	/* Length of the IN transaction in flight */
	uint16_t len;
	/* Endpoint is armed by the driver */
	bool active;
	/* IN endpoints other than EP0: finished transfers, reported by the thread */
	struct k_fifo done;
};

enum wch_usbhs_event_type {
	/* SETUP packet received */
	WCH_USBHS_EVT_SETUP,
	/* Trigger new transfer (except control OUT) */
	WCH_USBHS_EVT_XFER_NEW,
	/* Transfer for specific endpoint is finished */
	WCH_USBHS_EVT_XFER_FINISHED,
};

struct wch_usbhs_data {
	struct k_thread thread_data;
	struct k_event events;
	/* Bits 31..16 for IN endpoints, bits 15..0 for OUT endpoints */
	atomic_t xfer_new;
	atomic_t xfer_finished;
	struct wch_usbhs_ep_data out_ep[WCH_USBHS_MAX_EPS];
	struct wch_usbhs_ep_data in_ep[WCH_USBHS_MAX_EPS];
	uint8_t setup[8];
	/* Bus reset seen, not yet reported to the stack */
	bool reset_pending;
};

static inline int udc_ep_to_bnum(const uint8_t ep)
{
	if (USB_EP_DIR_IS_IN(ep)) {
		return 16 + USB_EP_GET_IDX(ep);
	}

	return USB_EP_GET_IDX(ep);
}

static inline uint8_t udc_pull_ep_from_bmsk(uint32_t *const bitmap)
{
	unsigned int bit;

	__ASSERT_NO_MSG(bitmap != NULL && *bitmap != 0U);

	bit = find_lsb_set(*bitmap) - 1;
	*bitmap &= ~BIT(bit);

	if (bit >= 16U) {
		return USB_EP_DIR_IN | (bit - 16U);
	}

	return USB_EP_DIR_OUT | bit;
}

static struct wch_usbhs_ep_data *get_ep_data(const struct device *dev, const uint8_t ep)
{
	struct wch_usbhs_data *priv = udc_get_private(dev);

	if (USB_EP_DIR_IS_OUT(ep)) {
		return &priv->out_ep[USB_EP_GET_IDX(ep)];
	}

	return &priv->in_ep[USB_EP_GET_IDX(ep)];
}

/* Per-endpoint registers are laid out with a stride of 4 bytes */
static volatile uint8_t *ep_ctrl_reg(USBHSD_TypeDef *regs, const uint8_t ep)
{
	const uintptr_t off = USB_EP_GET_IDX(ep) * 4U;

	if (USB_EP_DIR_IS_IN(ep)) {
		return (volatile uint8_t *)((uintptr_t)&regs->UEP0_TX_CTRL + off);
	}

	return (volatile uint8_t *)((uintptr_t)&regs->UEP0_RX_CTRL + off);
}

static volatile uint16_t *ep_tx_len_reg(USBHSD_TypeDef *regs, const uint8_t idx)
{
	return (volatile uint16_t *)((uintptr_t)&regs->UEP0_TX_LEN + idx * 4U);
}

static volatile uint16_t *ep_max_len_reg(USBHSD_TypeDef *regs, const uint8_t idx)
{
	return (volatile uint16_t *)((uintptr_t)&regs->UEP0_MAX_LEN + idx * 4U);
}

static volatile uint32_t *ep_dma_reg(USBHSD_TypeDef *regs, const uint8_t ep)
{
	const uint8_t idx = USB_EP_GET_IDX(ep);

	if (idx == 0U) {
		return &regs->UEP0_DMA;
	}

	if (USB_EP_DIR_IS_IN(ep)) {
		return &regs->UEP1_TX_DMA + (idx - 1U);
	}

	return &regs->UEP1_RX_DMA + (idx - 1U);
}

static inline uint32_t ep_config_bit(const uint8_t ep)
{
	if (USB_EP_DIR_IS_IN(ep)) {
		return BIT(USB_EP_GET_IDX(ep));
	}

	return BIT(16U + USB_EP_GET_IDX(ep));
}

static void ep_set_res(USBHSD_TypeDef *regs, const uint8_t ep, const uint8_t res)
{
	volatile uint8_t *ctrl = ep_ctrl_reg(regs, ep);

	*ctrl = (*ctrl & ~EP_RES_MASK) | res;
}

static void wch_usbhs_prep_tx(const struct device *dev, struct udc_ep_config *const cfg,
			      struct net_buf *const buf)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, cfg->addr);
	const uint16_t len = MIN(udc_mps_ep_size(cfg), buf->len);
	const uintptr_t data = (uintptr_t)buf->data;
	const bool zero_copy = USB_EP_GET_IDX(cfg->addr) != 0U &&
			       (data & WCH_USBHS_DMA_ALIGN_MASK) == 0U &&
			       (data & ~WCH_USBHS_DMA_WINDOW_MASK) ==
				       ((uintptr_t)ep_data->buf & ~WCH_USBHS_DMA_WINDOW_MASK);
	unsigned int key;

	key = irq_lock();

	if (zero_copy) {
		/* The net_buf stays queued until the transfer finishes */
		*ep_dma_reg(config->regs, cfg->addr) = (uint32_t)data;
	} else {
		*ep_dma_reg(config->regs, cfg->addr) = (uint32_t)(uintptr_t)ep_data->buf;
		memcpy(ep_data->buf, buf->data, len);
	}

	ep_data->len = len;
	ep_data->active = true;
	*ep_tx_len_reg(config->regs, USB_EP_GET_IDX(cfg->addr)) = len;
	ep_set_res(config->regs, cfg->addr, EP_RES_ACK);

	irq_unlock(key);

	LOG_DBG("Prepare TX ep 0x%02x len %u", cfg->addr, len);
}

static void wch_usbhs_prep_rx(const struct device *dev, struct udc_ep_config *const cfg)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, cfg->addr);
	unsigned int key;

	key = irq_lock();

	ep_data->active = true;
	ep_set_res(config->regs, cfg->addr, EP_RES_ACK);

	irq_unlock(key);

	LOG_DBG("Prepare RX ep 0x%02x", cfg->addr);
}

static void wch_usbhs_ep_abort(const struct device *dev, const uint8_t ep)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, ep);
	unsigned int key;

	key = irq_lock();

	ep_set_res(config->regs, ep, EP_RES_NAK);
	ep_data->active = false;
	udc_ep_set_busy(udc_get_ep_cfg(dev, ep), false);

	irq_unlock(key);
}

static void wch_usbhs_xfer_finished(const struct device *dev, const uint8_t ep)
{
	struct wch_usbhs_data *priv = udc_get_private(dev);

	get_ep_data(dev, ep)->active = false;
	atomic_set_bit(&priv->xfer_finished, udc_ep_to_bnum(ep));
	k_event_post(&priv->events, BIT(WCH_USBHS_EVT_XFER_FINISHED));
}

/* Move the finished transfer to the done list and start the next queued one */
static void wch_usbhs_in_chain(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct wch_usbhs_data *priv = udc_get_private(dev);
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, cfg->addr);
	struct net_buf *next;

	k_fifo_put(&ep_data->done, udc_buf_get(cfg));

	next = udc_buf_peek(cfg);
	if (next != NULL && !cfg->stat.halted) {
		wch_usbhs_prep_tx(dev, cfg, next);
	} else {
		/* The thread starts the next transfer that is queued */
		ep_data->active = false;
		udc_ep_set_busy(cfg, false);
	}

	atomic_set_bit(&priv->xfer_finished, udc_ep_to_bnum(cfg->addr));
	k_event_post(&priv->events, BIT(WCH_USBHS_EVT_XFER_FINISHED));
}

static void wch_usbhs_isr_in(const struct device *dev, const uint8_t idx)
{
	const struct wch_usbhs_config *config = dev->config;
	const uint8_t ep = USB_EP_DIR_IN | idx;
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, ep);
	volatile uint8_t *ctrl = ep_ctrl_reg(config->regs, ep);
	struct net_buf *buf;

	/* Transaction acknowledged by the host: flip the toggle and stop */
	*ctrl = ((*ctrl ^ EP_TOG_DATA1) & ~EP_RES_MASK) | EP_RES_NAK;

	if (!ep_data->active) {
		return;
	}

	buf = udc_buf_peek(cfg);
	if (buf == NULL) {
		ep_data->active = false;
		LOG_ERR("No buffer for ep 0x%02x", ep);
		udc_submit_event(dev, UDC_EVT_ERROR, -ENOBUFS);
		return;
	}

	net_buf_pull(buf, ep_data->len);

	if (buf->len != 0U) {
		wch_usbhs_prep_tx(dev, cfg, buf);
	} else if (udc_ep_buf_has_zlp(buf)) {
		udc_ep_buf_clear_zlp(buf);
		wch_usbhs_prep_tx(dev, cfg, buf);
	} else if (idx != 0U) {
		wch_usbhs_in_chain(dev, cfg);
	} else {
		wch_usbhs_xfer_finished(dev, ep);
	}
}

static void wch_usbhs_isr_out(const struct device *dev, const uint8_t idx, const uint8_t int_st)
{
	const struct wch_usbhs_config *config = dev->config;
	USBHSD_TypeDef *regs = config->regs;
	const uint8_t ep = USB_EP_DIR_OUT | idx;
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, ep);
	volatile uint8_t *ctrl = ep_ctrl_reg(regs, ep);
	const uint16_t len = regs->RX_LEN;
	struct net_buf *buf;
	size_t copy_len;

	if ((int_st & USBHS_UIS_TOG_OK) == 0U) {
		/* Retransmission with the old toggle, payload is discarded */
		LOG_DBG("ep 0x%02x toggle mismatch", ep);
		return;
	}

	*ctrl = ((*ctrl ^ EP_TOG_DATA1) & ~EP_RES_MASK) | EP_RES_NAK;

	if (!ep_data->active) {
		return;
	}

	buf = udc_buf_peek(cfg);
	if (buf == NULL) {
		ep_data->active = false;
		LOG_ERR("No buffer for ep 0x%02x", ep);
		udc_submit_event(dev, UDC_EVT_ERROR, -ENOBUFS);
		return;
	}

	copy_len = MIN(len, net_buf_tailroom(buf));
	net_buf_add_mem(buf, ep_data->buf, copy_len);

	if (net_buf_tailroom(buf) != 0U && len == udc_mps_ep_size(cfg)) {
		wch_usbhs_prep_rx(dev, cfg);
	} else {
		wch_usbhs_xfer_finished(dev, ep);
	}
}

static void wch_usbhs_isr_setup(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_data *priv = udc_get_private(dev);
	USBHSD_TypeDef *regs = config->regs;

	/* Data or status stage after SETUP always starts with DATA1 */
	regs->UEP0_TX_CTRL = EP_TOG_DATA1 | EP_RES_NAK;
	regs->UEP0_RX_CTRL = EP_TOG_DATA1 | EP_RES_NAK;
	priv->in_ep[0].active = false;
	priv->out_ep[0].active = false;

	if (priv->reset_pending) {
		/*
		 * The bus reset interrupt fires before the high-speed handshake
		 * ends, report the reset once the speed is known. Both events
		 * reach the stack through the same queue, the reset first.
		 */
		priv->reset_pending = false;
		udc_submit_event(dev, UDC_EVT_RESET, 0);
	}

	memcpy(priv->setup, config->ep0_buf, sizeof(priv->setup));
	k_event_post(&priv->events, BIT(WCH_USBHS_EVT_SETUP));
}

static void wch_usbhs_isr(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_data *priv = udc_get_private(dev);
	USBHSD_TypeDef *regs = config->regs;
	const uint8_t int_fg = regs->INT_FG;
	const uint8_t int_st = regs->INT_ST;

	if ((int_fg & USBHS_UIF_BUS_RST) != 0U) {
		regs->DEV_AD = 0U;
		regs->UEP0_TX_CTRL = EP_RES_NAK;
		regs->UEP0_RX_CTRL = EP_RES_NAK;
		regs->INT_FG = USBHS_UIF_BUS_RST;
		priv->reset_pending = true;
	}

	if ((int_fg & USBHS_UIF_TRANSFER) != 0U) {
		const uint8_t idx = int_st & USBHS_UIS_ENDP_MASK;

		switch (int_st & USBHS_UIS_TOKEN_MASK) {
		case USBHS_UIS_TOKEN_IN:
			wch_usbhs_isr_in(dev, idx);
			break;
		case USBHS_UIS_TOKEN_OUT:
			wch_usbhs_isr_out(dev, idx, int_st);
			break;
		default:
			break;
		}

		regs->INT_FG = USBHS_UIF_TRANSFER;
	}

	/* After TRANSFER, so that completions are reported in order */
	if ((int_fg & USBHS_UIF_SETUP_ACT) != 0U) {
		wch_usbhs_isr_setup(dev);
		regs->INT_FG = USBHS_UIF_SETUP_ACT;
	}

	if ((int_fg & USBHS_UIF_SUSPEND) != 0U) {
		regs->INT_FG = USBHS_UIF_SUSPEND;

		if ((regs->MIS_ST & USBHS_UMS_SUSPEND) != 0U) {
			udc_set_suspended(dev, true);
			udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
		} else if (udc_is_suspended(dev)) {
			udc_set_suspended(dev, false);
			udc_submit_event(dev, UDC_EVT_RESUME, 0);
		}
	}

	/* Clear flags that are not enabled or not handled */
	regs->INT_FG = int_fg & ~(USBHS_UIF_BUS_RST | USBHS_UIF_TRANSFER |
				  USBHS_UIF_SETUP_ACT | USBHS_UIF_SUSPEND);
}

static void wch_usbhs_handle_xfer_next(const struct device *dev,
				       struct udc_ep_config *const cfg)
{
	struct net_buf *buf;

	buf = udc_buf_peek(cfg);
	if (buf == NULL) {
		return;
	}

	if (cfg->addr == USB_CONTROL_EP_OUT && udc_get_buf_info(buf)->setup) {
		/* SETUP is received without arming the endpoint */
		return;
	}

	if (cfg->stat.halted) {
		return;
	}

	/* Busy before arming: the IN ISR clears it when it finds no next transfer */
	udc_ep_set_busy(cfg, true);

	if (USB_EP_DIR_IS_OUT(cfg->addr)) {
		wch_usbhs_prep_rx(dev, cfg);
	} else {
		wch_usbhs_prep_tx(dev, cfg, buf);
	}
}

static void wch_usbhs_handle_xfer_finished(const struct device *dev,
					   struct udc_ep_config *const cfg)
{
	struct net_buf *buf;

	buf = udc_buf_get(cfg);
	if (buf == NULL) {
		LOG_ERR("No buffer for ep 0x%02x", cfg->addr);
		udc_submit_event(dev, UDC_EVT_ERROR, -ENOBUFS);
		return;
	}

	udc_ep_set_busy(cfg, false);
	udc_submit_ep_event(dev, buf, 0);
}

/* Report the transfers the IN ISR finished; start one if the ISR found none queued */
static void wch_usbhs_handle_in_done(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, cfg->addr);
	struct net_buf *buf;

	while ((buf = k_fifo_get(&ep_data->done, K_NO_WAIT)) != NULL) {
		udc_submit_ep_event(dev, buf, 0);
	}

	if (!udc_ep_is_busy(cfg)) {
		wch_usbhs_handle_xfer_next(dev, cfg);
	}
}

static void wch_usbhs_handle_setup(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_data *priv = udc_get_private(dev);
	unsigned int key;

	/*
	 * A stale data or status stage may have been armed before this SETUP
	 * was handled. Disarm EP0 again, udc_setup_received() cancels the
	 * queued buffers.
	 */
	key = irq_lock();
	config->regs->UEP0_TX_CTRL = EP_TOG_DATA1 | EP_RES_NAK;
	config->regs->UEP0_RX_CTRL = EP_TOG_DATA1 | EP_RES_NAK;
	priv->in_ep[0].active = false;
	priv->out_ep[0].active = false;
	irq_unlock(key);

	udc_setup_received(dev, priv->setup);
}

static void wch_usbhs_thread_handler(void *const arg)
{
	const struct device *dev = (const struct device *)arg;
	struct wch_usbhs_data *priv = udc_get_private(dev);
	struct udc_ep_config *ep_cfg;
	uint32_t evt;
	uint32_t eps;
	uint8_t ep;

	evt = k_event_wait(&priv->events, UINT32_MAX, false, K_FOREVER);
	udc_lock_internal(dev, K_FOREVER);

	if ((evt & BIT(WCH_USBHS_EVT_XFER_FINISHED)) != 0U) {
		k_event_clear(&priv->events, BIT(WCH_USBHS_EVT_XFER_FINISHED));
		eps = atomic_clear(&priv->xfer_finished);

		while (eps != 0U) {
			ep = udc_pull_ep_from_bmsk(&eps);
			ep_cfg = udc_get_ep_cfg(dev, ep);
			LOG_DBG("Finished event ep 0x%02x", ep);

			if (USB_EP_DIR_IS_IN(ep) && USB_EP_GET_IDX(ep) != 0U) {
				wch_usbhs_handle_in_done(dev, ep_cfg);
				continue;
			}

			wch_usbhs_handle_xfer_finished(dev, ep_cfg);
			if (!udc_ep_is_busy(ep_cfg)) {
				wch_usbhs_handle_xfer_next(dev, ep_cfg);
			}
		}
	}

	if ((evt & BIT(WCH_USBHS_EVT_XFER_NEW)) != 0U) {
		k_event_clear(&priv->events, BIT(WCH_USBHS_EVT_XFER_NEW));
		eps = atomic_clear(&priv->xfer_new);

		while (eps != 0U) {
			ep = udc_pull_ep_from_bmsk(&eps);
			ep_cfg = udc_get_ep_cfg(dev, ep);
			LOG_DBG("New transfer ep 0x%02x in the queue", ep);

			if (!udc_ep_is_busy(ep_cfg)) {
				wch_usbhs_handle_xfer_next(dev, ep_cfg);
			}
		}
	}

	if ((evt & BIT(WCH_USBHS_EVT_SETUP)) != 0U) {
		k_event_clear(&priv->events, BIT(WCH_USBHS_EVT_SETUP));
		LOG_DBG("SETUP event");
		wch_usbhs_handle_setup(dev);
	}

	udc_unlock_internal(dev);
}

static int udc_wch_usbhs_ep_enqueue(const struct device *dev,
				    struct udc_ep_config *const cfg,
				    struct net_buf *buf)
{
	struct wch_usbhs_data *priv = udc_get_private(dev);

	udc_buf_put(cfg, buf);

	if (!cfg->stat.halted) {
		atomic_set_bit(&priv->xfer_new, udc_ep_to_bnum(cfg->addr));
		k_event_post(&priv->events, BIT(WCH_USBHS_EVT_XFER_NEW));
	}

	return 0;
}

static int udc_wch_usbhs_ep_dequeue(const struct device *dev,
				    struct udc_ep_config *const cfg)
{
	unsigned int key;

	key = irq_lock();

	wch_usbhs_ep_abort(dev, cfg->addr);
	udc_ep_cancel_queued(dev, cfg);

	irq_unlock(key);

	return 0;
}

static int udc_wch_usbhs_ep_enable(const struct device *dev,
				   struct udc_ep_config *const cfg)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, cfg->addr);
	USBHSD_TypeDef *regs = config->regs;
	const uint8_t idx = USB_EP_GET_IDX(cfg->addr);
	const uint16_t mps = udc_mps_ep_size(cfg);
	int err;

	if (idx == 0U) {
		ep_data->buf = config->ep0_buf;
		*ep_max_len_reg(regs, 0U) = WCH_USBHS_EP0_MPS;
	} else {
		err = sys_mem_blocks_alloc_contiguous(config->mem_block,
						      DIV_ROUND_UP(mps, WCH_USBHS_BUF_BLK_SIZE),
						      (void **)&ep_data->buf);
		if (err != 0) {
			LOG_ERR("Failed to allocate DMA buffer for ep 0x%02x", cfg->addr);
			return err;
		}

		if (USB_EP_DIR_IS_OUT(cfg->addr)) {
			*ep_max_len_reg(regs, idx) = mps;
		}

		if ((cfg->attributes & USB_EP_TRANSFER_TYPE_MASK) == USB_EP_TYPE_ISO) {
			regs->ENDP_TYPE |= ep_config_bit(cfg->addr);
		} else {
			regs->ENDP_TYPE &= ~ep_config_bit(cfg->addr);
		}
	}

	*ep_dma_reg(regs, cfg->addr) = (uint32_t)(uintptr_t)ep_data->buf;
	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		*ep_tx_len_reg(regs, idx) = 0U;
	}

	*ep_ctrl_reg(regs, cfg->addr) = EP_TOG_DATA0 | EP_RES_NAK;
	ep_data->active = false;
	regs->ENDP_CONFIG |= ep_config_bit(cfg->addr);

	LOG_DBG("Enable ep 0x%02x mps %u buf %p", cfg->addr, mps, (void *)ep_data->buf);

	return 0;
}

static int udc_wch_usbhs_ep_disable(const struct device *dev,
				    struct udc_ep_config *const cfg)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, cfg->addr);
	USBHSD_TypeDef *regs = config->regs;
	const uint8_t idx = USB_EP_GET_IDX(cfg->addr);
	int err;

	wch_usbhs_ep_abort(dev, cfg->addr);
	regs->ENDP_CONFIG &= ~ep_config_bit(cfg->addr);

	if (idx != 0U && ep_data->buf != NULL) {
		err = sys_mem_blocks_free_contiguous(config->mem_block, ep_data->buf,
						     DIV_ROUND_UP(udc_mps_ep_size(cfg),
								  WCH_USBHS_BUF_BLK_SIZE));
		if (err != 0) {
			LOG_ERR("Failed to free DMA buffer for ep 0x%02x", cfg->addr);
			return err;
		}

		ep_data->buf = NULL;
	}

	LOG_DBG("Disable ep 0x%02x", cfg->addr);

	return 0;
}

static int udc_wch_usbhs_ep_set_halt(const struct device *dev,
				     struct udc_ep_config *const cfg)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_ep_data *ep_data = get_ep_data(dev, cfg->addr);
	unsigned int key;

	key = irq_lock();

	/* A protocol stall on EP0 is cleared by the next SETUP */
	ep_set_res(config->regs, cfg->addr, EP_RES_STALL);
	ep_data->active = false;
	udc_ep_set_busy(cfg, false);

	if (USB_EP_GET_IDX(cfg->addr) != 0U) {
		cfg->stat.halted = true;
	}

	irq_unlock(key);

	LOG_DBG("Set halt ep 0x%02x", cfg->addr);

	return 0;
}

static int udc_wch_usbhs_ep_clear_halt(const struct device *dev,
				       struct udc_ep_config *const cfg)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_data *priv = udc_get_private(dev);
	unsigned int key;

	key = irq_lock();

	*ep_ctrl_reg(config->regs, cfg->addr) = EP_TOG_DATA0 | EP_RES_NAK;
	cfg->stat.halted = false;

	irq_unlock(key);

	/* Restart a transfer that was queued while the endpoint was halted */
	if (udc_buf_peek(cfg) != NULL) {
		atomic_set_bit(&priv->xfer_new, udc_ep_to_bnum(cfg->addr));
		k_event_post(&priv->events, BIT(WCH_USBHS_EVT_XFER_NEW));
	}

	LOG_DBG("Clear halt ep 0x%02x", cfg->addr);

	return 0;
}

static int udc_wch_usbhs_set_address(const struct device *dev, const uint8_t addr)
{
	const struct wch_usbhs_config *config = dev->config;

	config->regs->DEV_AD = addr & USBHS_MASK_USB_ADDR;
	LOG_DBG("Set new address %u", addr);

	return 0;
}

static int udc_wch_usbhs_host_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);

	return -ENOTSUP;
}

static enum udc_bus_speed udc_wch_usbhs_device_speed(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;

	switch (config->regs->SPEED_TYPE & USBHS_USB_SPEED_TYPE) {
	case USBHS_USB_SPEED_HIGH:
		return UDC_BUS_SPEED_HS;
	case USBHS_USB_SPEED_FULL:
		return UDC_BUS_SPEED_FS;
	default:
		return UDC_BUS_UNKNOWN;
	}
}

static int wch_usbhs_phy_clock_init(void)
{
	static const struct {
		uint32_t freq;
		uint32_t ckref;
	} refs[] = {
		{MHZ(4), WCH_USBHS_CKREF_4M},
		{MHZ(8), WCH_USBHS_CKREF_8M},
		{MHZ(5), WCH_USBHS_CKREF_5M},
		{MHZ(3), WCH_USBHS_CKREF_3M},
	};
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(clk_hse))
	const uint32_t src_freq = DT_PROP(DT_NODELABEL(clk_hse), clock_frequency);
	const uint32_t src_sel = 0U;
#else
	const uint32_t src_freq = DT_PROP(DT_NODELABEL(clk_hsi), clock_frequency);
	const uint32_t src_sel = RCC_USBHSPLLSRC;
#endif

	for (size_t i = 0; i < ARRAY_SIZE(refs); i++) {
		const uint32_t div = src_freq / refs[i].freq;

		if ((src_freq % refs[i].freq) != 0U || div < 1U || div > 8U) {
			continue;
		}

		RCC->CFGR2 &= ~(RCC_USBHSDIV_MASK | RCC_USBHSPLLSRC | RCC_USBHSCLK_MASK);
		RCC->CFGR2 |= ((div - 1U) << RCC_USBHSDIV_OFFSET) | src_sel |
			      (refs[i].ckref << RCC_USBHSCLK_OFFSET) | RCC_USBHSPLL;

		LOG_DBG("PHY PLL reference %u Hz (source %u Hz / %u)", refs[i].freq, src_freq,
			div);
		return 0;
	}

	LOG_ERR("No PHY PLL reference possible from %u Hz", src_freq);

	return -ENOTSUP;
}

static int udc_wch_usbhs_init(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;
	USBHSD_TypeDef *regs = config->regs;
	uint8_t speed;
	int err;

	err = wch_usbhs_phy_clock_init();
	if (err != 0) {
		return err;
	}

	err = clock_control_on(config->clk_dev, (clock_control_subsys_t)(uintptr_t)config->clk_id);
	if (err != 0) {
		LOG_ERR("Failed to enable USBHS clock");
		return err;
	}

	speed = (config->speed_idx == 2) ? USBHS_UC_SPEED_HIGH : USBHS_UC_SPEED_FULL;

	regs->CONTROL = USBHS_UC_CLR_ALL | USBHS_UC_RESET_SIE;
	k_busy_wait(10);
	regs->CONTROL &= ~USBHS_UC_RESET_SIE;
	regs->HOST_CTRL = USBHS_UH_PHY_SUSPENDM;
	regs->CONTROL = USBHS_UC_DMA_EN | USBHS_UC_INT_BUSY | speed;
	regs->ENDP_CONFIG = 0U;
	regs->ENDP_TYPE = 0U;
	regs->BUF_MODE = 0U;
	regs->DEV_AD = 0U;

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT, USB_EP_TYPE_CONTROL,
				   WCH_USBHS_EP0_MPS, 0) != 0) {
		LOG_ERR("Failed to enable control endpoint");
		return -EIO;
	}

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_IN, USB_EP_TYPE_CONTROL,
				   WCH_USBHS_EP0_MPS, 0) != 0) {
		LOG_ERR("Failed to enable control endpoint");
		return -EIO;
	}

	regs->INT_FG = 0xFFU;
	regs->INT_EN = USBHS_UIE_SETUP_ACT | USBHS_UIE_TRANSFER | USBHS_UIE_BUS_RST |
		       USBHS_UIE_SUSPEND;
	irq_enable(DT_INST_IRQN(0));

	return 0;
}

static int udc_wch_usbhs_enable(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;

	config->regs->CONTROL |= USBHS_UC_DEV_PU_EN;
	LOG_DBG("Enable device %p", dev);

	return 0;
}

static int udc_wch_usbhs_disable(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;

	config->regs->CONTROL &= ~USBHS_UC_DEV_PU_EN;
	LOG_DBG("Disable device %p", dev);

	return 0;
}

static int udc_wch_usbhs_shutdown(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;
	USBHSD_TypeDef *regs = config->regs;

	irq_disable(DT_INST_IRQN(0));
	regs->INT_EN = 0U;

	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT) != 0) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_IN) != 0) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	regs->CONTROL = USBHS_UC_CLR_ALL | USBHS_UC_RESET_SIE;

	return clock_control_off(config->clk_dev,
				 (clock_control_subsys_t)(uintptr_t)config->clk_id);
}

static int udc_wch_usbhs_driver_preinit(const struct device *dev)
{
	const struct wch_usbhs_config *config = dev->config;
	struct wch_usbhs_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;
	uint16_t mps = 1023U;
	int err;

	k_mutex_init(&data->mutex);
	k_event_init(&priv->events);
	atomic_clear(&priv->xfer_new);
	atomic_clear(&priv->xfer_finished);

	for (size_t i = 0; i < WCH_USBHS_MAX_EPS; i++) {
		k_fifo_init(&priv->in_ep[i].done);
	}

	data->caps.rwup = false;
	data->caps.mps0 = UDC_MPS0_64;
	if (config->speed_idx == 2) {
		data->caps.hs = true;
		mps = 1024U;
	}

	for (size_t i = 0; i < config->num_of_eps; i++) {
		config->ep_cfg_out[i].caps.out = 1;
		if (i == 0U) {
			config->ep_cfg_out[i].caps.control = 1;
			config->ep_cfg_out[i].caps.mps = WCH_USBHS_EP0_MPS;
		} else {
			config->ep_cfg_out[i].caps.bulk = 1;
			config->ep_cfg_out[i].caps.interrupt = 1;
			config->ep_cfg_out[i].caps.iso = 1;
			config->ep_cfg_out[i].caps.mps = mps;
		}

		config->ep_cfg_out[i].addr = USB_EP_DIR_OUT | i;
		err = udc_register_ep(dev, &config->ep_cfg_out[i]);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

	for (size_t i = 0; i < config->num_of_eps; i++) {
		config->ep_cfg_in[i].caps.in = 1;
		if (i == 0U) {
			config->ep_cfg_in[i].caps.control = 1;
			config->ep_cfg_in[i].caps.mps = WCH_USBHS_EP0_MPS;
		} else {
			config->ep_cfg_in[i].caps.bulk = 1;
			config->ep_cfg_in[i].caps.interrupt = 1;
			config->ep_cfg_in[i].caps.iso = 1;
			config->ep_cfg_in[i].caps.mps = mps;
		}

		config->ep_cfg_in[i].addr = USB_EP_DIR_IN | i;
		err = udc_register_ep(dev, &config->ep_cfg_in[i]);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

	config->irq_config_func(dev);
	config->make_thread(dev);

	LOG_INF("Device %p (max. speed %d)", dev, config->speed_idx);

	return 0;
}

static void udc_wch_usbhs_lock(const struct device *dev)
{
	k_sched_lock();
	udc_lock_internal(dev, K_FOREVER);
}

static void udc_wch_usbhs_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
	k_sched_unlock();
}

static const struct udc_api udc_wch_usbhs_api = {
	.lock = udc_wch_usbhs_lock,
	.unlock = udc_wch_usbhs_unlock,
	.device_speed = udc_wch_usbhs_device_speed,
	.init = udc_wch_usbhs_init,
	.enable = udc_wch_usbhs_enable,
	.disable = udc_wch_usbhs_disable,
	.shutdown = udc_wch_usbhs_shutdown,
	.set_address = udc_wch_usbhs_set_address,
	.host_wakeup = udc_wch_usbhs_host_wakeup,
	.ep_enable = udc_wch_usbhs_ep_enable,
	.ep_disable = udc_wch_usbhs_ep_disable,
	.ep_set_halt = udc_wch_usbhs_ep_set_halt,
	.ep_clear_halt = udc_wch_usbhs_ep_clear_halt,
	.ep_enqueue = udc_wch_usbhs_ep_enqueue,
	.ep_dequeue = udc_wch_usbhs_ep_dequeue,
};

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
	     "Only one USBHS controller instance is supported");

#define UDC_WCH_USBHS_DEVICE_DEFINE(n)                                                             \
	BUILD_ASSERT(DT_INST_PROP(n, num_bidir_endpoints) <= WCH_USBHS_MAX_EPS,                    \
		     "Too many endpoints");                                                        \
                                                                                                   \
	K_THREAD_STACK_DEFINE(udc_wch_usbhs_stack_##n, CONFIG_UDC_WCH_USBHS_STACK_SIZE);           \
                                                                                                   \
	SYS_MEM_BLOCKS_DEFINE_STATIC(udc_wch_usbhs_mb_##n, WCH_USBHS_BUF_BLK_SIZE,                 \
				     CONFIG_UDC_WCH_USBHS_BUF_POOL_SIZE / WCH_USBHS_BUF_BLK_SIZE,  \
				     4);                                                           \
                                                                                                   \
	static uint8_t udc_wch_usbhs_ep0_buf_##n[WCH_USBHS_EP0_MPS] __aligned(4);                  \
                                                                                                   \
	static void udc_wch_usbhs_thread_##n(void *dev, void *arg1, void *arg2)                    \
	{                                                                                          \
		ARG_UNUSED(arg1);                                                                  \
		ARG_UNUSED(arg2);                                                                  \
		while (true) {                                                                     \
			wch_usbhs_thread_handler(dev);                                             \
		}                                                                                  \
	}                                                                                          \
                                                                                                   \
	static void udc_wch_usbhs_make_thread_##n(const struct device *dev)                        \
	{                                                                                          \
		struct wch_usbhs_data *priv = udc_get_private(dev);                                \
                                                                                                   \
		k_thread_create(&priv->thread_data, udc_wch_usbhs_stack_##n,                       \
				K_THREAD_STACK_SIZEOF(udc_wch_usbhs_stack_##n),                    \
				udc_wch_usbhs_thread_##n, (void *)dev, NULL, NULL,                 \
				K_PRIO_COOP(CONFIG_UDC_WCH_USBHS_THREAD_PRIORITY), K_ESSENTIAL,    \
				K_NO_WAIT);                                                        \
		k_thread_name_set(&priv->thread_data, dev->name);                                  \
	}                                                                                          \
                                                                                                   \
	static void udc_wch_usbhs_irq_config_##n(const struct device *dev)                         \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), 0, wch_usbhs_isr, DEVICE_DT_INST_GET(n), 0);          \
	}                                                                                          \
                                                                                                   \
	static struct udc_ep_config ep_cfg_out_##n[DT_INST_PROP(n, num_bidir_endpoints)];          \
	static struct udc_ep_config ep_cfg_in_##n[DT_INST_PROP(n, num_bidir_endpoints)];           \
                                                                                                   \
	static const struct wch_usbhs_config udc_wch_usbhs_config_##n = {                          \
		.regs = (USBHSD_TypeDef *)DT_INST_REG_ADDR(n),                                     \
		.mem_block = &udc_wch_usbhs_mb_##n,                                                \
		.ep0_buf = udc_wch_usbhs_ep0_buf_##n,                                              \
		.num_of_eps = DT_INST_PROP(n, num_bidir_endpoints),                                \
		.ep_cfg_in = ep_cfg_in_##n,                                                        \
		.ep_cfg_out = ep_cfg_out_##n,                                                      \
		.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                  \
		.clk_id = DT_INST_CLOCKS_CELL(n, id),                                              \
		.speed_idx = DT_ENUM_IDX(DT_DRV_INST(n), maximum_speed),                           \
		.make_thread = udc_wch_usbhs_make_thread_##n,                                      \
		.irq_config_func = udc_wch_usbhs_irq_config_##n,                                   \
	};                                                                                         \
                                                                                                   \
	static struct wch_usbhs_data udc_priv_##n;                                                 \
                                                                                                   \
	static struct udc_data udc_data_##n = {                                                    \
		.mutex = Z_MUTEX_INITIALIZER(udc_data_##n.mutex),                                  \
		.priv = &udc_priv_##n,                                                             \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, udc_wch_usbhs_driver_preinit, NULL, &udc_data_##n,                \
			      &udc_wch_usbhs_config_##n, POST_KERNEL,                              \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &udc_wch_usbhs_api);

DT_INST_FOREACH_STATUS_OKAY(UDC_WCH_USBHS_DEVICE_DEFINE)
