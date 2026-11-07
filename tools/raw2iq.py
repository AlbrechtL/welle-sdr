#!/usr/bin/env python3
# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Convert welle-sdr raw samples to I/Q on the host.

Reference for the conversion a host does in raw mode, with the values of the
frame header (include/welle_sdr_raw.h): mix the real ADC samples by the IF
to baseband, low-pass, decimate. The output is unsigned 8-bit interleaved
I/Q like rtl_sdr writes, for iq_peaks.py or any RTL-SDR file input.

The input is a file of raw samples without headers, as wsdr_raw.py -o
writes it.
"""

import argparse

import numpy as np

TAPS = 255


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("input", help="raw samples (u8), headers stripped")
    parser.add_argument("output", help="I/Q output (u8 interleaved)")
    parser.add_argument(
        "-s", "--adc-rate", type=float, required=True, help="ADC rate from the header"
    )
    parser.add_argument(
        "--if", dest="if_hz", type=float, required=True, help="IF from the header"
    )
    parser.add_argument(
        "--inverted",
        action="store_true",
        help="header flag WSDR_RAW_FLAG_INVERTED is set",
    )
    parser.add_argument(
        "-d", "--decim", type=int, default=4, help="decimation to the output rate"
    )
    args = parser.parse_args()

    x = np.fromfile(args.input, dtype=np.uint8).astype(np.float32)
    x -= x.mean()

    # Inverted spectrum (tuner LO above the RF): RF above the center is below the IF,
    # mixing with +IF brings it to positive offsets
    sign = 1.0 if args.inverted else -1.0
    n = np.arange(x.size)
    bb = x * np.exp(sign * 2j * np.pi * (args.if_hz / args.adc_rate) * n).astype(
        np.complex64
    )

    # Low-pass at 0.45 of the output rate: Blackman-windowed sinc
    fc = 0.45 / args.decim
    k = np.arange(TAPS) - (TAPS - 1) / 2
    h = (2 * fc * np.sinc(2 * fc * k) * np.blackman(TAPS)).astype(np.float32)
    y = np.convolve(bb, h, mode="valid")[:: args.decim]

    # RMS at a quarter of full scale leaves room for peaks
    y = y / (np.sqrt(np.mean(np.abs(y) ** 2)) * 4) * 127
    iq = np.empty(2 * y.size, dtype=np.uint8)
    iq[0::2] = np.clip(np.round(y.real + 127.5), 0, 255)
    iq[1::2] = np.clip(np.round(y.imag + 127.5), 0, 255)
    iq.tofile(args.output)
    print(f"{y.size} I/Q samples at {args.adc_rate / args.decim:.0f} S/s")


if __name__ == "__main__":
    main()
