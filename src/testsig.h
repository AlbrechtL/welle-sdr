/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_TESTSIG_H_
#define WELLE_SDR_TESTSIG_H_

#include <stdint.h>

/** Lowest test signal frequency: 144 MHz / 65536, rounded up. */
#define TESTSIG_MIN_HZ 2198U
/** Highest test signal frequency: 144 MHz / 6. */
#define TESTSIG_MAX_HZ 24000000U

/**
 * @brief Set the square-wave test signal on PA7 (TIM3_CH2).
 *
 * The frequency is the timer clock divided by the nearest integer period;
 * the output is high for half the period, rounded down.
 *
 * @param freq_hz Frequency in Hz, or 0 to drive the output low.
 * @param actual_hz Set to the frequency generated, if not NULL.
 *
 * @retval 0 Test signal set.
 * @retval -ERANGE Frequency outside TESTSIG_MIN_HZ .. TESTSIG_MAX_HZ.
 * @retval -EIO Clock or pin setup failed.
 */
int testsig_set(uint32_t freq_hz, uint32_t *actual_hz);

#endif /* WELLE_SDR_TESTSIG_H_ */
