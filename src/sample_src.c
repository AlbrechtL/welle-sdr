/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Dummy sample source; to be replaced by the SDR signal path */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "sample_src.h"

/* 100 * cos(2 * pi * k / 16) */
static const int8_t cos_lut[16] = {
	100, 92, 71, 38, 0, -38, -71, -92, -100, -92, -71, -38, 0, 38, 71, 92,
};

static bool test_mode;
static uint8_t counter;
static uint8_t phase;
/* Next byte of a tone sample is Q */
static bool tone_q;

void sample_src_set_test_mode(bool enable)
{
	test_mode = enable;
}

void sample_src_set_rate(uint32_t rate)
{
	/* Unpaced: the stream runs as fast as the host reads */
	ARG_UNUSED(rate);
}

void sample_src_fill(uint8_t *buf, size_t len)
{
	if (test_mode) {
		for (size_t i = 0U; i < len; i++) {
			buf[i] = counter++;
		}

		return;
	}

	for (size_t i = 0U; i < len; i++) {
		if (!tone_q) {
			buf[i] = (uint8_t)(127 + cos_lut[phase]);
		} else {
			/* sin(x) = cos(x - pi / 2) */
			buf[i] = (uint8_t)(127 + cos_lut[(phase + 12U) % 16U]);
			phase = (phase + 1U) % 16U;
		}

		tone_q = !tone_q;
	}
}
