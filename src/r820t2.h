/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTLSDR_EMU_R820T2_H_
#define RTLSDR_EMU_R820T2_H_

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

#endif /* RTLSDR_EMU_R820T2_H_ */
