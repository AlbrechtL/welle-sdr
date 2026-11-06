/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * IF AGC: PWM on the tuner board's IF_AGC low-pass, which sets the R820T2
 * VGA gain while register 0x0c bit 4 selects pin control.
 *
 * The timer is programmed directly instead of through the Zephyr PWM API:
 * pwm_wch_gptm.c names its configs pwm_wch_gptm_<instance>_config for both
 * the wch,gptm-pwm and the wch,adtm-pwm instances, so enabling a second
 * timer next to TIM1 (the ADC clock) defines pwm_wch_gptm_0_config twice.
 *
 * The loop runs in the producer thread on the raw ADC blocks and only
 * writes the compare register, so it needs no I2C traffic.
 *
 * The PWM edges couple into the bit-banged tuner I2C (measured: 2 to 5 of 10
 * standalone tunes failed with a NACK, none with a constant level on the
 * pin). r820t2.c therefore floats the pin around every transfer with
 * if_agc_bus_hold(); the low-pass capacitor holds the voltage meanwhile.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include <hal_ch32fun.h>

#include "if_agc.h"
#include "if_agc_ctl.h"
#include "r820t2.h"

LOG_MODULE_REGISTER(if_agc, LOG_LEVEL_INF);

#define IF_AGC_NODE  DT_NODELABEL(if_agc)
#define IF_AGC_TIMER DT_PHANDLE(IF_AGC_NODE, timer)

/* PWM mode 1 for channel 1: output high while CNT < CH1CVR */
#define IF_AGC_OC1M_PWM1 (6U << 4)

BUILD_ASSERT(CONFIG_WELLE_SDR_IF_AGC_MIN_MV < CONFIG_WELLE_SDR_IF_AGC_MAX_MV,
	     "IF AGC voltage range is empty");
BUILD_ASSERT(CONFIG_WELLE_SDR_IF_AGC_FIXED_MV >= CONFIG_WELLE_SDR_IF_AGC_MIN_MV &&
		     CONFIG_WELLE_SDR_IF_AGC_FIXED_MV <= CONFIG_WELLE_SDR_IF_AGC_MAX_MV,
	     "IF AGC fixed voltage outside the range");

PINCTRL_DT_DEFINE(IF_AGC_NODE);

static TIM_TypeDef *const tim = (TIM_TypeDef *)DT_REG_ADDR(IF_AGC_TIMER);
static const struct gpio_dt_spec pin = GPIO_DT_SPEC_GET(IF_AGC_NODE, gpios);
/* The PWM runs, if_agc_bus_hold() may switch the pin */
static bool pwm_ready;

/* Serializes the compare register and stats between the producer, USB and shell threads */
static struct k_spinlock lock;
static struct if_agc_stats stats = {
	.fixed_mv = CONFIG_WELLE_SDR_IF_AGC_FIXED_MV,
	.target_q4 = CONFIG_WELLE_SDR_IF_AGC_TARGET * 16U,
};
static atomic_t active;
/* Set to make the producer restart the loop from the current compare value */
static atomic_t restart;

/* Called with lock held */
static void set_counts(uint16_t counts)
{
	tim->CH1CVR = counts;
	stats.counts = counts;
}

int if_agc_init(void)
{
	const struct device *const clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(IF_AGC_TIMER));
	const clock_control_subsys_t clk_id =
		(clock_control_subsys_t)(uintptr_t)DT_CLOCKS_CELL(IF_AGC_TIMER, id);

	if (clock_control_on(clk, clk_id) != 0) {
		return -EIO;
	}

	tim->CTLR1 = 0U;
	tim->PSC = 0U;
	tim->ATRLR = (uint16_t)(IF_AGC_CTL_PERIOD - 1U);
	tim->CHCTLR1 = (tim->CHCTLR1 & ~(TIM_OC1M | TIM_OC1PE)) | IF_AGC_OC1M_PWM1 | TIM_OC1PE;

	K_SPINLOCK(&lock) {
		set_counts(if_agc_ctl_mv_to_counts(stats.fixed_mv));
	}

	tim->CCER |= TIM_CC1E;
	/* Load the preloaded period and compare value now */
	tim->SWEVGR = TIM_UG;
	tim->CTLR1 = TIM_ARPE | TIM_CEN;

	/* The pin only becomes a timer output once the PWM runs */
	if (!gpio_is_ready_dt(&pin) ||
	    pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(IF_AGC_NODE), PINCTRL_STATE_DEFAULT) !=
		    0) {
		return -EIO;
	}

	pwm_ready = true;

	LOG_INF("IF AGC PWM at %u mV", stats.fixed_mv);

	return 0;
}

void if_agc_set_active(bool on)
{
	int err;

	/* Only called from the USB thread */
	if ((atomic_get(&active) != 0) == on) {
		return;
	}

	/* Before the loop becomes active, so the producer initializes it first */
	(void)atomic_set(&restart, 1);
	(void)atomic_set(&active, on ? 1 : 0);

	K_SPINLOCK(&lock) {
		stats.active = on;
		stats.updates = 0U;

		if (!on) {
			set_counts(if_agc_ctl_mv_to_counts(stats.fixed_mv));
		}
	}

	err = r820t2_set_vga_pin_mode(on);
	if (err != 0) {
		LOG_WRN("VGA mode not switched (err %d)", err);
	}

	LOG_INF("IF AGC %s", on ? "on" : "off");
}

void if_agc_bus_hold(bool hold)
{
	int err;

	/* Only called with the tuner lock held, from one thread at a time */
	if (!pwm_ready) {
		return;
	}

	if (hold) {
		err = gpio_pin_configure_dt(&pin, GPIO_INPUT);
	} else {
		err = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(IF_AGC_NODE),
					  PINCTRL_STATE_DEFAULT);
	}

	if (err != 0) {
		LOG_ERR("IF_AGC pin %s failed (err %d)", hold ? "hold" : "release", err);
	}
}

void if_agc_restart(void)
{
	(void)atomic_set(&restart, 1);
}

void if_agc_process(const uint8_t *in, size_t len, uint32_t adc_rate)
{
	/* Owned by the producer thread */
	static struct if_agc_ctl ctl;
	static struct if_agc_meas meas;
	static uint32_t samples;
	uint16_t counts;

	if (atomic_get(&active) == 0) {
		return;
	}

	if (atomic_cas(&restart, 1, 0)) {
		uint16_t start;
		uint16_t target;

		K_SPINLOCK(&lock) {
			start = stats.counts;
			target = stats.target_q4;
		}

		if_agc_ctl_init(&ctl, CONFIG_WELLE_SDR_IF_AGC_MIN_MV,
				CONFIG_WELLE_SDR_IF_AGC_MAX_MV, target, start);
		meas = (struct if_agc_meas){0};
		samples = 0U;
	}

	if_agc_ctl_measure(&meas, in, len, CONFIG_WELLE_SDR_IF_AGC_STRIDE, ctl.dc);
	samples += (uint32_t)len;

	if (samples < adc_rate / CONFIG_WELLE_SDR_IF_AGC_RATE_HZ) {
		return;
	}

	counts = if_agc_ctl_update(&ctl, &meas);

	K_SPINLOCK(&lock) {
		/* The loop may have been turned off meanwhile */
		if (stats.active) {
			set_counts(counts);
			stats.updates++;
		}

		stats.dc = ctl.dc;
		stats.mad_q4 = ctl.mad_q4;
		stats.clip_ppm = (uint32_t)((uint64_t)meas.clipped * 1000000U / meas.count);
	}

	meas = (struct if_agc_meas){0};
	samples = 0U;
}

int if_agc_set_fixed_mv(uint32_t mv)
{
	if (mv < CONFIG_WELLE_SDR_IF_AGC_MIN_MV || mv > CONFIG_WELLE_SDR_IF_AGC_MAX_MV) {
		return -ERANGE;
	}

	K_SPINLOCK(&lock) {
		stats.fixed_mv = mv;

		if (!stats.active) {
			set_counts(if_agc_ctl_mv_to_counts(mv));
		}
	}

	return 0;
}

int if_agc_set_target(uint16_t target_q4)
{
	if (target_q4 == 0U) {
		return -EINVAL;
	}

	K_SPINLOCK(&lock) {
		stats.target_q4 = target_q4;
	}

	(void)atomic_set(&restart, 1);

	return 0;
}

void if_agc_get_stats(struct if_agc_stats *out)
{
	K_SPINLOCK(&lock) {
		*out = stats;
	}
}
