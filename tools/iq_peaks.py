#!/usr/bin/env python3
# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""List the strongest spectral lines of an rtl_sdr recording.

The file holds unsigned 8-bit interleaved I/Q (the RTL2832U format). The
lines are printed as offset from the center frequency, strongest first, in
dB relative to the strongest line.
"""

import argparse
import sys

import numpy as np


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("file", help="rtl_sdr output file (u8 I/Q)")
    parser.add_argument(
        "-s",
        "--rate",
        type=float,
        required=True,
        help="sample rate in S/s, as passed to rtl_sdr -s",
    )
    parser.add_argument(
        "-n", "--lines", type=int, default=8, help="number of lines to list"
    )
    parser.add_argument("--fft", type=int, default=65536, help="FFT length")
    parser.add_argument(
        "--skip",
        type=int,
        default=16384,
        help="samples to skip at the start (filter and USB start-up)",
    )
    args = parser.parse_args()

    raw = np.fromfile(args.file, dtype=np.uint8)
    if raw.size % 2 != 0:
        raw = raw[:-1]

    iq = (raw[0::2].astype(np.float64) - 127.5) + 1j * (
        raw[1::2].astype(np.float64) - 127.5
    )
    iq = iq[args.skip :]
    if iq.size < args.fft:
        sys.exit(f"{iq.size} samples after skipping, need {args.fft}")

    # Averaged, windowed spectrum
    window = np.blackman(args.fft)
    count = iq.size // args.fft
    power = np.zeros(args.fft)
    for k in range(count):
        seg = iq[k * args.fft : (k + 1) * args.fft]
        power += np.abs(np.fft.fft((seg - seg.mean()) * window)) ** 2
    power = np.fft.fftshift(power / count)
    freqs = np.fft.fftshift(np.fft.fftfreq(args.fft, 1.0 / args.rate))
    db = 10.0 * np.log10(power + 1e-12)

    # Local maxima, strongest first; neighbours within 3 bins belong to the same line
    lines = []
    for idx in np.argsort(db)[::-1]:
        if all(abs(idx - other) > 3 for other in lines):
            lines.append(idx)
        if len(lines) == args.lines:
            break

    ref = db[lines[0]]
    print(
        f"{iq.size} samples, {count} x {args.fft}-point FFT, "
        f"resolution {args.rate / args.fft:.1f} Hz"
    )
    print(
        f"mean I {iq.real.mean():+.2f}, mean Q {iq.imag.mean():+.2f}, "
        f"rms {np.sqrt(np.mean(np.abs(iq) ** 2)):.1f} codes"
    )
    for idx in lines:
        print(f"{freqs[idx]:+12.0f} Hz  {db[idx] - ref:6.1f} dB")


if __name__ == "__main__":
    main()
