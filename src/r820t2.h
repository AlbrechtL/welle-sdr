/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WELLE_SDR_R820T2_H_
#define WELLE_SDR_R820T2_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 8-bit I2C address as used by librtlsdr */
#define R820T2_I2C_ADDR 0x34U
/* 7-bit I2C address on the bus */
#define R820T2_I2C_ADDR_7BIT (R820T2_I2C_ADDR >> 1)

#define R820T2_NUM_REGS 32U

/* Raw (bit-reversed) chip ID in register 0 */
#define R820T2_CHIP_ID_RAW 0x69U

/* Lowest and highest LO the PLL reaches (VCO 1770..3540 MHz, divider 2..64) */
#define R820T2_LO_MIN_HZ 27656250U
#define R820T2_LO_MAX_HZ 1769999999U

/** Cause of the last failed tuner bus access. */
enum r820t2_bus_err {
	/** Last access succeeded. */
	R820T2_BUS_OK,
	/** I2C controller not ready. */
	R820T2_BUS_NO_DEV,
	/** No external pull-up found on SCL or SDA, access not attempted. */
	R820T2_BUS_NO_PULLUP,
	/** SCL or SDA held low while idle, access not attempted. */
	R820T2_BUS_LINE_LOW,
	/** No acknowledge from the tuner. */
	R820T2_BUS_NACK,
	/** Transfer timed out. */
	R820T2_BUS_TIMEOUT,
};

/** Register fields, decoded from logical (not bit-reversed) register values. */
struct r820t2_fields {
	/** Register 0 holds the R820T2 chip ID. */
	bool chip_id_ok;
	/** Register 2 bit 6: PLL locked. */
	bool pll_locked;
	/** Register 4 bits 5:4: VCO fine tune, nominal 2. */
	uint8_t vco_fine;
	/** Register 4 bits 3:0: filter calibration code. */
	uint8_t filt_cal;
	/** Register 5 bit 4: LNA gain set manually, not by the AGC. */
	bool lna_manual;
	/** Register 5 bits 3:0: LNA gain index. */
	uint8_t lna_idx;
	/** LNA gain for @ref lna_idx in 0.1 dB, librtlsdr gain steps. */
	int32_t lna_gain;
	/** Register 7 bit 4 clear: mixer gain set manually, not by the AGC. */
	bool mixer_manual;
	/** Register 7 bits 3:0: mixer gain index. */
	uint8_t mixer_idx;
	/** Mixer gain for @ref mixer_idx in 0.1 dB, librtlsdr gain steps. */
	int32_t mixer_gain;
	/** Register 3 bits 3:0: LNA gain index picked by the RF AGC. */
	uint8_t agc_lna_idx;
	/** Register 3 bits 7:4: mixer gain index picked by the RF AGC. */
	uint8_t agc_mixer_idx;
	/** Register 0x0c bits 3:0: VGA gain code. */
	uint8_t vga_code;
	/** Register 0x0c bit 4: VGA gain set by the IF_AGC pin, not by @ref vga_code. */
	bool vga_pin;
	/** Register 0x0c bit 6: VGA powered on. */
	bool vga_on;
	/** Register 0x10 bits 7:5: mixer divider, 2 << field (2 to 256). */
	uint16_t mix_div;
	/** Register 0x10 bit 4: PLL reference divided by 2. */
	bool refdiv2;
	/** Register 0x10 bits 1:0: crystal load capacitor code. */
	uint8_t xtal_cap;
	/** Register 0x12 bits 7:5: VCO current setting. */
	uint8_t vco_cur;
	/** Register 0x12 bit 3 clear: sigma-delta modulator on. */
	bool sdm_on;
	/** Register 0x14: integer divider, Nint = 4 * Ni + Si + 13. */
	uint16_t nint;
	/** Registers 0x15 (low) and 0x16 (high): SDM fraction in 1/65536. */
	uint16_t sdm;
	/** Register 0x1a bits 3:2: PLL autotune clock, 0 is 128 kHz, 2 is 8 kHz. */
	uint8_t autotune;
	/** VCO frequency in Hz, 2 * reference * (Nint + SDM / 65536). */
	uint64_t vco_hz;
	/** LO frequency in Hz, VCO / mixer divider. */
	uint32_t lo_hz;
};

/** Source of the VGA gain, register 0x0c bit 4. */
enum r820t2_vga_ctl {
	/** Bit 4 as written by the host. */
	R820T2_VGA_CTL_HOST,
	/** Bit 4 set in every write: gain from the IF_AGC pin. */
	R820T2_VGA_CTL_PIN,
	/** Bit 4 cleared in every write: gain from the VGA code. */
	R820T2_VGA_CTL_REGISTER,
};

/** Tuner bus wiring state. */
struct r820t2_bus_diag {
	/** SCL has an external pull-up. */
	bool scl_pullup;
	/** SDA has an external pull-up. */
	bool sda_pullup;
	/** SCL idle level. */
	bool scl_high;
	/** SDA idle level. */
	bool sda_high;
	/** Cause of the last failed access. */
	enum r820t2_bus_err last_err;
};

/**
 * @brief Reset the register shadow and probe the tuner.
 *
 * Checks the bus wiring and reads the chip ID. The result is logged.
 *
 * @retval 0 R820T2 found.
 * @retval -ENODEV I2C controller not ready or unexpected chip ID.
 * @retval -EIO Wiring fault or no acknowledge.
 * @retval -ETIMEDOUT Transfer timed out.
 */
int r820t2_init(void);

/**
 * @brief Forward an I2C write from the host to the tuner.
 *
 * Updates the register shadow when the tuner acknowledges the write. Forces
 * register 0x0c bit 4 as selected with r820t2_set_vga_control() and clears
 * bit 6 while r820t2_set_vga_off() is on.
 *
 * @param data First byte is the start register, followed by register values.
 * @param len Number of bytes in @p data.
 *
 * @retval 0 Success.
 * @retval -EINVAL Empty write.
 * @retval -ENODEV I2C controller not ready.
 * @retval -EIO Wiring fault or no acknowledge.
 * @retval -ETIMEDOUT Transfer timed out.
 */
int r820t2_i2c_write(const uint8_t *data, size_t len);

/**
 * @brief Forward an I2C read from the host to the tuner.
 *
 * The R820T2 always reads from register 0 and returns each byte
 * bit-reversed, librtlsdr reverses them again. The bytes are passed on
 * unchanged; status registers 0 to 4 are copied into the shadow.
 *
 * @param data Buffer for the bytes on the bus.
 * @param len Number of bytes to read.
 *
 * @retval 0 Success.
 * @retval -ENODEV I2C controller not ready.
 * @retval -EIO Wiring fault or no acknowledge.
 * @retval -ETIMEDOUT Transfer timed out.
 */
int r820t2_i2c_read(uint8_t *data, size_t len);

/**
 * @brief Select the VGA gain control.
 *
 * Switching writes register 0x0c: the last written value with bit 4 forced
 * as selected. Nothing is written before register 0x0c has been written once.
 *
 * @param ctl Source of the VGA gain.
 *
 * @retval 0 Success.
 * @retval -ENODEV I2C controller not ready.
 * @retval -EIO Wiring fault or no acknowledge.
 * @retval -ETIMEDOUT Transfer timed out.
 */
int r820t2_set_vga_control(enum r820t2_vga_ctl ctl);

/**
 * @brief Power the VGA off, whatever the host writes.
 *
 * While on, register 0x0c bit 6 (VGA power) is cleared in every write, by
 * the host and by the standalone tune. Switching writes register 0x0c: the
 * last written value with bit 6 cleared (on) or as written (off). Nothing is
 * written before register 0x0c has been written once.
 *
 * @param off VGA powered off.
 *
 * @retval 0 Success.
 * @retval -ENODEV I2C controller not ready.
 * @retval -EIO Wiring fault or no acknowledge.
 * @retval -ETIMEDOUT Transfer timed out.
 */
int r820t2_set_vga_off(bool off);

/**
 * @retval true The VGA is forced off by r820t2_set_vga_off().
 * @retval false The VGA power follows the written register value.
 */
bool r820t2_vga_off(void);

/**
 * @retval true Register 0x0c has been written by the host or the standalone tune.
 * @retval false Register 0x0c holds its power-on value, the shadow does not know it.
 */
bool r820t2_vga_written(void);

/**
 * @return Register shadow, not bit-reversed: registers 0 to 4 as last read
 * from the tuner, 5 to 31 as last written.
 */
const uint8_t *r820t2_regs(void);

/**
 * @brief Decode the local oscillator frequency from the PLL registers.
 *
 * Inverse of the librtlsdr PLL setup, with the 28.8 MHz reference. The host
 * computes the fractional part in kHz steps, so the result is within about
 * 1 kHz of the frequency it asked for.
 *
 * @return LO frequency in Hz, 0 until the PLL has been programmed.
 */
uint32_t r820t2_lo_freq(void);

/**
 * @brief Decode the LNA and mixer gain setting.
 *
 * The gain is the sum of the librtlsdr LNA and mixer gain steps for the
 * programmed indices, the same value rtl_test lists. The VGA is not included.
 *
 * @param gain_tenth_db Set to the gain in 0.1 dB in manual mode.
 *
 * @retval true Manual gain, @p gain_tenth_db is valid.
 * @retval false Automatic gain control.
 */
bool r820t2_manual_gain(int32_t *gain_tenth_db);

/**
 * @brief Decode the register fields.
 *
 * @param regs R820T2_NUM_REGS logical (not bit-reversed) register values, for
 *             example from r820t2_regs() or r820t2_read_live().
 * @param fields Filled with the decoded fields.
 */
void r820t2_decode(const uint8_t *regs, struct r820t2_fields *fields);

/**
 * @brief Check the tuner bus wiring.
 *
 * Checks SCL and SDA for external pull-ups and samples their idle levels.
 * Host accesses to the tuner wait until the check is done.
 *
 * @param diag Filled with the result and the cause of the last failed access.
 */
void r820t2_bus_check(struct r820t2_bus_diag *diag);

/**
 * @brief Take the tuner bus for another device on it.
 *
 * Locks out tuner accesses, checks the wiring and floats the IF_AGC pin, as
 * before a tuner transfer. Transfers to the other device are then made with
 * the I2C API on the bus and end with r820t2_bus_release(). Keep them short:
 * host tuner requests wait meanwhile.
 *
 * @retval 0 Bus taken, release it with r820t2_bus_release().
 * @retval -ENODEV I2C controller not ready, bus not taken.
 * @retval -EIO Wiring fault, bus not taken.
 */
int r820t2_bus_acquire(void);

/** @brief Release the bus taken with r820t2_bus_acquire(). */
void r820t2_bus_release(void);

/**
 * @param err Bus error cause.
 *
 * @return Short description of @p err.
 */
const char *r820t2_bus_err_str(enum r820t2_bus_err err);

/**
 * @brief Scan the tuner bus for devices.
 *
 * Probes the 7-bit addresses 0x08 to 0x77 with an empty write. Stops at the
 * first wiring fault.
 *
 * @param found Buffer for the acknowledging 7-bit addresses.
 * @param max Size of @p found.
 * @param count Set to the number of acknowledging addresses, may exceed @p max.
 *
 * @retval 0 Scan completed.
 * @retval -EIO Wiring fault, scan not completed.
 * @retval -ENODEV I2C controller not ready.
 */
int r820t2_bus_scan(uint8_t *found, size_t max, size_t *count);

/**
 * @brief Read the tuner registers.
 *
 * Reads from register 0 and stores the values not bit-reversed. Status
 * registers 0 to 4 are copied into the shadow.
 *
 * @param regs Buffer for the register values.
 * @param len Number of registers, at most R820T2_NUM_REGS.
 *
 * @retval 0 Success.
 * @retval -EINVAL @p len out of range.
 * @retval -ENODEV I2C controller not ready.
 * @retval -EIO Wiring fault or no acknowledge.
 * @retval -ETIMEDOUT Transfer timed out.
 */
int r820t2_read_live(uint8_t *regs, size_t len);

/**
 * @brief Initialize the tuner and tune the PLL without a host.
 *
 * Writes the power-on register set and programs the PLL for @p lo_hz from
 * the 28.8 MHz reference, then checks PLL lock. Retries once with VCO
 * current setting 3 instead of 4 when the PLL does not lock. The host
 * re-initializes the tuner when it opens the device.
 *
 * @param lo_hz LO frequency in Hz.
 *
 * @retval 0 PLL locked.
 * @retval -ERANGE @p lo_hz outside R820T2_LO_MIN_HZ to R820T2_LO_MAX_HZ.
 * @retval -EAGAIN PLL not locked.
 * @retval -ENODEV I2C controller not ready.
 * @retval -EIO Wiring fault or no acknowledge.
 * @retval -ETIMEDOUT Transfer timed out.
 */
int r820t2_standalone_tune(uint32_t lo_hz);

#endif /* WELLE_SDR_R820T2_H_ */
