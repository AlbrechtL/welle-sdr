/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_SAMPLE_SRC_H_
#define WELLE_SDR_SAMPLE_SRC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Sample source state for the shell. */
struct sample_src_stats {
	/** The producer streams ADC data. */
	bool running;
	/** A plan exists for the current rate and IF. */
	bool planned;
	/** The output rate is exactly the host rate. */
	bool exact;
	/** Host rate in complex samples per second. */
	uint32_t out_rate;
	/** Rate actually produced. */
	uint32_t actual_rate;
	/** Error of the produced rate in ppm. */
	int32_t ppm_error;
	/** ADC rate in samples per second. */
	uint32_t adc_rate;
	/** Decimation from the ADC rate to the output rate. */
	uint8_t decim;
	/** Offset of the IF from a quarter of the ADC rate, removed by rotation, in Hz. */
	int32_t residual_hz;
	/** IF converted to baseband, in Hz. */
	int32_t if_freq;
	/** Output conjugated (spectrum inversion). */
	bool invert;
	/** Bulk IN transfers submitted since the stream started. */
	uint32_t xfers;
	/** Output bytes dropped because no transfer buffer was free. */
	uint32_t dropped;
	/** CPU time in the down-converter over the last second, in percent. */
	uint32_t load_pct;
	/** Load estimated for the plan, in percent. */
	uint32_t est_load_pct;
};

/**
 * @brief Select the RTL2832U test mode output.
 *
 * In test mode the stream is an 8-bit counter, continuous across buffers,
 * which rtl_test checks for lost bytes, filled by sample_src_fill().
 * Otherwise the producer thread streams down-converted ADC data.
 */
void sample_src_set_test_mode(bool enable);

/** Set the sample rate configured by the host, in samples per second. */
void sample_src_set_rate(uint32_t rate);

/** Set the IF the host programmed into the demodulator, in Hz. */
void sample_src_set_if(int32_t if_hz);

/** Set the demodulator spectrum inversion. */
void sample_src_set_inversion(bool invert);

/** Fill @p buf with the next @p len bytes of the test mode counter. */
void sample_src_fill(uint8_t *buf, size_t len);

/** Start streaming ADC data, called when the host starts the stream. */
void sample_src_start(void);

/** Stop streaming ADC data. */
void sample_src_stop(void);

/**
 * @brief Get the sample source state.
 *
 * @param stats Filled with the current state.
 */
void sample_src_get_stats(struct sample_src_stats *stats);

#endif /* WELLE_SDR_SAMPLE_SRC_H_ */
