/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_IF_AGC_CTL_H_
#define WELLE_SDR_IF_AGC_CTL_H_

#include <stddef.h>
#include <stdint.h>

/** PWM period in timer counts (12-bit resolution). */
#define IF_AGC_CTL_PERIOD 4096U
/** PWM high level in mV. */
#define IF_AGC_CTL_VDD_MV 3300U

/** IF_AGC voltages and VGA gains (0.1 dB) at the ends of the datasheet curve. */
#define IF_AGC_CTL_LOW_MV    600U
#define IF_AGC_CTL_LOW_GAIN  (-15)
#define IF_AGC_CTL_HIGH_MV   2500U
#define IF_AGC_CTL_HIGH_GAIN 455

/** Largest gain step per update, in 1/16 dB. */
#define IF_AGC_CTL_STEP_MAX_Q4 (3 * 16)
/** Level errors up to this size cause no step, in 1/16 dB. */
#define IF_AGC_CTL_DEADBAND_Q4 16
/** More than count >> IF_AGC_CTL_CLIP_SHIFT clipped samples step the gain down at once. */
#define IF_AGC_CTL_CLIP_SHIFT 10U

/** Level statistics of the ADC codes over one update interval. */
struct if_agc_meas {
	/** Samples looked at. */
	uint32_t count;
	/** Sum of the codes. */
	uint32_t sum;
	/** Sum of |code - dc|. */
	uint32_t dev_sum;
	/** Samples at code 0 or 255. */
	uint32_t clipped;
};

/** Loop state. */
struct if_agc_ctl {
	/** Lowest and highest PWM compare value. */
	uint16_t min_counts;
	uint16_t max_counts;
	/** Mean absolute deviation to reach, in 1/16 code. */
	uint16_t target_q4;
	/** Current PWM compare value. */
	uint16_t counts;
	/** Mean absolute deviation of the last interval, in 1/16 code. */
	uint16_t mad_q4;
	/** Mean code of the last interval, the reference for the next one. */
	uint8_t dc;
};

/**
 * @param mv Voltage in mV.
 *
 * @return PWM compare value for @p mv, at most IF_AGC_CTL_PERIOD.
 */
uint16_t if_agc_ctl_mv_to_counts(uint32_t mv);

/**
 * @param counts PWM compare value.
 *
 * @return Voltage after the low-pass in mV.
 */
uint32_t if_agc_ctl_counts_to_mv(uint16_t counts);

/**
 * @brief Approximate VGA gain for a PWM compare value.
 *
 * Linear between the ends of the datasheet curve, extended beyond them.
 *
 * @param counts PWM compare value.
 *
 * @return Gain in 0.1 dB.
 */
int32_t if_agc_ctl_gain(uint16_t counts);

/**
 * @param x Value, at least 1.
 *
 * @return log2(@p x) in 1/256, 0 for @p x = 0.
 */
int32_t if_agc_ctl_log2_q8(uint32_t x);

/**
 * @brief Initialize the loop.
 *
 * @param ctl State to initialize.
 * @param min_mv Lowest IF_AGC voltage in mV.
 * @param max_mv Highest IF_AGC voltage in mV.
 * @param target_q4 Mean absolute deviation to reach, in 1/16 code.
 * @param counts Start value of the PWM compare value, clamped to the range.
 */
void if_agc_ctl_init(struct if_agc_ctl *ctl, uint32_t min_mv, uint32_t max_mv,
		     uint16_t target_q4, uint16_t counts);

/**
 * @brief Add every @p stride-th sample of a block to the statistics.
 *
 * @param meas Statistics to add to.
 * @param in ADC codes.
 * @param len Number of codes in @p in.
 * @param stride Distance between the samples looked at, at least 1.
 * @param dc Reference code for the deviation.
 */
void if_agc_ctl_measure(struct if_agc_meas *meas, const uint8_t *in, size_t len, size_t stride,
			uint8_t dc);

/**
 * @brief Compute the next PWM compare value from one interval.
 *
 * Steps the gain down by IF_AGC_CTL_STEP_MAX_Q4 when too many samples clip.
 * Otherwise steps by half the level error in dB, limited to
 * IF_AGC_CTL_STEP_MAX_Q4, with a dead band of IF_AGC_CTL_DEADBAND_Q4.
 *
 * @param ctl Loop state, updated.
 * @param meas Statistics of the interval.
 *
 * @return New PWM compare value, also in @p ctl.
 */
uint16_t if_agc_ctl_update(struct if_agc_ctl *ctl, const struct if_agc_meas *meas);

#endif /* WELLE_SDR_IF_AGC_CTL_H_ */
