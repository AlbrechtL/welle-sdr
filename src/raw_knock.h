/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_RAW_KNOCK_H_
#define WELLE_SDR_RAW_KNOCK_H_

#include <stdint.h>

/** Decoder for the sample rate sequence that selects the stream mode. */
struct raw_knock {
	/* Knock rates seen so far, 0 .. 2 */
	uint8_t step;
	/* Time of the last knock rate in ms */
	uint32_t last_ms;
};

/** Forget a partial sequence. */
void raw_knock_reset(struct raw_knock *k);

/**
 * @brief Feed a sample rate set by the host.
 *
 * @param k Decoder state.
 * @param rate Sample rate decoded from the resampler ratio, in S/s.
 * @param now_ms Uptime in ms.
 *
 * @return Knock code (WSDR_RAW_KNOCK_CODE()) when @p rate completes a
 *	   sequence, -1 otherwise.
 */
int raw_knock_feed(struct raw_knock *k, uint32_t rate, uint32_t now_ms);

#endif /* WELLE_SDR_RAW_KNOCK_H_ */
