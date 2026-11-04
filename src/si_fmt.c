/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <zephyr/sys/util.h>

#include "si_fmt.h"

const char *si_fmt(char *buf, size_t size, uint64_t value, const char *unit)
{
	static const struct {
		uint64_t scale;
		uint8_t digits;
		char prefix;
	} prefixes[] = {
		{1000000000ULL, 9U, 'G'},
		{1000000ULL, 6U, 'M'},
		{1000ULL, 3U, 'k'},
	};

	for (size_t i = 0U; i < ARRAY_SIZE(prefixes); i++) {
		uint64_t frac = value % prefixes[i].scale;
		int digits = prefixes[i].digits;

		if (value < prefixes[i].scale) {
			continue;
		}

		while (frac != 0U && (frac % 10U) == 0U) {
			frac /= 10U;
			digits--;
		}

		if (frac == 0U) {
			(void)snprintf(buf, size, "%llu %c%s",
				       (unsigned long long)(value / prefixes[i].scale),
				       prefixes[i].prefix, unit);
		} else {
			(void)snprintf(buf, size, "%llu.%0*llu %c%s",
				       (unsigned long long)(value / prefixes[i].scale), digits,
				       (unsigned long long)frac, prefixes[i].prefix, unit);
		}

		return buf;
	}

	(void)snprintf(buf, size, "%llu %s", (unsigned long long)value, unit);

	return buf;
}
