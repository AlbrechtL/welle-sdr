/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DVP_ADC_H_
#define DVP_ADC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

/** Capture statistics, counted since the last start. */
struct dvp_adc_stats {
	/** Sample rate in samples per second, 0 while stopped. */
	uint32_t rate;
	/** Capture is running. */
	bool running;
	/** Completed DMA blocks. */
	uint32_t blocks;
	/** Completed blocks dropped because the consumer was too slow. */
	uint32_t overruns;
	/** DVP receive FIFO overflows. */
	uint32_t fifo_overflows;
	/** Frame starts seen by the DVP (VSYNC inactive to active). */
	uint32_t frame_starts;
	/** Block interrupts without a DVP buffer switch, ignored. */
	uint32_t spurious;
	/** The guard area after the last block is unchanged (no DMA write past a block). */
	bool guard_intact;
};

/**
 * @brief Start the capture.
 *
 * Starts the ADC clock and the DVP. @p rate_hz must be the PWM clock over
 * an integer number of cycles of at least 6, rounded to 1 Hz (at 144 MHz:
 * 24, 20.571429, 18, 16, 14.4 MHz and lower). The clock is high for half the period,
 * rounded down, which keeps the AD9280 minimum clock high and low time of
 * 14.7 ns (3 cycles = 20.8 ns). The DVP latches on the falling clock edge
 * below 20 MHz and on the rising edge above.
 *
 * @param dev Capture device.
 * @param rate_hz Sample rate in samples per second.
 *
 * @retval 0 Capture started.
 * @retval -EINVAL Rate not reachable.
 * @retval -EALREADY Capture already running.
 * @retval -EIO PWM or GPIO error.
 */
int dvp_adc_start(const struct device *dev, uint32_t rate_hz);

/**
 * @brief Stop the capture.
 *
 * Stops the DVP and the ADC clock. Blocks not yet taken are discarded.
 *
 * @param dev Capture device.
 *
 * @retval 0 Capture stopped.
 * @retval -EALREADY Capture not running.
 */
int dvp_adc_stop(const struct device *dev);

/**
 * @brief Take the oldest completed block.
 *
 * The block stays valid until dvp_adc_release_block(). Only one block can
 * be taken at a time.
 *
 * @param dev Capture device.
 * @param data Set to the block data.
 * @param len Set to the block length in bytes.
 * @param timeout Time to wait for a block.
 *
 * @retval 0 Block taken.
 * @retval -EAGAIN No block completed within @p timeout.
 * @retval -EBUSY A block is already taken.
 */
int dvp_adc_get_block(const struct device *dev, const uint8_t **data, size_t *len,
		      k_timeout_t timeout);

/**
 * @brief Release the block taken with dvp_adc_get_block().
 *
 * @param dev Capture device.
 */
void dvp_adc_release_block(const struct device *dev);

/**
 * @brief Get the capture statistics.
 *
 * @param dev Capture device.
 * @param stats Filled with the current statistics.
 */
void dvp_adc_get_stats(const struct device *dev, struct dvp_adc_stats *stats);

#endif /* DVP_ADC_H_ */
