/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "ddc.h"
#include "raw_frame.h"

/* ADC rate per band edge: Nyquist plus 10 % for the tuner's filter skirts, in tenths */
#define RAW_FRAME_NEED_TENTHS 22U

static size_t raw_frame_min(size_t a, size_t b)
{
	return (a < b) ? a : b;
}

static void raw_frame_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void raw_frame_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

int raw_frame_plan(uint32_t host_rate, int32_t if_hz, uint32_t max_rate,
		   struct raw_frame_plan *plan)
{
	const uint64_t abs_if = (if_hz < 0) ? (uint64_t)-(int64_t)if_hz : (uint64_t)if_hz;
	const uint64_t edge = abs_if + host_rate / 2U;
	uint64_t need;
	uint64_t p_hi;
	uint64_t p_lo;
	uint64_t period = 0U;

	if (host_rate == 0U || max_rate == 0U) {
		return -EINVAL;
	}

	need = (edge * RAW_FRAME_NEED_TENTHS + 9U) / 10U;
	if (need == 0U) {
		need = 1U;
	}

	/* Shortest period the maximum rate allows */
	p_lo = (DDC_CLOCK_HZ + max_rate - 1U) / max_rate;
	if (p_lo < DDC_MIN_PERIOD) {
		p_lo = DDC_MIN_PERIOD;
	}

	/* Longest period that still gives the needed rate */
	p_hi = DDC_CLOCK_HZ / need;
	if (p_hi > DDC_MAX_PERIOD) {
		p_hi = DDC_MAX_PERIOD;
	}

	/* Lowest exact rate at or above the need */
	for (uint64_t p = p_hi; p >= p_lo; p--) {
		if ((DDC_CLOCK_HZ % p) == 0U) {
			period = p;
			break;
		}
	}

	plan->capped = (period == 0U);

	/* Capped: highest exact rate the maximum allows */
	for (uint64_t p = p_lo; period == 0U && p <= DDC_MAX_PERIOD; p++) {
		if ((DDC_CLOCK_HZ % p) == 0U) {
			period = p;
		}
	}

	if (period == 0U) {
		return -EINVAL;
	}

	plan->period = (uint16_t)period;
	plan->adc_rate = (uint32_t)(DDC_CLOCK_HZ / period);
	plan->need_rate = (uint32_t)need;
	plan->aliased = (uint64_t)plan->adc_rate < 2U * edge;

	return 0;
}

void raw_frame_init(struct raw_frame *f, size_t frame_len)
{
	memset(f, 0, sizeof(*f));
	f->frame_len = frame_len;
}

void raw_frame_set_params(struct raw_frame *f, const struct raw_frame_params *params)
{
	f->params = *params;
}

void raw_frame_restart(struct raw_frame *f)
{
	/* Samples already written into the frame stay counted in the index */
	f->pos = 0U;
}

void raw_frame_skip(struct raw_frame *f, size_t len)
{
	f->sample_idx += (uint32_t)len;
}

static void raw_frame_build_hdr(struct raw_frame *f)
{
	uint8_t *h = f->hdr;

	memcpy(&h[WSDR_RAW_OFF_MAGIC], WSDR_RAW_MAGIC, WSDR_RAW_MAGIC_LEN);
	h[WSDR_RAW_OFF_VERSION] = WSDR_RAW_VERSION;
	h[WSDR_RAW_OFF_HDR_LEN] = WSDR_RAW_HDR_LEN;
	raw_frame_put_le16(&h[WSDR_RAW_OFF_FLAGS], f->params.flags);
	raw_frame_put_le32(&h[WSDR_RAW_OFF_SAMPLE_IDX], f->sample_idx);
	raw_frame_put_le32(&h[WSDR_RAW_OFF_ADC_RATE], f->params.adc_rate);
	raw_frame_put_le32(&h[WSDR_RAW_OFF_IF], (uint32_t)f->params.if_hz);
}

size_t raw_frame_fill(struct raw_frame *f, const uint8_t *in, size_t len, uint8_t *out,
		      size_t space, size_t *consumed)
{
	size_t written = 0U;
	size_t used = 0U;

	while (written < space) {
		size_t n;

		if (f->pos < WSDR_RAW_HDR_LEN) {
			if (f->pos == 0U) {
				raw_frame_build_hdr(f);
			}

			n = raw_frame_min(WSDR_RAW_HDR_LEN - f->pos, space - written);
			memcpy(&out[written], &f->hdr[f->pos], n);
			written += n;
			f->pos += n;
			continue;
		}

		if (used == len) {
			break;
		}

		n = raw_frame_min(raw_frame_min(f->frame_len - f->pos, space - written), len - used);
		memcpy(&out[written], &in[used], n);
		written += n;
		used += n;
		f->pos += n;
		f->sample_idx += (uint32_t)n;

		if (f->pos == f->frame_len) {
			f->pos = 0U;
		}
	}

	*consumed = used;

	return written;
}
