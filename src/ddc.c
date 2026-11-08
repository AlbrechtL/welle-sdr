/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Digital down-converter: real ADC samples at the IF to complex baseband at
 * the host rate, cheap enough for the CH32V307 at several MS/s.
 *
 * 1. Mixing by a quarter of the ADC rate fs: e^(-j pi n / 2) is the sequence
 *    1, -j, -1, +j, so each input is added to or subtracted from I or Q,
 *    without multiplication.
 * 2. Second-order CIC decimation by D: two integrators per input, two combs
 *    per output, additions only.
 * 3. Rotation by the residual |IF| - fs / 4 at the output rate, one complex
 *    multiplication per output.
 *
 * ddc_plan() picks fs = D * out_rate so that fs / 4 is close to the IF and
 * the residual stays small. The CIC droops towards the band edges (-1.1 dB
 * at 0.2 out_rate from the band center, -7.8 dB at 0.5 out_rate) and folds
 * signals around multiples of out_rate into the band with limited
 * suppression; the band center is fs / 4, so the residual shifts the
 * requested band against it.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include "ddc.h"

/*
 * Output in Q18: a tone of amplitude A at the input gives
 * A * DDC_GAIN * sqrt(decim) * 2^18 before saturation.
 */
#define DDC_OUT_SHIFT  18
#define DDC_OUT_CENTER 127

/*
 * Output gain over the input level, unless switched off in ddc_init(), is
 * DDC_GAIN * sqrt(decim). The CIC
 * lowers white ADC noise by sqrt(decim): the sqrt(decim) keeps the noise at
 * the same level in output codes at any rate, so that it is not rounded
 * away at high decimation (at 250 kS/s, decimation 32, the output was a
 * constant code between the pulses of a weak signal). DDC_GAIN, 6 dB,
 * makes up for the ADC level, which the AD9280 module's front end keeps
 * far below full scale.
 */
#define DDC_GAIN 2.0

#define DDC_ROT_BITS 8U
#define DDC_ROT_SIZE (1U << DDC_ROT_BITS)

#define DDC_PI 3.14159265358979

static uint64_t ddc_abs_diff(uint64_t a, uint64_t b)
{
	return (a > b) ? (a - b) : (b - a);
}

/* |IF| - fs / 4 in Hz for an ADC clock period, rounded */
static int64_t ddc_residual(uint64_t abs_if, uint64_t period)
{
	const int64_t num = (int64_t)(4U * abs_if * period) - (int64_t)DDC_CLOCK_HZ;
	const int64_t den = (int64_t)(4U * period);

	return (num >= 0) ? (num + den / 2) / den : -((-num + den / 2) / den);
}

/*
 * CPU cost of ddc_process() on the CH32V307 at 144 MHz (-O2), in tenths of
 * a cycle, fitted to measured block times: per input and per output sample,
 * for the group-end path (decimation a multiple of 4) and the general path.
 */
#define DDC_COST_FAST_IN  79U
#define DDC_COST_FAST_OUT 470U
#define DDC_COST_GEN_IN   133U
#define DDC_COST_GEN_OUT  865U
#define DDC_CPU_HZ        144000000U
/* Plans up to this estimated load are chosen by the smallest residual */
#define DDC_LOAD_BUDGET_PCT 60U

struct ddc_candidate {
	uint64_t period;
	uint64_t decim;
	/* Rate error in Hz is err / (period * decim) */
	uint64_t err;
	uint64_t abs_res;
	/* Estimated CPU load in percent */
	uint64_t load_pct;
};

static uint64_t ddc_load_pct(uint64_t period, uint64_t decim)
{
	const bool fast = (decim % 4U) == 0U;
	const uint64_t cost_in = fast ? DDC_COST_FAST_IN : DDC_COST_GEN_IN;
	const uint64_t cost_out = fast ? DDC_COST_FAST_OUT : DDC_COST_GEN_OUT;
	/* (adc_rate * cost_in + out_rate * cost_out) / 10 / DDC_CPU_HZ * 100 */
	const uint64_t tenth_cycles = DDC_CLOCK_HZ * cost_in / period +
				      DDC_CLOCK_HZ * cost_out / (period * decim);

	return tenth_cycles * 10U / DDC_CPU_HZ;
}

/*
 * Exact plans: within the load budget the smallest residual, else the
 * smallest load. Inexact plans: the smallest rate error.
 */
static bool ddc_better(const struct ddc_candidate *c, const struct ddc_candidate *best,
		       bool exact)
{
	bool c_ok;
	bool best_ok;

	if (best->period == 0U) {
		return true;
	}

	if (!exact) {
		return c->err * best->period * best->decim < best->err * c->period * c->decim;
	}

	c_ok = c->load_pct <= DDC_LOAD_BUDGET_PCT;
	best_ok = best->load_pct <= DDC_LOAD_BUDGET_PCT;

	if (c_ok != best_ok) {
		return c_ok;
	}

	if (c_ok && c->abs_res != best->abs_res) {
		return c->abs_res < best->abs_res;
	}

	return c->load_pct < best->load_pct;
}

static bool ddc_search(uint32_t out_rate, uint64_t abs_if, bool exact,
		       struct ddc_candidate *best)
{
	for (uint64_t decim = DDC_MIN_DECIM; decim <= DDC_MAX_DECIM; decim++) {
		const uint64_t p0 = DDC_CLOCK_HZ / ((uint64_t)out_rate * decim);

		for (uint64_t period = p0; period <= p0 + 1U; period++) {
			struct ddc_candidate c;
			int64_t res;

			if (period < DDC_MIN_PERIOD || period > DDC_MAX_PERIOD) {
				continue;
			}

			/* Band edge |IF| + out_rate / 2 at most half the ADC rate */
			if ((2U * abs_if + out_rate) * period > DDC_CLOCK_HZ) {
				continue;
			}

			c.period = period;
			c.decim = decim;
			c.err = ddc_abs_diff(DDC_CLOCK_HZ, (uint64_t)out_rate * period * decim);
			res = ddc_residual(abs_if, period);
			c.abs_res = (uint64_t)((res < 0) ? -res : res);
			c.load_pct = ddc_load_pct(period, decim);

			/* The IF must lie inside the output band */
			if (2U * c.abs_res > out_rate) {
				continue;
			}

			if (exact && c.err > period * decim) {
				continue;
			}

			if (ddc_better(&c, best, exact)) {
				*best = c;
			}
		}
	}

	return best->period != 0U;
}

int ddc_plan(uint32_t out_rate, int32_t if_hz, struct ddc_plan *plan)
{
	const uint64_t abs_if = (if_hz < 0) ? (uint64_t)(-(int64_t)if_hz) : (uint64_t)if_hz;
	struct ddc_candidate best = {0};
	bool exact;
	uint64_t div;
	int64_t err;

	if (out_rate == 0U) {
		return -EINVAL;
	}

	exact = ddc_search(out_rate, abs_if, true, &best);
	if (!exact && !ddc_search(out_rate, abs_if, false, &best)) {
		return -ERANGE;
	}

	div = best.period * best.decim;
	err = (int64_t)DDC_CLOCK_HZ - (int64_t)out_rate * (int64_t)div;

	plan->out_rate = out_rate;
	plan->period = (uint16_t)best.period;
	plan->decim = (uint8_t)best.decim;
	plan->exact = exact;
	plan->adc_rate = (uint32_t)((DDC_CLOCK_HZ + best.period / 2U) / best.period);
	plan->actual_rate = (uint32_t)((DDC_CLOCK_HZ + div / 2U) / div);
	plan->ppm_error = (int32_t)(err * 1000000 / ((int64_t)out_rate * (int64_t)div));
	plan->residual_hz = (int32_t)ddc_residual(abs_if, best.period);
	plan->load_pct = (uint32_t)best.load_pct;

	return 0;
}

void ddc_init(struct ddc *ddc, const struct ddc_plan *plan, int32_t if_hz, bool invert,
	      bool gain)
{
	const uint32_t decim2 = (uint32_t)plan->decim * plan->decim;
	const double out_rate = (double)DDC_CLOCK_HZ / ((double)plan->period * plan->decim);
	const double out_gain = gain ? DDC_GAIN * sqrt((double)plan->decim) : 1.0;

	(void)memset(ddc, 0, sizeof(*ddc));
	ddc->decim = plan->decim;
	/* The spectrum of a real input at -IF is the conjugate of the one at +IF */
	ddc->q_sign = (invert != (if_hz < 0)) ? -1 : 1;
	ddc->gain_db10 = (int16_t)lrint(200.0 * log10(out_gain));

	/*
	 * A real tone of amplitude A gives A / 2 at baseband and decim^2 in the
	 * CIC, so the gain correction is 2 / decim^2, times the output gain.
	 * |comb output| <= 128 decim^2 and |rot| <= 2^19 * 2 * sqrt(32) / decim^2,
	 * so each product stays below 2^29.5 and their sum below 2^30.
	 */
	const double scale = (double)(1UL << (DDC_OUT_SHIFT + 1U)) * out_gain / (double)decim2;

	for (uint32_t i = 0U; i < DDC_ROT_SIZE; i++) {
		/* sin(x) = cos(x - quarter turn), from the same table points */
		const uint32_t i_sin = (i - DDC_ROT_SIZE / 4U) & (DDC_ROT_SIZE - 1U);

		ddc->rot[i][0] = (int32_t)lrint(cos(2.0 * DDC_PI * (double)i /
						    (double)DDC_ROT_SIZE) * scale);
		ddc->rot[i][1] = (int32_t)lrint(cos(2.0 * DDC_PI * (double)i_sin /
						    (double)DDC_ROT_SIZE) * scale);
	}
	/* Rotation per output: residual / out_rate turns, as a 32-bit phase; wraps */
	ddc->nco_step = (uint32_t)llrint((double)plan->residual_hz / out_rate * 4294967296.0);
}

size_t ddc_out_len(const struct ddc *ddc, size_t n)
{
	return 2U * ((ddc->phase + n) / ddc->decim);
}

size_t ddc_in_len(const struct ddc *ddc, size_t out_len)
{
	const size_t outputs = out_len / 2U;

	/* The inputs up to the last output, plus the ones before the next */
	return outputs * ddc->decim + (ddc->decim - 1U - ddc->phase);
}

/* Q18 value to the output byte: round, add the center, saturate */
static inline uint8_t ddc_to_u8(int32_t y)
{
	const int32_t v = (y + (DDC_OUT_CENTER << DDC_OUT_SHIFT) + (1 << (DDC_OUT_SHIFT - 1))) >>
			  DDC_OUT_SHIFT;

	/* One compare in the normal case */
	if ((uint32_t)v > UINT8_MAX) {
		return (v < 0) ? 0U : UINT8_MAX;
	}

	return (uint8_t)v;
}

/* Down-converter state kept in local variables while processing */
struct ddc_run {
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

/*
 * Configuration copied into locals: out is a byte pointer and may alias the
 * struct, so the compiler would reload the fields after every store.
 */
struct ddc_cfg {
	const int32_t (*rot)[2];
	uint32_t nco_step;
	uint32_t decim;
	int32_t q_sign;
};

/* Combs, scaling and rotation for one output from the second integrator states */
static inline void ddc_emit(const struct ddc_cfg *cfg, struct ddc_run *r, uint32_t int2_i,
			    uint32_t int2_q, uint8_t *out)
{
	const int32_t *cs = cfg->rot[r->nco >> (32U - DDC_ROT_BITS)];
	const uint32_t c1_i = int2_i - r->comb1_i;
	const uint32_t c1_q = int2_q - r->comb1_q;
	const int32_t ai = (int32_t)(c1_i - r->comb2_i);
	const int32_t aq = (int32_t)(c1_q - r->comb2_q);
	/* y = a * e^(-j phase) * gain, Q18 */
	const int32_t yi = ai * cs[0] + aq * cs[1];
	const int32_t yq = (aq * cs[0] - ai * cs[1]) * cfg->q_sign;

	r->comb1_i = int2_i;
	r->comb1_q = int2_q;
	r->comb2_i = c1_i;
	r->comb2_q = c1_q;
	r->nco += cfg->nco_step;

	out[0] = ddc_to_u8(yi);
	out[1] = ddc_to_u8(yq);
}

/* One input sample: mix by 1, -j, -1, +j, integrate, emit at the decimation step */
static inline size_t ddc_step(const struct ddc_cfg *cfg, struct ddc_run *r, uint8_t in,
			      uint8_t *out)
{
	const uint32_t x = (uint32_t)((int32_t)in - 128);

	switch (r->mix) {
	case 0U:
		r->int1_i += x;
		break;
	case 1U:
		r->int1_q -= x;
		break;
	case 2U:
		r->int1_i -= x;
		break;
	default:
		r->int1_q += x;
		break;
	}

	r->mix = (r->mix + 1U) & 3U;
	r->int2_i += r->int1_i;
	r->int2_q += r->int1_q;
	r->count++;

	if (r->count == cfg->decim) {
		r->count = 0U;
		ddc_emit(cfg, r, r->int2_i, r->int2_q, out);
		return 2U;
	}

	return 0U;
}

/*
 * Four inputs starting at mixer phase 0. With I0, Q0 the first and A, B the
 * second integrators before the group, after j inputs:
 *   j = 1: A + I0 + x0,         B + Q0
 *   j = 2: A + 2 (I0 + x0),     B + 2 Q0 - x1
 *   j = 3: A + 3 (I0 + x0) - x2, B + 3 Q0 - 2 x1
 *   j = 4: A + 4 (I0 + x0) - 2 x2, B + 4 Q0 - 3 x1 + x3
 */
static inline size_t ddc_group(const struct ddc_cfg *cfg, struct ddc_run *r, const uint8_t *in,
			       uint8_t *out)
{
	const uint32_t x0 = (uint32_t)((int32_t)in[0] - 128);
	const uint32_t x1 = (uint32_t)((int32_t)in[1] - 128);
	const uint32_t x2 = (uint32_t)((int32_t)in[2] - 128);
	const uint32_t x3 = (uint32_t)((int32_t)in[3] - 128);
	const uint32_t i0x0 = r->int1_i + x0;
	const uint32_t q0 = r->int1_q;
	const uint32_t a = r->int2_i;
	const uint32_t b = r->int2_q;
	/* Inputs from the group start to the next output */
	uint32_t j = cfg->decim - r->count;
	size_t written = 0U;

	while (j <= 4U) {
		uint32_t oi;
		uint32_t oq;

		switch (j) {
		case 1U:
			oi = a + i0x0;
			oq = b + q0;
			break;
		case 2U:
			oi = a + 2U * i0x0;
			oq = b + 2U * q0 - x1;
			break;
		case 3U:
			oi = a + 3U * i0x0 - x2;
			oq = b + 3U * q0 - 2U * x1;
			break;
		default:
			oi = a + 4U * i0x0 - 2U * x2;
			oq = b + 4U * q0 - 3U * x1 + x3;
			break;
		}

		ddc_emit(cfg, r, oi, oq, &out[written]);
		written += 2U;
		j += cfg->decim;
	}

	r->count = cfg->decim - (j - 4U);
	r->int2_i = a + 4U * i0x0 - 2U * x2;
	r->int2_q = b + 4U * q0 - 3U * x1 + x3;
	r->int1_i = i0x0 - x2;
	r->int1_q = q0 - x1 + x3;

	return written;
}

size_t ddc_process(struct ddc *ddc, const uint8_t *in, size_t n, uint8_t *out)
{
	struct ddc_run r = {
		.int1_i = ddc->int1_i,
		.int1_q = ddc->int1_q,
		.int2_i = ddc->int2_i,
		.int2_q = ddc->int2_q,
		.comb1_i = ddc->comb1_i,
		.comb1_q = ddc->comb1_q,
		.comb2_i = ddc->comb2_i,
		.comb2_q = ddc->comb2_q,
		.nco = ddc->nco,
		.mix = ddc->mix_phase,
		.count = ddc->phase,
	};
	const struct ddc_cfg cfg = {
		.rot = ddc->rot,
		.nco_step = ddc->nco_step,
		.decim = ddc->decim,
		.q_sign = ddc->q_sign,
	};
	size_t written = 0U;
	size_t k = 0U;

	/* Single samples up to mixer phase 0, groups of four, single samples at the end */
	while (k < n && r.mix != 0U) {
		written += ddc_step(&cfg, &r, in[k], &out[written]);
		k++;
	}

	if ((cfg.decim % 4U) == 0U) {
		/*
		 * Outputs fall on group ends: only the end state of each group,
		 * from raw codes; the +-128 offsets leave -256 and +256 per group.
		 */
		uint32_t int1_i = r.int1_i;
		uint32_t int1_q = r.int1_q;
		uint32_t int2_i = r.int2_i;
		uint32_t int2_q = r.int2_q;
		uint32_t count = r.count;

		for (; k + 4U <= n; k += 4U) {
			const uint32_t x0 = in[k];
			const uint32_t x1 = in[k + 1U];
			const uint32_t x2 = in[k + 2U];
			const uint32_t x3 = in[k + 3U];

			int2_i += 4U * (int1_i + x0) - 2U * x2 - 256U;
			int2_q += 4U * int1_q - 3U * x1 + x3 + 256U;
			int1_i += x0 - x2;
			int1_q += x3 - x1;
			count += 4U;

			if (count == cfg.decim) {
				count = 0U;
				ddc_emit(&cfg, &r, int2_i, int2_q, &out[written]);
				written += 2U;
			}
		}

		r.int1_i = int1_i;
		r.int1_q = int1_q;
		r.int2_i = int2_i;
		r.int2_q = int2_q;
		r.count = count;
	} else {
		while (k + 4U <= n) {
			written += ddc_group(&cfg, &r, &in[k], &out[written]);
			k += 4U;
		}
	}

	while (k < n) {
		written += ddc_step(&cfg, &r, in[k], &out[written]);
		k++;
	}

	ddc->int1_i = r.int1_i;
	ddc->int1_q = r.int1_q;
	ddc->int2_i = r.int2_i;
	ddc->int2_q = r.int2_q;
	ddc->comb1_i = r.comb1_i;
	ddc->comb1_q = r.comb1_q;
	ddc->comb2_i = r.comb2_i;
	ddc->comb2_q = r.comb2_q;
	ddc->nco = r.nco;
	ddc->mix_phase = (uint8_t)r.mix;
	ddc->phase = (uint8_t)r.count;

	return written;
}
