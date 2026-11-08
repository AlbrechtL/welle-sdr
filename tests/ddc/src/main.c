/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "ddc.h"

#define TEST_IN_LEN    8000U
#define TEST_OUT_LEN   (2U * TEST_IN_LEN)
/* Outputs skipped at the start, while the CIC fills */
#define TEST_SETTLE    8U
/* Tone amplitude in codes: the output gain 2 sqrt(4) keeps it below saturation */
#define TEST_AMPLITUDE 12.0
#define TEST_PI        3.14159265358979

/* Real ADC samples of a tone of amp codes at freq_hz plus dc codes, around code 128 */
static void make_tone(uint8_t *buf, size_t n, double freq_hz, double amp, double dc,
		      uint32_t adc_rate)
{
	for (size_t k = 0U; k < n; k++) {
		const double v = 128.0 + dc + amp * cos(2.0 * TEST_PI * freq_hz * (double)k /
							(double)adc_rate);

		buf[k] = (uint8_t)lround(v);
	}
}

/* Level of the I/Q output at freq_hz in dB (single DFT bin, amplitude in codes) */
static double iq_level_db(const uint8_t *iq, size_t len, double freq_hz, uint32_t out_rate)
{
	double sum_re = 0.0;
	double sum_im = 0.0;
	size_t count = 0U;

	for (size_t m = TEST_SETTLE; m < len / 2U; m++) {
		const double i = iq[2U * m] - 127.0;
		const double q = iq[2U * m + 1U] - 127.0;
		const double a = -2.0 * TEST_PI * freq_hz * (double)m / (double)out_rate;

		/* (i + jq) * e^(ja) */
		sum_re += i * cos(a) - q * sin(a);
		sum_im += i * sin(a) + q * cos(a);
		count++;
	}

	return 20.0 * log10(sqrt(sum_re * sum_re + sum_im * sum_im) / (double)count + 1e-9);
}

/* The line at main_hz is at least min_db above the one at other_hz */
static void assert_line(const uint8_t *iq, size_t len, uint32_t out_rate, double main_hz,
			double other_hz, double min_db)
{
	const double main_db = iq_level_db(iq, len, main_hz, out_rate);
	const double other_db = iq_level_db(iq, len, other_hz, out_rate);

	zassert_true(main_db - other_db >= min_db, "main %d Hz %d dB, other %d Hz %d dB",
		     (int)main_hz, (int)main_db, (int)other_hz, (int)other_db);
}

/* Mean magnitude of the I/Q output */
static double iq_mag(const uint8_t *iq, size_t len)
{
	double sum = 0.0;
	size_t count = 0U;

	for (size_t m = TEST_SETTLE; m < len / 2U; m++) {
		const double i = iq[2U * m] - 127.0;
		const double q = iq[2U * m + 1U] - 127.0;

		sum += sqrt(i * i + q * q);
		count++;
	}

	return sum / (double)count;
}

static size_t run_tone(double tone_hz, double dc, int32_t if_hz, uint32_t out_rate, bool invert,
		       uint8_t *out, struct ddc_plan *plan)
{
	static uint8_t in[TEST_IN_LEN];
	struct ddc ddc;

	zassert_ok(ddc_plan(out_rate, if_hz, plan));
	ddc_init(&ddc, plan, if_hz, invert, true);
	make_tone(in, TEST_IN_LEN, tone_hz, TEST_AMPLITUDE, dc, plan->adc_rate);

	return ddc_process(&ddc, in, TEST_IN_LEN, out);
}

ZTEST(ddc, test_plan_exact)
{
	struct ddc_plan plan;

	/* Band 0.6 .. 1.6 MHz needs 3.2 MS/s; fs / 4 = 1 MHz is closest to 1.1 MHz */
	zassert_ok(ddc_plan(1000000U, 1100000, &plan));
	zassert_true(plan.exact);
	zassert_equal(plan.decim, 4U);
	zassert_equal(plan.period, 36U);
	zassert_equal(plan.adc_rate, 4000000U);
	zassert_equal(plan.actual_rate, 1000000U);
	zassert_equal(plan.ppm_error, 0);
	zassert_equal(plan.residual_hz, 100000);

	/* D 6 (fs / 4 = 1.5 MHz) is closer, but over the load budget; D 4 is within */
	zassert_ok(ddc_plan(1000000U, 1300000, &plan));
	zassert_equal(plan.decim, 4U);
	zassert_equal(plan.residual_hz, 300000);
	zassert_true(plan.load_pct <= 60U, "load %u", plan.load_pct);

	/* Nothing fits the budget: the cheapest plan, D 4 at 8 MS/s instead of D 3 at 6 MS/s */
	zassert_ok(ddc_plan(2000000U, 1100000, &plan));
	zassert_true(plan.exact);
	zassert_equal(plan.decim, 4U);
	zassert_equal(plan.period, 18U);
	zassert_equal(plan.residual_hz, -900000);
	zassert_true(plan.load_pct > 60U, "load %u", plan.load_pct);

	/* D 9 (residual 25 kHz) is estimated at 72 %; D 8 on the group-end path fits */
	zassert_ok(ddc_plan(500000U, 1100000, &plan));
	zassert_equal(plan.decim, 8U);
	zassert_equal(plan.residual_hz, 100000);
	zassert_true(plan.load_pct <= 60U, "load %u", plan.load_pct);

	/* A negative IF plans like the positive one */
	zassert_ok(ddc_plan(1000000U, -1100000, &plan));
	zassert_equal(plan.decim, 4U);
	zassert_equal(plan.residual_hz, 100000);
}

ZTEST(ddc, test_plan_inexact)
{
	struct ddc_plan plan;

	/* 2.048 MS/s (DAB) does not divide 144 MHz */
	zassert_ok(ddc_plan(2048000U, 1625000, &plan));
	zassert_false(plan.exact);
	zassert_not_equal(plan.ppm_error, 0);
	zassert_true(abs(plan.ppm_error) < 10000, "ppm %d", plan.ppm_error);
	zassert_equal(plan.out_rate, 2048000U);
	/* The band edge stays below half the ADC rate, the IF inside the band */
	zassert_true(2ULL * 1625000U + 2048000U <= plan.adc_rate);
	zassert_true(2U * (uint32_t)abs(plan.residual_hz) <= 2048000U);
}

ZTEST(ddc, test_plan_errors)
{
	struct ddc_plan plan;

	zassert_equal(ddc_plan(0U, 0, &plan), -EINVAL);
	/* Band edge 8.5 MHz needs 17 MS/s; 18 MS/s puts fs / 4 3.5 MHz off the IF */
	zassert_equal(ddc_plan(1000000U, 8000000, &plan), -ERANGE);
}

ZTEST(ddc, test_tone_above_if)
{
	static uint8_t out[TEST_OUT_LEN];
	struct ddc_plan plan;
	const size_t len = run_tone(1200000.0, 0.0, 1100000, 1000000U, false, out, &plan);

	zassert_equal(len, 2U * (TEST_IN_LEN / plan.decim));
	/* +100 kHz, not its mirror at -100 kHz */
	assert_line(out, len, plan.actual_rate, 100000.0, -100000.0, 30.0);
	/* The input's negative frequency, -1.2 MHz - IF, folds to -300 kHz */
	assert_line(out, len, plan.actual_rate, 100000.0, -300000.0, 20.0);
}

ZTEST(ddc, test_tone_below_if)
{
	static uint8_t out[TEST_OUT_LEN];
	struct ddc_plan plan;
	const size_t len = run_tone(1000000.0, 0.0, 1100000, 1000000U, false, out, &plan);

	assert_line(out, len, plan.actual_rate, -100000.0, 100000.0, 30.0);
}

ZTEST(ddc, test_invert)
{
	static uint8_t out[TEST_OUT_LEN];
	struct ddc_plan plan;
	const size_t len = run_tone(1200000.0, 0.0, 1100000, 1000000U, true, out, &plan);

	assert_line(out, len, plan.actual_rate, -100000.0, 100000.0, 30.0);
}

ZTEST(ddc, test_negative_if)
{
	static uint8_t out[TEST_OUT_LEN];
	struct ddc_plan plan;
	/* A negative IF mirrors the spectrum: the tone above |IF| appears below */
	const size_t len = run_tone(1200000.0, 0.0, -1100000, 1000000U, false, out, &plan);

	assert_line(out, len, plan.actual_rate, -100000.0, 100000.0, 30.0);
}

ZTEST(ddc, test_amplitude)
{
	/* Tones 30 kHz above the IF; amplitudes keep the output below saturation */
	static const struct {
		uint32_t rate;
		double amp;
		bool gain;
	} cases[] = {
		{1000000U, 12.0, true},
		{250000U, 4.0, true},
		{1000000U, 40.0, false},
		{250000U, 40.0, false},
	};
	static uint8_t in[TEST_IN_LEN];
	static uint8_t out[TEST_OUT_LEN];

	for (size_t c = 0U; c < ARRAY_SIZE(cases); c++) {
		struct ddc_plan plan;
		struct ddc ddc;
		size_t len;
		double f;
		double h;
		double expect;
		double mag;

		zassert_ok(ddc_plan(cases[c].rate, 1100000, &plan));
		ddc_init(&ddc, &plan, 1100000, false, cases[c].gain);
		/* Output gain in 0.1 dB: 20 log10(2 sqrt(decim)), or 0 for unity */
		zassert_equal(ddc.gain_db10,
			      cases[c].gain ? lround(200.0 * log10(2.0 * sqrt((double)plan.decim)))
					    : 0,
			      "case %u: gain %d", (unsigned int)c, ddc.gain_db10);
		make_tone(in, TEST_IN_LEN, 1130000.0, cases[c].amp, 0.0, plan.adc_rate);
		len = ddc_process(&ddc, in, TEST_IN_LEN, out);

		/* CIC response at the tone, from its center at fs / 4, times the gain */
		f = (1130000.0 - (double)plan.adc_rate / 4.0) / (double)plan.adc_rate;
		h = sin(TEST_PI * f * plan.decim) / (plan.decim * sin(TEST_PI * f));
		expect = cases[c].amp * h * h;
		if (cases[c].gain) {
			expect *= 2.0 * sqrt((double)plan.decim);
		}
		mag = iq_mag(out, len);

		zassert_within(mag, expect, 0.1 * expect, "case %u: mag %d, expected %d",
			       (unsigned int)c, (int)mag, (int)expect);
	}
}

/*
 * Noise of +-1 code at the input must still reach the output at high
 * decimation, not round to a constant code.
 */
ZTEST(ddc, test_small_noise)
{
	static uint8_t in[TEST_IN_LEN];
	static uint8_t out[TEST_OUT_LEN];
	struct ddc_plan plan;
	struct ddc ddc;
	uint32_t lcg = 777U;
	double sum = 0.0;
	double sum2 = 0.0;
	size_t count = 0U;
	size_t len;
	double var;

	zassert_ok(ddc_plan(250000U, 2124996, &plan));
	zassert_equal(plan.decim, 32U);
	ddc_init(&ddc, &plan, 2124996, true, true);
	/* 2 sqrt(32): +21.1 dB */
	zassert_equal(ddc.gain_db10, 211);

	for (size_t k = 0U; k < TEST_IN_LEN; k++) {
		lcg = lcg * 1103515245U + 12345U;
		in[k] = (uint8_t)(127U + (lcg >> 16) % 3U);
	}

	len = ddc_process(&ddc, in, TEST_IN_LEN, out);

	for (size_t m = 2U * TEST_SETTLE; m < len; m++) {
		sum += out[m];
		sum2 += (double)out[m] * out[m];
		count++;
	}

	var = sum2 / (double)count - (sum / (double)count) * (sum / (double)count);
	zassert_true(var > 0.25, "output variance %d / 100", (int)(var * 100.0));
}

ZTEST(ddc, test_dc)
{
	static uint8_t out[TEST_OUT_LEN];
	struct ddc_plan plan;
	/* DC lands on -fs / 4 after mixing, a CIC null when the decimation is a multiple of 4 */
	const size_t len = run_tone(0.0, 40.0 - TEST_AMPLITUDE, 1100000, 1000000U, false, out,
				    &plan);

	zassert_equal(plan.decim % 4U, 0U);
	zassert_true(iq_mag(out, len) < 1.0, "mag %d", (int)iq_mag(out, len));
}

ZTEST(ddc, test_block_boundaries)
{
	static const size_t pieces[] = {1U, 2U, 7U, 100U, 3U, 2048U, 5U, 1U};
	static uint8_t in[TEST_IN_LEN];
	static uint8_t ref[TEST_OUT_LEN];
	static uint8_t out[TEST_OUT_LEN];
	struct ddc_plan plan;
	struct ddc ddc;
	size_t ref_len;
	size_t pos = 0U;
	size_t len = 0U;
	size_t i = 0U;

	zassert_ok(ddc_plan(2000000U, 1100000, &plan));
	make_tone(in, TEST_IN_LEN, 1230000.0, TEST_AMPLITUDE, 0.0, plan.adc_rate);

	ddc_init(&ddc, &plan, 1100000, false, true);
	ref_len = ddc_process(&ddc, in, TEST_IN_LEN, ref);

	ddc_init(&ddc, &plan, 1100000, false, true);
	while (pos < TEST_IN_LEN) {
		const size_t n = MIN(pieces[i % ARRAY_SIZE(pieces)], TEST_IN_LEN - pos);
		const size_t expect = ddc_out_len(&ddc, n);

		zassert_equal(ddc_process(&ddc, &in[pos], n, &out[len]), expect);
		len += expect;
		pos += n;
		i++;
	}

	zassert_equal(len, ref_len);
	zassert_mem_equal(out, ref, ref_len);
}

ZTEST(ddc, test_in_len)
{
	static uint8_t in[16];
	static uint8_t out[16];
	struct ddc_plan plan;
	struct ddc ddc;

	zassert_ok(ddc_plan(2000000U, 1100000, &plan));
	ddc_init(&ddc, &plan, 1100000, false, true);
	(void)memset(in, 128, sizeof(in));

	/* Leave the state at phase 1 */
	(void)ddc_process(&ddc, in, 4U, out);

	for (size_t space = 0U; space < 64U; space += 2U) {
		const size_t n = ddc_in_len(&ddc, space);

		/* n inputs fit, n + 1 inputs would not */
		zassert_true(ddc_out_len(&ddc, n) <= space);
		zassert_true(ddc_out_len(&ddc, n + 1U) > space);
	}
}

/*
 * Reference: the same down-converter one input at a time, without the
 * grouping of ddc_process(). Uses the public state of struct ddc.
 */
struct ref_ddc {
	uint32_t int1_i;
	uint32_t int1_q;
	uint32_t int2_i;
	uint32_t int2_q;
	uint32_t comb1_i;
	uint32_t comb1_q;
	uint32_t comb2_i;
	uint32_t comb2_q;
	uint32_t nco;
	uint32_t mix;
	uint32_t count;
};

static uint8_t ref_clamp(int32_t v)
{
	v += 127;
	return (v < 0) ? 0U : ((v > 255) ? 255U : (uint8_t)v);
}

static size_t ref_process(const struct ddc *cfg, struct ref_ddc *r, const uint8_t *in, size_t n,
			  uint8_t *out)
{
	size_t written = 0U;

	for (size_t k = 0U; k < n; k++) {
		const uint32_t x = (uint32_t)((int32_t)in[k] - 128);

		if (r->mix == 0U) {
			r->int1_i += x;
		} else if (r->mix == 1U) {
			r->int1_q -= x;
		} else if (r->mix == 2U) {
			r->int1_i -= x;
		} else {
			r->int1_q += x;
		}

		r->mix = (r->mix + 1U) & 3U;
		r->int2_i += r->int1_i;
		r->int2_q += r->int1_q;

		if (++r->count == cfg->decim) {
			const uint32_t idx = r->nco >> 24;
			const double g = (double)(1UL << 19) * 2.0 * sqrt((double)cfg->decim) /
					 ((double)cfg->decim * cfg->decim);
			const int32_t c = (int32_t)lrint(cos(2.0 * TEST_PI * idx / 256.0) * g);
			const int32_t s = (int32_t)lrint(cos(2.0 * TEST_PI * ((idx - 64U) & 255U) /
							     256.0) * g);
			const uint32_t c1_i = r->int2_i - r->comb1_i;
			const uint32_t c1_q = r->int2_q - r->comb1_q;
			const int32_t ai = (int32_t)(c1_i - r->comb2_i);
			const int32_t aq = (int32_t)(c1_q - r->comb2_q);
			const int32_t yi = ai * c + aq * s;
			const int32_t yq = (aq * c - ai * s) * cfg->q_sign;

			r->count = 0U;
			r->comb1_i = r->int2_i;
			r->comb1_q = r->int2_q;
			r->comb2_i = c1_i;
			r->comb2_q = c1_q;
			out[written++] = ref_clamp((yi + (1 << 17)) >> 18);
			out[written++] = ref_clamp((yq + (1 << 17)) >> 18);
			r->nco += cfg->nco_step;
		}
	}

	return written;
}

ZTEST(ddc, test_matches_reference)
{
	static const struct {
		uint32_t rate;
		int32_t if_hz;
	} cases[] = {
		{1000000U, 1100000}, {2000000U, 1100000}, {2400000U, 1800000},
		{250000U, 1100000}, {1200000U, -900000}, {960000U, 3000000},
	};
	static const size_t pieces[] = {3U, 1U, 4U, 2048U, 6U, 5U, 129U};
	static uint8_t in[TEST_IN_LEN];
	static uint8_t ref[TEST_OUT_LEN];
	static uint8_t out[TEST_OUT_LEN];
	uint32_t lcg = 12345U;

	for (size_t k = 0U; k < TEST_IN_LEN; k++) {
		lcg = lcg * 1103515245U + 12345U;
		in[k] = (uint8_t)(lcg >> 24);
	}

	for (size_t c = 0U; c < ARRAY_SIZE(cases); c++) {
		struct ddc_plan plan;
		struct ddc ddc;
		struct ref_ddc r = {0};
		size_t ref_len;
		size_t len = 0U;
		size_t pos = 0U;
		size_t i = c;

		zassert_ok(ddc_plan(cases[c].rate, cases[c].if_hz, &plan), "case %u", (unsigned int)c);
		ddc_init(&ddc, &plan, cases[c].if_hz, (c % 2U) == 0U, true);
		ref_len = ref_process(&ddc, &r, in, TEST_IN_LEN, ref);

		while (pos < TEST_IN_LEN) {
			const size_t n = MIN(pieces[i % ARRAY_SIZE(pieces)], TEST_IN_LEN - pos);

			len += ddc_process(&ddc, &in[pos], n, &out[len]);
			pos += n;
			i++;
		}

		zassert_equal(len, ref_len, "case %u", (unsigned int)c);
		zassert_mem_equal(out, ref, ref_len, "case %u, decim %u", (unsigned int)c, plan.decim);
	}
}

ZTEST_SUITE(ddc, NULL, NULL, NULL, NULL, NULL);
