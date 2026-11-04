/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_RTL2832U_H_
#define WELLE_SDR_RTL2832U_H_

#include <stdbool.h>
#include <stdint.h>

#define RTL2832U_DEMOD_PAGES     5U
#define RTL2832U_DEMOD_PAGE_SIZE 256U

/** Snapshot of the emulated RTL2832U and tuner state. */
struct rtl2832u_status {
	/** USB configuration is enabled. */
	bool configured;
	/** The host has started streaming (USB_EPA_CTL is 0). */
	bool streaming;
	/** A host driver has opened the device (demodulator powered on). */
	bool host_open;
	/** Counter test output selected. */
	bool test_mode;
	/** Sample rate set by the host in S/s, 0 if not set yet. */
	uint32_t sample_rate;
	/** IF frequency programmed into the demodulator in Hz. */
	uint32_t if_freq;
	/** Tuner LO frequency in Hz, 0 if the PLL is not programmed yet. */
	uint32_t lo_freq;
	/** Tuned center frequency (LO minus IF) in Hz. */
	uint32_t center_freq;
	/** Completed bulk IN transfers. */
	uint32_t xfers;
};

/**
 * @brief Read the current emulator state.
 *
 * @param status Filled with the current state.
 */
void rtl2832u_get_status(struct rtl2832u_status *status);

/**
 * @brief Get a demodulator register page.
 *
 * @param page Page number.
 *
 * @return RTL2832U_DEMOD_PAGE_SIZE register bytes, or NULL if @p page does
 *	   not exist.
 */
const uint8_t *rtl2832u_demod_regs(uint8_t page);

#endif /* WELLE_SDR_RTL2832U_H_ */
