/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * welle-sdr raw mode protocol, version 1.
 *
 * Plain C without Zephyr dependencies, for host software.
 *
 * Switching: the host sets three sample rates in a row with
 * rtlsdr_set_sample_rate() (rtl_tcp command 0x02), each within
 * WSDR_RAW_KNOCK_WINDOW_MS of the previous one:
 *
 *   WSDR_RAW_KNOCK_RATE_1, WSDR_RAW_KNOCK_RATE_2,
 *   WSDR_RAW_KNOCK_RATE_3(WSDR_RAW_KNOCK_CODE(mode))
 *
 * and then its real sample rate. On a real RTL2832U this only changes the
 * rate for a moment.
 *
 * Stream in raw mode: frames of one bulk transfer each (4096 bytes): a
 * WSDR_RAW_HDR_LEN byte header, then real unsigned 8-bit ADC samples. The
 * samples continue across frames. A plain RTL2832U stream never carries
 * the header, which tells the host whether the device is a welle-sdr.
 *
 * The device falls back to plain I/Q when the host opens or closes it, on
 * a knock with WSDR_RAW_MODE_IQ, and when no bulk transfer completes for
 * one second (the host stopped reading, e.g. an rtl_tcp client left).
 */

#ifndef WELLE_SDR_RAW_H_
#define WELLE_SDR_RAW_H_

#define WSDR_RAW_VERSION 1U

/** Mode: interleaved unsigned 8-bit I/Q, as a RTL2832U. */
#define WSDR_RAW_MODE_IQ  0U
/** Mode: framed real unsigned 8-bit ADC samples. */
#define WSDR_RAW_MODE_RAW 1U

#define WSDR_RAW_KNOCK_RATE_1      3141592U
#define WSDR_RAW_KNOCK_RATE_2      2718281U
#define WSDR_RAW_KNOCK_RATE_3_BASE 1100000U
#define WSDR_RAW_KNOCK_RATE_3_STEP 1000U
/** Knock code: protocol version in bits 7..4, mode in bits 3..0. */
#define WSDR_RAW_KNOCK_CODE(mode)  ((WSDR_RAW_VERSION << 4) | (mode))
#define WSDR_RAW_KNOCK_RATE_3(code)                                                                \
	(WSDR_RAW_KNOCK_RATE_3_BASE + WSDR_RAW_KNOCK_RATE_3_STEP * (code))
/** The device accepts each knock rate within this tolerance. */
#define WSDR_RAW_KNOCK_TOL_HZ      250U
/** Longest time between two knock rates. */
#define WSDR_RAW_KNOCK_WINDOW_MS   1000U

/* Frame header, multi-byte fields little-endian */
#define WSDR_RAW_MAGIC          "WSDR"
#define WSDR_RAW_MAGIC_LEN      4U
#define WSDR_RAW_OFF_MAGIC      0U
/** u8: WSDR_RAW_VERSION */
#define WSDR_RAW_OFF_VERSION    4U
/** u8: header length, the samples start there */
#define WSDR_RAW_OFF_HDR_LEN    5U
/** u16: WSDR_RAW_FLAG_* */
#define WSDR_RAW_OFF_FLAGS      6U
/** u32: index of the frame's first sample since raw mode started, wraps */
#define WSDR_RAW_OFF_SAMPLE_IDX 8U
/** u32: ADC rate in samples per second */
#define WSDR_RAW_OFF_ADC_RATE   12U
/** s32: IF in Hz, as programmed into the demodulator by the host */
#define WSDR_RAW_OFF_IF         16U
#define WSDR_RAW_HDR_LEN        20U

/** Spectrum inverted (librtlsdr sets it for the R820T): mix with +IF. */
#define WSDR_RAW_FLAG_INVERTED (1U << 0)
/** The band IF +/- host rate / 2 reaches above half the ADC rate. */
#define WSDR_RAW_FLAG_ALIASED  (1U << 1)

#endif /* WELLE_SDR_RAW_H_ */
