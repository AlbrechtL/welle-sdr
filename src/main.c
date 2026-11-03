/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(rtlsdr_emu, LOG_LEVEL_INF);

/* Generic RTL2832U OEM dongle, as listed in librtlsdr known_devices */
#define RTL_USB_VID 0x0bda
#define RTL_USB_PID 0x2838

/* bMaxPower in 2 mA units */
#define RTL_USB_MAX_POWER 250U

USBD_DEVICE_DEFINE(rtl_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), RTL_USB_VID, RTL_USB_PID);

USBD_DESC_LANG_DEFINE(rtl_lang);
/* librtlsdr matches VID/PID only, the strings tell users what they are talking to */
USBD_DESC_MANUFACTURER_DEFINE(rtl_mfr, "Zephyr CH32V307");
USBD_DESC_PRODUCT_DEFINE(rtl_product, "RTL-SDR compatible (RTL2832U/R820T2 emulation)");
USBD_DESC_STRING_DEFINE(rtl_sn, "00000001", USBD_DUT_STRING_SERIAL_NUMBER);

USBD_CONFIGURATION_DEFINE(rtl_fs_config, 0, RTL_USB_MAX_POWER, NULL);
USBD_CONFIGURATION_DEFINE(rtl_hs_config, 0, RTL_USB_MAX_POWER, NULL);

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

	err = rtl_usbd_init();
	if (err != 0) {
		LOG_ERR("USB device setup failed (%d)", err);
		return err;
	}

	LOG_INF("RTL2832U/R820T2 emulator ready");

	return 0;
}
