#!/usr/bin/env python3
# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Select the welle-sdr raw mode, capture the stream and check its frames.

The mode is selected with the sample rate knock of include/welle_sdr_raw.h,
through the installed librtlsdr (default) or an rtl_tcp server (--tcp), the
two ways a host such as welle.io reaches the device. The raw samples are
written to a file without the frame headers, for iq_peaks.py --real.

Exit status 0 when the stream matches the expectation: frames with the raw
mode, no frames with --mode iq or --no-knock.
"""

import argparse
import ctypes
import ctypes.util
import socket
import struct
import sys
import time

VERSION = 1
MODE_IQ = 0
MODE_RAW = 1
KNOCK_RATE_1 = 3141592
KNOCK_RATE_2 = 2718281
KNOCK_RATE_3_BASE = 1100000
KNOCK_RATE_3_STEP = 1000

MAGIC = b"WSDR"
HDR = struct.Struct("<4sBBHIIi")
FRAME_LEN = 4096
FLAG_INVERTED = 1 << 0
FLAG_ALIASED = 1 << 1

CHUNK = 16 * 32 * 512
# Host transfers kept queued, as librtlsdr's default
ASYNC_BUFS = 15
READ_CB = ctypes.CFUNCTYPE(
    None, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_uint32, ctypes.c_void_p
)


def knock_rates(mode):
    code = (VERSION << 4) | mode
    return [KNOCK_RATE_1, KNOCK_RATE_2, KNOCK_RATE_3_BASE + KNOCK_RATE_3_STEP * code]


class LocalDevice:
    """librtlsdr on a local USB device."""

    def __init__(self, index):
        name = ctypes.util.find_library("rtlsdr")
        if name is None:
            sys.exit("librtlsdr not found")
        self.lib = ctypes.CDLL(name)
        self.lib.rtlsdr_open.argtypes = [
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_uint32,
        ]
        for fn in ("set_sample_rate", "set_center_freq"):
            getattr(self.lib, "rtlsdr_" + fn).argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint32,
            ]
        self.lib.rtlsdr_set_tuner_gain_mode.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.rtlsdr_set_tuner_gain.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.rtlsdr_set_agc_mode.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.rtlsdr_reset_buffer.argtypes = [ctypes.c_void_p]
        self.lib.rtlsdr_close.argtypes = [ctypes.c_void_p]
        self.lib.rtlsdr_read_async.argtypes = [
            ctypes.c_void_p,
            READ_CB,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]
        self.lib.rtlsdr_cancel_async.argtypes = [ctypes.c_void_p]
        self.lib.rtlsdr_get_usb_strings.argtypes = [ctypes.c_void_p] + [
            ctypes.c_char_p
        ] * 3

        self.dev = ctypes.c_void_p()
        if self.lib.rtlsdr_open(ctypes.byref(self.dev), index) != 0:
            sys.exit(f"cannot open device {index}")

        strings = [ctypes.create_string_buffer(256) for _ in range(3)]
        self.lib.rtlsdr_get_usb_strings(self.dev, *strings)
        print("Device:", ", ".join(s.value.decode(errors="replace") for s in strings))

    def set_rate(self, rate):
        if self.lib.rtlsdr_set_sample_rate(self.dev, rate) != 0:
            sys.exit(f"set_sample_rate({rate}) failed")

    def set_freq(self, freq):
        self.lib.rtlsdr_set_center_freq(self.dev, freq)

    def set_auto_gain(self):
        self.lib.rtlsdr_set_tuner_gain_mode(self.dev, 0)

    def set_gain(self, tenth_db):
        self.lib.rtlsdr_set_tuner_gain_mode(self.dev, 1)
        self.lib.rtlsdr_set_tuner_gain(self.dev, tenth_db)

    def set_rtl_agc(self, on):
        self.lib.rtlsdr_set_agc_mode(self.dev, 1 if on else 0)

    def start(self):
        self.lib.rtlsdr_reset_buffer(self.dev)

    def read(self, size):
        # Asynchronous, so that host transfers stay queued while Python copies
        chunks = []
        total = 0

        def callback(buf, length, _ctx):
            nonlocal total
            if total < size:
                chunks.append(ctypes.string_at(buf, length))
                total += length
                if total >= size:
                    self.lib.rtlsdr_cancel_async(self.dev)

        cb = READ_CB(callback)
        if self.lib.rtlsdr_read_async(self.dev, cb, None, ASYNC_BUFS, CHUNK) != 0:
            sys.exit("read_async failed")
        return b"".join(chunks)[:size]

    def close(self):
        self.lib.rtlsdr_close(self.dev)


class TcpDevice:
    """rtl_tcp server: 12-byte greeting, then samples; 5-byte commands."""

    def __init__(self, addr):
        host, _, port = addr.rpartition(":")
        self.sock = socket.create_connection((host, int(port)))
        greeting = self.recv_exact(12)
        magic, tuner, gains = struct.unpack(">4sII", greeting)
        if magic != b"RTL0":
            sys.exit(f"no rtl_tcp greeting: {greeting!r}")
        print(f"rtl_tcp: tuner type {tuner}, {gains} gains")

    def recv_exact(self, size):
        out = bytearray()
        while len(out) < size:
            data = self.sock.recv(min(1 << 20, size - len(out)))
            if not data:
                sys.exit("rtl_tcp closed the connection")
            out += data
        return bytes(out)

    def command(self, cmd, param):
        self.sock.sendall(struct.pack(">BI", cmd, param & 0xFFFFFFFF))

    def set_rate(self, rate):
        self.command(0x02, rate)

    def set_freq(self, freq):
        self.command(0x01, freq)

    def set_auto_gain(self):
        self.command(0x03, 0)

    def set_gain(self, tenth_db):
        self.command(0x03, 1)
        self.command(0x04, tenth_db)

    def set_rtl_agc(self, on):
        self.command(0x08, 1 if on else 0)

    def start(self):
        """rtl_tcp streams from the connection on."""

    def read(self, size):
        return self.recv_exact(size)

    def close(self):
        self.sock.close()


def parse_hdr(data, pos):
    """Header at pos, or None."""
    if pos + HDR.size > len(data):
        return None
    magic, version, hdr_len, flags, idx, adc_rate, if_hz = HDR.unpack_from(data, pos)
    if (
        magic != MAGIC
        or version != VERSION
        or hdr_len < HDR.size
        or hdr_len >= FRAME_LEN
    ):
        return None
    return {
        "hdr_len": hdr_len,
        "flags": flags,
        "idx": idx,
        "adc_rate": adc_rate,
        "if": if_hz,
    }


def find_sync(data, start):
    """Offset of a header confirmed by the next frame's header, or -1."""
    pos = data.find(MAGIC, start)
    while pos >= 0:
        first = parse_hdr(data, pos)
        second = parse_hdr(data, pos + FRAME_LEN)
        if first is not None and second is not None:
            payload = FRAME_LEN - first["hdr_len"]
            if (second["idx"] - first["idx"]) % (1 << 32) >= payload:
                return pos
        pos = data.find(MAGIC, pos + 1)
    return -1


def deframe(data):
    """Samples, headers seen, lost samples and the stream offset of the first frame."""
    samples = bytearray()
    headers = []
    lost = 0
    expected = None
    first = find_sync(data, 0)
    pos = first
    while pos >= 0 and pos + FRAME_LEN <= len(data):
        hdr = parse_hdr(data, pos)
        if hdr is None:
            pos = find_sync(data, pos + 1)
            expected = None
            continue
        if expected is not None:
            lost += (hdr["idx"] - expected) % (1 << 32)
        headers.append(hdr)
        samples += data[pos + hdr["hdr_len"] : pos + FRAME_LEN]
        expected = (hdr["idx"] + FRAME_LEN - hdr["hdr_len"]) % (1 << 32)
        pos += FRAME_LEN
    return bytes(samples), headers, lost, first


def describe(hdr):
    flags = []
    if hdr["flags"] & FLAG_INVERTED:
        flags.append("inverted")
    if hdr["flags"] & FLAG_ALIASED:
        flags.append("aliased")
    return (
        f"ADC {hdr['adc_rate']} S/s, IF {hdr['if']} Hz"
        + (f", {', '.join(flags)}" if flags else "")
        + f", sample index {hdr['idx']}"
    )


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "-d", "--device", type=int, default=0, help="librtlsdr device index"
    )
    parser.add_argument(
        "--tcp", metavar="HOST:PORT", help="use an rtl_tcp server instead"
    )
    parser.add_argument(
        "-s", "--rate", type=int, default=1024000, help="host sample rate"
    )
    parser.add_argument(
        "-f", "--freq", type=int, default=100000000, help="center frequency"
    )
    parser.add_argument(
        "-n", "--bytes", type=int, default=8000000, help="bytes to capture"
    )
    parser.add_argument(
        "-g", "--gain", type=float, help="tuner gain in dB (default auto)"
    )
    parser.add_argument(
        "--rtl-agc", action="store_true", help="RTL AGC on (welle-sdr: IF AGC loop)"
    )
    parser.add_argument(
        "--mode", choices=["raw", "iq"], default="raw", help="mode to knock"
    )
    parser.add_argument(
        "--no-knock", action="store_true", help="capture without a knock"
    )
    parser.add_argument(
        "-o", "--output", help="file for the raw samples, headers stripped"
    )
    args = parser.parse_args()

    dev = TcpDevice(args.tcp) if args.tcp else LocalDevice(args.device)
    mode = MODE_RAW if args.mode == "raw" else MODE_IQ
    try:
        # Frequency first: every rate change retunes the tuner to it
        dev.set_freq(args.freq)
        if not args.no_knock:
            for rate in knock_rates(mode):
                dev.set_rate(rate)
        dev.set_rate(args.rate)
        if args.gain is None:
            dev.set_auto_gain()
        else:
            dev.set_gain(round(args.gain * 10))
        dev.set_rtl_agc(args.rtl_agc)
        dev.start()
        start = time.monotonic()
        data = dev.read(args.bytes)
        elapsed = time.monotonic() - start
    finally:
        dev.close()

    print(
        f"Captured {len(data)} bytes in {elapsed:.2f} s ({len(data) / elapsed / 1e6:.2f} MB/s)"
    )
    samples, headers, lost, first = deframe(data)
    expect_frames = mode == MODE_RAW and not args.no_knock

    if not headers:
        print("No raw frames: plain I/Q stream")
        return 1 if expect_frames else 0

    print(f"First frame at byte {first}")
    print(f"Frames: {len(headers)}, samples {len(samples)}, lost samples {lost}")
    print("First: " + describe(headers[0]))
    changes = [
        h
        for a, h in zip(headers, headers[1:])
        if (a["adc_rate"], a["if"], a["flags"]) != (h["adc_rate"], h["if"], h["flags"])
    ]
    for hdr in changes:
        print("Change: " + describe(hdr))
    print("Last:  " + describe(headers[-1]))

    if args.output:
        with open(args.output, "wb") as out:
            out.write(samples)
        print(f"Wrote {len(samples)} samples to {args.output}")

    return 0 if expect_frames else 1


if __name__ == "__main__":
    sys.exit(main())
