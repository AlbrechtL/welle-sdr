/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Square-wave test signal on PA7 (TIM3_CH2) for the ADC input.
 *
 * TIM3 is programmed directly instead of through the Zephyr PWM API:
 * pwm_wch_gptm.c names its configs pwm_wch_gptm_<instance>_config for both
 * the wch,gptm-pwm and the wch,adtm-pwm instances, so enabling TIM3 next to
 * TIM1 (the ADC clock) defines pwm_wch_gptm_0_config twice.
 */

#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>

#include <hal_ch32fun.h>

#include "testsig.h"

#define TESTSIG_NODE DT_PATH(zephyr_user)

/* PWM mode 1 for channel 2: output high while CNT < CH2CVR */
#define TESTSIG_OC2M_PWM1 (6U << 12)

#define TESTSIG_MAX_PERIOD 65536U

PINCTRL_DT_DEFINE(TESTSIG_NODE);

int testsig_set(uint32_t freq_hz, uint32_t *actual_hz)
{
	static bool ready;
	TIM_TypeDef *const tim = (TIM_TypeDef *)DT_REG_ADDR(DT_NODELABEL(tim3));
	const struct device *const clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(TESTSIG_NODE));
	const clock_control_subsys_t clk_id =
		(clock_control_subsys_t)(uintptr_t)DT_CLOCKS_CELL(TESTSIG_NODE, id);
	uint32_t rate;
	uint32_t period;

	if (freq_hz != 0U && (freq_hz < TESTSIG_MIN_HZ || freq_hz > TESTSIG_MAX_HZ)) {
		return -ERANGE;
	}

	if (!ready) {
		if (clock_control_on(clk, clk_id) != 0 ||
		    pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(TESTSIG_NODE),
					PINCTRL_STATE_DEFAULT) != 0) {
			return -EIO;
		}

		ready = true;
	}

	if (freq_hz == 0U) {
		/* Compare value 0: the output stays low */
		tim->CH2CVR = 0U;
		return 0;
	}

	if (clock_control_get_rate(clk, clk_id, &rate) != 0) {
		return -EIO;
	}

	period = (rate + freq_hz / 2U) / freq_hz;
	if (period < 2U || period > TESTSIG_MAX_PERIOD) {
		return -ERANGE;
	}

	tim->CTLR1 = 0U;
	tim->PSC = 0U;
	tim->ATRLR = (uint16_t)(period - 1U);
	tim->CH2CVR = (uint16_t)(period / 2U);
	tim->CHCTLR1 = (tim->CHCTLR1 & ~(TIM_OC2M | TIM_OC2PE)) | TESTSIG_OC2M_PWM1 | TIM_OC2PE;
	tim->CCER |= TIM_CC2E;
	/* Load the preloaded period and compare value now */
	tim->SWEVGR = TIM_UG;
	tim->CTLR1 = TIM_ARPE | TIM_CEN;

	if (actual_hz != NULL) {
		*actual_hz = rate / period;
	}

	return 0;
}
