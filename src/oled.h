/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_OLED_H_
#define WELLE_SDR_OLED_H_

/**
 * @brief Start the status display thread.
 *
 * The thread initializes the display on the tuner bus and then redraws it
 * every CONFIG_WELLE_SDR_OLED_PERIOD_MS. Call it after if_agc_init(): it
 * floats the IF_AGC pin around each display write.
 */
void oled_start(void);

#endif /* WELLE_SDR_OLED_H_ */
