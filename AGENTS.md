<!--
Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# welle-sdr: notes for coding agents

Zephyr firmware that makes a WCH CH32V307V-EVT-R1 enumerate as an RTL-SDR dongle (RTL2832U +
R820T2, USB `0bda:2838`), so the stock librtlsdr tools work without a host driver. It streams
dummy data for now; real SDR logic and a physical R820T2 come later. This file records what is
not obvious from the code. Code follows Zephyr style (`zephyr/doc/contribute/style/`); new files
start with the SPDX header lines used in the existing files.

## Layout

- `modules/udc_wch_usbhs/`: out-of-tree Zephyr module with the UDC driver for the CH32V307 USBHS
  controller, the `wch,usbhs` binding and Kconfig. It is loaded through `ZEPHYR_EXTRA_MODULES` in
  `CMakeLists.txt`. The zephyr tree stays unmodified: keep it that way.
- `boards/ch32v307v_evt_r1_ch32v307_ram_64k.overlay`: the USBHS node (`0x40023400`, IRQ 85,
  clock `CH32V20X_V30X_CLOCK_USBHS`), labeled `zephyr_udc0`. The SoC dtsi has no USB node.
  App board overlays only match the full qualified name (`zephyr_build_string()`), not the bare
  board name: a build for another variant fails with `zephyr_udc0` undeclared.
- `src/main.c`: USB device setup (VID/PID, strings, HS and FS configurations).
- `src/rtl2832u.c`: vendor class with the librtlsdr control protocol and bulk IN streaming.
- `src/r820t2.c`: emulated tuner register file. Replace its `r820t2_i2c_write()`/`_read()` to
  talk to a real R820T2.
- `src/sample_src.c`: dummy sample source. This is the hook for the SDR signal path.
- `src/welle_sdr_shell.c`: read-only `welle-sdr` shell commands, using the accessors in
  `src/rtl2832u.h` and `src/r820t2.h`.
- `src/status_led.c`: USB state on `led0`, polled every 50 ms from `rtl2832u_get_status()` and
  `usbd_is_suspended()`. Off while not configured or suspended (an unplug reads as a suspend),
  one blink and then on when the host configures the device (also after a resume), 5 Hz blink
  while streaming.

## Hardware and host setup

- USB data goes over USBHS connector P6 (PB6 DM, PB7 DP, built-in HS PHY, no pinctrl). The
  WCH-Link debugger shows up as `/dev/ttyACM0`; its serial port carries the USART1 console at
  115200 baud.
- The board LEDs are not routed to the SoC. The status LED needs a jumper wire from J3 LED1
  to PD0 (active low). The board dtsi disables the `leds` node; the app overlay enables it.
- `west` is only available after `source ~/src/zephyrproject/.venv/bin/activate`.
- Build from the workspace root: `west build -b ch32v307v_evt_r1/ch32v307/ram_64k
  apps/welle-sdr -d apps/welle-sdr/build`, flash with `west flash -d apps/welle-sdr/build`
  (runner minichlink).
- Console: `picocom -b 115200 /dev/ttyACM0` (exit with Ctrl-A Ctrl-X). Start it before
  flashing, because the boot log is printed right after reset. For non-interactive capture
  (scripts, agents), pyserial from the venv works too.
- Target `ram_64k`: 64 KB SRAM, about 31 KB used with the shell. The variant must match the
  SRAM_CODE_MODE option bits; `west flash` does not program them. Check with
  `minichlink -r ob.bin option 16`: USER byte (offset 2) bits 7:6 = `10` is 256 KB flash /
  64 KB RAM (FLASH_OBR `0x4002201C` bits 9:8 hold the same). The board here is set that way.

## Verifying on the host

- `lsusb -t` must show the device at `480M`, and `lsusb -v -d 0bda:2838` must show bulk EP 0x81
  with `wMaxPacketSize 512` (64 means the stack picked the FS descriptors at HS).
- `rtl_test -t`: must print `Found Rafael Micro R820T tuner` and the gain list with no
  `r82xx_write: i2c wr failed` lines.
- `rtl_test` (test mode, 8-bit counter): `Samples per million lost (minimum): 0`. Stop it with
  SIGINT and run it through `stdbuf -o0 -e0`, otherwise the output is lost.
- `rtl_test -p`: reports the real sample rate every 10 s (currently about 2.25 MS/s unpaced).
- `rtl_sdr -n N`: N must not be a multiple of 131072, or rtl_sdr never stops (host tool bug:
  its stop condition is `bytes_to_read < len`).
- The Linux `dvb_usb_rtl28xxu` driver may bind first. librtlsdr detaches it; no blacklist is
  needed with the current firmware.
- To see rejected control requests, build with `-- -DCONFIG_USBD_LOG_LEVEL_INF=y`: the stack
  logs `protocol error` with a hexdump of the SETUP packet. The option sticks in the CMake
  cache; rebuild with `-p` to drop it. Expected stalls: I2C probes to 0xc8 (E4000) and 0xc6
  (FC0013).

## librtlsdr protocol (emulated in rtl2832u.c)

- Every request uses `bRequest 0`, vendor type, device recipient. Read: `0xC0`,
  `wIndex = block << 8`. Write: `0x40`, `wIndex = block << 8 | 0x10`. Blocks: DEMOD 0, USB 1,
  SYS 2, TUN 3, ROM 4, IR 5, IIC 6. 2-byte register writes are big-endian.
- Demod: `wValue = reg << 8 | 0x20`, page in `wIndex` bits 3..0. librtlsdr follows every demod
  write with a dummy read of page 0x0a reg 0x01.
  - Page 0 reg 0x19: `0x03` test mode (counter output), `0x05` normal. Test is bit 1.
  - Page 1 reg 0x01 bit 3: I2C repeater to the tuner.
  - Page 1 regs 0x9f..0xa2: resampler ratio, big-endian, high half written first. Sample rate =
    28.8 MHz * 2^22 / ratio.
  - Page 1 regs 0x19..0x1b: IF frequency.
- USB block `USB_EPA_CTL` (0x2148): `0x1002` stops and flushes the FIFO, `0x0000` runs. Streaming
  only runs while it is 0.
- SYS block `DEMOD_CTL` (0x3000): librtlsdr writes `0xe8` (demod, PLL and ADCs on) in
  `rtlsdr_open()` and `0x20` in `rtlsdr_close()`. This is the only sign of a close: librtlsdr
  does not write `USB_EPA_CTL` then, so power-off also stops the stream.
- IIC: `wValue` is the 8-bit I2C address. A NACK must be a STALL (`to_host` returns NULL,
  `to_dev` returns an error).
  - 0x20 (the demodulator itself) always ACKs. The kernel driver identifies RTL2832U against
    RTL2831U with a zero-length read there; if it fails, it loads `rtl2830`.
  - 0x34 (R820T2) ACKs only while the repeater is on.
- R820T2: reads always start at register 0 and return bit-reversed bytes. Raw byte 0 must be
  0x69 (chip ID check). Reg 2 bit 6 (logical) means PLL locked, otherwise librtlsdr prints
  `PLL not locked`. Reg 4 holds VCO fine tune (bits 5:4) and the filter calibration code (bits
  3:0). Writes to addresses beyond the 32 registers must still ACK: librtlsdr writes reg 0x21.

## Shell and logs

- USART1 runs the Zephyr shell (prompt `welle-sdr:~$`), and logs go through the shell backend.
  `welle-sdr status`, `welle-sdr tuner regs` (logical values, not bit-reversed) and
  `welle-sdr demod <page> <reg> [len]` are read-only. `SHELL_CMD_REGISTER()` cannot register a
  name with `-`, so the root entry is defined by hand in `welle_sdr_shell.c`.
- Logged at INF: test mode, sample rate, streaming start/stop, and, debounced by 20 ms after
  the last tuner or IF write, `Center frequency` and `Tuner gain` when they change. librtlsdr
  programs the PLL with several separate I2C writes, so logging per write would show
  intermediate values.
- USB events are logged from the `usbd_msg` callback in `main.c`: `USB host connected` (first
  bus reset after boot or after a suspend), `USB bus reset`, `Configured by host`,
  `USB suspended`, `USB resumed`. The USBHS controller has no VBUS detection: an unplug reads
  as `USB suspended (cable removed or host idle)`, and the idle bus before the first reset is
  a suspend that is only logged at DBG.
- Host driver milestones are logged from `rtl2832u.c`: the first vendor request after
  configuration, demod power on/off (`DEMOD_CTL`, librtlsdr open/close) and the first R820T2
  read after power-on. Right after enumeration, before any user tool runs, the host opens and
  closes the device once (probably `dvb_usb_rtl28xxu`): `Streaming started`, then a stop with
  0 transfers.
- LO from the R820T2 registers: `mix_div = 2 << (reg 0x10 >> 5)`,
  `Nint = 4 * (reg 0x14 & 0x3f) + (reg 0x14 >> 6) + 13`, `SDM = reg 0x16 << 8 | reg 0x15`,
  `LO = 2 * 28.8 MHz * (Nint + SDM / 65536) / mix_div`. The center is LO minus the demod IF
  (page 1 regs 0x19..0x1b, `-raw * 28.8 MHz / 2^22`). librtlsdr computes the fraction in kHz,
  so the center is within about 1 kHz of the request. The installed librtlsdr uses an IF of
  1.6 to 1.7 MHz depending on the sample rate, not 3.57 MHz, so do not hardcode the IF.
- Gain is the sum of the librtlsdr LNA (reg 0x05) and mixer (reg 0x07) gain steps, manual when
  reg 0x05 bit 4 is set. `rtlsdr_close()` puts the tuner in standby, which reads as auto gain.

## Zephyr USB stack details

- librtlsdr hardcodes EP 0x81. The stack assigns the first free IN endpoint, so the RTL2832U
  class must stay the only class with an IN endpoint (no CDC-ACM, no loopback in this build).
- Requests with `wLength == 0` are treated as to-device whatever bit 7 of `bmRequestType` says,
  so they arrive in `control_to_dev`.
- `control_to_dev` is called twice for writes with data: first with `buf == NULL` (accept or
  reject), then with the received data.
- Bulk IN buffers come from a header-only `UDC_BUF_POOL_DEFINE` pool with
  `CONFIG_WELLE_SDR_XFER_SIZE` data each. The size must be a multiple of 512: a short packet
  ends the host's bulk transfer.
- `usbd_ep_dequeue()` on an enabled endpoint logs `ep 0x81 is not halted|disabled` at INF, but
  still cancels the queue. That is how the FIFO flush works.

## UDC driver (udc_wch_usbhs.c)

- Register map and bit macros come from `modules/hal/wch/ch32fun/ch32v30xhw.h` (`USBHSD_TypeDef`,
  `USBHS_*`). The driver includes Zephyr's private `drivers/usb/udc/udc_common.h`, which is not
  stable API. A Zephyr update may break the build.
- PHY PLL: `RCC->CFGR2` is set up by the driver (HSE 8 MHz / 2 gives the 4 MHz reference),
  following the `eth_wch.c` precedent.
- The bus reset interrupt fires before the HS chirp finishes, so `SPEED_TYPE` still reads FS.
  `UDC_EVT_RESET` is therefore submitted at the first SETUP after a reset; do not move it back
  into the reset handler.
- `USBHS_UC_INT_BUSY` makes the controller NAK everything while an interrupt flag is pending, so
  only one completion is reported at a time. Data toggles are handled in software (flip after
  each acknowledged packet; DATA1 after SETUP).
- EP0 uses one DMA buffer for SETUP, OUT and IN. The stack queues data IN and status OUT
  together, which is safe because status is a ZLP.
- Non-control endpoint DMA buffers come from `sys_mem_blocks` with
  `sys_mem_blocks_alloc_contiguous()` (`CONFIG_UDC_WCH_USBHS_BUF_POOL_SIZE`).
- ch32fun masks the endpoint DMA address registers to 16 bits. This is unverified with more than
  64 KB SRAM: check it when switching to a larger RAM variant.

## Known limits and next steps

- Throughput is about 2.25 MS/s (4.5 MB/s), about one 512-byte packet per microframe, probably
  because every packet is re-armed from the ISR and NAKed in between. Candidates: double
  buffering (`BUF_MODE`) or DMA straight from the net_buf. Real dongles do 2.4 to 3.2 MS/s.
- The stream is unpaced; `sample_src_set_rate()` receives the host's rate but ignores it.
- The driver has not been tested with Linux `testusb` (loopback build) or with OUT endpoints
  other than EP0.
- Later phases: use a real R820T2 over I2C and replace the dummy sample source.
