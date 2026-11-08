/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>

#include "cpu_load.h"
#include "if_agc.h"
#include "status_led.h"

LOG_MODULE_REGISTER(welle_sdr, LOG_LEVEL_INF);

/* Generic RTL2832U OEM dongle, as listed in librtlsdr known_devices */
#define RTL_USB_VID 0x0bda
#define RTL_USB_PID 0x2838

/* bMaxPower in 2 mA units */
#define RTL_USB_MAX_POWER 250U

USBD_DEVICE_DEFINE(rtl_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), RTL_USB_VID, RTL_USB_PID);

USBD_DESC_LANG_DEFINE(rtl_lang);
/* librtlsdr matches VID/PID only, the strings tell users what they are talking to */
USBD_DESC_MANUFACTURER_DEFINE(rtl_mfr, "Zephyr CH32V307");
USBD_DESC_PRODUCT_DEFINE(rtl_product, "welle-sdr (RTL2832U/R820T2 emulation)");
USBD_DESC_STRING_DEFINE(rtl_sn, "00000001", USBD_DUT_STRING_SERIAL_NUMBER);

USBD_CONFIGURATION_DEFINE(rtl_fs_config, 0, RTL_USB_MAX_POWER, NULL);
USBD_CONFIGURATION_DEFINE(rtl_hs_config, 0, RTL_USB_MAX_POWER, NULL);

/*
 * The USBHS controller cannot detect VBUS: the first bus reset after boot or
 * after a suspend is reported as a connection, and a removed cable looks like
 * a suspend. The idle bus before the first reset also reads as suspended.
 */
static void rtl_usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	static bool host_seen;
	static bool new_session = true;
	const char *speed;

	switch (msg->type) {
	case USBD_MSG_RESET:
		speed = (usbd_bus_speed(ctx) == USBD_SPEED_HS) ? "high" : "full";

		if (new_session) {
			LOG_INF("USB host connected (%s speed)", speed);
		} else {
			LOG_INF("USB bus reset (%s speed)", speed);
		}

		new_session = false;
		host_seen = true;
		break;
	case USBD_MSG_CONFIGURATION:
		if (msg->status != 0) {
			LOG_INF("Configured by host (configuration %d)", msg->status);
		} else {
			LOG_INF("Deconfigured by host");
		}
		break;
	case USBD_MSG_SUSPEND:
		new_session = true;

		if (host_seen) {
			LOG_INF("USB suspended (cable removed or host idle)");
		} else {
			LOG_DBG("USB bus idle before first reset");
		}
		break;
	case USBD_MSG_RESUME:
		if (host_seen) {
			LOG_INF("USB resumed");
		} else {
			LOG_DBG("USB bus active");
		}
		break;
	case USBD_MSG_UDC_ERROR:
	case USBD_MSG_STACK_ERROR:
		LOG_ERR("%s (%d)", usbd_msg_type_string(msg->type), msg->status);
		break;
	default:
		LOG_DBG("%s", usbd_msg_type_string(msg->type));
		break;
	}
}

static int rtl_add_configuration(const enum usbd_speed speed, struct usbd_config_node *const cfg)
{
	int err;

	err = usbd_add_configuration(&rtl_usbd, speed, cfg);
	if (err != 0) {
		LOG_ERR("Failed to add configuration (%d)", err);
		return err;
	}

	err = usbd_register_class(&rtl_usbd, "rtl2832u", speed, 1);
	if (err != 0) {
		LOG_ERR("Failed to register class (%d)", err);
		return err;
	}

	return usbd_device_set_code_triple(&rtl_usbd, speed, 0, 0, 0);
}

static int rtl_usbd_init(void)
{
	struct usbd_desc_node *const descs[] = {&rtl_lang, &rtl_mfr, &rtl_product, &rtl_sn};
	int err;

	for (size_t i = 0U; i < ARRAY_SIZE(descs); i++) {
		err = usbd_add_descriptor(&rtl_usbd, descs[i]);
		if (err != 0) {
			LOG_ERR("Failed to add descriptor (%d)", err);
			return err;
		}
	}

	if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&rtl_usbd) == USBD_SPEED_HS) {
		err = rtl_add_configuration(USBD_SPEED_HS, &rtl_hs_config);
		if (err != 0) {
			return err;
		}
	}

	err = rtl_add_configuration(USBD_SPEED_FS, &rtl_fs_config);
	if (err != 0) {
		return err;
	}

	err = usbd_device_set_bcd_device(&rtl_usbd, 0x0100);
	if (err != 0) {
		return err;
	}

	err = usbd_msg_register_cb(&rtl_usbd, rtl_usbd_msg_cb);
	if (err != 0) {
		LOG_ERR("Failed to register message callback (%d)", err);
		return err;
	}

	err = usbd_init(&rtl_usbd);
	if (err != 0) {
		LOG_ERR("Failed to initialize device support (%d)", err);
		return err;
	}

	return usbd_enable(&rtl_usbd);
}

int main(void)
{
	int err;

	/* Before USB: the IF_AGC pin must not float once the host sets pin control */
	err = if_agc_init();
	if (err != 0) {
		LOG_WRN("IF AGC PWM not available (%d)", err);
	}

	cpu_load_init();

	err = status_led_init(&rtl_usbd);
	if (err != 0) {
		LOG_WRN("Status LED not available (%d)", err);
	}

	err = rtl_usbd_init();
	if (err != 0) {
		LOG_ERR("USB device setup failed (%d)", err);
		return err;
	}

	LOG_INF("RTL2832U/R820T2 emulator ready");

	return 0;
}
