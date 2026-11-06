/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "if_agc_ctl.h"

#define TEST_LEN       4096U
#define TEST_DC        128.0
#define TEST_TARGET_Q4 (24U * 16U)
#define TEST_PI        3.14159265358979
/* Sine periods per block, not a divisor of TEST_LEN, so the samples cover all phases */
#define TEST_CYCLES    37.3

/* PWM counts for one dB of VGA gain, from the datasheet curve */
static double counts_per_db(void)
{
	return (double)(if_agc_ctl_mv_to_counts(IF_AGC_CTL_HIGH_MV) -
			if_agc_ctl_mv_to_counts(IF_AGC_CTL_LOW_MV)) /
	       ((IF_AGC_CTL_HIGH_GAIN - IF_AGC_CTL_LOW_GAIN) / 10.0);
}

/* A sine of amplitude amp around TEST_DC, clipped to the code range */
static void make_sine(uint8_t *buf, size_t n, double amp)
{
	for (size_t k = 0U; k < n; k++) {
		double v = TEST_DC + amp * sin(2.0 * TEST_PI * TEST_CYCLES * (double)k / (double)n);

		v = (v < 0.0) ? 0.0 : ((v > 255.0) ? 255.0 : v);
		buf[k] = (uint8_t)lround(v);
	}
}

/* Run one update on a sine of amplitude amp, starting at start_mv; returns the count change */
static int32_t step_for_sine(double amp, uint32_t start_mv)
{
	static uint8_t buf[TEST_LEN];
	struct if_agc_meas meas = {0};
	struct if_agc_ctl ctl;
	uint16_t start;

	if_agc_ctl_init(&ctl, IF_AGC_CTL_LOW_MV, IF_AGC_CTL_HIGH_MV, TEST_TARGET_Q4,
			if_agc_ctl_mv_to_counts(start_mv));
	start = ctl.counts;

	make_sine(buf, TEST_LEN, amp);
	if_agc_ctl_measure(&meas, buf, TEST_LEN, 1U, ctl.dc);

	return (int32_t)if_agc_ctl_update(&ctl, &meas) - (int32_t)start;
}

ZTEST(if_agc, test_log2_accuracy)
{
	for (uint32_t x = 1U; x < 1000000U; x = x * 9U / 8U + 1U) {
		const double err_db = 20.0 * log10(2.0) *
				      fabs(if_agc_ctl_log2_q8(x) / 256.0 - log2((double)x));

		zassert_true(err_db < 0.05, "log2(%u) off by %.3f dB", x, err_db);
	}

	zassert_equal(if_agc_ctl_log2_q8(0U), 0);
	zassert_equal(if_agc_ctl_log2_q8(1U), 0);
	zassert_equal(if_agc_ctl_log2_q8(1024U), 10 * 256);
}

ZTEST(if_agc, test_voltage_conversion)
{
	zassert_equal(if_agc_ctl_mv_to_counts(0U), 0U);
	zassert_equal(if_agc_ctl_mv_to_counts(IF_AGC_CTL_VDD_MV / 2U), IF_AGC_CTL_PERIOD / 2U);
	zassert_equal(if_agc_ctl_mv_to_counts(IF_AGC_CTL_VDD_MV), IF_AGC_CTL_PERIOD);
	zassert_equal(if_agc_ctl_mv_to_counts(5000U), IF_AGC_CTL_PERIOD, "not clamped");
	zassert_equal(if_agc_ctl_counts_to_mv(IF_AGC_CTL_PERIOD / 2U), IF_AGC_CTL_VDD_MV / 2U);

	for (uint32_t mv = 0U; mv <= IF_AGC_CTL_VDD_MV; mv += 50U) {
		const uint32_t back = if_agc_ctl_counts_to_mv(if_agc_ctl_mv_to_counts(mv));

		zassert_true(abs((int)back - (int)mv) <= 1, "%u mV back as %u mV", mv, back);
	}
}

ZTEST(if_agc, test_gain_curve)
{
	zassert_equal(if_agc_ctl_gain(if_agc_ctl_mv_to_counts(IF_AGC_CTL_LOW_MV)),
		      IF_AGC_CTL_LOW_GAIN);
	zassert_equal(if_agc_ctl_gain(if_agc_ctl_mv_to_counts(IF_AGC_CTL_HIGH_MV)),
		      IF_AGC_CTL_HIGH_GAIN);
	zassert_true(if_agc_ctl_gain(if_agc_ctl_mv_to_counts(1550U)) > 210 &&
			     if_agc_ctl_gain(if_agc_ctl_mv_to_counts(1550U)) < 230,
		     "1550 mV not about +22 dB");
}

ZTEST(if_agc, test_measure_stride_and_accumulate)
{
	static uint8_t buf[TEST_LEN];
	struct if_agc_meas meas = {0};

	memset(buf, 130, sizeof(buf));
	buf[0] = 0U;
	buf[4] = 255U;
	/* Not on the stride, not counted */
	buf[1] = 0U;

	if_agc_ctl_measure(&meas, buf, TEST_LEN, 4U, 128U);
	zassert_equal(meas.count, TEST_LEN / 4U);
	zassert_equal(meas.clipped, 2U);
	zassert_equal(meas.sum, 130U * (TEST_LEN / 4U - 2U) + 255U);
	zassert_equal(meas.dev_sum, 2U * (TEST_LEN / 4U - 2U) + 128U + 127U);

	if_agc_ctl_measure(&meas, buf, TEST_LEN, 4U, 128U);
	zassert_equal(meas.count, TEST_LEN / 2U, "second block not added");
	zassert_equal(meas.clipped, 4U);
}

ZTEST(if_agc, test_no_signal_steps_up)
{
	static uint8_t buf[TEST_LEN];
	struct if_agc_meas meas = {0};
	struct if_agc_ctl ctl;
	const double max_step = IF_AGC_CTL_STEP_MAX_Q4 / 16.0 * counts_per_db();
	uint16_t start;
	int32_t step;

	if_agc_ctl_init(&ctl, IF_AGC_CTL_LOW_MV, IF_AGC_CTL_HIGH_MV, TEST_TARGET_Q4,
			if_agc_ctl_mv_to_counts(1550U));
	start = ctl.counts;

	memset(buf, 128, sizeof(buf));
	if_agc_ctl_measure(&meas, buf, TEST_LEN, 1U, ctl.dc);
	step = (int32_t)if_agc_ctl_update(&ctl, &meas) - (int32_t)start;

	zassert_equal(ctl.mad_q4, 0U);
	zassert_true(fabs(step - max_step) <= 1.0, "step %d, expected %.1f", step, max_step);
}

ZTEST(if_agc, test_clipping_steps_down)
{
	const double max_step = IF_AGC_CTL_STEP_MAX_Q4 / 16.0 * counts_per_db();
	/* Far beyond full scale: many samples at 0 and 255 */
	const int32_t step = step_for_sine(400.0, 1550U);

	zassert_true(fabs(-step - max_step) <= 1.0, "step %d, expected -%.1f", step, max_step);
}

ZTEST(if_agc, test_at_target_no_step)
{
	/* The MAD of a sine is 2 / pi of its amplitude */
	const double amp = (TEST_TARGET_Q4 / 16.0) * TEST_PI / 2.0;

	zassert_equal(step_for_sine(amp, 1550U), 0);
	/* Within the 1 dB dead band */
	zassert_equal(step_for_sine(amp * pow(10.0, 0.8 / 20.0), 1550U), 0);
	zassert_equal(step_for_sine(amp * pow(10.0, -0.8 / 20.0), 1550U), 0);
}

ZTEST(if_agc, test_half_error_step)
{
	const double amp = (TEST_TARGET_Q4 / 16.0) * TEST_PI / 2.0;
	/* 2 dB too low: one step of 1 dB up; 2 dB too high: 1 dB down */
	const int32_t up = step_for_sine(amp * pow(10.0, -2.0 / 20.0), 1550U);
	const int32_t down = step_for_sine(amp * pow(10.0, 2.0 / 20.0), 1550U);

	zassert_true(fabs(up - counts_per_db()) < 0.25 * counts_per_db(), "up %d", up);
	zassert_true(fabs(-down - counts_per_db()) < 0.25 * counts_per_db(), "down %d", down);
}

ZTEST(if_agc, test_step_limit)
{
	const double amp = (TEST_TARGET_Q4 / 16.0) * TEST_PI / 2.0;
	const double max_step = IF_AGC_CTL_STEP_MAX_Q4 / 16.0 * counts_per_db();
	/* 20 dB too low: half the error is 10 dB, limited to 3 dB */
	const int32_t step = step_for_sine(amp / 10.0, 1550U);

	zassert_true(fabs(step - max_step) <= 1.0, "step %d, expected %.1f", step, max_step);
}

ZTEST(if_agc, test_range_clamp)
{
	const uint16_t lo = if_agc_ctl_mv_to_counts(IF_AGC_CTL_LOW_MV);
	const uint16_t hi = if_agc_ctl_mv_to_counts(IF_AGC_CTL_HIGH_MV);
	struct if_agc_ctl ctl;

	/* Start values outside the range are clamped */
	if_agc_ctl_init(&ctl, IF_AGC_CTL_LOW_MV, IF_AGC_CTL_HIGH_MV, TEST_TARGET_Q4, 0U);
	zassert_equal(ctl.counts, lo);
	if_agc_ctl_init(&ctl, IF_AGC_CTL_LOW_MV, IF_AGC_CTL_HIGH_MV, TEST_TARGET_Q4,
			IF_AGC_CTL_PERIOD);
	zassert_equal(ctl.counts, hi);

	/* Steps stop at the ends */
	zassert_equal(step_for_sine(0.0, IF_AGC_CTL_HIGH_MV), 0, "stepped above the range");
	zassert_equal(step_for_sine(400.0, IF_AGC_CTL_LOW_MV), 0, "stepped below the range");
}

ZTEST(if_agc, test_empty_interval)
{
	struct if_agc_meas meas = {0};
	struct if_agc_ctl ctl;

	if_agc_ctl_init(&ctl, IF_AGC_CTL_LOW_MV, IF_AGC_CTL_HIGH_MV, TEST_TARGET_Q4,
			if_agc_ctl_mv_to_counts(1550U));

	zassert_equal(if_agc_ctl_update(&ctl, &meas), if_agc_ctl_mv_to_counts(1550U));
}

ZTEST_SUITE(if_agc, NULL, NULL, NULL, NULL, NULL);
