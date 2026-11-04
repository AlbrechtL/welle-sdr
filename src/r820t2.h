/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_R820T2_H_
#define WELLE_SDR_R820T2_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 8-bit I2C address as used by librtlsdr (7-bit 0x1a) */
#define R820T2_I2C_ADDR 0x34U

#define R820T2_NUM_REGS 32U

/** Reset the emulated register file to its power-on state. */
void r820t2_init(void);

/**
 * @brief Handle an I2C write to the tuner.
 *
 * @param data First byte is the start register, followed by register values.
 * @param len Number of bytes in @p data.
 *
 * @retval 0 Success.
 * @retval -EINVAL Empty write.
 */
int r820t2_i2c_write(const uint8_t *data, size_t len);

/**
 * @brief Handle an I2C read from the tuner.
 *
 * The R820T2 always reads from register 0 and returns each byte
 * bit-reversed, librtlsdr reverses them again.
 *
 * @param data Buffer for the bytes on the bus.
 * @param len Number of bytes to read.
 *
 * @retval 0 Success.
 */
int r820t2_i2c_read(uint8_t *data, size_t len);

/** @return Register file as written by the host (not bit-reversed). */
const uint8_t *r820t2_regs(void);

/**
 * @brief Decode the local oscillator frequency from the PLL registers.
 *
 * Inverse of the librtlsdr PLL setup, with the 28.8 MHz reference. The host
 * computes the fractional part in kHz steps, so the result is within about
 * 1 kHz of the frequency it asked for.
 *
 * @return LO frequency in Hz, 0 until the host has programmed the PLL.
 */
uint32_t r820t2_lo_freq(void);

/**
 * @brief Decode the LNA and mixer gain setting.
 *
 * The gain is the sum of the librtlsdr LNA and mixer gain steps for the
 * programmed indices, the same value rtl_test lists. The VGA is not included.
 *
 * @param gain_tenth_db Set to the gain in 0.1 dB in manual mode.
 *
 * @retval true Manual gain, @p gain_tenth_db is valid.
 * @retval false Automatic gain control.
 */
bool r820t2_manual_gain(int32_t *gain_tenth_db);

#endif /* WELLE_SDR_R820T2_H_ */
