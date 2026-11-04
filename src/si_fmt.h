/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_SI_FMT_H_
#define WELLE_SDR_SI_FMT_H_

#include <stddef.h>
#include <stdint.h>

/* Buffer size that fits any si_fmt() result with a unit of up to 4 characters */
#define SI_FMT_LEN 32U

/**
 * @brief Format a value with an SI prefix (k, M, G), keeping all digits.
 *
 * Trailing zeros of the fraction are dropped: 433920000 with unit "Hz" gives
 * "433.92 MHz", 100000000 gives "100 MHz", 999 gives "999 Hz".
 *
 * @param buf Output buffer, SI_FMT_LEN bytes are enough.
 * @param size Size of @p buf.
 * @param value Value in base units.
 * @param unit Unit without prefix, for example "Hz" or "S/s".
 *
 * @return @p buf.
 */
const char *si_fmt(char *buf, size_t size, uint64_t value, const char *unit);

#endif /* WELLE_SDR_SI_FMT_H_ */
