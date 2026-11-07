#!/usr/bin/env python3
# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Convert welle-sdr raw samples to I/Q on the host.

Reference for the conversion a host does in raw mode, with the values of the
frame header (include/welle_sdr_raw.h): mix the real ADC samples by the IF
to baseband, low-pass and resample to the output rate with a polyphase
filter (any rational ratio, e.g. 6 MS/s to 2.048 MS/s is 128 / 375). The
output is unsigned 8-bit interleaved I/Q like rtl_sdr writes, for
iq_peaks.py, welle-cli -f or any RTL-SDR file input.

The input is a file of raw samples without headers, as wsdr_raw.py -o
writes it.
"""

import argparse
from fractions import Fraction

import numpy as np

# Output samples per processing chunk: the index array is CHUNK x taps
CHUNK = 1 << 15
# Pass band edge and stop band start, in output rates
PASS = 0.40
STOP = 0.50
KAISER_BETA = 8.0
# Stop band attenuation of a Kaiser window with this beta, in dB
KAISER_ATTEN_DB = 80.0


def design(up, down, in_rate):
    """Polyphase low-pass, shape (up, taps per phase), cut-off between PASS and STOP."""
    out_rate = in_rate * up / down
    # Kaiser estimate: (A - 8) / (2.285 * 2 pi * transition), transition in input rates
    transition = (STOP - PASS) * out_rate / in_rate
    taps = int(np.ceil((KAISER_ATTEN_DB - 8.0) / (2.285 * 2 * np.pi * transition)))
    n = taps * up
    cutoff = (PASS + STOP) / 2 * out_rate / (in_rate * up)
    k = np.arange(n) - (n - 1) / 2
    h = 2 * cutoff * np.sinc(2 * cutoff * k) * np.kaiser(n, KAISER_BETA) * up
    # h[k * up + phase] is tap k of phase `phase`
    return h.reshape(taps, up).T.astype(np.float32)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("input", help="raw samples (u8), headers stripped")
    parser.add_argument("output", help="I/Q output (u8 interleaved)")
    parser.add_argument(
        "-s", "--adc-rate", type=int, required=True, help="ADC rate from the header"
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
        "-r", "--out-rate", type=int, required=True, help="output rate in S/s"
    )
    args = parser.parse_args()

    ratio = Fraction(args.out_rate, args.adc_rate)
    up, down = ratio.numerator, ratio.denominator
    hp = design(up, down, args.adc_rate)
    taps = hp.shape[1]

    x = np.fromfile(args.input, dtype=np.uint8).astype(np.float32)
    x -= x.mean()

    # Inverted spectrum (tuner LO above the RF): RF above the center is below the IF,
    # mixing with +IF brings it to positive offsets
    sign = 1.0 if args.inverted else -1.0
    step = sign * 2 * np.pi * args.if_hz / args.adc_rate

    n_out = (x.size - taps) * up // down
    out = np.empty(n_out, dtype=np.complex64)
    for start in range(0, n_out, CHUNK):
        m = np.arange(start, min(start + CHUNK, n_out))
        pos = m * down
        base = pos // up + taps - 1
        phase = pos % up
        lo = base[0] - (taps - 1)
        hi = base[-1] + 1
        seg = x[lo:hi] * np.exp(1j * step * np.arange(lo, hi)).astype(np.complex64)
        # y[m] = sum_k h[k * up + phase] * x[base - k]
        idx = (base - lo)[:, None] - np.arange(taps)[None, :]
        out[m] = np.sum(seg[idx] * hp[phase], axis=1)

    # RMS at a quarter of full scale leaves room for peaks
    out *= 127 / (np.sqrt(np.mean(np.abs(out) ** 2)) * 4)
    iq = np.empty(2 * out.size, dtype=np.uint8)
    iq[0::2] = np.clip(np.round(out.real + 127.5), 0, 255)
    iq[1::2] = np.clip(np.round(out.imag + 127.5), 0, 255)
    iq.tofile(args.output)
    print(
        f"{out.size} I/Q samples at {args.out_rate} S/s ({up}/{down}, {taps} taps per phase)"
    )


if __name__ == "__main__":
    main()
