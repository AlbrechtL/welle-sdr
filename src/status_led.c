/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* USB state on the led0 LED, polled from the emulator state */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>

#include "rtl2832u.h"
#include "status_led.h"

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led0)), "Status LED needs the led0 alias");

#define STATUS_LED_TICK K_MSEC(50)

/*
 * Half period of the single blink when the host configures the device: on,
 * off, then on for as long as the device stays configured.
 */
#define STATUS_LED_BLINK_TICKS 5U

static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct usbd_context *status_led_ctx;

/* What the LED shows: configured by the host and not suspended */
static atomic_t usb_up;

static void status_led_tick(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(status_led_work, status_led_tick);

static void status_led_tick(struct k_work *work)
{
	static bool was_configured;
	static uint32_t blink_ticks;
	struct rtl2832u_status status;
	bool configured;

	ARG_UNUSED(work);

	rtl2832u_get_status(&status);
	configured = status.configured && !usbd_is_suspended(status_led_ctx);
	(void)atomic_set(&usb_up, configured ? 1 : 0);

	if (configured && !was_configured) {
		blink_ticks = 2U * STATUS_LED_BLINK_TICKS;
	}

	was_configured = configured;

	if (!configured) {
		/* No host, or the cable is removed: a removed cable reads as suspended */
		blink_ticks = 0U;
		(void)gpio_pin_set_dt(&status_led, 0);
	} else if (status.streaming) {
		blink_ticks = 0U;
		(void)gpio_pin_toggle_dt(&status_led);
	} else if (blink_ticks > 0U) {
		blink_ticks--;
		(void)gpio_pin_set_dt(&status_led, (blink_ticks >= STATUS_LED_BLINK_TICKS) ? 1 : 0);
	} else {
		(void)gpio_pin_set_dt(&status_led, 1);
	}

	(void)k_work_schedule(&status_led_work, STATUS_LED_TICK);
}

int status_led_init(struct usbd_context *ctx)
{
	int err;

	if (!gpio_is_ready_dt(&status_led)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		return err;
	}

	status_led_ctx = ctx;
	(void)k_work_schedule(&status_led_work, STATUS_LED_TICK);

	return 0;
}

bool status_led_usb_up(void)
{
	return atomic_get(&usb_up) != 0;
}
