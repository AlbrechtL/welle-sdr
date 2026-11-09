/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * CPU load from the time spent in the idle thread, measured at each thread
 * switch (tracing hook) against TIM4 counting at 10 kHz.
 *
 * TIM4 rather than the kernel clock: without
 * CONFIG_RISCV_ALWAYS_SWITCH_THROUGH_ECALL, the PFIC held back all
 * interrupts, SysTick included, while a thread ran that an interrupt had
 * switched to (README "CPU load and the kernel clock"), so neither tick
 * sampling nor CONFIG_SCHED_THREAD_USAGE, which reads the SysTick-based
 * cycle counter, saw the busy time. Thread switches are not delayed.
 * Interrupts that do not switch count for the thread they interrupt.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/tracing/tracing.h>

#include <hal_ch32fun.h>

#include "cpu_load.h"

LOG_MODULE_REGISTER(cpu_load, LOG_LEVEL_INF);

#define CPU_LOAD_TIMER DT_NODELABEL(tim4)

/* CPU_LOAD_CLOCK_HZ: 100 us resolution, the 16-bit counter wraps after 6.5 s */
/*
 * Window the load is reported over. Its end is scheduled on the kernel
 * clock, which runs late here; its length comes from the counter.
 */
#define CPU_LOAD_WINDOW K_SECONDS(1)

BUILD_ASSERT(IS_ENABLED(CONFIG_TRACING_USER), "Needs the tracing user hooks");

static TIM_TypeDef *const tim = (TIM_TypeDef *)DT_REG_ADDR(CPU_LOAD_TIMER);

/* Updated with interrupts locked: in the switch hook and at the end of a window */
static struct {
	bool running;
	bool in_idle;
	uint16_t last_cnt;
	/* Time since the start, in counter clocks, wrapping */
	uint32_t now;
	/* Time and idle time in the current window, in counter clocks */
	uint32_t span;
	uint32_t idle;
} load;

static atomic_t load_pct;

/* Account the time since the last call, at least once per window */
static void cpu_load_advance(void)
{
	const uint16_t cnt = (uint16_t)tim->CNT;
	const uint16_t delta = cnt - load.last_cnt;

	load.last_cnt = cnt;
	load.now += delta;
	load.span += delta;
	if (load.in_idle) {
		load.idle += delta;
	}
}

static void cpu_load_window(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	unsigned int key = irq_lock();
	uint32_t span;
	uint32_t idle;

	cpu_load_advance();
	span = load.span;
	idle = MIN(load.idle, span);
	load.span = 0U;
	load.idle = 0U;
	irq_unlock(key);

	if (span > 0U) {
		(void)atomic_set(&load_pct,
				 (atomic_val_t)(((uint64_t)(span - idle) * 100U + span / 2U) / span));
	}

	(void)k_work_schedule(dwork, CPU_LOAD_WINDOW);
}

static K_WORK_DELAYABLE_DEFINE(cpu_load_work, cpu_load_window);

/* Overrides the weak hook of CONFIG_TRACING_USER, called with interrupts locked */
void sys_trace_thread_switched_in_user(void)
{
	if (!load.running) {
		return;
	}

	cpu_load_advance();
	/* The TLS pointer may still be the old thread's here, _current is the new one */
	load.in_idle = (k_thread_priority_get(k_sched_current_thread_query()) == K_IDLE_PRIO);
}

void cpu_load_init(void)
{
	const struct device *const clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(CPU_LOAD_TIMER));
	const clock_control_subsys_t clk_id =
		(clock_control_subsys_t)(uintptr_t)DT_CLOCKS_CELL(CPU_LOAD_TIMER, id);
	unsigned int key;
	uint32_t rate;

	if (clock_control_on(clk, clk_id) != 0 || clock_control_get_rate(clk, clk_id, &rate) != 0 ||
	    rate / CPU_LOAD_CLOCK_HZ == 0U || rate / CPU_LOAD_CLOCK_HZ > 65536U) {
		LOG_WRN("CPU load timer not available");
		return;
	}

	tim->CTLR1 = 0U;
	tim->PSC = (uint16_t)(rate / CPU_LOAD_CLOCK_HZ - 1U);
	tim->ATRLR = UINT16_MAX;
	/* Load the prescaler now */
	tim->SWEVGR = TIM_UG;
	tim->CTLR1 = TIM_CEN;

	key = irq_lock();
	load.last_cnt = (uint16_t)tim->CNT;
	/* Called from main, not idle */
	load.in_idle = false;
	load.running = true;
	irq_unlock(key);

	(void)k_work_schedule(&cpu_load_work, CPU_LOAD_WINDOW);
}

uint32_t cpu_load_pct(void)
{
	return (uint32_t)atomic_get(&load_pct);
}

uint32_t cpu_load_clock(void)
{
	unsigned int key = irq_lock();
	uint32_t now = 0U;

	if (load.running) {
		cpu_load_advance();
		now = load.now;
	}

	irq_unlock(key);

	return now;
}
