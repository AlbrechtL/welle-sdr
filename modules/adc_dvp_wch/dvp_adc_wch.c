/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * 8-bit parallel ADC captured with the WCH CH32V30x digital video port.
 *
 * The DVP runs in JPEG mode: COL_NUM is the DMA block length, and the DMA
 * writes alternately to DMA_BUF0 and DMA_BUF1, raising ROW_DONE after each
 * block. The ISR hands the completed block to the consumer and points that
 * address register at a free block before the DMA comes back to it.
 * HSYNC and VSYNC are driven by GPIO outputs wired to the DVP pins.
 *
 * Measured behavior the code depends on:
 * - The DVP samples VSYNC and HSYNC with PCLK, so PCLK must run before the
 *   sync edges.
 * - Data is stored only after HSYNC becomes active inside a frame; an HSYNC
 *   that is already active at the frame start does not count.
 * - While the CPU is busy (not in WFI), ROW_DONE can fire a second time for
 *   the same block, with no buffer switch until the next block (about one
 *   block time later). Without load it fires once per block. The ISR
 *   therefore takes a block as complete only when RB_DVP_BUF_TOG shows that
 *   the DVP switched to the other buffer, and ignores ROW_DONE otherwise. A
 *   ROW_DONE before its buffer switch was not observed.
 */

#define DT_DRV_COMPAT wch_dvp_adc

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <hal_ch32fun.h>

#include <dvp_adc.h>

LOG_MODULE_REGISTER(dvp_adc_wch, CONFIG_ADC_DVP_WCH_LOG_LEVEL);

#define BLOCK_SIZE  CONFIG_ADC_DVP_WCH_BLOCK_SIZE
#define BLOCK_COUNT CONFIG_ADC_DVP_WCH_BLOCK_COUNT

BUILD_ASSERT((BLOCK_SIZE % 4) == 0, "The DVP DMA moves 4 bytes per request");

/* Fill pattern of the ring at start, to detect DMA writes past a block */
#define FILL_PATTERN 0x55U
#define GUARD_SIZE   64U

/* At 144 MHz, 6 cycles keep 20.8 ns high and low time (AD9280 minimum 14.7 ns) */
#define MIN_PERIOD_CYCLES 6U

/* Above this rate the falling edge falls into the AD9280 output transition (t_OD 25 ns) */
#define FALLING_EDGE_MAX_RATE 20000000U

/*
 * Wait after starting the PWM: the period register is preloaded, so a new
 * period takes effect only after the counter has run through the old one
 * (0xffff cycles after reset, 455 us at 144 MHz).
 */
#define CLOCK_SETTLE K_MSEC(1)

/* PCLK periods between sync edges, so that the DVP samples each level */
#define SYNC_PCLK_PERIODS 4U
#define SYNC_RETRIES      3U

#define NO_BLOCK UINT8_MAX

struct dvp_adc_config {
	DVP_TypeDef *regs;
	const struct device *clk_dev;
	uint8_t clk_id;
	struct pwm_dt_spec clock;
	struct gpio_dt_spec hsync;
	struct gpio_dt_spec vsync;
	const struct gpio_dt_spec *inputs;
	size_t num_inputs;
	void (*irq_config)(void);
};

struct dvp_adc_data {
	struct k_spinlock lock;
	/* Counts the blocks in ready[] */
	struct k_sem ready_sem;
	/* Completed blocks, oldest first, starting at ready_head */
	uint8_t ready[BLOCK_COUNT];
	uint8_t ready_head;
	uint8_t ready_count;
	/* Blocks neither assigned to the DMA, completed nor taken */
	uint8_t free[BLOCK_COUNT];
	uint8_t free_count;
	/* Blocks assigned to DMA_BUF0 and DMA_BUF1 */
	uint8_t dma_block[2];
	/* DMA buffer the DVP is writing: 0 or 1 */
	uint8_t writing;
	/* Block held by the consumer, or NO_BLOCK */
	uint8_t taken;
	/* Wait between sync edges */
	uint32_t sync_wait_us;
	struct dvp_adc_stats stats;
	uint8_t ring[BLOCK_COUNT][BLOCK_SIZE] __aligned(4);
	uint8_t guard[GUARD_SIZE];
};

static void ready_push(struct dvp_adc_data *data, uint8_t block)
{
	data->ready[(data->ready_head + data->ready_count) % BLOCK_COUNT] = block;
	data->ready_count++;
}

static uint8_t ready_pop(struct dvp_adc_data *data)
{
	const uint8_t block = data->ready[data->ready_head];

	data->ready_head = (data->ready_head + 1U) % BLOCK_COUNT;
	data->ready_count--;

	return block;
}

static void set_dma_addr(DVP_TypeDef *regs, uint8_t buf, const uint8_t *addr)
{
	if (buf == 0U) {
		regs->DMA_BUF0 = (uint32_t)(uintptr_t)addr;
	} else {
		regs->DMA_BUF1 = (uint32_t)(uintptr_t)addr;
	}
}

static uint8_t buf_toggle(const DVP_TypeDef *regs)
{
	return ((regs->CR1 & RB_DVP_BUF_TOG) != 0U) ? 1U : 0U;
}

/* Called with the lock held, after the DVP switched away from buffer @p buf */
static void block_done(const struct dvp_adc_config *config, struct dvp_adc_data *data,
		       uint8_t buf)
{
	uint8_t block;

	data->stats.blocks++;

	ready_push(data, data->dma_block[buf]);
	k_sem_give(&data->ready_sem);

	if (data->free_count > 0U) {
		data->free_count--;
		block = data->free[data->free_count];
	} else {
		/* Consumer too slow: reuse the oldest completed block */
		block = ready_pop(data);
		(void)k_sem_take(&data->ready_sem, K_NO_WAIT);
		data->stats.overruns++;
	}

	data->dma_block[buf] = block;
	set_dma_addr(config->regs, buf, data->ring[block]);
}

static void dvp_adc_isr(const struct device *dev)
{
	const struct dvp_adc_config *config = dev->config;
	struct dvp_adc_data *data = dev->data;
	DVP_TypeDef *regs = config->regs;
	const uint8_t flags = regs->IFR;
	k_spinlock_key_t key;

	if ((flags & RB_DVP_IF_ROW_DONE) != 0U) {
		const uint8_t buf = data->writing;
		const uint8_t now = buf_toggle(regs);

		/* Flags are cleared by writing 0 */
		regs->IFR &= (uint8_t)~RB_DVP_IF_ROW_DONE;

		key = k_spin_lock(&data->lock);

		/* A block is complete only once the DVP writes the other buffer */
		if (now != buf) {
			data->writing = now;
			block_done(config, data, buf);
		} else {
			data->stats.spurious++;
		}

		k_spin_unlock(&data->lock, key);
	}

	if ((flags & RB_DVP_IF_FIFO_OV) != 0U) {
		regs->IFR &= (uint8_t)~RB_DVP_IF_FIFO_OV;
		data->stats.fifo_overflows++;
	}
}

/* Drive VSYNC active and wait for the frame start flag */
static int frame_start(const struct dvp_adc_config *config, struct dvp_adc_data *data)
{
	DVP_TypeDef *regs = config->regs;
	int err;

	for (uint32_t attempt = 0U; attempt < SYNC_RETRIES; attempt++) {
		k_busy_wait(data->sync_wait_us);

		err = gpio_pin_set_dt(&config->vsync, 1);
		if (err != 0) {
			return -EIO;
		}

		k_busy_wait(data->sync_wait_us);

		if ((regs->IFR & RB_DVP_IF_STR_FRM) != 0U) {
			regs->IFR &= (uint8_t)~RB_DVP_IF_STR_FRM;
			data->stats.frame_starts++;
			return 0;
		}

		LOG_DBG("No frame start, attempt %u", attempt);
		(void)gpio_pin_set_dt(&config->vsync, 0);
	}

	return -EIO;
}

int dvp_adc_start(const struct device *dev, uint32_t rate_hz)
{
	const struct dvp_adc_config *config = dev->config;
	struct dvp_adc_data *data = dev->data;
	DVP_TypeDef *regs = config->regs;
	uint64_t cycles_per_sec;
	k_spinlock_key_t key;
	uint32_t period;
	uint8_t cr0;
	uint8_t n = 0U;
	int err;

	if (data->stats.running) {
		return -EALREADY;
	}

	err = pwm_get_cycles_per_sec(config->clock.dev, config->clock.channel, &cycles_per_sec);
	if (err != 0) {
		return -EIO;
	}

	if (rate_hz == 0U || (cycles_per_sec % rate_hz) != 0U) {
		return -EINVAL;
	}

	period = (uint32_t)(cycles_per_sec / rate_hz);
	if (period < MIN_PERIOD_CYCLES || (period % 2U) != 0U || period > UINT16_MAX) {
		return -EINVAL;
	}

	/* The DVP is stopped, nothing else writes the ring now */
	for (uint8_t i = 0U; i < BLOCK_COUNT; i++) {
		if (i != data->taken) {
			(void)memset(data->ring[i], FILL_PATTERN, BLOCK_SIZE);
		}
	}

	(void)memset(data->guard, FILL_PATTERN, sizeof(data->guard));

	key = k_spin_lock(&data->lock);

	data->ready_head = 0U;
	data->ready_count = 0U;
	data->free_count = 0U;
	k_sem_reset(&data->ready_sem);

	for (uint8_t i = 0U; i < BLOCK_COUNT; i++) {
		if (i == data->taken) {
			continue;
		}

		if (n < 2U) {
			data->dma_block[n] = i;
			n++;
		} else {
			data->free[data->free_count] = i;
			data->free_count++;
		}
	}

	/* The DVP starts each frame with buffer 0 */
	data->writing = 0U;
	data->sync_wait_us = (SYNC_PCLK_PERIODS * USEC_PER_SEC) / rate_hz + 1U;
	data->stats = (struct dvp_adc_stats){
		.rate = rate_hz,
		.running = true,
	};

	k_spin_unlock(&data->lock, key);

	err = pwm_set_cycles(config->clock.dev, config->clock.channel, period, period / 2U,
			     config->clock.flags);
	if (err != 0) {
		data->stats.running = false;
		return -EIO;
	}

	k_sleep(CLOCK_SETTLE);

	cr0 = RB_DVP_JPEG | RB_DVP_D8_MOD | RB_DVP_V_POLAR;
	if (rate_hz < FALLING_EDGE_MAX_RATE) {
		cr0 |= RB_DVP_P_POLAR;
	}

	/* Continuous mode, every frame, DMA off, receive logic and FIFO out of reset */
	regs->CR1 = 0U;
	regs->CR0 = cr0;
	regs->COL_NUM = BLOCK_SIZE;
	set_dma_addr(regs, 0U, data->ring[data->dma_block[0]]);
	set_dma_addr(regs, 1U, data->ring[data->dma_block[1]]);
	regs->IFR = 0U;
	regs->IER = RB_DVP_IE_ROW_DONE | RB_DVP_IE_FIFO_OV;
	regs->CR1 |= RB_DVP_DMA_EN;
	regs->CR0 |= RB_DVP_ENABLE;

	/* Frame start, then line start: data is stored from the HSYNC edge on */
	err = frame_start(config, data);
	if (err == 0) {
		k_busy_wait(data->sync_wait_us);
		err = gpio_pin_set_dt(&config->hsync, 1);
	}

	if (err != 0) {
		LOG_ERR("DVP did not start a frame");
		(void)dvp_adc_stop(dev);
		return -EIO;
	}

	LOG_DBG("Started at %u S/s, period %u cycles", rate_hz, period);

	return 0;
}

int dvp_adc_stop(const struct device *dev)
{
	const struct dvp_adc_config *config = dev->config;
	struct dvp_adc_data *data = dev->data;
	DVP_TypeDef *regs = config->regs;
	k_spinlock_key_t key;

	if (!data->stats.running) {
		return -EALREADY;
	}

	(void)gpio_pin_set_dt(&config->hsync, 0);
	(void)gpio_pin_set_dt(&config->vsync, 0);
	k_busy_wait(data->sync_wait_us);

	regs->IER = 0U;
	regs->CR0 &= (uint8_t)~RB_DVP_ENABLE;
	regs->CR1 &= (uint8_t)~RB_DVP_DMA_EN;
	regs->CR1 |= RB_DVP_ALL_CLR | RB_DVP_RCV_CLR;

	/* Period 0 drives the clock output inactive (low) */
	(void)pwm_set_cycles(config->clock.dev, config->clock.channel, 0U, 0U,
			     config->clock.flags);

	key = k_spin_lock(&data->lock);

	regs->IFR = 0U;
	data->stats.running = false;
	data->stats.rate = 0U;
	data->ready_count = 0U;
	k_sem_reset(&data->ready_sem);

	k_spin_unlock(&data->lock, key);

	return 0;
}

int dvp_adc_get_block(const struct device *dev, const uint8_t **block_data, size_t *len,
		      k_timeout_t timeout)
{
	struct dvp_adc_data *data = dev->data;
	k_spinlock_key_t key;
	uint8_t block;

	if (data->taken != NO_BLOCK) {
		return -EBUSY;
	}

	if (k_sem_take(&data->ready_sem, timeout) != 0) {
		return -EAGAIN;
	}

	key = k_spin_lock(&data->lock);

	if (data->ready_count == 0U) {
		/* Stopped meanwhile */
		k_spin_unlock(&data->lock, key);
		return -EAGAIN;
	}

	block = ready_pop(data);
	data->taken = block;

	k_spin_unlock(&data->lock, key);

	*block_data = data->ring[block];
	*len = BLOCK_SIZE;

	return 0;
}

void dvp_adc_release_block(const struct device *dev)
{
	struct dvp_adc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	if (data->taken != NO_BLOCK) {
		data->free[data->free_count] = data->taken;
		data->free_count++;
		data->taken = NO_BLOCK;
	}

	k_spin_unlock(&data->lock, key);
}

void dvp_adc_get_stats(const struct device *dev, struct dvp_adc_stats *stats)
{
	struct dvp_adc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	*stats = data->stats;
	k_spin_unlock(&data->lock, key);

	stats->guard_intact = true;
	for (size_t i = 0U; i < sizeof(data->guard); i++) {
		if (data->guard[i] != FILL_PATTERN) {
			stats->guard_intact = false;
			break;
		}
	}
}

static int dvp_adc_init(const struct device *dev)
{
	const struct dvp_adc_config *config = dev->config;
	struct dvp_adc_data *data = dev->data;
	DVP_TypeDef *regs = config->regs;
	int err;

	if (!device_is_ready(config->clk_dev) || !pwm_is_ready_dt(&config->clock) ||
	    !gpio_is_ready_dt(&config->hsync) || !gpio_is_ready_dt(&config->vsync)) {
		return -ENODEV;
	}

	err = clock_control_on(config->clk_dev, (clock_control_subsys_t)(uintptr_t)config->clk_id);
	if (err != 0) {
		return err;
	}

	for (size_t i = 0U; i < config->num_inputs; i++) {
		if (!gpio_is_ready_dt(&config->inputs[i])) {
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&config->inputs[i], GPIO_INPUT);
		if (err != 0) {
			return err;
		}
	}

	err = gpio_pin_configure_dt(&config->hsync, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		return err;
	}

	err = gpio_pin_configure_dt(&config->vsync, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		return err;
	}

	regs->IER = 0U;
	regs->CR0 = 0U;
	regs->CR1 = RB_DVP_ALL_CLR | RB_DVP_RCV_CLR;

	k_sem_init(&data->ready_sem, 0, BLOCK_COUNT);
	data->taken = NO_BLOCK;
	(void)memset(data->guard, FILL_PATTERN, sizeof(data->guard));

	config->irq_config();

	return 0;
}

#define DVP_ADC_INPUT(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx)

#define DVP_ADC_DEFINE(n)                                                                          \
	static const struct gpio_dt_spec dvp_adc_inputs_##n[] = {                                  \
		DT_INST_FOREACH_PROP_ELEM_SEP(n, input_gpios, DVP_ADC_INPUT, (,))};                \
                                                                                                   \
	static void dvp_adc_irq_config_##n(void)                                                   \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), 0, dvp_adc_isr, DEVICE_DT_INST_GET(n), 0);            \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static struct dvp_adc_data dvp_adc_data_##n;                                               \
                                                                                                   \
	static const struct dvp_adc_config dvp_adc_config_##n = {                                  \
		.regs = (DVP_TypeDef *)DT_INST_REG_ADDR(n),                                        \
		.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                  \
		.clk_id = DT_INST_CLOCKS_CELL(n, id),                                              \
		.clock = PWM_DT_SPEC_INST_GET(n),                                                  \
		.hsync = GPIO_DT_SPEC_INST_GET(n, hsync_gpios),                                    \
		.vsync = GPIO_DT_SPEC_INST_GET(n, vsync_gpios),                                    \
		.inputs = dvp_adc_inputs_##n,                                                      \
		.num_inputs = ARRAY_SIZE(dvp_adc_inputs_##n),                                      \
		.irq_config = dvp_adc_irq_config_##n,                                              \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, dvp_adc_init, NULL, &dvp_adc_data_##n, &dvp_adc_config_##n,       \
			      POST_KERNEL, CONFIG_ADC_DVP_WCH_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(DVP_ADC_DEFINE)
