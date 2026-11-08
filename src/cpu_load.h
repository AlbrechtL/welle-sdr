/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_CPU_LOAD_H_
#define WELLE_SDR_CPU_LOAD_H_

#include <stdint.h>

/* Rate of cpu_load_clock() */
#define CPU_LOAD_CLOCK_HZ 10000U

/** @brief Start measuring the CPU load, on TIM4. */
void cpu_load_init(void);

/**
 * @brief CPU load over the last second.
 *
 * Share of the time outside the idle thread over the last window of about
 * one second. Interrupts count for the thread they interrupt. 0 until the
 * first window has ended.
 *
 * @return Load in percent, 0 to 100.
 */
uint32_t cpu_load_pct(void);

/**
 * @brief Time from the CPU load timer.
 *
 * Unlike the kernel clock it does not stop or fall behind while interrupts
 * are held back (README "CPU load and the kernel clock").
 *
 * @return Time since cpu_load_init() in 1 / CPU_LOAD_CLOCK_HZ s, wrapping;
 *	   0 if the timer is not available.
 */
uint32_t cpu_load_clock(void);

#endif /* WELLE_SDR_CPU_LOAD_H_ */
