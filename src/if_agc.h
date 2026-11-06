/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_IF_AGC_H_
#define WELLE_SDR_IF_AGC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** IF AGC state for the shell. */
struct if_agc_stats {
	/** The loop controls the IF_AGC voltage. */
	bool active;
	/** Current PWM compare value. */
	uint16_t counts;
	/** Voltage while the loop is off, in mV. */
	uint32_t fixed_mv;
	/** Mean absolute deviation the loop aims at, in 1/16 code. */
	uint16_t target_q4;
	/** Mean code of the last interval. */
	uint8_t dc;
	/** Mean absolute deviation of the last interval, in 1/16 code. */
	uint16_t mad_q4;
	/** Clipped samples in the last interval, per million. */
	uint32_t clip_ppm;
	/** Loop updates since the loop was turned on. */
	uint32_t updates;
};

/**
 * @brief Start the PWM on the IF_AGC pin at the fixed voltage.
 *
 * @retval 0 Success.
 * @retval -EIO Clock or pin configuration failed.
 */
int if_agc_init(void);

/**
 * @brief Turn the loop on or off.
 *
 * On: the R820T2 VGA follows the IF_AGC pin and the loop sets the voltage.
 * Off: the PWM returns to the fixed voltage and the VGA to the host's
 * register setting.
 *
 * @param on Loop on.
 */
void if_agc_set_active(bool on);

/**
 * @brief Feed one block of ADC codes to the loop.
 *
 * Called by the producer thread for every block. Does nothing while the loop
 * is off. Updates the PWM every 1 / CONFIG_WELLE_SDR_IF_AGC_RATE_HZ seconds
 * of samples.
 *
 * @param in ADC codes.
 * @param len Number of codes in @p in.
 * @param adc_rate ADC sample rate in samples per second.
 */
void if_agc_process(const uint8_t *in, size_t len, uint32_t adc_rate);

/**
 * @brief Float the IF_AGC pin around a tuner I2C transfer.
 *
 * The PWM edges disturb the bit-banged tuner I2C. While held, the pin is a
 * floating input and the RC low-pass capacitor keeps the IF_AGC voltage.
 * Does nothing before if_agc_init() has started the PWM.
 *
 * @param hold true before the transfer, false after it.
 */
void if_agc_bus_hold(bool hold);

/** Restart the measurement, for example when the stream restarts. */
void if_agc_restart(void);

/**
 * @brief Set the voltage used while the loop is off.
 *
 * @param mv Voltage in mV.
 *
 * @retval 0 Success.
 * @retval -ERANGE @p mv outside CONFIG_WELLE_SDR_IF_AGC_MIN_MV to
 *	   CONFIG_WELLE_SDR_IF_AGC_MAX_MV.
 */
int if_agc_set_fixed_mv(uint32_t mv);

/**
 * @brief Set the level the loop aims at.
 *
 * @param target_q4 Mean absolute deviation of the ADC codes in 1/16 code.
 *
 * @retval 0 Success.
 * @retval -EINVAL @p target_q4 is 0.
 */
int if_agc_set_target(uint16_t target_q4);

/**
 * @brief Read the IF AGC state.
 *
 * @param stats Filled with the current state.
 */
void if_agc_get_stats(struct if_agc_stats *stats);

#endif /* WELLE_SDR_IF_AGC_H_ */
