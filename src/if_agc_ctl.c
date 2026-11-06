/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * IF AGC loop: keeps the level of the ADC codes at a target by setting the
 * IF_AGC voltage of the R820T2, which controls its VGA gain. The level is the
 * mean absolute deviation (MAD) of the codes from their mean; the error is
 * converted to dB with an integer log2 and to PWM counts with the slope of
 * the datasheet gain curve.
 */

#include "if_agc_ctl.h"

/* Initial reference code, the ADC mid-scale */
#define IF_AGC_CTL_DC_INIT 128U
/* Highest ADC code */
#define IF_AGC_CTL_CODE_MAX 255U
/* 20 * log10(2) * 16 / 256 in 1/4096: log2 in 1/256 to dB in 1/16 */
#define IF_AGC_CTL_LOG2_TO_DB 1541
/* log2(1 + i / 16) in 1/256 */
static const uint16_t log2_frac[17] = {
	0, 22, 44, 63, 82, 100, 118, 134, 150, 165, 179, 193, 207, 220, 232, 244, 256,
};

uint16_t if_agc_ctl_mv_to_counts(uint32_t mv)
{
	const uint32_t counts = (mv * IF_AGC_CTL_PERIOD + IF_AGC_CTL_VDD_MV / 2U) /
				IF_AGC_CTL_VDD_MV;

	return (uint16_t)((counts > IF_AGC_CTL_PERIOD) ? IF_AGC_CTL_PERIOD : counts);
}

uint32_t if_agc_ctl_counts_to_mv(uint16_t counts)
{
	return ((uint32_t)counts * IF_AGC_CTL_VDD_MV + IF_AGC_CTL_PERIOD / 2U) /
	       IF_AGC_CTL_PERIOD;
}

int32_t if_agc_ctl_gain(uint16_t counts)
{
	const int32_t lo = if_agc_ctl_mv_to_counts(IF_AGC_CTL_LOW_MV);
	const int32_t hi = if_agc_ctl_mv_to_counts(IF_AGC_CTL_HIGH_MV);

	return IF_AGC_CTL_LOW_GAIN +
	       ((int32_t)counts - lo) * (IF_AGC_CTL_HIGH_GAIN - IF_AGC_CTL_LOW_GAIN) / (hi - lo);
}

int32_t if_agc_ctl_log2_q8(uint32_t x)
{
	uint32_t msb = 31U;
	uint32_t norm;
	uint32_t idx;
	uint32_t frac;

	if (x == 0U) {
		return 0;
	}

	while ((x & (1UL << msb)) == 0U) {
		msb--;
	}

	/* Leading one at bit 31, then 4 bits table index and 8 bits interpolation */
	norm = x << (31U - msb);
	idx = (norm >> 27) & 0x0FU;
	frac = (norm >> 19) & 0xFFU;

	return (int32_t)(msb * 256U + log2_frac[idx] +
			 (((uint32_t)(log2_frac[idx + 1U] - log2_frac[idx]) * frac + 128U) >> 8));
}

void if_agc_ctl_init(struct if_agc_ctl *ctl, uint32_t min_mv, uint32_t max_mv,
		     uint16_t target_q4, uint16_t counts)
{
	ctl->min_counts = if_agc_ctl_mv_to_counts(min_mv);
	ctl->max_counts = if_agc_ctl_mv_to_counts(max_mv);
	ctl->target_q4 = target_q4;
	ctl->mad_q4 = 0U;
	ctl->dc = IF_AGC_CTL_DC_INIT;

	if (counts < ctl->min_counts) {
		counts = ctl->min_counts;
	} else if (counts > ctl->max_counts) {
		counts = ctl->max_counts;
	} else {
		/* In range */
	}

	ctl->counts = counts;
}

void if_agc_ctl_measure(struct if_agc_meas *meas, const uint8_t *in, size_t len, size_t stride,
			uint8_t dc)
{
	uint32_t count = 0U;
	uint32_t sum = 0U;
	uint32_t dev_sum = 0U;
	uint32_t clipped = 0U;

	for (size_t i = 0U; i < len; i += stride) {
		const uint32_t code = in[i];

		sum += code;
		dev_sum += (code >= dc) ? (code - dc) : (dc - code);

		if (code == 0U || code == IF_AGC_CTL_CODE_MAX) {
			clipped++;
		}

		count++;
	}

	meas->count += count;
	meas->sum += sum;
	meas->dev_sum += dev_sum;
	meas->clipped += clipped;
}

/* Gain step for the level error, in 1/16 dB */
static int32_t level_step(const struct if_agc_ctl *ctl)
{
	int32_t err;

	if (ctl->mad_q4 == 0U) {
		/* No signal at all */
		return IF_AGC_CTL_STEP_MAX_Q4;
	}

	err = (if_agc_ctl_log2_q8(ctl->target_q4) - if_agc_ctl_log2_q8(ctl->mad_q4)) *
	      IF_AGC_CTL_LOG2_TO_DB / 4096;

	if (err >= -IF_AGC_CTL_DEADBAND_Q4 && err <= IF_AGC_CTL_DEADBAND_Q4) {
		return 0;
	}

	/* Half the error: the RC low-pass delays the effect of a step */
	err /= 2;

	if (err > IF_AGC_CTL_STEP_MAX_Q4) {
		return IF_AGC_CTL_STEP_MAX_Q4;
	}

	if (err < -IF_AGC_CTL_STEP_MAX_Q4) {
		return -IF_AGC_CTL_STEP_MAX_Q4;
	}

	return err;
}

uint16_t if_agc_ctl_update(struct if_agc_ctl *ctl, const struct if_agc_meas *meas)
{
	/* PWM counts per dB in 1/256 */
	const int32_t lo = if_agc_ctl_mv_to_counts(IF_AGC_CTL_LOW_MV);
	const int32_t hi = if_agc_ctl_mv_to_counts(IF_AGC_CTL_HIGH_MV);
	const int32_t counts_per_db_q8 =
		(hi - lo) * 2560 / (IF_AGC_CTL_HIGH_GAIN - IF_AGC_CTL_LOW_GAIN);
	int32_t step_q4;
	int32_t counts;

	if (meas->count == 0U) {
		return ctl->counts;
	}

	ctl->dc = (uint8_t)((meas->sum + meas->count / 2U) / meas->count);
	ctl->mad_q4 = (uint16_t)((meas->dev_sum * 16U + meas->count / 2U) / meas->count);

	if ((meas->clipped << IF_AGC_CTL_CLIP_SHIFT) > meas->count) {
		step_q4 = -IF_AGC_CTL_STEP_MAX_Q4;
	} else {
		step_q4 = level_step(ctl);
	}

	counts = (int32_t)ctl->counts + step_q4 * counts_per_db_q8 / 4096;

	if (counts < (int32_t)ctl->min_counts) {
		counts = ctl->min_counts;
	} else if (counts > (int32_t)ctl->max_counts) {
		counts = ctl->max_counts;
	} else {
		/* In range */
	}

	ctl->counts = (uint16_t)counts;

	return ctl->counts;
}
