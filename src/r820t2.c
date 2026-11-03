/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Register-level emulation of a Rafael Micro R820T2 tuner. Registers 0 to 4
 * are read-only status registers, 5 to 31 hold what the host writes. Values
 * are kept in the logical (not bit-reversed) form used by librtlsdr.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "r820t2.h"

LOG_MODULE_REGISTER(r820t2, LOG_LEVEL_INF);

#define R820T2_FIRST_RW_REG 5U

/* Chip ID, librtlsdr compares the raw (bit-reversed) value 0x69 */
#define R820T2_CHIP_ID       0x96U
/* Register 2 bit 6: PLL locked */
#define R820T2_PLL_LOCK      0x40U
/* Register 4: VCO fine tune 2 (bits 5:4, nominal), filter calibration code 8 */
#define R820T2_REG4_STATUS   0x28U

static uint8_t regs[R820T2_NUM_REGS];

static uint8_t bitrev8(uint8_t byte)
{
	byte = (uint8_t)(((byte & 0xF0U) >> 4) | ((byte & 0x0FU) << 4));
	byte = (uint8_t)(((byte & 0xCCU) >> 2) | ((byte & 0x33U) << 2));
	byte = (uint8_t)(((byte & 0xAAU) >> 1) | ((byte & 0x55U) << 1));

	return byte;
}

void r820t2_init(void)
{
	memset(regs, 0, sizeof(regs));
	regs[0] = R820T2_CHIP_ID;
	regs[2] = R820T2_PLL_LOCK;
	regs[4] = R820T2_REG4_STATUS;
}

int r820t2_i2c_write(const uint8_t *data, size_t len)
{
	size_t reg;

	if (len == 0U) {
		return -EINVAL;
	}

	/*
	 * The chip acknowledges any register address, writes beyond the
	 * register file are discarded. A pointer-only write sets nothing,
	 * reads always start at register 0.
	 */
	reg = data[0];

	for (size_t i = 1U; i < len && reg < R820T2_NUM_REGS; i++, reg++) {
		if (reg >= R820T2_FIRST_RW_REG) {
			regs[reg] = data[i];
		}
	}

	LOG_DBG("write reg 0x%02x len %zu", data[0], len - 1U);

	return 0;
}

int r820t2_i2c_read(uint8_t *data, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		data[i] = (i < R820T2_NUM_REGS) ? bitrev8(regs[i]) : 0U;
	}

	return 0;
}

const uint8_t *r820t2_regs(void)
{
	return regs;
}
