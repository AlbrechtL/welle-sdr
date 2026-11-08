/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Shell commands to inspect the emulator state, test the R820T2 tuner and the ADC */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include <dvp_adc.h>

#include "cpu_load.h"
#include "if_agc.h"
#include "if_agc_ctl.h"
#include "r820t2.h"
#include "rtl2832u.h"
#include "sample_src.h"
#include "si_fmt.h"
#include "testsig.h"

/* Nominal VCO fine tune value, register 4 bits 5:4 */
#define TUNER_VCO_FINE_NOMINAL 2U

/* Largest IF AGC target in codes, half the ADC range */
#define AGC_TARGET_MAX 127UL

static void print_if_agc_line(const struct shell *sh)
{
	struct if_agc_stats agc;
	int32_t gain;
	int32_t mag;

	if_agc_get_stats(&agc);

	if (!agc.pwm_on) {
		shell_print(sh, "IF AGC:       PWM off, PA15 at 0 V, VGA gain from its register");
		return;
	}

	gain = if_agc_ctl_gain(agc.counts);
	/* Sign printed separately, so -0.5 dB keeps it */
	mag = (gain < 0) ? -gain : gain;

	shell_print(sh, "IF AGC:       %s, %u mV (VGA ~%s%d.%d dB)", agc.active ? "loop" : "fixed",
		    if_agc_ctl_counts_to_mv(agc.counts), (gain < 0) ? "-" : "+", mag / 10,
		    mag % 10);
}

static void print_raw(const struct shell *sh, const struct sample_src_stats *raw)
{
	char buf[SI_FMT_LEN];
	char need[SI_FMT_LEN];

	shell_print(sh, "Raw:          %s, IF %d Hz%s", raw->running ? "running" : "stopped",
		    raw->if_freq, raw->invert ? ", inverted" : "");

	if (!raw->planned) {
		shell_print(sh, "ADC rate:     no plan");
		return;
	}

	shell_print(sh, "ADC rate:     %s (band needs %s%s)%s",
		    si_fmt(buf, sizeof(buf), raw->adc_rate, "S/s"),
		    si_fmt(need, sizeof(need), raw->raw_need_rate, "S/s"),
		    raw->raw_capped ? ", capped" : "", raw->raw_aliased ? ", aliased" : "");
	shell_print(sh, "Raw load:     %u %%", raw->load_pct);
	shell_print(sh, "Raw output:   %u transfers, %u bytes dropped, %u lease ends", raw->xfers,
		    raw->dropped, raw->raw_lease_ends);
}

static void print_ddc(const struct shell *sh)
{
	struct sample_src_stats ddc;
	char buf[SI_FMT_LEN];

	sample_src_get_stats(&ddc);

	if (ddc.mode == SAMPLE_SRC_MODE_RAW) {
		print_raw(sh, &ddc);
		return;
	}

	if (!ddc.planned) {
		shell_print(sh, "DDC:          %s, no plan", ddc.running ? "running" : "stopped");
		return;
	}

	shell_print(sh, "DDC:          %s, IF %d Hz%s", ddc.running ? "running" : "stopped",
		    ddc.if_freq, ddc.invert ? ", inverted" : "");
	shell_print(sh, "ADC rate:     %s, decimation %u, residual %d Hz",
		    si_fmt(buf, sizeof(buf), ddc.adc_rate, "S/s"), ddc.decim, ddc.residual_hz);

	if (ddc.exact) {
		shell_print(sh, "Output rate:  %s (exact)",
			    si_fmt(buf, sizeof(buf), ddc.actual_rate, "S/s"));
	} else {
		shell_print(sh, "Output rate:  %s (%d ppm off the host rate)",
			    si_fmt(buf, sizeof(buf), ddc.actual_rate, "S/s"), ddc.ppm_error);
	}

	shell_print(sh, "DDC load:     %u %% (estimated %u %%)", ddc.load_pct, ddc.est_load_pct);
	shell_print(sh, "DDC output:   %u transfers, %u bytes dropped", ddc.xfers, ddc.dropped);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rtl2832u_status status;
	struct sample_src_stats src;
	char buf[SI_FMT_LEN];
	int32_t gain;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rtl2832u_get_status(&status);

	shell_print(sh, "USB:          %s, %s", status.configured ? "configured" : "not configured",
		    status.streaming ? "streaming" : "idle");
	shell_print(sh, "Host driver:  %s", status.host_open ? "open" : "closed");
	sample_src_get_stats(&src);
	shell_print(sh, "Output:       %s",
		    status.test_mode                    ? "test counter"
		    : (src.mode == SAMPLE_SRC_MODE_RAW) ? "raw ADC samples, framed"
							: "ADC I/Q");
	shell_print(sh, "Sample rate:  %s", si_fmt(buf, sizeof(buf), status.sample_rate, "S/s"));
	shell_print(sh, "Center freq:  %s", si_fmt(buf, sizeof(buf), status.center_freq, "Hz"));
	shell_print(sh, "Tuner LO:     %s", si_fmt(buf, sizeof(buf), status.lo_freq, "Hz"));
	shell_print(sh, "IF:           %s", si_fmt(buf, sizeof(buf), status.if_freq, "Hz"));

	if (r820t2_manual_gain(&gain)) {
		shell_print(sh, "Tuner gain:   %d.%d dB", gain / 10, gain % 10);
	} else {
		shell_print(sh, "Tuner gain:   auto");
	}

	print_if_agc_line(sh);
	shell_print(sh, "Streamed:     %u transfers, %llu bytes", status.xfers,
		    (unsigned long long)status.xfers * CONFIG_WELLE_SDR_XFER_SIZE);

	print_ddc(sh);
	shell_print(sh, "CPU load:     %u %%", cpu_load_pct());

	return 0;
}

static void print_gain(const struct shell *sh, const char *name, bool manual, uint8_t idx,
		       int32_t gain_tenth_db)
{
	if (manual) {
		/* The gain step sums are never negative */
		shell_print(sh, "%-13s manual, index %u: %d.%d dB", name, idx, gain_tenth_db / 10,
			    gain_tenth_db % 10);
	} else {
		shell_print(sh, "%-13s auto (AGC)", name);
	}
}

static const char *autotune_str(uint8_t autotune)
{
	switch (autotune) {
	case 0U:
		return "128 kHz";
	case 2U:
		return "8 kHz";
	default:
		return "other";
	}
}

/* Decoded fields of a register file in logical (not bit-reversed) values */
static void print_fields(const struct shell *sh, const uint8_t *regs, bool pll_programmed)
{
	char buf[SI_FMT_LEN];
	struct r820t2_fields f;

	r820t2_decode(regs, &f);

	if (f.chip_id_ok) {
		shell_print(sh, "Chip ID:      ok");
	} else {
		shell_print(sh, "Chip ID:      unexpected (0x%02x)", regs[0]);
	}

	shell_print(sh, "PLL lock:     %s", f.pll_locked ? "yes" : "no");
	shell_print(sh, "VCO fine:     %u (nominal %u)", f.vco_fine, TUNER_VCO_FINE_NOMINAL);
	shell_print(sh, "Filter cal:   %u", f.filt_cal);
	print_gain(sh, "LNA gain:", f.lna_manual, f.lna_idx, f.lna_gain);
	print_gain(sh, "Mixer gain:", f.mixer_manual, f.mixer_idx, f.mixer_gain);
	shell_print(sh, "RF AGC:       LNA index %u, mixer index %u (register 3)", f.agc_lna_idx,
		    f.agc_mixer_idx);

	if (!f.vga_on) {
		shell_print(sh, "VGA:          off");
	} else if (f.vga_pin) {
		shell_print(sh, "VGA:          IF_AGC pin");
	} else {
		shell_print(sh, "VGA:          register, code %u", f.vga_code);
	}

	if (!pll_programmed) {
		shell_print(sh, "PLL:          not programmed yet");
		return;
	}

	shell_print(sh, "Xtal cap:     code %u (configured %u)", f.xtal_cap,
		    CONFIG_WELLE_SDR_R820T2_XTAL_CAP);

	shell_print(sh, "PLL:          Nint %u, SDM %u/65536 (%s), reference %s", f.nint, f.sdm,
		    f.sdm_on ? "on" : "off", f.refdiv2 ? "14.4 MHz" : "28.8 MHz");
	shell_print(sh, "VCO current:  %u", f.vco_cur);
	shell_print(sh, "Autotune:     %s", autotune_str(f.autotune));
	shell_print(sh, "VCO:          %s", si_fmt(buf, sizeof(buf), f.vco_hz, "Hz"));
	shell_print(sh, "LO:           %s (VCO / %u)", si_fmt(buf, sizeof(buf), f.lo_hz, "Hz"),
		    f.mix_div);
}

static int cmd_tuner_regs(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t live[R820T2_NUM_REGS];
	int err;

	if (argc < 2) {
		shell_print(sh, "Register shadow: 0 to 4 as last read, 5 to 31 as last written");
		shell_hexdump(sh, r820t2_regs(), R820T2_NUM_REGS);
		print_fields(sh, r820t2_regs(), r820t2_lo_freq() != 0U);
		return 0;
	}

	if (strcmp(argv[1], "live") != 0) {
		shell_error(sh, "Unknown source %s, use live", argv[1]);
		return -EINVAL;
	}

	err = r820t2_read_live(live, sizeof(live));
	if (err != 0) {
		shell_error(sh, "Reading the tuner failed (err %d), see welle-sdr tuner test", err);
		return err;
	}

	shell_print(sh, "Registers read from the chip:");
	shell_hexdump(sh, live, sizeof(live));
	print_fields(sh, live, true);

	return 0;
}

#define TUNER_SCAN_MAX          8U

static void print_hint(const struct shell *sh, const struct r820t2_bus_diag *diag,
		       int scan_err, const uint8_t *found, size_t count, int read_err,
		       uint8_t chip_id)
{
	const bool scl_bad = !diag->scl_pullup || !diag->scl_high;
	const bool sda_bad = !diag->sda_pullup || !diag->sda_high;
	bool tuner_acked = false;

	for (size_t i = 0U; i < MIN(count, TUNER_SCAN_MAX); i++) {
		if (found[i] == R820T2_I2C_ADDR_7BIT) {
			tuner_acked = true;
		}
	}

	if (scl_bad && sda_bad) {
		shell_print(sh, "Hint:         SCL+SDA low: tuner power, GND, pull-ups to 3.3 V?");
	} else if (scl_bad) {
		shell_print(sh, "Hint:         SCL low: check the PB10 wire and its pull-up");
	} else if (sda_bad) {
		shell_print(sh, "Hint:         SDA low: check the PB11 wire and its pull-up");
	} else if (scan_err != 0) {
		shell_print(sh, "Hint:         bus scan failed (err %d)", scan_err);
	} else if (count == 0U) {
		shell_print(sh, "Hint:         no answer: SDA/SCL swapped or open, tuner power, GND?");
	} else if (!tuner_acked) {
		shell_print(sh, "Hint:         no answer at 0x%02x: not an R820T2?",
			    R820T2_I2C_ADDR_7BIT);
	} else if (read_err != 0) {
		shell_print(sh, "Hint:         0x%02x answers, reads fail (err %d): pull-up values?",
			    R820T2_I2C_ADDR_7BIT, read_err);
	} else {
		shell_print(sh, "Hint:         0x%02x answers with chip ID 0x%02x: not an R820T2?",
			    R820T2_I2C_ADDR_7BIT, chip_id);
	}
}

/* Wiring report and chip readout, returns 0 if the R820T2 answers with its ID */
static int tuner_report(const struct shell *sh, bool dump)
{
	uint8_t live[R820T2_NUM_REGS];
	uint8_t found[TUNER_SCAN_MAX];
	struct r820t2_fields fields;
	struct r820t2_bus_diag diag;
	size_t count = 0U;
	int scan_err;
	int err;

	r820t2_bus_check(&diag);

	shell_print(sh, "Pull-ups:     SCL %s, SDA %s", diag.scl_pullup ? "ok" : "missing",
		    diag.sda_pullup ? "ok" : "missing");

	shell_print(sh, "Idle levels:  SCL %s, SDA %s", diag.scl_high ? "high" : "low",
		    diag.sda_high ? "high" : "low");

	scan_err = r820t2_bus_scan(found, ARRAY_SIZE(found), &count);
	if (scan_err != 0) {
		/* The scan records why it stopped */
		r820t2_bus_check(&diag);
		shell_print(sh, "Bus scan:     skipped (%s)", r820t2_bus_err_str(diag.last_err));
	} else if (count == 0U) {
		shell_print(sh, "Bus scan:     no device");
	} else {
		shell_fprintf_normal(sh, "Bus scan:    ");
		for (size_t i = 0U; i < MIN(count, ARRAY_SIZE(found)); i++) {
			shell_fprintf_normal(sh, " 0x%02x", found[i]);
		}
		shell_fprintf_normal(sh, (count > ARRAY_SIZE(found)) ? " ...\n" : "\n");
	}

	memset(live, 0, sizeof(live));
	err = r820t2_read_live(live, dump ? R820T2_NUM_REGS : 1U);
	r820t2_decode(live, &fields);
	if (err == 0 && fields.chip_id_ok) {
		if (!dump) {
			/* Before a standalone tune: the PLL state is printed after tuning */
			shell_print(sh, "Chip ID:      ok");
			return 0;
		}

		shell_print(sh, "Registers read from the chip:");
		shell_hexdump(sh, live, R820T2_NUM_REGS);
		print_fields(sh, live, true);

		return 0;
	}

	if (err != 0) {
		r820t2_bus_check(&diag);
		shell_print(sh, "Chip ID:      FAIL (%s)", r820t2_bus_err_str(diag.last_err));
	} else {
		shell_print(sh, "Chip ID:      FAIL (raw 0x%02x, expected 0x%02x)", live[0],
			    R820T2_CHIP_ID_RAW);
	}

	print_hint(sh, &diag, scan_err, found, count, err, live[0]);

	return -ENODEV;
}

static int cmd_tuner_test(const struct shell *sh, size_t argc, char **argv)
{
	char buf[SI_FMT_LEN];
	struct r820t2_fields fields;
	struct rtl2832u_status status;
	unsigned long freq;
	int err = 0;

	if (argc < 2) {
		return tuner_report(sh, true);
	}

	freq = shell_strtoul(argv[1], 0, &err);
	if (err != 0) {
		shell_error(sh, "Invalid number");
		return -EINVAL;
	}

	if (freq < R820T2_LO_MIN_HZ || freq > R820T2_LO_MAX_HZ) {
		char max[SI_FMT_LEN];

		shell_error(sh, "Out of range: %s to %s",
			    si_fmt(buf, sizeof(buf), R820T2_LO_MIN_HZ, "Hz"),
			    si_fmt(max, sizeof(max), R820T2_LO_MAX_HZ, "Hz"));
		return -ERANGE;
	}

	rtl2832u_get_status(&status);
	if (status.host_open) {
		shell_error(sh, "Busy: the host has the device open");
		return -EBUSY;
	}

	err = tuner_report(sh, false);
	if (err != 0) {
		shell_error(sh, "Tuner check failed, not tuning");
		return err;
	}

	err = r820t2_standalone_tune((uint32_t)freq);
	if (err != 0 && err != -EAGAIN) {
		shell_error(sh, "Tuning failed (err %d)", err);
		return err;
	}

	shell_print(sh, "Requested LO: %s", si_fmt(buf, sizeof(buf), freq, "Hz"));
	shell_print(sh, "Programmed:   %s", si_fmt(buf, sizeof(buf), r820t2_lo_freq(), "Hz"));
	shell_print(sh, "PLL lock:     %s", (err == 0) ? "yes" : "no");
	r820t2_decode(r820t2_regs(), &fields);
	shell_print(sh, "VCO fine:     %u (nominal %u)", fields.vco_fine, TUNER_VCO_FINE_NOMINAL);

	return err;
}

static int cmd_demod(const struct shell *sh, size_t argc, char **argv)
{
	const uint8_t *regs;
	unsigned long page;
	unsigned long reg;
	unsigned long len = 1UL;
	int err = 0;

	/* shell_strtoul() only sets err on failure, so one check covers all */
	page = shell_strtoul(argv[1], 0, &err);
	reg = shell_strtoul(argv[2], 0, &err);
	if (argc > 3) {
		len = shell_strtoul(argv[3], 0, &err);
	}

	if (err != 0) {
		shell_error(sh, "Invalid number");
		return -EINVAL;
	}

	if (page >= RTL2832U_DEMOD_PAGES || reg >= RTL2832U_DEMOD_PAGE_SIZE || len == 0UL ||
	    len > RTL2832U_DEMOD_PAGE_SIZE - reg) {
		shell_error(sh, "Out of range: page 0..%u, reg + len <= %u",
			    RTL2832U_DEMOD_PAGES - 1U, RTL2832U_DEMOD_PAGE_SIZE);
		return -EINVAL;
	}

	regs = rtl2832u_demod_regs((uint8_t)page);
	shell_hexdump(sh, &regs[reg], len);

	return 0;
}

/* Default ADC sample rate: 144 MHz / 18 */
#define ADC_DEFAULT_RATE_HZ 8000000U
#define ADC_BLOCK_TIMEOUT   K_MSEC(100)
#define ADC_CAPTURE_DEFAULT 4UL
#define ADC_CAPTURE_MAX     64UL
#define ADC_DUMP_LEN        32U
#define ADC_TOP_CODES       4U
/* The driver fills its ring with this byte at start */
#define ADC_FILL_PATTERN    0x55U

static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc_dvp));

static int adc_start(const struct shell *sh, uint32_t rate)
{
	char buf[SI_FMT_LEN];
	int err;

	if (!device_is_ready(adc_dev)) {
		shell_error(sh, "ADC capture device not ready");
		return -ENODEV;
	}

	err = dvp_adc_start(adc_dev, rate);
	if (err == -EINVAL) {
		shell_error(sh, "Rate not reachable: 144 MHz / integer divider >= 6 "
				"(24, 20.57, 18, 16, 14.4, 12, ... MHz)");
	} else if (err == -EALREADY) {
		shell_error(sh, "Already running");
	} else if (err != 0) {
		shell_error(sh, "Start failed (err %d)", err);
	} else {
		shell_print(sh, "Capture running at %s", si_fmt(buf, sizeof(buf), rate, "S/s"));
	}

	return err;
}

static void adc_print_stats(const struct shell *sh)
{
	char buf[SI_FMT_LEN];
	struct dvp_adc_stats stats;

	dvp_adc_get_stats(adc_dev, &stats);

	if (stats.running) {
		shell_print(sh, "Running:        yes, %s", si_fmt(buf, sizeof(buf), stats.rate, "S/s"));
	} else {
		shell_print(sh, "Running:        no");
	}

	shell_print(sh, "Blocks:         %u (%u bytes each)", stats.blocks,
		    CONFIG_ADC_DVP_WCH_BLOCK_SIZE);
	shell_print(sh, "Overruns:       %u", stats.overruns);
	shell_print(sh, "FIFO overflows: %u", stats.fifo_overflows);
	shell_print(sh, "Frame starts:   %u", stats.frame_starts);
	shell_print(sh, "Spurious IRQs:  %u", stats.spurious);
	shell_print(sh, "Guard:          %s",
		    stats.guard_intact ? "intact" : "overwritten (DMA wrote past a block)");
}

static int cmd_adc_start(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long rate = ADC_DEFAULT_RATE_HZ;
	int err = 0;

	if (argc > 1) {
		rate = shell_strtoul(argv[1], 0, &err);
		if (err != 0 || rate > UINT32_MAX) {
			shell_error(sh, "Invalid number");
			return -EINVAL;
		}
	}

	return adc_start(sh, (uint32_t)rate);
}

static int cmd_adc_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (dvp_adc_stop(adc_dev) != 0) {
		shell_error(sh, "Not running");
		return -EALREADY;
	}

	return 0;
}

static int cmd_adc_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	adc_print_stats(sh);

	return 0;
}

/* Print the most frequent codes; clears their histogram entries */
static void adc_print_top_codes(const struct shell *sh, uint32_t *hist, uint32_t total)
{
	for (uint32_t k = 0U; k < ADC_TOP_CODES; k++) {
		uint32_t best = 0U;

		for (uint32_t code = 1U; code < 256U; code++) {
			if (hist[code] > hist[best]) {
				best = code;
			}
		}

		if (hist[best] == 0U) {
			break;
		}

		shell_print(sh, "  0x%02x (%3u): %u (%u.%u %%)", best, best, hist[best],
			    (uint32_t)((uint64_t)hist[best] * 100U / total),
			    (uint32_t)((uint64_t)hist[best] * 1000U / total % 10U));
		hist[best] = 0U;
	}
}

static int cmd_adc_capture(const struct shell *sh, size_t argc, char **argv)
{
	/* Code histogram, too large for the shell stack */
	static uint32_t hist[256];
	struct dvp_adc_stats stats;
	unsigned long blocks = ADC_CAPTURE_DEFAULT;
	const uint8_t *data;
	uint64_t sum = 0U;
	uint32_t total = 0U;
	uint8_t min = UINT8_MAX;
	uint8_t max = 0U;
	size_t tail = 0U;
	bool started = false;
	size_t len;
	int err = 0;

	if (argc > 1) {
		blocks = shell_strtoul(argv[1], 0, &err);
		if (err != 0 || blocks == 0UL || blocks > ADC_CAPTURE_MAX) {
			shell_error(sh, "Block count 1..%lu", ADC_CAPTURE_MAX);
			return -EINVAL;
		}
	}

	dvp_adc_get_stats(adc_dev, &stats);
	if (!stats.running) {
		err = adc_start(sh, ADC_DEFAULT_RATE_HZ);
		if (err != 0) {
			return err;
		}

		started = true;
	}

	(void)memset(hist, 0, sizeof(hist));

	for (unsigned long i = 0UL; i < blocks; i++) {
		err = dvp_adc_get_block(adc_dev, &data, &len, ADC_BLOCK_TIMEOUT);
		if (err != 0) {
			shell_error(sh, "No block within 100 ms (err %d)", err);
			break;
		}

		if (i == 0UL) {
			shell_print(sh, "First %u bytes:", ADC_DUMP_LEN);
			shell_hexdump(sh, data, ADC_DUMP_LEN);

			/* Unwritten bytes at the end of the first block after start */
			while (tail < len && data[len - 1U - tail] == ADC_FILL_PATTERN) {
				tail++;
			}
		}

		for (size_t j = 0U; j < len; j++) {
			hist[data[j]]++;
			sum += data[j];
			min = MIN(min, data[j]);
			max = MAX(max, data[j]);
		}

		total += (uint32_t)len;
		dvp_adc_release_block(adc_dev);
	}

	if (started) {
		(void)dvp_adc_stop(adc_dev);
	}

	if (total == 0U) {
		adc_print_stats(sh);
		return (err != 0) ? err : -EIO;
	}

	shell_print(sh, "Samples:        %u", total);
	shell_print(sh, "Min / max:      0x%02x / 0x%02x", min, max);
	shell_print(sh, "Mean:           %u.%u", (uint32_t)(sum / total),
		    (uint32_t)(sum * 10U / total % 10U));
	if (started) {
		shell_print(sh, "First block:    %u trailing bytes still 0x%02x", tail,
			    ADC_FILL_PATTERN);
	}

	shell_print(sh, "Most frequent codes:");
	adc_print_top_codes(sh, hist, total);
	adc_print_stats(sh);

	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc,
	SHELL_CMD_ARG(start, NULL, "Start the capture: [rate_hz], default 8 MS/s", cmd_adc_start,
		      1, 1),
	SHELL_CMD_ARG(stop, NULL, "Stop the capture", cmd_adc_stop, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show the capture statistics", cmd_adc_status, 1, 0),
	SHELL_CMD_ARG(capture, NULL,
		      "Take [blocks] blocks (default 4) and show code statistics; starts and "
		      "stops the capture at 8 MS/s if it is not running",
		      cmd_adc_capture, 1, 1),
	SHELL_SUBCMD_SET_END
);

static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "iq") == 0) {
		sample_src_set_mode(SAMPLE_SRC_MODE_IQ);
	} else if (strcmp(argv[1], "raw") == 0) {
		sample_src_set_mode(SAMPLE_SRC_MODE_RAW);
	} else {
		shell_error(sh, "Mode is iq or raw");
		return -EINVAL;
	}

	shell_print(sh, "Output %s", argv[1]);

	return 0;
}

static int cmd_testsig(const struct shell *sh, size_t argc, char **argv)
{
	char buf[SI_FMT_LEN];
	unsigned long freq;
	uint32_t actual;
	int err = 0;

	ARG_UNUSED(argc);

	if (strcmp(argv[1], "off") == 0) {
		return testsig_set(0U, NULL);
	}

	freq = shell_strtoul(argv[1], 0, &err);
	if (err != 0 || freq == 0UL || freq > UINT32_MAX) {
		shell_error(sh, "Invalid number");
		return -EINVAL;
	}

	err = testsig_set((uint32_t)freq, &actual);
	if (err == -ERANGE) {
		char max[SI_FMT_LEN];

		shell_error(sh, "Out of range: %s to %s",
			    si_fmt(buf, sizeof(buf), TESTSIG_MIN_HZ, "Hz"),
			    si_fmt(max, sizeof(max), TESTSIG_MAX_HZ, "Hz"));
		return err;
	}

	if (err != 0) {
		shell_error(sh, "Test signal error %d", err);
		return err;
	}

	shell_print(sh, "Test signal on PA7: %s", si_fmt(buf, sizeof(buf), actual, "Hz"));

	return 0;
}

static int cmd_agc(const struct shell *sh, size_t argc, char **argv)
{
	struct if_agc_stats agc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	print_if_agc_line(sh);
	if_agc_get_stats(&agc);

	shell_print(sh, "Switch:       host RTL AGC %s", agc.active ? "on" : "off");
	shell_print(sh, "PWM:          %s, %u of %u counts on PA15", agc.pwm_on ? "on" : "off",
		    agc.counts, IF_AGC_CTL_PERIOD);
	shell_print(sh, "Fixed:        %u mV (range %u to %u mV)", agc.fixed_mv,
		    CONFIG_WELLE_SDR_IF_AGC_MIN_MV, CONFIG_WELLE_SDR_IF_AGC_MAX_MV);
	shell_print(sh, "Target:       MAD %u.%02u codes", agc.target_q4 / 16U,
		    (agc.target_q4 % 16U) * 100U / 16U);

	if (!agc.active) {
		return 0;
	}

	shell_print(sh, "Level:        MAD %u.%02u codes, mean %u, %u ppm clipped",
		    agc.mad_q4 / 16U, (agc.mad_q4 % 16U) * 100U / 16U, agc.dc, agc.clip_ppm);
	shell_print(sh, "Updates:      %u", agc.updates);

	return 0;
}

static int cmd_agc_fixed(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long mv;
	int err = 0;

	ARG_UNUSED(argc);

	mv = shell_strtoul(argv[1], 0, &err);
	if (err != 0 || mv > UINT32_MAX) {
		shell_error(sh, "Invalid number");
		return -EINVAL;
	}

	err = if_agc_set_fixed_mv((uint32_t)mv);
	if (err != 0) {
		shell_error(sh, "Out of range: %u to %u mV", CONFIG_WELLE_SDR_IF_AGC_MIN_MV,
			    CONFIG_WELLE_SDR_IF_AGC_MAX_MV);
		return err;
	}

	print_if_agc_line(sh);

	return 0;
}

static int cmd_agc_pwm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "off") == 0) {
		if_agc_set_pwm(false);
	} else if (strcmp(argv[1], "on") == 0) {
		if_agc_set_pwm(true);
	} else {
		shell_error(sh, "Use on or off");
		return -EINVAL;
	}

	print_if_agc_line(sh);

	return 0;
}

static int cmd_agc_target(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long codes;
	int err = 0;

	ARG_UNUSED(argc);

	codes = shell_strtoul(argv[1], 0, &err);
	if (err != 0 || codes == 0UL || codes > AGC_TARGET_MAX) {
		shell_error(sh, "Invalid target: 1 to %lu codes", AGC_TARGET_MAX);
		return -EINVAL;
	}

	return if_agc_set_target((uint16_t)(codes * 16UL));
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_agc,
	SHELL_CMD_ARG(fixed, NULL, "Voltage while the loop is off: <mV>", cmd_agc_fixed, 2, 0),
	SHELL_CMD_ARG(pwm, NULL,
		      "Stop the PWM (PA15 at 0 V, VGA gain from its register) or run it: "
		      "<on|off>",
		      cmd_agc_pwm, 2, 0),
	SHELL_CMD_ARG(target, NULL, "Level the loop aims at, MAD of the ADC codes: <codes>",
		      cmd_agc_target, 2, 0),
	SHELL_SUBCMD_SET_END
);

static int cmd_tuner_vga(const struct shell *sh, size_t argc, char **argv)
{
	struct r820t2_fields f;
	int err;

	if (argc > 1) {
		if (strcmp(argv[1], "off") == 0) {
			err = r820t2_set_vga_off(true);
		} else if (strcmp(argv[1], "on") == 0) {
			err = r820t2_set_vga_off(false);
		} else {
			shell_error(sh, "Use on or off");
			return -EINVAL;
		}

		if (err != 0) {
			shell_error(sh, "Writing register 0x0c failed (err %d)", err);
			return err;
		}
	}

	r820t2_decode(r820t2_regs(), &f);
	shell_print(sh, "VGA:          %s, now %s", r820t2_vga_off() ? "forced off" : "as written",
		    !r820t2_vga_written() ? "not written yet"
					  : (f.vga_on ? "powered on" : "powered off"));

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_tuner,
	SHELL_CMD_ARG(regs, NULL, "Dump and decode the R820T2 registers, [live] reads the chip",
		      cmd_tuner_regs, 1, 1),
	SHELL_CMD_ARG(vga, NULL,
		      "Power the VGA off whatever the host writes, or follow the host: "
		      "[on|off]",
		      cmd_tuner_vga, 1, 1),
	SHELL_CMD_ARG(test, NULL,
		      "Check the wiring and read the chip, or tune without a host: [lo_hz]",
		      cmd_tuner_test, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_welle_sdr,
	SHELL_CMD_ARG(status, NULL, "Show USB, stream and tuner state", cmd_status, 1, 0),
	SHELL_CMD(tuner, &sub_tuner, "R820T2 tuner commands", NULL),
	SHELL_CMD(adc, &sub_adc, "AD9280 ADC capture commands", NULL),
	SHELL_CMD_ARG(agc, &sub_agc, "IF AGC on the R820T2 IF_AGC pin (PA15)", cmd_agc, 1, 0),
	SHELL_CMD_ARG(testsig, NULL, "Square wave on PA7 for AD IN: <hz> or off", cmd_testsig,
		      2, 0),
	SHELL_CMD_ARG(demod, NULL, "Dump demod registers: <page> <reg> [len]", cmd_demod, 3, 1),
	SHELL_CMD_ARG(mode, NULL, "Stream format: iq or raw (raw ends when the host stops reading)",
		      cmd_mode, 2, 0),
	SHELL_SUBCMD_SET_END
);

/*
 * SHELL_CMD_REGISTER() turns the command name into C identifiers, which
 * cannot contain '-'. This is what it expands to, with the name as a string.
 */
static const struct shell_static_entry welle_sdr_entry = {
	.syntax = "welle-sdr",
	.help = "RTL-SDR emulator commands",
	.subcmd = &sub_welle_sdr,
	.handler = NULL,
	.args = {.mandatory = 0, .optional = 0},
};

static const TYPE_SECTION_ITERABLE(union shell_cmd_entry, shell_cmd_welle_sdr, shell_root_cmds,
				   shell_cmd_welle_sdr) = {
	.entry = &welle_sdr_entry,
};
