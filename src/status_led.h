/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_STATUS_LED_H_
#define WELLE_SDR_STATUS_LED_H_

#include <stdbool.h>

#include <zephyr/usb/usbd.h>

/**
 * @brief Start showing the USB state on the led0 LED.
 *
 * The LED is off while no host has configured the device or the bus is
 * suspended (also when the cable is removed). When the host configures the
 * device it blinks once and then stays on; it blinks at 5 Hz while streaming.
 *
 * @param ctx USB device context to watch.
 *
 * @retval 0 Success.
 * @retval -ENODEV GPIO controller not ready.
 * @retval -errno Error from configuring the GPIO pin.
 */
int status_led_init(struct usbd_context *ctx);

/**
 * @brief USB state as the LED shows it.
 *
 * Updated every 50 ms. The controller has no VBUS detection: a removed
 * cable reads as a suspend, while the device stays configured.
 *
 * @retval true The host has configured the device and the bus is not suspended.
 * @retval false Not configured, suspended or cable removed (LED off).
 */
bool status_led_usb_up(void);

#endif /* WELLE_SDR_STATUS_LED_H_ */
