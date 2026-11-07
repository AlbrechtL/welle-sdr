/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_RAW_FRAME_H_
#define WELLE_SDR_RAW_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <welle_sdr_raw.h>

/** ADC clock for the raw mode band. */
struct raw_frame_plan {
	/** ADC clock period in DDC_CLOCK_HZ cycles, a divisor of DDC_CLOCK_HZ. */
	uint16_t period;
	/** ADC rate, exactly DDC_CLOCK_HZ / period. */
	uint32_t adc_rate;
	/** Rate the band asks for, with margin for the tuner's filter skirts. */
	uint32_t need_rate;
	/** The maximum rate limited the ADC rate below need_rate. */
	bool capped;
	/** The band reaches above half the ADC rate (WSDR_RAW_FLAG_ALIASED). */
	bool aliased;
};

/** Header fields of the next frames. */
struct raw_frame_params {
	uint32_t adc_rate;
	int32_t if_hz;
	/** WSDR_RAW_FLAG_* */
	uint16_t flags;
};

/** Framer state. */
struct raw_frame {
	/* Frame length including the header */
	size_t frame_len;
	/* Bytes of the current frame written, 0 at a frame start */
	size_t pos;
	/* Index of the next sample */
	uint32_t sample_idx;
	struct raw_frame_params params;
	/* Header of the current frame */
	uint8_t hdr[WSDR_RAW_HDR_LEN];
};

/**
 * @brief Choose the ADC rate for a band.
 *
 * The band is |IF| +/- host_rate / 2. The ADC rate is the lowest exact
 * DDC_CLOCK_HZ / period of at least 2.2 times the band's upper edge, but
 * not above @p max_rate.
 *
 * @param host_rate Sample rate set by the host, in S/s.
 * @param if_hz IF programmed by the host, in Hz.
 * @param max_rate Highest ADC rate, in S/s.
 * @param plan Filled with the result.
 *
 * @retval 0 Planned.
 * @retval -EINVAL @p host_rate or @p max_rate is 0, or no ADC period fits.
 */
int raw_frame_plan(uint32_t host_rate, int32_t if_hz, uint32_t max_rate,
		   struct raw_frame_plan *plan);

/**
 * @brief Start a new stream of frames.
 *
 * @param f Framer state.
 * @param frame_len Frame length including the header, more than WSDR_RAW_HDR_LEN.
 */
void raw_frame_init(struct raw_frame *f, size_t frame_len);

/** Set the header fields, used from the next frame on. */
void raw_frame_set_params(struct raw_frame *f, const struct raw_frame_params *params);

/**
 * @brief Abandon the current frame.
 *
 * The next output starts a new frame. Its sample index tells the host that
 * the samples written into the abandoned frame are missing.
 */
void raw_frame_restart(struct raw_frame *f);

/**
 * @brief Skip samples that are not sent, at a frame start.
 *
 * @param f Framer state.
 * @param len Number of samples dropped.
 */
void raw_frame_skip(struct raw_frame *f, size_t len);

/**
 * @brief Write samples as frames.
 *
 * Writes a header at each frame start and copies samples until the input
 * is used up or @p out is full. The caller passes a new buffer of one frame
 * length at each frame start to keep frames aligned with transfers.
 *
 * @param f Framer state.
 * @param in ADC samples.
 * @param len Number of samples in @p in.
 * @param out Output buffer.
 * @param space Free bytes in @p out.
 * @param consumed Set to the number of samples taken from @p in.
 *
 * @return Bytes written to @p out.
 */
size_t raw_frame_fill(struct raw_frame *f, const uint8_t *in, size_t len, uint8_t *out,
		      size_t space, size_t *consumed);

#endif /* WELLE_SDR_RAW_FRAME_H_ */
