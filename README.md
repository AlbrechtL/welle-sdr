<!--
Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# welle-sdr

Zephyr firmware that makes a WCH CH32V307V-EVT-R1 enumerate as an RTL-SDR dongle
(RTL2832U with an R820T2 tuner, USB `0bda:2838`) on the USBHS connector P6. The
stock librtlsdr tools (`rtl_test`, `rtl_sdr`, `rtl_tcp`) work without a new driver.
For now it streams dummy data: a byte counter in `rtl_test` test mode, otherwise
an I/Q tone at fs/16.

The USBHS device controller driver is an out-of-tree module in
`modules/udc_wch_usbhs`.

```
west build -b ch32v307v_evt_r1/ch32v307/ram_64k apps/welle-sdr
west flash
rtl_test
```

The firmware needs the 256 KB flash / 64 KB SRAM split in the chip's option bytes.

The console is on USART1 at 115200 baud, available through the WCH-Link serial port. It runs
the Zephyr shell: `welle-sdr status` shows the stream state, sample rate, center frequency and
tuner gain, and changes made by the host are logged, as are USB connection, enumeration and
host driver open/close.

LED1 shows the USB state once a jumper wire connects J3 LED1 to PD0: it is off while no host
has configured the device or the cable is removed, blinks once and then stays on when the host
configures the device, and blinks at 5 Hz while samples are streamed.
