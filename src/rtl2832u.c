/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB function emulating a Realtek RTL2832U as seen by librtlsdr.
 *
 * All register access uses vendor control requests with bRequest 0:
 *   wValue: register address (demod: reg << 8 | 0x20, IIC: I2C address)
 *   wIndex: block << 8, bit 4 set for writes; demod: page in bits 3..0
 * Samples are streamed on the bulk IN endpoint (0x81).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc.h>

#include <zephyr/logging/log.h>

#include "r820t2.h"
#include "rtl2832u.h"
#include "sample_src.h"

LOG_MODULE_REGISTER(rtl2832u, LOG_LEVEL_INF);

#define RTL_VENDOR_REQ 0x00U

#define RTL_CTRL_WRITE        0x10U
#define RTL_CTRL_BLOCK(index) ((index) >> 8)

enum rtl_block {
	RTL_BLOCK_DEMOD = 0,
	RTL_BLOCK_USB = 1,
	RTL_BLOCK_SYS = 2,
	RTL_BLOCK_TUN = 3,
	RTL_BLOCK_ROM = 4,
	RTL_BLOCK_IR = 5,
	RTL_BLOCK_IIC = 6,
};

#define RTL_USB_REG_BASE 0x2000U
#define RTL_USB_REG_SIZE 0x200U
#define RTL_SYS_REG_BASE 0x3000U
#define RTL_SYS_REG_SIZE 0x100U

/* USB_EPA_CTL, written big-endian: 0x1002 stalls and flushes the FIFO */
#define RTL_USB_EPA_CTL 0x2148U

/* Demod page 0 reg 0x19: 0x03 selects the counter test output, 0x05 normal */
#define RTL_DEMOD_TEST_PAGE 0U
#define RTL_DEMOD_TEST_REG  0x19U
#define RTL_DEMOD_TEST_BIT  0x02U

/* Demod page 1 reg 0x01 bit 3: I2C repeater to the tuner */
#define RTL_DEMOD_I2C_PAGE 1U
#define RTL_DEMOD_I2C_REG  0x01U
#define RTL_DEMOD_I2C_BIT  0x08U

/*
 * The demodulator answers on its own I2C address. The Linux dvb_usb_rtl28xxu
 * driver probes it with a zero-length read to tell RTL2832U from RTL2831U.
 */
#define RTL_DEMOD_I2C_ADDR 0x20U

/* Demod page 1 regs 0x9f..0xa2: resampler ratio, big-endian */
#define RTL_DEMOD_RSAMP_PAGE 1U
#define RTL_DEMOD_RSAMP_REG  0x9fU
#define RTL_DEMOD_RSAMP_LEN  4U

/* Demod page 1 regs 0x19..0x1b: IF frequency, 22-bit signed, big-endian */
#define RTL_DEMOD_IF_PAGE 1U
#define RTL_DEMOD_IF_REG  0x19U
#define RTL_DEMOD_IF_LEN  3U

/* Tuner state is logged once the host has not written it for this long */
#define RTL_TUNER_LOG_DELAY K_MSEC(20)

#define RTL_XTAL_FREQ 28800000ULL

#define RTL_FS_BULK_MPS 64U
#define RTL_HS_BULK_MPS 512U

BUILD_ASSERT(CONFIG_RTLSDR_EMU_XFER_SIZE % RTL_HS_BULK_MPS == 0,
	     "Transfer size must be a multiple of 512 to avoid short packets");

/* Streaming state bits */
#define RTL_STATE_ENABLED 0
#define RTL_STATE_RUNNING 1

UDC_BUF_POOL_DEFINE(rtl_ep_pool, CONFIG_RTLSDR_EMU_XFER_COUNT, CONFIG_RTLSDR_EMU_XFER_SIZE,
		    sizeof(struct udc_buf_info), NULL);

struct rtl_regs {
	uint8_t usb[RTL_USB_REG_SIZE];
	uint8_t sys[RTL_SYS_REG_SIZE];
	uint8_t demod[RTL2832U_DEMOD_PAGES][RTL2832U_DEMOD_PAGE_SIZE];
};

struct rtl_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_ep_descriptor if0_hs_in_ep;
	struct usb_desc_header nil_desc;
};

struct rtl_data {
	struct rtl_desc *const desc;
	const struct usb_desc_header *const *const fs_desc;
	const struct usb_desc_header *const *const hs_desc;
	struct rtl_regs regs;
	atomic_t state;
	/* Completed bulk IN transfers */
	atomic_t xfers;
	uint32_t sample_rate;
};

static void rtl_tuner_log_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(rtl_tuner_log_work, rtl_tuner_log_handler);

static uint8_t rtl_get_bulk_in(struct usbd_class_data *const c_data)
{
	struct rtl_data *data = usbd_class_get_private(c_data);
	struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED && usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
		return data->desc->if0_hs_in_ep.bEndpointAddress;
	}

	return data->desc->if0_in_ep.bEndpointAddress;
}

static void rtl_stream_kick(struct usbd_class_data *const c_data)
{
	struct rtl_data *data = usbd_class_get_private(c_data);
	const uint8_t ep = rtl_get_bulk_in(c_data);
	struct udc_buf_info *bi;
	struct net_buf *buf;
	int err;

	while (atomic_test_bit(&data->state, RTL_STATE_ENABLED) &&
	       atomic_test_bit(&data->state, RTL_STATE_RUNNING)) {
		buf = net_buf_alloc(&rtl_ep_pool, K_NO_WAIT);
		if (buf == NULL) {
			/* All transfers are in flight */
			return;
		}

		sample_src_fill(net_buf_add(buf, CONFIG_RTLSDR_EMU_XFER_SIZE),
				CONFIG_RTLSDR_EMU_XFER_SIZE);

		bi = udc_get_buf_info(buf);
		bi->ep = ep;

		err = usbd_ep_enqueue(c_data, buf);
		if (err != 0) {
			LOG_ERR("Failed to enqueue bulk IN transfer (%d)", err);
			net_buf_unref(buf);
			return;
		}
	}
}

static void rtl_stream_set_running(struct usbd_class_data *const c_data, const bool running)
{
	struct rtl_data *data = usbd_class_get_private(c_data);

	if (running) {
		if (!atomic_test_and_set_bit(&data->state, RTL_STATE_RUNNING)) {
			LOG_INF("Streaming started");
			rtl_stream_kick(c_data);
		}

		return;
	}

	if (atomic_test_and_clear_bit(&data->state, RTL_STATE_RUNNING)) {
		LOG_INF("Streaming stopped, FIFO flushed");

		if (atomic_test_bit(&data->state, RTL_STATE_ENABLED)) {
			(void)usbd_ep_dequeue(usbd_class_get_ctx(c_data), rtl_get_bulk_in(c_data));
		}
	}
}

/* Whether a device on the I2C bus acknowledges the 8-bit address */
static bool rtl_i2c_addr_acked(const struct rtl_data *const data, const uint16_t addr)
{
	const bool repeater = (data->regs.demod[RTL_DEMOD_I2C_PAGE][RTL_DEMOD_I2C_REG] &
			       RTL_DEMOD_I2C_BIT) != 0U;

	if (addr == RTL_DEMOD_I2C_ADDR) {
		return true;
	}

	return repeater && addr == R820T2_I2C_ADDR;
}

static uint8_t *rtl_reg_ptr(struct rtl_data *const data, const uint8_t block,
			    const uint16_t addr, const uint16_t len)
{
	switch (block) {
	case RTL_BLOCK_USB:
		if (addr >= RTL_USB_REG_BASE &&
		    addr + len <= RTL_USB_REG_BASE + RTL_USB_REG_SIZE) {
			return &data->regs.usb[addr - RTL_USB_REG_BASE];
		}
		break;
	case RTL_BLOCK_SYS:
		if (addr >= RTL_SYS_REG_BASE &&
		    addr + len <= RTL_SYS_REG_BASE + RTL_SYS_REG_SIZE) {
			return &data->regs.sys[addr - RTL_SYS_REG_BASE];
		}
		break;
	default:
		break;
	}

	return NULL;
}

static void rtl_demod_written(struct usbd_class_data *const c_data, const uint8_t page,
			      const uint8_t reg, const uint16_t len)
{
	struct rtl_data *data = usbd_class_get_private(c_data);
	const uint8_t *p = data->regs.demod[page];

	if (page == RTL_DEMOD_TEST_PAGE && reg <= RTL_DEMOD_TEST_REG &&
	    reg + len > RTL_DEMOD_TEST_REG) {
		const bool test = (p[RTL_DEMOD_TEST_REG] & RTL_DEMOD_TEST_BIT) != 0U;

		sample_src_set_test_mode(test);
		LOG_INF("Test mode %s", test ? "on" : "off");
	}

	if (page == RTL_DEMOD_RSAMP_PAGE && reg <= RTL_DEMOD_RSAMP_REG + 3U &&
	    reg + len > RTL_DEMOD_RSAMP_REG) {
		const uint32_t ratio = sys_get_be32(&p[RTL_DEMOD_RSAMP_REG]);

		/* librtlsdr writes the high half first, wait for the low half */
		if (ratio != 0U && reg + len > RTL_DEMOD_RSAMP_REG + 2U) {
			const uint32_t rate = (uint32_t)((RTL_XTAL_FREQ << 22) / ratio);

			data->sample_rate = rate;
			sample_src_set_rate(rate);
			LOG_INF("Sample rate %u S/s", rate);
		}
	}

	if (page == RTL_DEMOD_IF_PAGE && reg < RTL_DEMOD_IF_REG + RTL_DEMOD_IF_LEN &&
	    reg + len > RTL_DEMOD_IF_REG) {
		(void)k_work_reschedule(&rtl_tuner_log_work, RTL_TUNER_LOG_DELAY);
	}
}

static int rtl_write(struct usbd_class_data *const c_data,
		     const struct usb_setup_packet *const setup, const uint8_t *const src,
		     const uint16_t len)
{
	struct rtl_data *data = usbd_class_get_private(c_data);
	const uint8_t block = RTL_CTRL_BLOCK(setup->wIndex);
	uint8_t *dst;

	switch (block) {
	case RTL_BLOCK_DEMOD: {
		const uint8_t page = setup->wIndex & 0x0FU;
		const uint8_t reg = setup->wValue >> 8;

		if (page < RTL2832U_DEMOD_PAGES && reg + len <= RTL2832U_DEMOD_PAGE_SIZE) {
			memcpy(&data->regs.demod[page][reg], src, len);
			rtl_demod_written(c_data, page, reg, len);
		}

		return 0;
	}
	case RTL_BLOCK_IIC:
		if ((setup->wValue & 0xFFU) == R820T2_I2C_ADDR) {
			(void)k_work_reschedule(&rtl_tuner_log_work, RTL_TUNER_LOG_DELAY);
			return r820t2_i2c_write(src, len);
		}

		/* Demodulator registers over I2C are not emulated */
		return 0;
	case RTL_BLOCK_USB:
	case RTL_BLOCK_SYS:
		dst = rtl_reg_ptr(data, block, setup->wValue, len);
		if (dst != NULL) {
			memcpy(dst, src, len);
		}

		if (block == RTL_BLOCK_USB && setup->wValue == RTL_USB_EPA_CTL && len == 2U) {
			rtl_stream_set_running(c_data, sys_get_be16(src) == 0U);
		}

		return 0;
	default:
		/* Accept and discard writes to the other blocks */
		return 0;
	}
}

static int rtl_control_to_dev(struct usbd_class_data *const c_data,
			      const struct usb_setup_packet *const setup,
			      const struct net_buf *const buf)
{
	struct rtl_data *data = usbd_class_get_private(c_data);

	if (setup->RequestType.type != USB_REQTYPE_TYPE_VENDOR) {
		return -ENOTSUP;
	}

	if (RTL_CTRL_BLOCK(setup->wIndex) == RTL_BLOCK_IIC &&
	    !rtl_i2c_addr_acked(data, setup->wValue & 0xFFU)) {
		/* No device acknowledges this address: STALL like the real chip */
		return -EIO;
	}

	/* Without a data stage the stack handles reads here too: nothing to do */
	if (setup->wLength == 0U) {
		return 0;
	}

	if ((setup->wIndex & RTL_CTRL_WRITE) == 0U) {
		return -ENOTSUP;
	}

	if (buf == NULL) {
		/* Called before the data stage: accept it */
		return 0;
	}

	return rtl_write(c_data, setup, buf->data, buf->len);
}

static struct net_buf *rtl_control_to_host(struct usbd_class_data *const c_data,
					   const struct usb_setup_packet *const setup)
{
	struct rtl_data *data = usbd_class_get_private(c_data);
	const uint8_t block = RTL_CTRL_BLOCK(setup->wIndex);
	const uint16_t len = setup->wLength;
	struct net_buf *buf;
	uint8_t *dst;
	uint8_t *src;

	if (setup->RequestType.type != USB_REQTYPE_TYPE_VENDOR) {
		return NULL;
	}

	if (block == RTL_BLOCK_IIC && !rtl_i2c_addr_acked(data, setup->wValue & 0xFFU)) {
		return NULL;
	}

	buf = usbd_ep_ctrl_data_in_alloc(usbd_class_get_ctx(c_data), len);
	if (buf == NULL) {
		return NULL;
	}

	dst = net_buf_add(buf, len);
	memset(dst, 0, len);

	switch (block) {
	case RTL_BLOCK_DEMOD: {
		const uint8_t page = setup->wIndex & 0x0FU;
		const uint8_t reg = setup->wValue >> 8;

		if (page < RTL2832U_DEMOD_PAGES && reg + len <= RTL2832U_DEMOD_PAGE_SIZE) {
			memcpy(dst, &data->regs.demod[page][reg], len);
		}
		break;
	}
	case RTL_BLOCK_IIC:
		if ((setup->wValue & 0xFFU) == R820T2_I2C_ADDR) {
			(void)r820t2_i2c_read(dst, len);
		}
		break;
	case RTL_BLOCK_USB:
	case RTL_BLOCK_SYS:
		src = rtl_reg_ptr(data, block, setup->wValue, len);
		if (src != NULL) {
			memcpy(dst, src, len);
		}
		break;
	default:
		/* Other blocks read as zero */
		break;
	}

	return buf;
}

static int rtl_request(struct usbd_class_data *const c_data, struct net_buf *buf, int err)
{
	struct rtl_data *data = usbd_class_get_private(c_data);
	struct udc_buf_info *bi = udc_get_buf_info(buf);

	if (err != 0 && err != -ECONNABORTED) {
		LOG_WRN("ep 0x%02x transfer error %d", bi->ep, err);
	}

	net_buf_unref(buf);

	if (err == 0) {
		atomic_inc(&data->xfers);
		rtl_stream_kick(c_data);
	}

	return 0;
}

static void rtl_enable(struct usbd_class_data *const c_data)
{
	struct rtl_data *data = usbd_class_get_private(c_data);

	if (!atomic_test_and_set_bit(&data->state, RTL_STATE_ENABLED)) {
		LOG_INF("Configuration enabled");
		rtl_stream_kick(c_data);
	}
}

static void rtl_disable(struct usbd_class_data *const c_data)
{
	struct rtl_data *data = usbd_class_get_private(c_data);

	atomic_clear_bit(&data->state, RTL_STATE_ENABLED);
	LOG_INF("Configuration disabled");
}

static int rtl_init(struct usbd_class_data *const c_data)
{
	struct rtl_data *data = usbd_class_get_private(c_data);

	memset(&data->regs, 0, sizeof(data->regs));
	atomic_clear(&data->state);
	data->sample_rate = 0U;
	r820t2_init();

	return 0;
}

static const void *rtl_get_desc(struct usbd_class_data *const c_data,
				const enum usbd_speed speed)
{
	struct rtl_data *data = usbd_class_get_private(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
		return data->hs_desc;
	}

	return data->fs_desc;
}

static struct rtl_desc rtl_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0xFF,
		.bInterfaceProtocol = 0xFF,
		.iInterface = 0,
	},
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(RTL_FS_BULK_MPS),
		.bInterval = 0,
	},
	.if0_hs_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(RTL_HS_BULK_MPS),
		.bInterval = 0,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *const rtl_fs_desc[] = {
	(struct usb_desc_header *)&rtl_desc.if0,
	(struct usb_desc_header *)&rtl_desc.if0_in_ep,
	(struct usb_desc_header *)&rtl_desc.nil_desc,
};

static const struct usb_desc_header *const rtl_hs_desc[] = {
	(struct usb_desc_header *)&rtl_desc.if0,
	(struct usb_desc_header *)&rtl_desc.if0_hs_in_ep,
	(struct usb_desc_header *)&rtl_desc.nil_desc,
};

static struct rtl_data rtl_data = {
	.desc = &rtl_desc,
	.fs_desc = rtl_fs_desc,
	.hs_desc = rtl_hs_desc,
};

/* IF frequency in Hz; librtlsdr programs the negated IF */
static uint32_t rtl_if_freq(const struct rtl_data *const data)
{
	const uint8_t *p = &data->regs.demod[RTL_DEMOD_IF_PAGE][RTL_DEMOD_IF_REG];
	int32_t raw = (int32_t)(((uint32_t)(p[0] & 0x3FU) << 16) | ((uint32_t)p[1] << 8) | p[2]);
	int64_t if_freq;

	/* Sign-extend the 22-bit value */
	if ((raw & BIT(21)) != 0) {
		raw -= (int32_t)BIT(22);
	}

	/* IF = -raw * xtal / 2^22 */
	if_freq = ((int64_t)-raw * (int64_t)RTL_XTAL_FREQ) / (int64_t)BIT64(22);

	return (if_freq > 0) ? (uint32_t)if_freq : 0U;
}

void rtl2832u_get_status(struct rtl2832u_status *status)
{
	const struct rtl_data *data = &rtl_data;

	status->configured = atomic_test_bit(&data->state, RTL_STATE_ENABLED);
	status->streaming = atomic_test_bit(&data->state, RTL_STATE_RUNNING);
	status->test_mode = (data->regs.demod[RTL_DEMOD_TEST_PAGE][RTL_DEMOD_TEST_REG] &
			     RTL_DEMOD_TEST_BIT) != 0U;
	status->sample_rate = data->sample_rate;
	status->if_freq = rtl_if_freq(data);
	status->lo_freq = r820t2_lo_freq();
	status->xfers = (uint32_t)atomic_get(&data->xfers);

	/* librtlsdr tunes the LO to the center frequency plus the IF */
	if (status->lo_freq > status->if_freq) {
		status->center_freq = status->lo_freq - status->if_freq;
	} else {
		status->center_freq = status->lo_freq;
	}
}

const uint8_t *rtl2832u_demod_regs(uint8_t page)
{
	if (page >= RTL2832U_DEMOD_PAGES) {
		return NULL;
	}

	return rtl_data.regs.demod[page];
}

/* Log tuner changes once the host has finished a sequence of register writes */
static void rtl_tuner_log_handler(struct k_work *work)
{
	static uint32_t last_center;
	static bool last_manual;
	static int32_t last_gain = INT32_MIN;
	struct rtl2832u_status status;
	int32_t gain = 0;
	bool manual;

	ARG_UNUSED(work);

	rtl2832u_get_status(&status);

	if (status.lo_freq != 0U && status.center_freq != last_center) {
		last_center = status.center_freq;
		LOG_INF("Center frequency %u Hz (LO %u Hz, IF %u Hz)", status.center_freq,
			status.lo_freq, status.if_freq);
	}

	manual = r820t2_manual_gain(&gain);
	if (manual != last_manual || gain != last_gain) {
		last_manual = manual;
		last_gain = gain;

		if (manual) {
			/* The gain step sums are never negative */
			LOG_INF("Tuner gain %d.%d dB", gain / 10, gain % 10);
		} else {
			LOG_INF("Tuner gain auto");
		}
	}
}

static const struct usbd_class_api rtl_api = {
	.control_to_dev = rtl_control_to_dev,
	.control_to_host = rtl_control_to_host,
	.request = rtl_request,
	.enable = rtl_enable,
	.disable = rtl_disable,
	.init = rtl_init,
	.get_desc = rtl_get_desc,
};

static const struct usbd_cctx_vendor_req rtl_vregs = USBD_VENDOR_REQ(RTL_VENDOR_REQ);

USBD_DEFINE_CLASS(rtl2832u, &rtl_api, &rtl_data, &rtl_vregs);
