<!--
Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# rtlsdr_emu

Zephyr firmware that makes a WCH CH32V307V-EVT-R1 enumerate as an RTL-SDR dongle
(RTL2832U with an R820T2 tuner, USB `0bda:2838`) on the USBHS connector P6. The
stock librtlsdr tools (`rtl_test`, `rtl_sdr`, `rtl_tcp`) work without a new driver.
For now it streams dummy data: a byte counter in `rtl_test` test mode, otherwise
an I/Q tone at fs/16.

The USBHS device controller driver is an out-of-tree module in
`modules/udc_wch_usbhs`.

```
west build -b ch32v307v_evt_r1 apps/rtlsdr_emu
west flash
rtl_test
```

The console is on USART1 at 115200 baud, available through the WCH-Link serial port.
