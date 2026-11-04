/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Shell commands to inspect the emulator state and test the R820T2 tuner */

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include "r820t2.h"
#include "rtl2832u.h"
#include "si_fmt.h"

/* Nominal VCO fine tune value, register 4 bits 5:4 */
#define TUNER_VCO_FINE_NOMINAL 2U

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rtl2832u_status status;
	char buf[SI_FMT_LEN];
	int32_t gain;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rtl2832u_get_status(&status);

	shell_print(sh, "USB:          %s, %s", status.configured ? "configured" : "not configured",
		    status.streaming ? "streaming" : "idle");
	shell_print(sh, "Host driver:  %s", status.host_open ? "open" : "closed");
	shell_print(sh, "Output:       %s", status.test_mode ? "test counter" : "I/Q tone");
	shell_print(sh, "Sample rate:  %s", si_fmt(buf, sizeof(buf), status.sample_rate, "S/s"));
	shell_print(sh, "Center freq:  %s", si_fmt(buf, sizeof(buf), status.center_freq, "Hz"));
	shell_print(sh, "Tuner LO:     %s", si_fmt(buf, sizeof(buf), status.lo_freq, "Hz"));
	shell_print(sh, "IF:           %s", si_fmt(buf, sizeof(buf), status.if_freq, "Hz"));

	if (r820t2_manual_gain(&gain)) {
		shell_print(sh, "Tuner gain:   %d.%d dB", gain / 10, gain % 10);
	} else {
		shell_print(sh, "Tuner gain:   auto");
	}

	shell_print(sh, "Streamed:     %u transfers, %llu bytes", status.xfers,
		    (unsigned long long)status.xfers * CONFIG_WELLE_SDR_XFER_SIZE);

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
	shell_print(sh, "VGA code:     %u", f.vga_code);

	if (!pll_programmed) {
		shell_print(sh, "PLL:          not programmed yet");
		return;
	}

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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_tuner,
	SHELL_CMD_ARG(regs, NULL, "Dump and decode the R820T2 registers, [live] reads the chip",
		      cmd_tuner_regs, 1, 1),
	SHELL_CMD_ARG(test, NULL,
		      "Check the wiring and read the chip, or tune without a host: [lo_hz]",
		      cmd_tuner_test, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_welle_sdr,
	SHELL_CMD_ARG(status, NULL, "Show USB, stream and tuner state", cmd_status, 1, 0),
	SHELL_CMD(tuner, &sub_tuner, "R820T2 tuner commands", NULL),
	SHELL_CMD_ARG(demod, NULL, "Dump demod registers: <page> <reg> [len]", cmd_demod, 3, 1),
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
