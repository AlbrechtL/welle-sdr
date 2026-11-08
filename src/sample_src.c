/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Sample source for the bulk IN stream.
 *
 * Test mode: an 8-bit counter, pulled by rtl2832u.c through sample_src_fill().
 * Otherwise a thread converts ADC blocks to I/Q with the down-converter, or
 * in raw mode frames the ADC samples as they are, and submits full transfer
 * buffers. The ADC paces the stream; when the host does not take the data
 * fast enough, output is dropped and counted.
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
#include "raw_frame.h"
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

/* Host settings, written from the USB, shell and producer threads */
struct sample_src_settings {
	uint32_t rate;
	int32_t if_hz;
	bool invert;
	/* Down-converter output gain on */
	bool gain;
	enum sample_src_mode mode;
	/* Uptime when the mode was set, in ms: start of the raw mode lease */
	uint32_t mode_ms;
};

static struct k_spinlock settings_lock;
/* The down-converter output gain is off after boot: welle-sdr ddc gain on */
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

static struct sample_src_settings settings_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&settings_lock);
	const struct sample_src_settings cur = settings;

	k_spin_unlock(&settings_lock, key);

	return cur;
}

void sample_src_set_rate(uint32_t rate)
{
	K_SPINLOCK(&settings_lock) {
		settings.rate = rate;
	}

	(void)atomic_inc(&settings_gen);
}

void sample_src_set_if(int32_t if_hz)
{
	K_SPINLOCK(&settings_lock) {
		settings.if_hz = if_hz;
	}

	(void)atomic_inc(&settings_gen);
}

void sample_src_set_inversion(bool invert)
{
	K_SPINLOCK(&settings_lock) {
		settings.invert = invert;
	}

	(void)atomic_inc(&settings_gen);
}

void sample_src_set_ddc_gain(bool enable)
{
	K_SPINLOCK(&settings_lock) {
		settings.gain = enable;
	}

	K_SPINLOCK(&stats_lock) {
		stats.gain_on = enable;
	}

	(void)atomic_inc(&settings_gen);
}

void sample_src_set_mode(enum sample_src_mode mode)
{
	K_SPINLOCK(&settings_lock) {
		settings.mode = mode;
		settings.mode_ms = k_uptime_get_32();
	}

	(void)atomic_inc(&settings_gen);
}

void sample_src_fill(uint8_t *buf, size_t len)
{
	static uint8_t counter;
	size_t i = 0U;

	/* Bytes up to a word boundary, then four counter values per word store */
	for (; i < len && ((uintptr_t)&buf[i] & 3U) != 0U; i++) {
		buf[i] = counter++;
	}

	if (i + 4U <= len) {
		uint32_t *word = (uint32_t *)(void *)&buf[i];
		const size_t words = (len - i) / 4U;

		/* Little-endian: the lowest byte goes first on the wire */
		for (size_t n = 0U; n < words; n++) {
			word[n] = (uint32_t)counter | ((uint32_t)(uint8_t)(counter + 1U) << 8) |
				  ((uint32_t)(uint8_t)(counter + 2U) << 16) |
				  ((uint32_t)(uint8_t)(counter + 3U) << 24);
			counter += 4U;
		}

		i += words * 4U;
	}

	for (; i < len; i++) {
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
	/* ADC clock period and rate of the running plan */
	uint16_t period;
	uint32_t adc_rate;
	/* Raw mode: framer and plan */
	bool raw;
	struct raw_frame frame;
	struct raw_frame_plan raw_plan;
	/* Raw mode lease: mode_ms of the settings, last transfer progress */
	uint32_t mode_ms;
	uint32_t lease_ms;
	uint32_t lease_xfers;
	atomic_val_t gen;
	struct net_buf *buf;
	uint64_t busy_cycles;
	uint32_t window_start_ms;
};

static void stats_set_plan(const struct producer *p, int32_t if_hz)
{
	K_SPINLOCK(&stats_lock) {
		stats.mode = p->raw ? SAMPLE_SRC_MODE_RAW : SAMPLE_SRC_MODE_IQ;
		stats.raw_capped = p->raw && p->raw_plan.capped;
		stats.raw_aliased = p->raw && p->raw_plan.aliased;
		stats.raw_need_rate = p->raw ? p->raw_plan.need_rate : 0U;
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
		stats.gain_db10 = p->raw ? 0 : p->ddc.gain_db10;
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

/* Drop the partly filled transfer, so the next one starts a frame or a new format */
static void producer_drop_buf(struct producer *p)
{
	if (p->buf != NULL) {
		net_buf_unref(p->buf);
		p->buf = NULL;
	}

	if (p->raw) {
		raw_frame_restart(&p->frame);
	}
}

/* Switch between I/Q and raw output */
static void producer_set_mode(struct producer *p, const struct sample_src_settings *cur)
{
	const bool raw = cur->mode == SAMPLE_SRC_MODE_RAW;

	if (raw == p->raw && (!raw || cur->mode_ms == p->mode_ms)) {
		return;
	}

	producer_drop_buf(p);
	p->raw = raw;

	if (raw) {
		raw_frame_init(&p->frame, CONFIG_WELLE_SDR_XFER_SIZE);
		p->mode_ms = cur->mode_ms;
		p->lease_ms = cur->mode_ms;
		p->lease_xfers = rtl2832u_xfers();
	}

	LOG_INF("Output %s", raw ? "raw ADC samples" : "I/Q");
}

/* Plan the down-converter; returns the ADC period, 0 without a plan */
static uint16_t producer_plan_ddc(struct producer *p, const struct sample_src_settings *cur)
{
	int err;

	err = ddc_plan(cur->rate, cur->if_hz, &p->plan);
	if (err != 0) {
		LOG_ERR("No ADC rate for %u S/s at IF %d Hz (err %d)", cur->rate, cur->if_hz, err);
		return 0U;
	}

	ddc_init(&p->ddc, &p->plan, cur->if_hz, cur->invert, cur->gain);

	if (!p->plan.exact) {
		LOG_WRN("%u S/s not reachable, streaming %u S/s (%d ppm)", cur->rate,
			p->plan.actual_rate, p->plan.ppm_error);
	}

	return p->plan.period;
}

/* Plan the raw ADC rate; returns the ADC period, 0 without a plan */
static uint16_t producer_plan_raw(struct producer *p, const struct sample_src_settings *cur)
{
	struct raw_frame_params params;
	int err;

	err = raw_frame_plan(cur->rate, cur->if_hz, CONFIG_WELLE_SDR_RAW_MAX_RATE, &p->raw_plan);
	if (err != 0) {
		LOG_ERR("No raw ADC rate for %u S/s at IF %d Hz (err %d)", cur->rate, cur->if_hz,
			err);
		return 0U;
	}

	params.adc_rate = p->raw_plan.adc_rate;
	params.if_hz = cur->if_hz;
	params.flags = (cur->invert ? WSDR_RAW_FLAG_INVERTED : 0U) |
		       (p->raw_plan.aliased ? WSDR_RAW_FLAG_ALIASED : 0U);
	raw_frame_set_params(&p->frame, &params);

	/* The plain I/Q fields of the shell status */
	p->plan.out_rate = cur->rate;
	p->plan.actual_rate = p->raw_plan.adc_rate;
	p->plan.exact = true;
	p->plan.ppm_error = 0;
	p->plan.adc_rate = p->raw_plan.adc_rate;
	p->plan.decim = 1U;
	p->plan.residual_hz = 0;
	p->plan.load_pct = 0U;
	p->ddc.q_sign = cur->invert ? -1 : 1;

	if (p->raw_plan.aliased) {
		LOG_WRN("Band edge about %u Hz, above half the raw ADC rate %u S/s",
			p->raw_plan.need_rate * 5U / 11U, p->raw_plan.adc_rate);
	}

	return p->raw_plan.period;
}

/* Plan for the current settings; restart the ADC if its rate changes */
static void producer_replan(struct producer *p)
{
	const struct sample_src_settings cur = settings_get();
	const uint16_t old_period = p->adc_running ? p->period : 0U;
	char buf[SI_FMT_LEN];
	uint16_t period;
	int err;

	p->gen = atomic_get(&settings_gen);
	p->planned = false;

	producer_set_mode(p, &cur);

	period = p->raw ? producer_plan_raw(p, &cur) : producer_plan_ddc(p, &cur);
	if (period == 0U) {
		producer_adc_stop(p);
		stats_set_plan(p, cur.if_hz);
		return;
	}

	p->planned = true;
	stats_set_plan(p, cur.if_hz);

	if (p->adc_running && period == old_period) {
		return;
	}

	/* Samples at the old and the new rate must not share a frame */
	producer_drop_buf(p);
	producer_adc_stop(p);

	p->period = period;
	/* Rounded like dvp_adc_start() expects it */
	p->adc_rate = (DDC_CLOCK_HZ + period / 2U) / period;

	err = dvp_adc_start(p->adc, p->adc_rate);
	if (err != 0) {
		LOG_ERR("ADC start at %u S/s failed (err %d)", p->adc_rate, err);
		p->planned = false;
		return;
	}

	p->adc_running = true;

	if (p->raw) {
		LOG_INF("ADC %s raw%s", si_fmt(buf, sizeof(buf), p->adc_rate, "S/s"),
			p->raw_plan.capped ? ", capped by CONFIG_WELLE_SDR_RAW_MAX_RATE" : "");
		return;
	}

	LOG_INF("ADC %s, decimation %u, residual %d Hz, estimated load %u %%",
		si_fmt(buf, sizeof(buf), p->adc_rate, "S/s"), p->plan.decim,
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

/* Frame one ADC block into transfer buffers, one frame per transfer */
static void producer_block_raw(struct producer *p, const uint8_t *in, size_t len)
{
	size_t pos = 0U;

	while (pos < len) {
		size_t used;

		if (p->buf == NULL) {
			p->buf = rtl2832u_alloc_xfer();
			if (p->buf == NULL) {
				/* At a frame start: the next header tells the host */
				raw_frame_skip(&p->frame, len - pos);
				K_SPINLOCK(&stats_lock) {
					stats.dropped += (uint32_t)(len - pos);
				}

				return;
			}
		}

		(void)net_buf_add(p->buf, raw_frame_fill(&p->frame, &in[pos], len - pos,
							 net_buf_tail(p->buf),
							 CONFIG_WELLE_SDR_XFER_SIZE - p->buf->len,
							 &used));
		pos += used;

		if (p->buf->len == CONFIG_WELLE_SDR_XFER_SIZE) {
			producer_submit(p);
		}
	}
}

/* Convert one ADC block into transfer buffers */
static void producer_block(struct producer *p, const uint8_t *in, size_t len)
{
	size_t pos = 0U;

	if_agc_process(in, len, p->adc_rate);

	if (p->raw) {
		producer_block_raw(p, in, len);
		return;
	}

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

/* End raw mode when the host has not taken a transfer for CONFIG_WELLE_SDR_RAW_IDLE_MS */
static void producer_lease(struct producer *p)
{
	const uint32_t xfers = rtl2832u_xfers();
	const uint32_t now_ms = k_uptime_get_32();
	bool ended = false;

	if (xfers != p->lease_xfers) {
		p->lease_xfers = xfers;
		p->lease_ms = now_ms;
		return;
	}

	if (now_ms - p->lease_ms <= CONFIG_WELLE_SDR_RAW_IDLE_MS) {
		return;
	}

	/* Only the raw mode this lease belongs to, not one the host set meanwhile */
	K_SPINLOCK(&settings_lock) {
		if (settings.mode == SAMPLE_SRC_MODE_RAW && settings.mode_ms == p->mode_ms) {
			settings.mode = SAMPLE_SRC_MODE_IQ;
			settings.mode_ms = now_ms;
			ended = true;
		}
	}

	if (ended) {
		/* Queued raw frames must not reach the next host */
		producer_drop_buf(p);
		rtl2832u_flush();
		(void)atomic_inc(&settings_gen);
		K_SPINLOCK(&stats_lock) {
			stats.raw_lease_ends++;
		}
		LOG_INF("Raw mode ended: no transfer for %u ms", now_ms - p->lease_ms);
	}
}

static void producer_run(struct producer *p)
{
	p->planned = false;
	p->adc_running = false;
	p->raw = false;
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

		if (p->raw) {
			producer_lease(p);
		}

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
