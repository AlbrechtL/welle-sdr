/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTLSDR_EMU_SAMPLE_SRC_H_
#define RTLSDR_EMU_SAMPLE_SRC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Select the RTL2832U test mode output.
 *
 * In test mode the stream is an 8-bit counter, continuous across buffers,
 * which rtl_test checks for lost bytes. Otherwise the stream is unsigned
 * 8-bit interleaved I/Q of a tone at fs/16.
 */
void sample_src_set_test_mode(bool enable);

/** Set the sample rate configured by the host, in samples per second. */
void sample_src_set_rate(uint32_t rate);

/** Fill @p buf with the next @p len bytes of the stream. */
void sample_src_fill(uint8_t *buf, size_t len);

#endif /* RTLSDR_EMU_SAMPLE_SRC_H_ */
