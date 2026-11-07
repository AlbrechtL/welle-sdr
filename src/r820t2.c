/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rafael Micro R820T2 tuner on I2C. Host requests are forwarded to the chip.
 * A register shadow keeps the values in the logical (not bit-reversed) form
 * used by librtlsdr, for the shell and the logs: registers 0 to 4 as last
 * read, 5 to 31 as last written.
 *
 * The bus is bit-banged on PB10/PB11 (see the board overlay and the README).
 * Each access first checks for external pull-ups and that SCL and SDA are
 * high, so a wiring fault is reported as such instead of as a missing tuner.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "if_agc.h"
#include "r820t2.h"

LOG_MODULE_REGISTER(r820t2, LOG_LEVEL_INF);

#define R820T2_I2C_NODE DT_NODELABEL(r820t2_i2c)

#define R820T2_FIRST_RW_REG 5U
/* Registers 0 to 4 are status registers */
#define R820T2_NUM_STATUS_REGS R820T2_FIRST_RW_REG

/* Register 2 bit 6: PLL locked */
#define R820T2_REG_LOCK       0x02U
#define R820T2_PLL_LOCK       0x40U
/* Register 4: VCO fine tune (bits 5:4), filter calibration code (bits 3:0) */
#define R820T2_REG_CAL        0x04U
#define R820T2_VCO_FINE_SHIFT 4U
#define R820T2_VCO_FINE_MASK  0x03U
#define R820T2_FILT_CAL_MASK  0x0FU

/* Register 5: LNA gain index (bits 3:0), bit 4 set disables LNA auto gain */
#define R820T2_REG_LNA        0x05U
#define R820T2_LNA_MANUAL     0x10U
/* Register 7: mixer gain index (bits 3:0), bit 4 set enables mixer auto gain */
#define R820T2_REG_MIXER      0x07U
#define R820T2_MIXER_AUTO     0x10U
#define R820T2_GAIN_IDX_MASK  0x0FU
/* Register 3: gains picked by the RF AGC, LNA index (bits 3:0), mixer index (bits 7:4) */
#define R820T2_REG_AGC        0x03U
#define R820T2_AGC_MIXER_SHIFT 4U
/*
 * Register 0x0c: VGA gain code (bits 3:0), bit 4 VGA gain from the IF_AGC
 * pin, bit 6 VGA powered on
 */
#define R820T2_REG_VGA        0x0CU
#define R820T2_VGA_PIN        0x10U
#define R820T2_VGA_POWER      0x40U
/*
 * Register 0x10: divider (bits 7:5), reference divided by 2 (bit 4),
 * crystal load capacitors (bits 1:0)
 */
#define R820T2_REG_DIV        0x10U
#define R820T2_DIV_SHIFT      5U
#define R820T2_DIV_MASK       0xE0U
#define R820T2_REFDIV2        0x10U
#define R820T2_XTAL_CAP_MASK  0x03U
/* Register 0x12: VCO current (bits 7:5), bit 3 sigma-delta modulator powered off */
#define R820T2_REG_VCO        0x12U
#define R820T2_VCO_CUR_SHIFT  5U
#define R820T2_VCO_CUR_MASK   0xE0U
#define R820T2_SDM_OFF        0x08U
/* Register 0x14: Ni (bits 5:0) and Si (bits 7:6), Nint = 4 * Ni + Si + 13 */
#define R820T2_REG_NI_SI      0x14U
#define R820T2_SI_SHIFT       6U
#define R820T2_NINT_OFFSET    13U
/* Registers 0x15 (low) and 0x16 (high): SDM fraction, in 1/65536 */
#define R820T2_REG_SDM_LO     0x15U
#define R820T2_REG_SDM_HI     0x16U
/* Register 0x1a bits 3:2: PLL autotune clock, 0 is 128 kHz, 2 is 8 kHz */
#define R820T2_REG_AUTOTUNE   0x1AU
#define R820T2_AUTOTUNE_MASK  0x0CU
#define R820T2_AUTOTUNE_FAST  0x00U
#define R820T2_AUTOTUNE_SLOW  0x08U

/* VCO current settings for the first and the second lock attempt */
#define R820T2_VCO_CUR_FIRST  4U
#define R820T2_VCO_CUR_RETRY  3U

#define R820T2_XTAL_FREQ      28800000ULL
#define R820T2_VCO_MIN_HZ     1770000000ULL
#define R820T2_MIX_DIV_MAX    64U
#define R820T2_SDM_ONE        65536ULL
/*
 * The PLL needs up to about 3.5 ms to lock after the dividers change
 * (measured). Lock is polled R820T2_LOCK_POLLS times, R820T2_LOCK_POLL_US
 * apart plus the read time: at least 5 ms. Counted, not timed: the system
 * cycle counter can jump back by up to one tick on this SoC.
 */
#define R820T2_LOCK_POLLS     20U
#define R820T2_LOCK_POLL_US   250U

/* Lowest and highest 7-bit address probed by the bus scan */
#define R820T2_SCAN_FIRST     0x08U
#define R820T2_SCAN_LAST      0x77U

/* Time for a line without external pull-up to fall through the pull-down */
#define R820T2_PULL_SETTLE_US 20U

/* Power-on values of registers 5 to 0x1f, written in one block */
static const uint8_t init_regs[R820T2_NUM_REGS - R820T2_FIRST_RW_REG] = {
	0x83, 0x32, 0x75,	/* 0x05: LNA auto gain, 0x07: mixer auto gain */
	0xC0, 0x40, 0xD6, 0x6C,	/* 0x08 to 0x0b */
	0xF5, 0x63, 0x75, 0x68,	/* 0x0c to 0x0f */
	0x6C, 0x83, 0x80, 0x00,	/* 0x10: divider 16, 0x12: VCO current 4, SDM on */
	0x0F, 0x00, 0xC0, 0x30,	/* 0x14 to 0x17 */
	0x48, 0xCC, 0x60, 0x00,	/* 0x18 to 0x1b */
	0x54, 0xAE, 0x4A, 0xC0,	/* 0x1c to 0x1f */
};

static const struct device *const i2c_dev = DEVICE_DT_GET(R820T2_I2C_NODE);
static const struct gpio_dt_spec scl_pin = GPIO_DT_SPEC_GET(R820T2_I2C_NODE, scl_gpios);
static const struct gpio_dt_spec sda_pin = GPIO_DT_SPEC_GET(R820T2_I2C_NODE, sda_gpios);

/* Serializes bus access and shadow updates between the usbd and shell threads */
static K_MUTEX_DEFINE(tuner_lock);

/*
 * Register shadow. Readers (status, shell, logs) do not take the lock: a
 * decoded value may mix two host writes, which only affects what is shown.
 */
static uint8_t regs[R820T2_NUM_REGS];
/* The PLL integer divider has been programmed */
static bool pll_valid;
/* The PLL dividers were written since the last read */
static bool pll_changed;
static struct r820t2_bus_diag diag;
/* Register 0x0c bits set and cleared in every write, over the written value */
static uint8_t vga_force_set;
static uint8_t vga_force_clear;
/* Last register 0x0c value written by the host or the standalone tune, before forcing */
static uint8_t vga_base;
static bool vga_base_valid;

/* LNA and mixer gain steps in 0.1 dB, as in librtlsdr tuner_r82xx.c */
static const int8_t lna_gain_steps[16] = {
	0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13,
};
static const int8_t mixer_gain_steps[16] = {
	0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8,
};

static uint8_t bitrev8(uint8_t byte)
{
	byte = (uint8_t)(((byte & 0xF0U) >> 4) | ((byte & 0x0FU) << 4));
	byte = (uint8_t)(((byte & 0xCCU) >> 2) | ((byte & 0x33U) << 2));
	byte = (uint8_t)(((byte & 0xAAU) >> 1) | ((byte & 0x55U) << 1));

	return byte;
}

const char *r820t2_bus_err_str(enum r820t2_bus_err err)
{
	switch (err) {
	case R820T2_BUS_OK:
		return "ok";
	case R820T2_BUS_NO_DEV:
		return "I2C controller not ready";
	case R820T2_BUS_NO_PULLUP:
		return "pull-up missing";
	case R820T2_BUS_LINE_LOW:
		return "line held low";
	case R820T2_BUS_NACK:
		return "no acknowledge";
	case R820T2_BUS_TIMEOUT:
		return "timeout";
	default:
		return "unknown";
	}
}

/* Record the result of a tuner access, log when it changes */
static void bus_result(enum r820t2_bus_err err)
{
	if (err == diag.last_err) {
		return;
	}

	if (err == R820T2_BUS_OK) {
		LOG_INF("R820T2 bus ok");
	} else if (err == R820T2_BUS_NO_PULLUP || err == R820T2_BUS_LINE_LOW) {
		LOG_WRN("R820T2 bus: %s (SCL %s, SDA %s)", r820t2_bus_err_str(err),
			diag.scl_high ? "high" : "low", diag.sda_high ? "high" : "low");
	} else {
		LOG_WRN("R820T2 bus: %s", r820t2_bus_err_str(err));
	}

	diag.last_err = err;
}

/*
 * Read both lines with the internal pull-down enabled: a line stays high only
 * with an external pull-up. Then make them released open-drain outputs again,
 * as the bit-bang driver configures them.
 */
static void pullup_check(void)
{
	int err;

	err = gpio_pin_configure_dt(&scl_pin, GPIO_INPUT | GPIO_PULL_DOWN);
	if (err == 0) {
		err = gpio_pin_configure_dt(&sda_pin, GPIO_INPUT | GPIO_PULL_DOWN);
	}

	if (err == 0) {
		k_busy_wait(R820T2_PULL_SETTLE_US);
		diag.scl_pullup = gpio_pin_get_dt(&scl_pin) > 0;
		diag.sda_pullup = gpio_pin_get_dt(&sda_pin) > 0;
	} else {
		LOG_ERR("Pull-up check failed (err %d)", err);
	}

	err = gpio_pin_configure_dt(&scl_pin, GPIO_INPUT | GPIO_OUTPUT_HIGH);
	if (err == 0) {
		err = gpio_pin_configure_dt(&sda_pin, GPIO_INPUT | GPIO_OUTPUT_HIGH);
	}
	if (err != 0) {
		LOG_ERR("Restoring the I2C pins failed (err %d)", err);
	}
}

static void sample_idle_levels(void)
{
	diag.scl_high = gpio_pin_get_dt(&scl_pin) > 0;
	diag.sda_high = gpio_pin_get_dt(&sda_pin) > 0;
}

/* Check the wiring before a transfer, called with tuner_lock held */
static int bus_guard(void)
{
	if (!device_is_ready(i2c_dev) || !gpio_is_ready_dt(&scl_pin) ||
	    !gpio_is_ready_dt(&sda_pin)) {
		bus_result(R820T2_BUS_NO_DEV);
		return -ENODEV;
	}

	if (!diag.scl_pullup || !diag.sda_pullup) {
		/* Check again, the wiring may have been fixed */
		pullup_check();
		if (!diag.scl_pullup || !diag.sda_pullup) {
			sample_idle_levels();
			bus_result(R820T2_BUS_NO_PULLUP);
			return -EIO;
		}
	}

	sample_idle_levels();
	if (!diag.scl_high || !diag.sda_high) {
		bus_result(R820T2_BUS_LINE_LOW);
		return -EIO;
	}

	return 0;
}

static int bus_finish(int err)
{
	if (err == 0) {
		bus_result(R820T2_BUS_OK);
	} else if (err == -ETIMEDOUT) {
		bus_result(R820T2_BUS_TIMEOUT);
	} else {
		bus_result(R820T2_BUS_NACK);
	}

	return err;
}

static int bus_write(const uint8_t *data, size_t len)
{
	int err = bus_guard();

	if (err != 0) {
		return err;
	}

	/* No IF_AGC PWM edges while SCL and SDA toggle */
	if_agc_bus_hold(true);
	err = i2c_write(i2c_dev, data, len, R820T2_I2C_ADDR_7BIT);
	if_agc_bus_hold(false);

	return bus_finish(err);
}

/* Read raw (bit-reversed) bytes and update the status shadow */
static int bus_read(uint8_t *data, size_t len)
{
	int err = bus_guard();

	if (err != 0) {
		return err;
	}

	if_agc_bus_hold(true);
	err = bus_finish(i2c_read(i2c_dev, data, len, R820T2_I2C_ADDR_7BIT));
	if_agc_bus_hold(false);
	if (err == 0) {
		for (size_t i = 0U; i < MIN(len, R820T2_NUM_STATUS_REGS); i++) {
			regs[i] = bitrev8(data[i]);
		}
	}

	return err;
}

/* Apply an acknowledged write to the shadow */
static void shadow_write(const uint8_t *data, size_t len)
{
	size_t reg = data[0];

	/*
	 * The chip acknowledges any register address, writes beyond the
	 * register file are discarded. Reads always start at register 0.
	 */
	for (size_t i = 1U; i < len && reg < R820T2_NUM_REGS; i++, reg++) {
		if (reg >= R820T2_FIRST_RW_REG) {
			regs[reg] = data[i];
		}

		if (reg == R820T2_REG_NI_SI) {
			pll_valid = true;
		}

		if (reg >= R820T2_REG_NI_SI && reg <= R820T2_REG_SDM_HI) {
			pll_changed = true;
		}
	}
}

int r820t2_init(void)
{
	uint8_t raw;
	int err;

	k_mutex_lock(&tuner_lock, K_FOREVER);

	memset(regs, 0, sizeof(regs));
	pll_valid = false;
	pll_changed = false;
	/* The forced 0x0c bits belong to their callers and stay */
	vga_base_valid = false;
	diag.last_err = R820T2_BUS_OK;

	err = bus_read(&raw, 1U);
	if (err != 0) {
		LOG_WRN("R820T2 not responding: %s", r820t2_bus_err_str(diag.last_err));
	} else if (raw != R820T2_CHIP_ID_RAW) {
		LOG_WRN("R820T2 unexpected chip ID 0x%02x", raw);
		err = -ENODEV;
	} else {
		LOG_INF("R820T2 found");
	}

	k_mutex_unlock(&tuner_lock);

	return err;
}

/* Offset of register reg in a write block, 0 if the block does not cover it */
static size_t reg_offset(const uint8_t *data, size_t len, uint8_t reg)
{
	if (data[0] > reg || reg - data[0] + 1U >= len) {
		return 0U;
	}

	return reg - data[0] + 1U;
}

/* Register 0x10 value with the configured crystal load capacitors */
static uint8_t xtal_forced(uint8_t val)
{
	if (CONFIG_WELLE_SDR_R820T2_XTAL_CAP == 0) {
		return val;
	}

	return (uint8_t)((val & ~R820T2_XTAL_CAP_MASK) | CONFIG_WELLE_SDR_R820T2_XTAL_CAP);
}

/* Register 0x0c value with the forced bits applied, called with tuner_lock held */
static uint8_t vga_forced(uint8_t val)
{
	return (uint8_t)((val | vga_force_set) & ~vga_force_clear);
}

/*
 * Write a block with the forced register 0x0c and 0x10 bits applied and
 * record the unforced 0x0c value, called with tuner_lock held. A block
 * longer than the register file is never written by librtlsdr; it is passed
 * on as is.
 */
static int forced_block_write(const uint8_t *data, size_t len)
{
	uint8_t forced[R820T2_NUM_REGS + 1U];
	const size_t vga = reg_offset(data, len, R820T2_REG_VGA);
	const size_t xtal = reg_offset(data, len, R820T2_REG_DIV);
	const uint8_t *out = data;
	int err;

	if ((vga != 0U || xtal != 0U) && len <= sizeof(forced)) {
		memcpy(forced, data, len);
		if (vga != 0U) {
			forced[vga] = vga_forced(data[vga]);
		}
		if (xtal != 0U) {
			forced[xtal] = xtal_forced(data[xtal]);
		}
		out = forced;
	}

	err = bus_write(out, len);
	if (err == 0) {
		shadow_write(out, len);

		if (vga != 0U) {
			vga_base = data[vga];
			vga_base_valid = true;
		}
	}

	return err;
}

/* Write register 0x0c with the current forced bits, called with tuner_lock held */
static int vga_update(void)
{
	uint8_t buf[2] = {R820T2_REG_VGA, 0U};
	int err;

	/*
	 * The shadow is 0 until register 0x0c has been written; a value derived
	 * from it would also clear the fixed bits 7 and 5.
	 */
	if (!vga_base_valid) {
		return 0;
	}

	buf[1] = vga_forced(vga_base);
	err = bus_write(buf, sizeof(buf));
	if (err == 0) {
		shadow_write(buf, sizeof(buf));
	}

	return err;
}

int r820t2_i2c_write(const uint8_t *data, size_t len)
{
	int err;

	if (len == 0U) {
		return -EINVAL;
	}

	k_mutex_lock(&tuner_lock, K_FOREVER);

	err = forced_block_write(data, len);

	k_mutex_unlock(&tuner_lock);

	LOG_DBG("write reg 0x%02x len %zu err %d", data[0], len - 1U, err);

	return err;
}

/*
 * Read until the PLL lock bit is set or R820T2_LOCK_POLLS reads have been
 * made, called with tuner_lock held. The status shadow holds the last
 * result. The caller's thread stays busy for up to about 13 ms (3-byte
 * reads at 100 kHz).
 */
static int read_until_locked(uint8_t *raw, size_t len)
{
	int err;

	pll_changed = false;

	for (uint32_t i = 0U; ; i++) {
		err = bus_read(raw, len);
		if (err != 0 || (regs[R820T2_REG_LOCK] & R820T2_PLL_LOCK) != 0U ||
		    i + 1U >= R820T2_LOCK_POLLS) {
			return err;
		}

		/* A sleep would last a whole 10 ms tick */
		k_busy_wait(R820T2_LOCK_POLL_US);
	}
}

int r820t2_i2c_read(uint8_t *data, size_t len)
{
	int err;

	k_mutex_lock(&tuner_lock, K_FOREVER);

	/*
	 * librtlsdr checks PLL lock with the first read after its last divider
	 * write. That read reaches the chip sooner than through a real RTL2832U,
	 * often before the PLL has locked, so wait for lock first.
	 */
	if (pll_changed && len > R820T2_REG_LOCK) {
		err = read_until_locked(data, len);
	} else {
		err = bus_read(data, len);
	}

	k_mutex_unlock(&tuner_lock);

	return err;
}

const uint8_t *r820t2_regs(void)
{
	return regs;
}

/* Gain for a gain index: the sum of the steps up to it, in 0.1 dB */
static int32_t gain_sum(const int8_t *steps, uint8_t idx)
{
	int32_t gain = 0;

	for (uint8_t i = 1U; i <= idx; i++) {
		gain += steps[i];
	}

	return gain;
}

void r820t2_decode(const uint8_t *r, struct r820t2_fields *f)
{
	const uint8_t div = r[R820T2_REG_DIV];
	uint64_t pll_ref = R820T2_XTAL_FREQ;

	f->chip_id_ok = (r[0] == bitrev8(R820T2_CHIP_ID_RAW));
	f->pll_locked = (r[R820T2_REG_LOCK] & R820T2_PLL_LOCK) != 0U;
	f->vco_fine = (r[R820T2_REG_CAL] >> R820T2_VCO_FINE_SHIFT) & R820T2_VCO_FINE_MASK;
	f->filt_cal = r[R820T2_REG_CAL] & R820T2_FILT_CAL_MASK;

	f->lna_manual = (r[R820T2_REG_LNA] & R820T2_LNA_MANUAL) != 0U;
	f->lna_idx = r[R820T2_REG_LNA] & R820T2_GAIN_IDX_MASK;
	f->lna_gain = gain_sum(lna_gain_steps, f->lna_idx);
	f->mixer_manual = (r[R820T2_REG_MIXER] & R820T2_MIXER_AUTO) == 0U;
	f->mixer_idx = r[R820T2_REG_MIXER] & R820T2_GAIN_IDX_MASK;
	f->mixer_gain = gain_sum(mixer_gain_steps, f->mixer_idx);
	f->agc_lna_idx = r[R820T2_REG_AGC] & R820T2_GAIN_IDX_MASK;
	f->agc_mixer_idx = r[R820T2_REG_AGC] >> R820T2_AGC_MIXER_SHIFT;
	f->vga_code = r[R820T2_REG_VGA] & R820T2_GAIN_IDX_MASK;
	f->vga_pin = (r[R820T2_REG_VGA] & R820T2_VGA_PIN) != 0U;
	f->vga_on = (r[R820T2_REG_VGA] & R820T2_VGA_POWER) != 0U;

	f->mix_div = (uint16_t)(2U << (div >> R820T2_DIV_SHIFT));
	f->refdiv2 = (div & R820T2_REFDIV2) != 0U;
	f->xtal_cap = div & R820T2_XTAL_CAP_MASK;
	f->vco_cur = r[R820T2_REG_VCO] >> R820T2_VCO_CUR_SHIFT;
	f->sdm_on = (r[R820T2_REG_VCO] & R820T2_SDM_OFF) == 0U;
	f->nint = (uint16_t)(4U * (r[R820T2_REG_NI_SI] & 0x3FU) +
			     (r[R820T2_REG_NI_SI] >> R820T2_SI_SHIFT) + R820T2_NINT_OFFSET);
	f->sdm = (uint16_t)(((uint16_t)r[R820T2_REG_SDM_HI] << 8) | r[R820T2_REG_SDM_LO]);
	f->autotune = (r[R820T2_REG_AUTOTUNE] & R820T2_AUTOTUNE_MASK) >> 2;

	if (f->refdiv2) {
		pll_ref /= 2U;
	}

	/* VCO = 2 * ref * (Nint + SDM / 65536), LO = VCO / mix_div */
	f->vco_hz = (2U * pll_ref * (((uint64_t)f->nint << 16) + (f->sdm_on ? f->sdm : 0U))) >> 16;
	f->lo_hz = (uint32_t)(f->vco_hz / f->mix_div);
}

uint32_t r820t2_lo_freq(void)
{
	struct r820t2_fields f;

	if (!pll_valid) {
		return 0U;
	}

	r820t2_decode(regs, &f);

	return f.lo_hz;
}

bool r820t2_manual_gain(int32_t *gain_tenth_db)
{
	struct r820t2_fields f;

	r820t2_decode(regs, &f);
	if (!f.lna_manual) {
		return false;
	}

	*gain_tenth_db = f.lna_gain + f.mixer_gain;

	return true;
}

int r820t2_set_vga_control(enum r820t2_vga_ctl ctl)
{
	int err;

	k_mutex_lock(&tuner_lock, K_FOREVER);

	vga_force_set &= (uint8_t)~R820T2_VGA_PIN;
	vga_force_clear &= (uint8_t)~R820T2_VGA_PIN;

	if (ctl == R820T2_VGA_CTL_PIN) {
		vga_force_set |= R820T2_VGA_PIN;
	} else if (ctl == R820T2_VGA_CTL_REGISTER) {
		vga_force_clear |= R820T2_VGA_PIN;
	} else {
		/* Bit 4 as written */
	}

	err = vga_update();

	k_mutex_unlock(&tuner_lock);

	return err;
}

int r820t2_set_vga_off(bool off)
{
	int err;

	k_mutex_lock(&tuner_lock, K_FOREVER);

	if (off) {
		vga_force_clear |= R820T2_VGA_POWER;
	} else {
		vga_force_clear &= (uint8_t)~R820T2_VGA_POWER;
	}

	err = vga_update();

	k_mutex_unlock(&tuner_lock);

	return err;
}

bool r820t2_vga_off(void)
{
	return (vga_force_clear & R820T2_VGA_POWER) != 0U;
}

bool r820t2_vga_written(void)
{
	return vga_base_valid;
}

void r820t2_bus_check(struct r820t2_bus_diag *out)
{
	k_mutex_lock(&tuner_lock, K_FOREVER);

	if (gpio_is_ready_dt(&scl_pin) && gpio_is_ready_dt(&sda_pin)) {
		pullup_check();
		sample_idle_levels();
	}

	*out = diag;

	k_mutex_unlock(&tuner_lock);
}

int r820t2_bus_scan(uint8_t *found, size_t max, size_t *count)
{
	struct i2c_msg msg = {
		.buf = NULL,
		.len = 0U,
		.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
	};
	int probe;
	int err = 0;

	*count = 0U;

	k_mutex_lock(&tuner_lock, K_FOREVER);

	for (uint16_t addr = R820T2_SCAN_FIRST; addr <= R820T2_SCAN_LAST; addr++) {
		/* Stop at the first wiring fault instead of probing a dead bus */
		err = bus_guard();
		if (err != 0) {
			break;
		}

		/* No result recording: most addresses are expected not to answer */
		if_agc_bus_hold(true);
		probe = i2c_transfer(i2c_dev, &msg, 1U, addr);
		if_agc_bus_hold(false);

		if (probe == 0) {
			if (*count < max) {
				found[*count] = (uint8_t)addr;
			}
			(*count)++;
		}
	}

	k_mutex_unlock(&tuner_lock);

	return err;
}

int r820t2_read_live(uint8_t *out, size_t len)
{
	uint8_t raw[R820T2_NUM_REGS];
	int err;

	if (len == 0U || len > R820T2_NUM_REGS) {
		return -EINVAL;
	}

	k_mutex_lock(&tuner_lock, K_FOREVER);
	err = bus_read(raw, len);
	k_mutex_unlock(&tuner_lock);

	if (err == 0) {
		for (size_t i = 0U; i < len; i++) {
			out[i] = bitrev8(raw[i]);
		}
	}

	return err;
}

/* Read-modify-write one register through the shadow, called with tuner_lock held */
static int reg_update(uint8_t reg, uint8_t val, uint8_t mask)
{
	const uint8_t buf[2] = {reg, (uint8_t)((regs[reg] & ~mask) | (val & mask))};
	int err;

	err = bus_write(buf, sizeof(buf));
	if (err == 0) {
		shadow_write(buf, sizeof(buf));
	}

	return err;
}

/* Program the PLL for lo_hz and check lock, called with tuner_lock held */
static int pll_tune(uint32_t lo_hz, uint8_t vco_cur)
{
	const uint64_t ref2 = 2ULL * R820T2_XTAL_FREQ;
	uint32_t mix_div = 2U;
	uint8_t div_num = 0U;
	uint8_t status[R820T2_NUM_STATUS_REGS];
	uint64_t vco;
	uint64_t nint;
	uint64_t sdm;
	int err;

	/* Smallest divider that puts the VCO into 1770..3540 MHz */
	while ((uint64_t)lo_hz * mix_div < R820T2_VCO_MIN_HZ && mix_div < R820T2_MIX_DIV_MAX) {
		mix_div <<= 1;
		div_num++;
	}

	/* VCO = 2 * ref * (Nint + SDM / 65536), SDM rounded to the nearest step */
	vco = (uint64_t)lo_hz * mix_div;
	nint = vco / ref2;
	sdm = (((vco - nint * ref2) * R820T2_SDM_ONE) + ref2 / 2U) / ref2;
	if (sdm == R820T2_SDM_ONE) {
		nint++;
		sdm = 0U;
	}

	/* Divider, reference not divided by 2 */
	err = reg_update(R820T2_REG_DIV, (uint8_t)(div_num << R820T2_DIV_SHIFT),
			 R820T2_DIV_MASK | R820T2_REFDIV2);
	if (err == 0) {
		err = reg_update(R820T2_REG_VCO,
				 (uint8_t)((vco_cur << R820T2_VCO_CUR_SHIFT) |
					   ((sdm == 0U) ? R820T2_SDM_OFF : 0U)),
				 R820T2_VCO_CUR_MASK | R820T2_SDM_OFF);
	}
	if (err == 0) {
		/* Fast autotune while locking */
		err = reg_update(R820T2_REG_AUTOTUNE, R820T2_AUTOTUNE_FAST, R820T2_AUTOTUNE_MASK);
	}
	if (err == 0) {
		const uint8_t ni = (uint8_t)((nint - R820T2_NINT_OFFSET) / 4U);
		const uint8_t si = (uint8_t)((nint - R820T2_NINT_OFFSET) % 4U);

		err = reg_update(R820T2_REG_NI_SI, (uint8_t)((si << R820T2_SI_SHIFT) | ni), 0xFFU);
	}
	if (err == 0) {
		err = reg_update(R820T2_REG_SDM_HI, (uint8_t)(sdm >> 8), 0xFFU);
	}
	if (err == 0) {
		err = reg_update(R820T2_REG_SDM_LO, (uint8_t)sdm, 0xFFU);
	}
	if (err != 0) {
		return err;
	}

	err = read_until_locked(status, sizeof(status));
	if (err != 0) {
		return err;
	}

	err = reg_update(R820T2_REG_AUTOTUNE, R820T2_AUTOTUNE_SLOW, R820T2_AUTOTUNE_MASK);
	if (err != 0) {
		return err;
	}

	return ((regs[R820T2_REG_LOCK] & R820T2_PLL_LOCK) != 0U) ? 0 : -EAGAIN;
}

int r820t2_standalone_tune(uint32_t lo_hz)
{
	uint8_t buf[1U + sizeof(init_regs)];
	int err;

	if (lo_hz < R820T2_LO_MIN_HZ || lo_hz > R820T2_LO_MAX_HZ) {
		return -ERANGE;
	}

	buf[0] = R820T2_FIRST_RW_REG;
	memcpy(&buf[1], init_regs, sizeof(init_regs));

	k_mutex_lock(&tuner_lock, K_FOREVER);

	err = forced_block_write(buf, sizeof(buf));
	if (err == 0) {
		err = pll_tune(lo_hz, R820T2_VCO_CUR_FIRST);
	}

	if (err == -EAGAIN) {
		LOG_DBG("PLL not locked, retrying with VCO current %u", R820T2_VCO_CUR_RETRY);
		err = pll_tune(lo_hz, R820T2_VCO_CUR_RETRY);
	}

	k_mutex_unlock(&tuner_lock);

	return err;
}
