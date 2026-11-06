/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Sample source for the bulk IN stream.
 *
 * Test mode: an 8-bit counter, pulled by rtl2832u.c through sample_src_fill().
 * Otherwise a thread converts ADC blocks to I/Q with the down-converter and
 * submits full transfer buffers. The ADC paces the stream; when the host
 * does not take the data fast enough, output is dropped and counted.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <dvp_adc.h>

#include "ddc.h"
#include "if_agc.h"
#include "rtl2832u.h"
#include "sample_src.h"
#include "si_fmt.h"

LOG_MODULE_REGISTER(sample_src, LOG_LEVEL_INF);

#define SAMPLE_SRC_BLOCK_TIMEOUT K_MSEC(100)
#define SAMPLE_SRC_STACK_SIZE    2048
#define SAMPLE_SRC_PRIORITY      10
/* Estimated down-converter load above which USB and interrupts no longer fit */
#define SAMPLE_SRC_LOAD_WARN_PCT 80U
/* Load is reported over this window */
#define SAMPLE_SRC_LOAD_WINDOW_MS 1000U
/*
 * The cycle counter can jump back by one tick (see AGENTS.md); block times
 * above one tick are such jumps and are dropped.
 */
#define SAMPLE_SRC_CYCLES_PER_TICK                                                                 \
	((uint32_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

/* Host settings, written from the USB thread */
struct sample_src_settings {
	uint32_t rate;
	int32_t if_hz;
	bool invert;
};

static struct k_spinlock settings_lock;
static struct sample_src_settings settings;
/* Incremented on every settings change, so the producer re-plans */
static atomic_t settings_gen;

static atomic_t test_mode;
static atomic_t run;
static K_SEM_DEFINE(run_sem, 0, 1);

static struct k_spinlock stats_lock;
static struct sample_src_stats stats;

void sample_src_set_test_mode(bool enable)
{
	(void)atomic_set(&test_mode, enable ? 1 : 0);
}

static void settings_update(const struct sample_src_settings *next)
{
	k_spinlock_key_t key = k_spin_lock(&settings_lock);

	settings = *next;
	k_spin_unlock(&settings_lock, key);
	(void)atomic_inc(&settings_gen);
}

static struct sample_src_settings settings_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&settings_lock);
	const struct sample_src_settings cur = settings;

	k_spin_unlock(&settings_lock, key);

	return cur;
}

void sample_src_set_rate(uint32_t rate)
{
	struct sample_src_settings next = settings_get();

	next.rate = rate;
	settings_update(&next);
}

void sample_src_set_if(int32_t if_hz)
{
	struct sample_src_settings next = settings_get();

	next.if_hz = if_hz;
	settings_update(&next);
}

void sample_src_set_inversion(bool invert)
{
	struct sample_src_settings next = settings_get();

	next.invert = invert;
	settings_update(&next);
}

void sample_src_fill(uint8_t *buf, size_t len)
{
	static uint8_t counter;

	for (size_t i = 0U; i < len; i++) {
		buf[i] = counter++;
	}
}

void sample_src_start(void)
{
	if (atomic_get(&test_mode) != 0) {
		return;
	}

	(void)atomic_set(&run, 1);
	k_sem_give(&run_sem);
}

void sample_src_stop(void)
{
	(void)atomic_set(&run, 0);
}

void sample_src_get_stats(struct sample_src_stats *out)
{
	K_SPINLOCK(&stats_lock) {
		*out = stats;
	}
}

/* Producer state, owned by the thread */
struct producer {
	const struct device *adc;
	struct ddc ddc;
	struct ddc_plan plan;
	bool planned;
	bool adc_running;
	atomic_val_t gen;
	struct net_buf *buf;
	uint64_t busy_cycles;
	uint32_t window_start_ms;
};

static void stats_set_plan(const struct producer *p, int32_t if_hz)
{
	K_SPINLOCK(&stats_lock) {
		stats.planned = p->planned;
		stats.exact = p->plan.exact;
		stats.out_rate = p->plan.out_rate;
		stats.actual_rate = p->plan.actual_rate;
		stats.ppm_error = p->plan.ppm_error;
		stats.adc_rate = p->plan.adc_rate;
		stats.decim = p->plan.decim;
		stats.residual_hz = p->plan.residual_hz;
		stats.est_load_pct = p->plan.load_pct;
		stats.invert = p->ddc.q_sign < 0;
		stats.if_freq = if_hz;
	}
}

static void producer_adc_stop(struct producer *p)
{
	if (p->adc_running) {
		(void)dvp_adc_stop(p->adc);
		p->adc_running = false;
	}
}

/* Plan for the current settings; restart the ADC if its rate changes */
static void producer_replan(struct producer *p)
{
	const struct sample_src_settings cur = settings_get();
	const uint16_t old_period = p->planned ? p->plan.period : 0U;
	char buf[SI_FMT_LEN];
	int err;

	p->gen = atomic_get(&settings_gen);
	p->planned = false;

	err = ddc_plan(cur.rate, cur.if_hz, &p->plan);
	if (err == 0) {
		ddc_init(&p->ddc, &p->plan, cur.if_hz, cur.invert);
	}

	if (err != 0) {
		LOG_ERR("No ADC rate for %u S/s at IF %d Hz (err %d)", cur.rate, cur.if_hz, err);
		producer_adc_stop(p);
		stats_set_plan(p, cur.if_hz);
		return;
	}

	p->planned = true;
	stats_set_plan(p, cur.if_hz);

	if (!p->plan.exact) {
		LOG_WRN("%u S/s not reachable, streaming %u S/s (%d ppm)", cur.rate,
			p->plan.actual_rate, p->plan.ppm_error);
	}

	if (p->adc_running && p->plan.period == old_period) {
		return;
	}

	producer_adc_stop(p);

	err = dvp_adc_start(p->adc, p->plan.adc_rate);
	if (err != 0) {
		LOG_ERR("ADC start at %u S/s failed (err %d)", p->plan.adc_rate, err);
		p->planned = false;
		return;
	}

	p->adc_running = true;
	LOG_INF("ADC %s, decimation %u, residual %d Hz, estimated load %u %%",
		si_fmt(buf, sizeof(buf), p->plan.adc_rate, "S/s"), p->plan.decim,
		p->plan.residual_hz, p->plan.load_pct);

	if (p->plan.load_pct > SAMPLE_SRC_LOAD_WARN_PCT) {
		LOG_WRN("Down-converter load above %u %%: ADC blocks will be lost",
			SAMPLE_SRC_LOAD_WARN_PCT);
	}
}

static void producer_submit(struct producer *p)
{
	if (rtl2832u_submit(p->buf) == 0) {
		K_SPINLOCK(&stats_lock) {
			stats.xfers++;
		}
	}

	p->buf = NULL;
}

/* Convert one ADC block into transfer buffers */
static void producer_block(struct producer *p, const uint8_t *in, size_t len)
{
	size_t pos = 0U;

	if_agc_process(in, len, p->plan.adc_rate);

	while (pos < len) {
		size_t space;
		size_t n;

		if (p->buf == NULL) {
			p->buf = rtl2832u_alloc_xfer();
			if (p->buf == NULL) {
				/* All transfers in flight: the host is slower than the ADC */
				K_SPINLOCK(&stats_lock) {
					stats.dropped += (uint32_t)ddc_out_len(&p->ddc, len - pos);
				}

				return;
			}
		}

		space = net_buf_tailroom(p->buf);
		n = MIN(len - pos, ddc_in_len(&p->ddc, space));
		(void)net_buf_add(p->buf, ddc_process(&p->ddc, &in[pos], n,
						      net_buf_tail(p->buf)));
		pos += n;

		if (net_buf_tailroom(p->buf) < 2U) {
			producer_submit(p);
		}
	}
}

static void producer_load(struct producer *p, uint32_t busy)
{
	const uint32_t now_ms = k_uptime_get_32();
	const uint32_t elapsed_ms = now_ms - p->window_start_ms;

	if (busy < SAMPLE_SRC_CYCLES_PER_TICK) {
		p->busy_cycles += busy;
	}

	if (elapsed_ms >= SAMPLE_SRC_LOAD_WINDOW_MS) {
		const uint32_t pct = (uint32_t)(p->busy_cycles * 100U * MSEC_PER_SEC /
						((uint64_t)elapsed_ms *
						 CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC));

		K_SPINLOCK(&stats_lock) {
			stats.load_pct = pct;
		}

		p->busy_cycles = 0U;
		p->window_start_ms = now_ms;
	}
}

static void producer_run(struct producer *p)
{
	p->planned = false;
	p->adc_running = false;
	p->buf = NULL;
	p->busy_cycles = 0U;
	p->window_start_ms = k_uptime_get_32();
	K_SPINLOCK(&stats_lock) {
		stats.running = true;
		stats.xfers = 0U;
		stats.dropped = 0U;
		stats.load_pct = 0U;
	}

	if_agc_restart();
	producer_replan(p);

	while (atomic_get(&run) != 0 && atomic_get(&test_mode) == 0) {
		const uint8_t *block;
		uint32_t start;
		size_t len;

		if (p->gen != atomic_get(&settings_gen)) {
			producer_replan(p);
		}

		if (!p->adc_running) {
			k_sleep(SAMPLE_SRC_BLOCK_TIMEOUT);
			continue;
		}

		if (dvp_adc_get_block(p->adc, &block, &len, SAMPLE_SRC_BLOCK_TIMEOUT) != 0) {
			continue;
		}

		start = k_cycle_get_32();
		producer_block(p, block, len);
		dvp_adc_release_block(p->adc);
		producer_load(p, k_cycle_get_32() - start);
	}

	producer_adc_stop(p);

	if (p->buf != NULL) {
		net_buf_unref(p->buf);
		p->buf = NULL;
	}

	K_SPINLOCK(&stats_lock) {
		stats.running = false;
	}
}

static void sample_src_thread(void *p1, void *p2, void *p3)
{
	static struct producer producer;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	producer.adc = DEVICE_DT_GET(DT_NODELABEL(adc_dvp));
	if (!device_is_ready(producer.adc)) {
		LOG_ERR("ADC capture device not ready");
		return;
	}

	for (;;) {
		(void)k_sem_take(&run_sem, K_FOREVER);

		if (atomic_get(&run) != 0) {
			producer_run(&producer);
		}
	}
}

K_THREAD_DEFINE(sample_src_tid, SAMPLE_SRC_STACK_SIZE, sample_src_thread, NULL, NULL, NULL,
		SAMPLE_SRC_PRIORITY, 0, 0);
