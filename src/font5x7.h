/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_FONT5X7_H_
#define WELLE_SDR_FONT5X7_H_

#include <stdint.h>

/* Glyph size in pixels; a character cell adds one blank column */
#define FONT5X7_WIDTH  5U
#define FONT5X7_HEIGHT 7U

/**
 * @brief Get the glyph of a character.
 *
 * @param c Character, printable ASCII. Others are drawn as '?'.
 *
 * @return FONT5X7_WIDTH column bytes, bit 0 is the top row.
 */
const uint8_t *font5x7_glyph(char c);

#endif /* WELLE_SDR_FONT5X7_H_ */
