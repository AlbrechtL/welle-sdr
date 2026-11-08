/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Status display on an SSD1306 128x64 OLED on the tuner bus. The screen is a
 * grid of 21 x 8 text cells of 6 x 8 pixels, the first two rows partly
 * replaced by the center frequency in 12 x 16 cells. Only cells that changed
 * are sent: the bus is bit-banged and shared with the tuner, so each write is
 * short and takes the bus with r820t2_bus_acquire().
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <dvp_adc.h>

#include "cpu_load.h"
#include "font5x7.h"
#include "oled.h"
#include "r820t2.h"
#include "rtl2832u.h"
#include "sample_src.h"
#include "status_led.h"

LOG_MODULE_REGISTER(oled, LOG_LEVEL_INF);

#define OLED_NODE DT_NODELABEL(oled)

#define OLED_WIDTH  DT_PROP(OLED_NODE, width)
#define OLED_HEIGHT DT_PROP(OLED_NODE, height)

/* Text cells: 5 x 7 glyph plus one blank column, one display page high */
#define OLED_CELL_W 6U
#define OLED_PAGE_H 8U
#define OLED_COLS   (OLED_WIDTH / OLED_CELL_W)
#define OLED_ROWS   (OLED_HEIGHT / OLED_PAGE_H)

/* Center frequency: glyphs at twice the size, on rows 0 and 1 */
#define OLED_BIG_W     (2U * OLED_CELL_W)
#define OLED_BIG_H     (2U * OLED_PAGE_H)
#define OLED_BIG_CELLS 9U
/* First text column right of the big cells on rows 0 and 1 */
#define OLED_BIG_END_COL (OLED_BIG_CELLS * OLED_BIG_W / OLED_CELL_W)
#define OLED_BIG_ROWS    (OLED_BIG_H / OLED_PAGE_H)

/* Columns right of the last text cell, cleared once */
#define OLED_EDGE_X (OLED_COLS * OLED_CELL_W)
#define OLED_EDGE_W (OLED_WIDTH - OLED_EDGE_X)

/*
 * Pixel bytes per write, about 3 ms on the 100 kHz bus with the commands.
 * Host tuner requests wait that long at most.
 */
#define OLED_RUN_BYTES (OLED_BIG_W * OLED_BIG_ROWS)
#define OLED_RUN_CELLS (OLED_RUN_BYTES / OLED_CELL_W)

/* Window for the ADC loss: at least 0.1 s, at most three update intervals */
#define OLED_ADC_MIN_SPAN (CPU_LOAD_CLOCK_HZ / 10U)
#define OLED_ADC_MAX_SPAN (3U * CPU_LOAD_CLOCK_HZ * CONFIG_WELLE_SDR_OLED_PERIOD_MS / 1000U)
/*
 * Loss that shows the overload warning. Streams within the CPU budget lose
 * about 2.5 % to interrupt latency (README "Status display"), up to 8 % in
 * the first window; streams above it lose 40 % and more.
 */
#define OLED_OVERLOAD_PERMILLE 100U

#define OLED_STACK_SIZE 1024
/* Below the producer and all other threads: the display only gets idle CPU */
#define OLED_PRIORITY   K_LOWEST_APPLICATION_THREAD_PRIO

BUILD_ASSERT(OLED_WIDTH == 128 && OLED_HEIGHT == 64, "Layout is for a 128x64 display");
BUILD_ASSERT(OLED_EDGE_W * OLED_HEIGHT / OLED_PAGE_H <= OLED_RUN_BYTES,
	     "Edge columns must fit one write");

/* Screen content as characters; NUL marks a cell whose content is unknown */
struct oled_screen {
	char big[OLED_BIG_CELLS];
	char text[OLED_ROWS][OLED_COLS];
	/* Row drawn light on dark: the panel is monochrome, this is the highlight */
	bool inverse[OLED_ROWS];
};

static const struct device *const oled_dev = DEVICE_DT_GET(OLED_NODE);
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc_dvp));

/* Write pixel bytes (display pages, top to bottom) with the bus taken */
static int oled_write(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
		      const uint8_t *buf)
{
	const struct display_buffer_descriptor desc = {
		.buf_size = (uint32_t)width * height / OLED_PAGE_H,
		.width = width,
		.height = height,
		.pitch = width,
	};
	int err;

	err = r820t2_bus_acquire();
	if (err != 0) {
		return err;
	}

	err = display_write(oled_dev, x, y, &desc, buf);
	r820t2_bus_release();

	return err;
}

/* Scale a glyph column by two: 7 rows become 14, split into two pages */
static void big_column(uint8_t col, uint8_t *top, uint8_t *bottom)
{
	uint16_t out = 0U;

	for (uint8_t bit = 0U; bit < FONT5X7_HEIGHT; bit++) {
		if ((col & BIT(bit)) != 0U) {
			out |= (uint16_t)(3U << (2U * bit));
		}
	}

	*top = (uint8_t)out;
	*bottom = (uint8_t)(out >> 8);
}

static int draw_big(uint8_t cell, char c)
{
	uint8_t buf[OLED_RUN_BYTES] = {0};
	const uint8_t *glyph = font5x7_glyph(c);

	for (uint8_t i = 0U; i < FONT5X7_WIDTH; i++) {
		uint8_t top;
		uint8_t bottom;

		big_column(glyph[i], &top, &bottom);
		buf[2U * i] = top;
		buf[2U * i + 1U] = top;
		buf[OLED_BIG_W + 2U * i] = bottom;
		buf[OLED_BIG_W + 2U * i + 1U] = bottom;
	}

	return oled_write(cell * OLED_BIG_W, 0U, OLED_BIG_W, OLED_BIG_H, buf);
}

static int draw_text(uint8_t row, uint8_t col, const char *str, uint8_t len, bool inverse)
{
	uint8_t buf[OLED_RUN_BYTES] = {0};

	for (uint8_t i = 0U; i < len; i++) {
		memcpy(&buf[i * OLED_CELL_W], font5x7_glyph(str[i]), FONT5X7_WIDTH);
	}

	if (inverse) {
		for (size_t i = 0U; i < (size_t)len * OLED_CELL_W; i++) {
			buf[i] = (uint8_t)~buf[i];
		}
	}

	return oled_write(col * OLED_CELL_W, row * OLED_PAGE_H, len * OLED_CELL_W, OLED_PAGE_H,
			  buf);
}

/* Send the cells that differ from what the display shows */
static int oled_update(const struct oled_screen *next, struct oled_screen *shown)
{
	int err;

	for (uint8_t cell = 0U; cell < OLED_BIG_CELLS; cell++) {
		if (next->big[cell] == shown->big[cell]) {
			continue;
		}

		err = draw_big(cell, next->big[cell]);
		if (err != 0) {
			return err;
		}

		shown->big[cell] = next->big[cell];
	}

	for (uint8_t row = 0U; row < OLED_ROWS; row++) {
		uint8_t col = (row < OLED_BIG_ROWS) ? OLED_BIG_END_COL : 0U;

		if (next->inverse[row] != shown->inverse[row]) {
			/* Every cell of the row changes */
			memset(shown->text[row], 0, OLED_COLS);
			shown->inverse[row] = next->inverse[row];
		}

		while (col < OLED_COLS) {
			uint8_t len = 0U;

			while (col + len < OLED_COLS && len < OLED_RUN_CELLS &&
			       next->text[row][col + len] != shown->text[row][col + len]) {
				len++;
			}

			if (len == 0U) {
				col++;
				continue;
			}

			err = draw_text(row, col, &next->text[row][col], len, next->inverse[row]);
			if (err != 0) {
				return err;
			}

			memcpy(&shown->text[row][col], &next->text[row][col], len);
			col += len;
		}
	}

	return 0;
}

/* Format one text row, padded with blanks */
static void set_row(struct oled_screen *s, uint8_t row, const char *fmt, ...)
{
	char line[OLED_COLS + 1U];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	len = CLAMP(len, 0, (int)OLED_COLS);
	memset(s->text[row], ' ', OLED_COLS);
	memcpy(s->text[row], line, (size_t)len);
}

/* Rate with SI prefix and up to 3 decimals: 2.048M, 250k */
static void fmt_rate(char *buf, size_t size, uint32_t rate)
{
	uint32_t scale;
	uint32_t frac;
	int digits = 3;
	char prefix;

	if (rate >= 1000000U) {
		scale = 1000000U;
		prefix = 'M';
	} else if (rate >= 1000U) {
		scale = 1000U;
		prefix = 'k';
	} else {
		(void)snprintf(buf, size, "%u", rate);
		return;
	}

	/* Truncated to 3 decimals, trailing zeros dropped */
	frac = (rate % scale) / (scale / 1000U);
	while (frac != 0U && (frac % 10U) == 0U) {
		frac /= 10U;
		digits--;
	}

	if (frac == 0U) {
		(void)snprintf(buf, size, "%u%c", rate / scale, prefix);
	} else {
		(void)snprintf(buf, size, "%u.%0*u%c", rate / scale, digits, frac, prefix);
	}
}

/* Rate in M with at most one decimal, rounded: 10.3M, 8M, 0.3M */
static void fmt_mega(char *buf, size_t size, uint32_t rate)
{
	const uint32_t tenths = (rate + 50000U) / 100000U;

	if ((tenths % 10U) == 0U) {
		(void)snprintf(buf, size, "%uM", tenths / 10U);
	} else {
		(void)snprintf(buf, size, "%u.%uM", tenths / 10U, tenths % 10U);
	}
}

/* Bytes in RAM from the linker: data, bss, noinit and the stacks */
static size_t ram_used(void)
{
	return (size_t)(_image_ram_end - _image_ram_start);
}

/* Bytes in flash from the linker: code, read-only data and the data load image */
static size_t flash_used(void)
{
	return (size_t)(__rom_region_end - __rom_region_start);
}

static uint32_t mem_pct(size_t used, uint32_t size_kb)
{
	return (uint32_t)((used * 100U + size_kb * 512U) / (size_kb * 1024U));
}

static void fill_tuner(struct oled_screen *s, const struct rtl2832u_status *st)
{
	char line[OLED_BIG_CELLS + 1U];
	struct r820t2_fields fields;
	int32_t gain;
	char gain_str[16];

	if (st->lo_freq == 0U) {
		(void)snprintf(line, sizeof(line), "---");
	} else {
		/* librtlsdr tunes within about 1 kHz of the request: kHz resolution */
		uint32_t khz = (st->center_freq + 500U) / 1000U;

		(void)snprintf(line, sizeof(line), "%u.%03u", khz / 1000U, khz % 1000U);
		memcpy(&s->text[1][OLED_COLS - 3U], "MHz", 3U);
	}

	memset(s->big, ' ', sizeof(s->big));
	memcpy(s->big, line, strlen(line));

	if (r820t2_manual_gain(&gain)) {
		(void)snprintf(gain_str, sizeof(gain_str), "%d.%ddB", gain / 10, gain % 10);
	} else {
		(void)snprintf(gain_str, sizeof(gain_str), "auto");
	}

	r820t2_decode(r820t2_regs(), &fields);
	set_row(s, 4, "Gain %-7s PLL %s", gain_str, fields.pll_locked ? "ok" : "--");
}

/*
 * Share of the ADC samples lost since the last call, in 0.1 %: samples the
 * DVP never delivered (block interrupts that came late and merged, see the
 * README) and blocks the consumer did not take in time (overruns). Expected
 * is the ADC rate over the time on the CPU load clock, which keeps running
 * while the kernel clock is held back.
 *
 * Returns false when there is no valid window: capture stopped, restarted
 * or rate changed, or the last call too long ago.
 */
static bool adc_lost_permille(uint32_t *permille)
{
	static struct {
		bool valid;
		uint32_t rate;
		uint32_t blocks;
		uint32_t overruns;
		uint32_t clock;
	} last;
	struct dvp_adc_stats st;
	const uint32_t now = cpu_load_clock();
	const uint32_t span = now - last.clock;
	bool ok = false;

	dvp_adc_get_stats(adc_dev, &st);

	if (st.running && last.valid && st.rate == last.rate && st.blocks >= last.blocks &&
	    span >= OLED_ADC_MIN_SPAN && span <= OLED_ADC_MAX_SPAN) {
		const uint64_t expected = (uint64_t)st.rate * span / CPU_LOAD_CLOCK_HZ;
		const uint64_t kept = (uint64_t)((st.blocks - last.blocks) -
						 MIN(st.overruns - last.overruns,
						     st.blocks - last.blocks)) *
				      CONFIG_ADC_DVP_WCH_BLOCK_SIZE;

		if (expected > 0U) {
			*permille = (kept >= expected)
					    ? 0U
					    : (uint32_t)((expected - kept) * 1000U / expected);
			ok = true;
		}
	}

	last.valid = st.running && now != 0U;
	last.rate = st.rate;
	last.blocks = st.blocks;
	last.overruns = st.overruns;
	last.clock = now;

	return ok;
}

/* Returns true when the ADC loses so many samples that the CPU is overloaded */
static bool fill_stream(struct oled_screen *s, const struct rtl2832u_status *st)
{
	struct sample_src_stats src;
	char line[40];
	uint32_t lost;
	char rate[12];
	const char *mode;
	bool valid;
	int len;

	sample_src_get_stats(&src);

	if (st->test_mode) {
		mode = "TEST";
	} else if (src.mode == SAMPLE_SRC_MODE_RAW) {
		mode = "RAW";
	} else {
		mode = "IQ";
	}

	fmt_rate(rate, sizeof(rate), st->sample_rate);
	set_row(s, 2, "%-4s %sS/s %s", mode, rate, st->streaming ? "run" : "stop");

	valid = adc_lost_permille(&lost);
	if (!src.running) {
		return false;
	}

	/* Down-converter output gain; raw mode has none */
	if (src.mode == SAMPLE_SRC_MODE_IQ && src.planned) {
		if (src.gain_on) {
			set_row(s, 5, "DDC gain +%d.%ddB", src.gain_db10 / 10, src.gain_db10 % 10);
		} else {
			set_row(s, 5, "DDC gain off");
		}
	}

	fmt_mega(rate, sizeof(rate), src.adc_rate);
	if (!valid) {
		/* First window after a start or a rate change */
		set_row(s, 3, "ADC %sS/s", rate);
		return false;
	}

	/* One decimal while it fits the row */
	len = snprintf(line, sizeof(line), "ADC %sS/s lost %u.%u%%", rate, lost / 10U, lost % 10U);
	if (len < 0 || len > (int)OLED_COLS) {
		(void)snprintf(line, sizeof(line), "ADC %sS/s lost %u%%", rate, (lost + 5U) / 10U);
	}

	set_row(s, 3, "%s", line);

	/* The producer does not keep up with the ADC */
	return valid && lost >= OLED_OVERLOAD_PERMILLE;
}

static void oled_fill(struct oled_screen *s)
{
	struct rtl2832u_status st;
	/* As the LED: an unplug reads as a suspend, the device stays configured */
	const bool usb_up = status_led_usb_up();
	bool host_open;
	bool overloaded = false;

	memset(s->big, ' ', sizeof(s->big));
	memset(s->text, ' ', sizeof(s->text));
	memset(s->inverse, 0, sizeof(s->inverse));

	rtl2832u_get_status(&st);
	/* Without the cable the host never closes the device: do not show it as open */
	host_open = usb_up && st.host_open;

	if (host_open) {
		fill_tuner(s, &st);
		overloaded = fill_stream(s, &st);
	} else {
		memcpy(s->big, "welle-sdr", 9U);
	}

	/* RAM and flash are fixed at build time: there is no heap */
	set_row(s, 6, "CPU%u%% RAM%u%% FL%u%%", cpu_load_pct(), mem_pct(ram_used(), CONFIG_SRAM_SIZE),
		mem_pct(flash_used(), CONFIG_FLASH_SIZE));

	if (overloaded) {
		/* Over the USB state: USB is up while samples stream */
		set_row(s, 7, "   CPU OVERLOADED");
		s->inverse[7] = true;
	} else if (!usb_up) {
		set_row(s, 7, "USB not connected");
	} else {
		/* running: a host program has the device open */
		set_row(s, 7, "USB ok, %s", host_open ? "running" : "connected");
	}
}

static int oled_setup(void)
{
	static const uint8_t blank[OLED_RUN_BYTES];
	int err;

	err = r820t2_bus_acquire();
	if (err != 0) {
		return err;
	}

	/* Deferred in the devicetree: its init talks on the tuner bus */
	err = device_init(oled_dev);
	r820t2_bus_release();
	if (err != 0) {
		return err;
	}

	/* The text cells cover all but these columns, which are never written again */
	return oled_write(OLED_EDGE_X, 0U, OLED_EDGE_W, OLED_HEIGHT, blank);
}

static void oled_thread(void *p1, void *p2, void *p3)
{
	/* All cells unknown: the first update draws the whole screen */
	static struct oled_screen shown;
	static struct oled_screen next;
	bool failing = false;
	int err;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	err = oled_setup();
	if (err != 0) {
		LOG_WRN("Display not available (%d)", err);
		return;
	}

	LOG_INF("Display on");

	while (true) {
		oled_fill(&next);

		err = oled_update(&next, &shown);
		if (err != 0 && !failing) {
			LOG_WRN("Display write failed (%d)", err);
		} else if (err == 0 && failing) {
			LOG_INF("Display writes ok again");
		}
		failing = (err != 0);

		k_sleep(K_MSEC(CONFIG_WELLE_SDR_OLED_PERIOD_MS));
	}
}

K_THREAD_DEFINE(oled_tid, OLED_STACK_SIZE, oled_thread, NULL, NULL, NULL, OLED_PRIORITY, 0,
		SYS_FOREVER_MS);

void oled_start(void)
{
	k_thread_start(oled_tid);
}
