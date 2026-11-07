/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

#include <welle_sdr_raw.h>

#include "raw_knock.h"

#define RAW_KNOCK_MAX_CODE 255U

static bool raw_knock_match(uint32_t rate, uint32_t target)
{
	const uint32_t diff = (rate > target) ? rate - target : target - rate;

	return diff <= WSDR_RAW_KNOCK_TOL_HZ;
}

/* Code carried by the third rate, -1 if the rate is not on the code grid */
static int raw_knock_code(uint32_t rate)
{
	uint32_t code;

	if (rate + WSDR_RAW_KNOCK_TOL_HZ < WSDR_RAW_KNOCK_RATE_3_BASE) {
		return -1;
	}

	code = (rate + WSDR_RAW_KNOCK_RATE_3_STEP / 2U - WSDR_RAW_KNOCK_RATE_3_BASE) /
	       WSDR_RAW_KNOCK_RATE_3_STEP;
	if (code > RAW_KNOCK_MAX_CODE || !raw_knock_match(rate, WSDR_RAW_KNOCK_RATE_3(code))) {
		return -1;
	}

	return (int)code;
}

void raw_knock_reset(struct raw_knock *k)
{
	k->step = 0U;
	k->last_ms = 0U;
}

int raw_knock_feed(struct raw_knock *k, uint32_t rate, uint32_t now_ms)
{
	const bool in_time = (now_ms - k->last_ms) <= WSDR_RAW_KNOCK_WINDOW_MS;
	int code = -1;

	/* The first rate starts a sequence at any point */
	if (raw_knock_match(rate, WSDR_RAW_KNOCK_RATE_1)) {
		k->step = 1U;
		k->last_ms = now_ms;
		return -1;
	}

	if (k->step == 1U && in_time && raw_knock_match(rate, WSDR_RAW_KNOCK_RATE_2)) {
		k->step = 2U;
		k->last_ms = now_ms;
		return -1;
	}

	if (k->step == 2U && in_time) {
		code = raw_knock_code(rate);
	}

	k->step = 0U;

	return code;
}
