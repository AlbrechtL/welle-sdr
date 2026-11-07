/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <welle_sdr_raw.h>

#include "raw_frame.h"
#include "raw_knock.h"

#define XTAL_HZ   28800000ULL
#define FRAME_LEN 4096U
#define PAYLOAD   (FRAME_LEN - WSDR_RAW_HDR_LEN)

/* Rate the firmware decodes after librtlsdr set_sample_rate() with this crystal */
static uint32_t rate_seen(uint32_t rate, uint32_t rtl_xtal)
{
	uint32_t ratio = (uint32_t)(((double)rtl_xtal * (double)BIT64(22)) / (double)rate);

	ratio &= 0x0ffffffcU;

	return (uint32_t)((XTAL_HZ << 22) / ratio);
}

/* Feed a host sequence, 10 ms apart; return the last result */
static int knock_seq(struct raw_knock *k, const uint32_t *rates, size_t n, uint32_t rtl_xtal)
{
	static uint32_t now_ms = 5000U;
	int code = -1;

	for (size_t i = 0U; i < n; i++) {
		now_ms += 10U;
		code = raw_knock_feed(k, rate_seen(rates[i], rtl_xtal), now_ms);
	}

	return code;
}

ZTEST(raw_knock, test_sequence)
{
	const uint32_t raw[] = {WSDR_RAW_KNOCK_RATE_1, WSDR_RAW_KNOCK_RATE_2,
				WSDR_RAW_KNOCK_RATE_3(WSDR_RAW_KNOCK_CODE(WSDR_RAW_MODE_RAW))};
	const uint32_t iq[] = {WSDR_RAW_KNOCK_RATE_1, WSDR_RAW_KNOCK_RATE_2,
			       WSDR_RAW_KNOCK_RATE_3(WSDR_RAW_KNOCK_CODE(WSDR_RAW_MODE_IQ))};
	struct raw_knock k;

	raw_knock_reset(&k);
	zassert_equal(knock_seq(&k, raw, ARRAY_SIZE(raw), XTAL_HZ), 0x11);
	zassert_equal(knock_seq(&k, iq, ARRAY_SIZE(iq), XTAL_HZ), 0x10);
	/* A completed sequence does not repeat without a new one */
	zassert_equal(knock_seq(&k, &raw[2], 1U, XTAL_HZ), -1);
}

ZTEST(raw_knock, test_crystal_range)
{
	const uint32_t raw[] = {WSDR_RAW_KNOCK_RATE_1, WSDR_RAW_KNOCK_RATE_2,
				WSDR_RAW_KNOCK_RATE_3(WSDR_RAW_KNOCK_CODE(WSDR_RAW_MODE_RAW))};
	struct raw_knock k;

	/* librtlsdr accepts rtl_xtal within +/-1000 Hz */
	raw_knock_reset(&k);
	zassert_equal(knock_seq(&k, raw, ARRAY_SIZE(raw), XTAL_HZ - 1000U), 0x11);
	zassert_equal(knock_seq(&k, raw, ARRAY_SIZE(raw), XTAL_HZ + 1000U), 0x11);
}

ZTEST(raw_knock, test_tolerance)
{
	const uint32_t k3 = WSDR_RAW_KNOCK_RATE_3(0x11U);
	struct raw_knock k;

	raw_knock_reset(&k);
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_1 + WSDR_RAW_KNOCK_TOL_HZ, 0U), -1);
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_2 - WSDR_RAW_KNOCK_TOL_HZ, 1U), -1);
	zassert_equal(raw_knock_feed(&k, k3 + WSDR_RAW_KNOCK_TOL_HZ, 2U), 0x11);

	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_1 + WSDR_RAW_KNOCK_TOL_HZ + 1U, 3U),
		      -1);
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_2, 4U), -1);
	zassert_equal(raw_knock_feed(&k, k3, 5U), -1);

	/* Off the code grid */
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_1, 6U), -1);
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_2, 7U), -1);
	zassert_equal(raw_knock_feed(&k, k3 + WSDR_RAW_KNOCK_RATE_3_STEP / 2U, 8U), -1);
}

ZTEST(raw_knock, test_timeout)
{
	struct raw_knock k;

	raw_knock_reset(&k);
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_1, 100U), -1);
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_2, 100U + WSDR_RAW_KNOCK_WINDOW_MS),
		      -1);
	zassert_equal(raw_knock_feed(&k, WSDR_RAW_KNOCK_RATE_3(0x11U),
				     101U + 2U * WSDR_RAW_KNOCK_WINDOW_MS),
		      -1);
}

ZTEST(raw_knock, test_order)
{
	const uint32_t wrong[] = {WSDR_RAW_KNOCK_RATE_2, WSDR_RAW_KNOCK_RATE_1,
				  WSDR_RAW_KNOCK_RATE_3(0x11U)};
	const uint32_t restart[] = {WSDR_RAW_KNOCK_RATE_1, WSDR_RAW_KNOCK_RATE_2,
				    WSDR_RAW_KNOCK_RATE_1, WSDR_RAW_KNOCK_RATE_2,
				    WSDR_RAW_KNOCK_RATE_3(0x11U)};
	const uint32_t broken[] = {WSDR_RAW_KNOCK_RATE_1, WSDR_RAW_KNOCK_RATE_2, 2048000U,
				   WSDR_RAW_KNOCK_RATE_3(0x11U)};
	struct raw_knock k;

	raw_knock_reset(&k);
	zassert_equal(knock_seq(&k, wrong, ARRAY_SIZE(wrong), XTAL_HZ), -1);
	zassert_equal(knock_seq(&k, restart, ARRAY_SIZE(restart), XTAL_HZ), 0x11);
	zassert_equal(knock_seq(&k, broken, ARRAY_SIZE(broken), XTAL_HZ), -1);
}

ZTEST(raw_knock, test_common_rates)
{
	const uint32_t common[] = {250000U,  900001U,  1024000U, 1200000U, 1400000U,
				   1536000U, 1800000U, 1920000U, 2000000U, 2048000U,
				   2400000U, 2560000U, 2880000U, 3000000U, 3200000U};
	struct raw_knock k;

	raw_knock_reset(&k);

	/* Every valid rate on a 1 kHz grid, twice, never completes a knock */
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t rate = 225001U; rate <= 3200000U; rate += 1000U) {
			if (rate > 300000U && rate <= 900000U) {
				continue;
			}

			zassert_equal(raw_knock_feed(&k, rate_seen(rate, XTAL_HZ), 0U), -1,
				      "rate %u", rate);
		}
	}

	for (size_t i = 0U; i < ARRAY_SIZE(common); i++) {
		for (size_t j = 0U; j < ARRAY_SIZE(common); j++) {
			const uint32_t seq[] = {common[i], common[j], common[i]};

			zassert_equal(knock_seq(&k, seq, ARRAY_SIZE(seq), XTAL_HZ), -1);
		}
	}
}

ZTEST_SUITE(raw_knock, NULL, NULL, NULL, NULL, NULL);

static uint8_t input[3U * FRAME_LEN];
static uint8_t output[4U * FRAME_LEN];

static void fill_input(void)
{
	for (size_t i = 0U; i < sizeof(input); i++) {
		input[i] = (uint8_t)(i * 7U + 3U);
	}
}

static void check_hdr(const uint8_t *h, uint32_t idx, const struct raw_frame_params *params)
{
	zassert_mem_equal(h, WSDR_RAW_MAGIC, WSDR_RAW_MAGIC_LEN);
	zassert_equal(h[WSDR_RAW_OFF_VERSION], WSDR_RAW_VERSION);
	zassert_equal(h[WSDR_RAW_OFF_HDR_LEN], WSDR_RAW_HDR_LEN);
	zassert_equal(sys_get_le16(&h[WSDR_RAW_OFF_FLAGS]), params->flags);
	zassert_equal(sys_get_le32(&h[WSDR_RAW_OFF_SAMPLE_IDX]), idx, "idx %u != %u",
		      sys_get_le32(&h[WSDR_RAW_OFF_SAMPLE_IDX]), idx);
	zassert_equal(sys_get_le32(&h[WSDR_RAW_OFF_ADC_RATE]), params->adc_rate);
	zassert_equal((int32_t)sys_get_le32(&h[WSDR_RAW_OFF_IF]), params->if_hz);
}

ZTEST(raw_frame, test_header)
{
	const struct raw_frame_params params = {
		.adc_rate = 4500000U, .if_hz = -1625000, .flags = WSDR_RAW_FLAG_INVERTED};
	struct raw_frame f;
	size_t used;
	size_t n;

	fill_input();
	raw_frame_init(&f, FRAME_LEN);
	raw_frame_set_params(&f, &params);

	n = raw_frame_fill(&f, input, PAYLOAD + 10U, output, FRAME_LEN, &used);
	zassert_equal(n, FRAME_LEN);
	zassert_equal(used, PAYLOAD);
	check_hdr(output, 0U, &params);
	zassert_mem_equal(&output[WSDR_RAW_HDR_LEN], input, PAYLOAD);

	n = raw_frame_fill(&f, &input[used], 10U, output, FRAME_LEN, &used);
	zassert_equal(n, WSDR_RAW_HDR_LEN + 10U);
	zassert_equal(used, 10U);
	check_hdr(output, PAYLOAD, &params);
	zassert_mem_equal(&output[WSDR_RAW_HDR_LEN], &input[PAYLOAD], 10U);
}

/* Frame the input in pieces of the given lengths into frame-sized buffers */
static size_t frame_pieces(struct raw_frame *f, const size_t *pieces, size_t count,
			   size_t *frames)
{
	size_t in_pos = 0U;
	size_t out_len = 0U;
	size_t buf_used = 0U;

	*frames = 0U;

	for (size_t i = 0U; in_pos < sizeof(input); i++) {
		size_t len = MIN(pieces[i % count], sizeof(input) - in_pos);
		size_t pos = 0U;

		while (pos < len) {
			size_t used;
			size_t n;

			if (buf_used == FRAME_LEN) {
				buf_used = 0U;
			}

			if (buf_used == 0U) {
				(*frames)++;
			}

			n = raw_frame_fill(f, &input[in_pos + pos], len - pos,
					   &output[out_len], FRAME_LEN - buf_used, &used);
			out_len += n;
			buf_used += n;
			pos += used;
		}

		in_pos += len;
	}

	return out_len;
}

ZTEST(raw_frame, test_pieces)
{
	const size_t pieces[] = {7U, 2048U, 333U, 1U, 4096U};
	const struct raw_frame_params params = {.adc_rate = 4000000U, .if_hz = 1700000};
	struct raw_frame f;
	size_t frames;
	size_t out_len;
	size_t in_pos = 0U;

	fill_input();
	raw_frame_init(&f, FRAME_LEN);
	raw_frame_set_params(&f, &params);

	out_len = frame_pieces(&f, pieces, ARRAY_SIZE(pieces), &frames);
	zassert_equal(frames, DIV_ROUND_UP(sizeof(input), PAYLOAD));
	zassert_equal(out_len, sizeof(input) + frames * WSDR_RAW_HDR_LEN);

	for (size_t i = 0U; i < frames; i++) {
		const uint8_t *frame = &output[i * FRAME_LEN];
		const size_t len = MIN(PAYLOAD, sizeof(input) - in_pos);

		check_hdr(frame, (uint32_t)in_pos, &params);
		zassert_mem_equal(&frame[WSDR_RAW_HDR_LEN], &input[in_pos], len);
		in_pos += len;
	}
}

ZTEST(raw_frame, test_index_wrap)
{
	const struct raw_frame_params params = {.adc_rate = 4500000U};
	struct raw_frame f;
	size_t used;

	fill_input();
	raw_frame_init(&f, FRAME_LEN);
	raw_frame_set_params(&f, &params);
	raw_frame_skip(&f, UINT32_MAX - 99U);

	(void)raw_frame_fill(&f, input, PAYLOAD, output, FRAME_LEN, &used);
	check_hdr(output, UINT32_MAX - 99U, &params);
	(void)raw_frame_fill(&f, input, PAYLOAD, output, FRAME_LEN, &used);
	check_hdr(output, (uint32_t)(PAYLOAD - 100U), &params);
}

ZTEST(raw_frame, test_params_next_frame)
{
	const struct raw_frame_params old = {.adc_rate = 4500000U, .if_hz = 1000};
	const struct raw_frame_params next = {
		.adc_rate = 4000000U, .if_hz = -2000, .flags = WSDR_RAW_FLAG_ALIASED};
	struct raw_frame f;
	size_t used;

	fill_input();
	raw_frame_init(&f, FRAME_LEN);
	raw_frame_set_params(&f, &old);

	(void)raw_frame_fill(&f, input, 100U, output, FRAME_LEN, &used);
	raw_frame_set_params(&f, &next);
	(void)raw_frame_fill(&f, input, PAYLOAD, &output[WSDR_RAW_HDR_LEN + 100U],
			     FRAME_LEN - WSDR_RAW_HDR_LEN - 100U, &used);
	check_hdr(output, 0U, &old);

	(void)raw_frame_fill(&f, input, 10U, output, FRAME_LEN, &used);
	check_hdr(output, PAYLOAD, &next);
}

ZTEST(raw_frame, test_restart_and_skip)
{
	const struct raw_frame_params params = {.adc_rate = 4500000U};
	struct raw_frame f;
	size_t used;

	fill_input();
	raw_frame_init(&f, FRAME_LEN);
	raw_frame_set_params(&f, &params);

	/* 100 samples written into an abandoned frame are missing for the host */
	(void)raw_frame_fill(&f, input, 100U, output, FRAME_LEN, &used);
	raw_frame_restart(&f);
	(void)raw_frame_fill(&f, input, 10U, output, FRAME_LEN, &used);
	check_hdr(output, 100U, &params);

	/* Dropped samples at a frame start */
	raw_frame_restart(&f);
	raw_frame_skip(&f, 2048U);
	(void)raw_frame_fill(&f, input, 10U, output, FRAME_LEN, &used);
	check_hdr(output, 110U + 2048U, &params);
}

ZTEST_SUITE(raw_frame, NULL, NULL, NULL, NULL, NULL);

ZTEST(raw_plan, test_dab_capped)
{
	struct raw_frame_plan plan;

	/* librtlsdr's R820T IF for 2.048 MS/s is 1.625 MHz */
	zassert_ok(raw_frame_plan(2048000U, 1625000, 4500000U, &plan));
	zassert_equal(plan.period, 32U);
	zassert_equal(plan.adc_rate, 4500000U);
	zassert_true(plan.capped);
	zassert_true(plan.aliased);

	zassert_ok(raw_frame_plan(2048000U, -1625000, 4500000U, &plan));
	zassert_equal(plan.adc_rate, 4500000U);
	zassert_true(plan.aliased);
}

ZTEST(raw_plan, test_dab_uncapped)
{
	struct raw_frame_plan plan;

	/* 2.2 * (1.625 + 1.024) MHz = 5.83 MS/s, next exact rate 6 MS/s */
	zassert_ok(raw_frame_plan(2048000U, 1625000, 18000000U, &plan));
	zassert_equal(plan.period, 24U);
	zassert_equal(plan.adc_rate, 6000000U);
	zassert_false(plan.capped);
	zassert_false(plan.aliased);
}

ZTEST(raw_plan, test_fm_band)
{
	struct raw_frame_plan plan;

	/* librtlsdr's R820T IF for 1.024 MS/s is 1.7 MHz: band top 2.212 MHz */
	zassert_ok(raw_frame_plan(1024000U, 1700000, 4500000U, &plan));
	zassert_equal(plan.adc_rate, 4500000U);
	zassert_true(plan.capped);
	zassert_false(plan.aliased);
}

ZTEST(raw_plan, test_low_rate)
{
	struct raw_frame_plan plan;

	/* Band top 125 kHz: need 275 kS/s, exact 144 MHz / 512 */
	zassert_ok(raw_frame_plan(250000U, 0, 4500000U, &plan));
	zassert_equal(plan.period, 512U);
	zassert_equal(plan.adc_rate, 281250U);
	zassert_equal(plan.need_rate, 275000U);
	zassert_false(plan.capped);
	zassert_false(plan.aliased);
}

ZTEST(raw_plan, test_invalid)
{
	struct raw_frame_plan plan;

	zassert_equal(raw_frame_plan(0U, 0, 4500000U, &plan), -EINVAL);
	zassert_equal(raw_frame_plan(2048000U, 0, 0U, &plan), -EINVAL);
}

ZTEST_SUITE(raw_plan, NULL, NULL, NULL, NULL, NULL);
