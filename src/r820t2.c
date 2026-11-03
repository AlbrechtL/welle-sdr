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

/* Register 5: LNA gain index (bits 3:0), bit 4 set disables LNA auto gain */
#define R820T2_REG_LNA        0x05U
#define R820T2_LNA_MANUAL     0x10U
/* Register 7: mixer gain index (bits 3:0) */
#define R820T2_REG_MIXER      0x07U
#define R820T2_GAIN_IDX_MASK  0x0FU
/* Register 0x10: divider (bits 7:5), reference divided by 2 (bit 4) */
#define R820T2_REG_DIV        0x10U
#define R820T2_DIV_SHIFT      5U
#define R820T2_REFDIV2        0x10U
/* Register 0x12 bit 3: sigma-delta modulator powered off */
#define R820T2_REG_VCO        0x12U
#define R820T2_SDM_OFF        0x08U
/* Register 0x14: Ni (bits 5:0) and Si (bits 7:6), Nint = 4 * Ni + Si + 13 */
#define R820T2_REG_NI_SI      0x14U
/* Registers 0x15 (low) and 0x16 (high): SDM fraction, in 1/65536 */
#define R820T2_REG_SDM_LO     0x15U
#define R820T2_REG_SDM_HI     0x16U

#define R820T2_XTAL_FREQ      28800000ULL

static uint8_t regs[R820T2_NUM_REGS];
/* The host has programmed the PLL integer divider */
static bool pll_valid;

/* LNA and mixer gain steps in 0.1 dB, as in librtlsdr tuner_r82xx.c */
static const int8_t lna_gain_steps[16] = {
	0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13,
};
static const int8_t mixer_gain_steps[16] = {
	0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8,
};

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
	pll_valid = false;
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

		if (reg == R820T2_REG_NI_SI) {
			pll_valid = true;
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

uint32_t r820t2_lo_freq(void)
{
	const uint8_t div = regs[R820T2_REG_DIV];
	const uint64_t mix_div = 2ULL << (div >> R820T2_DIV_SHIFT);
	const uint64_t nint = 4U * (regs[R820T2_REG_NI_SI] & 0x3FU) +
			      (regs[R820T2_REG_NI_SI] >> 6) + 13U;
	uint64_t pll_ref = R820T2_XTAL_FREQ;
	uint64_t sdm = 0U;

	if (!pll_valid) {
		return 0U;
	}

	if ((div & R820T2_REFDIV2) != 0U) {
		pll_ref /= 2U;
	}

	if ((regs[R820T2_REG_VCO] & R820T2_SDM_OFF) == 0U) {
		sdm = ((uint64_t)regs[R820T2_REG_SDM_HI] << 8) | regs[R820T2_REG_SDM_LO];
	}

	/* VCO = 2 * ref * (Nint + SDM / 65536), LO = VCO / mix_div */
	return (uint32_t)((2U * pll_ref * ((nint << 16) + sdm)) / (mix_div << 16));
}

bool r820t2_manual_gain(int32_t *gain_tenth_db)
{
	const uint8_t lna = regs[R820T2_REG_LNA] & R820T2_GAIN_IDX_MASK;
	const uint8_t mixer = regs[R820T2_REG_MIXER] & R820T2_GAIN_IDX_MASK;
	int32_t gain = 0;

	if ((regs[R820T2_REG_LNA] & R820T2_LNA_MANUAL) == 0U) {
		return false;
	}

	for (uint8_t i = 1U; i <= lna; i++) {
		gain += lna_gain_steps[i];
	}

	for (uint8_t i = 1U; i <= mixer; i++) {
		gain += mixer_gain_steps[i];
	}

	*gain_tenth_db = gain;

	return true;
}
