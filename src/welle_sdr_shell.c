/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Read-only shell commands to inspect the emulator state */

#include <errno.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>

#include "r820t2.h"
#include "rtl2832u.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rtl2832u_status status;
	int32_t gain;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rtl2832u_get_status(&status);

	shell_print(sh, "USB:          %s, %s", status.configured ? "configured" : "not configured",
		    status.streaming ? "streaming" : "idle");
	shell_print(sh, "Output:       %s", status.test_mode ? "test counter" : "I/Q tone");
	shell_print(sh, "Sample rate:  %u S/s", status.sample_rate);
	shell_print(sh, "Center freq:  %u Hz", status.center_freq);
	shell_print(sh, "Tuner LO:     %u Hz", status.lo_freq);
	shell_print(sh, "IF:           %u Hz", status.if_freq);

	if (r820t2_manual_gain(&gain)) {
		shell_print(sh, "Tuner gain:   %d.%d dB", gain / 10, gain % 10);
	} else {
		shell_print(sh, "Tuner gain:   auto");
	}

	shell_print(sh, "Streamed:     %u transfers, %llu bytes", status.xfers,
		    (unsigned long long)status.xfers * CONFIG_RTLSDR_EMU_XFER_SIZE);

	return 0;
}

static int cmd_tuner_regs(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_hexdump(sh, r820t2_regs(), R820T2_NUM_REGS);

	return 0;
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
	SHELL_CMD_ARG(regs, NULL, "Dump the R820T2 registers (not bit-reversed)",
		      cmd_tuner_regs, 1, 0),
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
