/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_DDC_H_
#define WELLE_SDR_DDC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Clock the ADC clock (DCLK) is divided from, in Hz. */
#define DDC_CLOCK_HZ 144000000U

/** Smallest ADC clock period in DDC_CLOCK_HZ cycles: 18 MS/s, the measured clean range. */
#define DDC_MIN_PERIOD 8U
/** Largest ADC clock period (16-bit timer). */
#define DDC_MAX_PERIOD 65535U

#define DDC_MIN_DECIM 2U
#define DDC_MAX_DECIM 32U

/** ADC clock and decimation for a host sample rate and IF. */
struct ddc_plan {
	/** Requested output rate in complex samples per second. */
	uint32_t out_rate;
	/** ADC clock period in DDC_CLOCK_HZ cycles. */
	uint16_t period;
	/** Decimation from the ADC rate to the output rate. */
	uint8_t decim;
	/** The output rate is exactly the requested one (within 1 Hz). */
	bool exact;
	/** ADC rate, DDC_CLOCK_HZ / period, rounded. */
	uint32_t adc_rate;
	/** Actual output rate, DDC_CLOCK_HZ / (period * decim), rounded. */
	uint32_t actual_rate;
	/** Error of the actual output rate in ppm of the requested one. */
	int32_t ppm_error;
	/** |IF| minus a quarter of the ADC rate, in Hz: removed by the final rotation. */
	int32_t residual_hz;
	/** Estimated CPU load of ddc_process() on the CH32V307 at 144 MHz, in percent. */
	uint32_t load_pct;
};

/**
 * Down-converter state.
 *
 * Mixing by a quarter of the ADC rate (multiplier-free), 2nd-order CIC
 * decimation, rotation by the remaining IF offset at the output rate.
 */
struct ddc {
	/* CIC integrators and comb delays, wrapping arithmetic */
	uint32_t int1_i;
	uint32_t int1_q;
	uint32_t int2_i;
	uint32_t int2_q;
	uint32_t comb1_i;
	uint32_t comb1_q;
	uint32_t comb2_i;
	uint32_t comb2_q;
	/* Phase of the quarter-rate mixer: input index modulo 4 */
	uint8_t mix_phase;
	/* Input samples since the last output, 0 .. decim - 1 */
	uint8_t phase;
	uint8_t decim;
	/* -1 to conjugate the output (spectrum inversion), else 1 */
	int8_t q_sign;
	/*
	 * cos and sin of 2 pi i / 256, times the CIC gain correction
	 * 2 / decim^2, in Q21: rotation and gain in one multiplication
	 */
	int32_t rot[256][2];
	/* Rotation by the residual IF, full turn = 2^32 */
	uint32_t nco;
	uint32_t nco_step;
};

/**
 * @brief Choose the ADC clock period and decimation for a host sample rate.
 *
 * Considers the plans whose band |IF| + out_rate / 2 stays below half the
 * ADC rate and whose IF lies inside the output band. Among the exact output
 * rates it takes the one whose quarter ADC rate lies closest to |IF| (the
 * smallest residual for the final rotation) within an estimated CPU load of
 * 60 %, or the cheapest one if none fits. Without an exact rate the plan with
 * the smallest rate error is returned and @c exact is false.
 *
 * @param out_rate Host sample rate in complex samples per second.
 * @param if_hz IF to convert to baseband, in Hz.
 * @param plan Filled with the result.
 *
 * @retval 0 Plan found.
 * @retval -EINVAL @p out_rate is 0.
 * @retval -ERANGE No ADC rate up to 18 MS/s covers the band.
 */
int ddc_plan(uint32_t out_rate, int32_t if_hz, struct ddc_plan *plan);

/**
 * @brief Initialize the down-converter for a plan and an IF.
 *
 * @param ddc State to initialize.
 * @param plan ADC rate and decimation, from ddc_plan() for the same IF.
 * @param if_hz IF to convert to baseband, in Hz; a negative IF conjugates.
 * @param invert Conjugate the output (spectrum inversion).
 */
void ddc_init(struct ddc *ddc, const struct ddc_plan *plan, int32_t if_hz, bool invert);

/**
 * @brief Number of output bytes the next ddc_process() call produces from @p n inputs.
 *
 * @param ddc Down-converter state.
 * @param n Number of input samples.
 *
 * @return Output bytes (2 per complex sample).
 */
size_t ddc_out_len(const struct ddc *ddc, size_t n);

/**
 * @brief Number of input samples that produce at most @p out_len output bytes.
 *
 * @param ddc Down-converter state.
 * @param out_len Output space in bytes.
 *
 * @return Input samples.
 */
size_t ddc_in_len(const struct ddc *ddc, size_t out_len);

/**
 * @brief Convert ADC samples to I/Q.
 *
 * @p out must hold ddc_out_len(ddc, n) bytes. Output is unsigned 8-bit
 * interleaved I/Q around 127, the RTL2832U format.
 *
 * @param ddc Down-converter state.
 * @param in ADC samples, unsigned 8-bit.
 * @param n Number of input samples.
 * @param out Output buffer.
 *
 * @return Output bytes written.
 */
size_t ddc_process(struct ddc *ddc, const uint8_t *in, size_t n, uint8_t *out);

#endif /* WELLE_SDR_DDC_H_ */
