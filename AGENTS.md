<!--
Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# welle-sdr: notes for coding agents

Zephyr firmware that makes a WCH CH32V307V-EVT-R1 enumerate as an RTL-SDR dongle (RTL2832U +
R820T2, USB `0bda:2838`), so the stock librtlsdr tools work without a host driver. Tuner
requests go to a real R820T2 on a bit-banged I2C bus; the samples come from an AD9280 on the DVP,
down-converted on the MCU (README "I/Q stream"). This
file records what is not obvious from the code. Code follows Zephyr style
(`zephyr/doc/contribute/style/`); new files start with the SPDX header lines used in the
existing files.

## Layout

- `modules/udc_wch_usbhs/`: out-of-tree Zephyr module with the UDC driver for the CH32V307 USBHS
  controller, the `wch,usbhs` binding and Kconfig. IN endpoints other than EP0 send from the
  net_buf (DMA address per packet, 16-bit window) and chain transfers in the ISR, finished
  buffers go to a per-endpoint done list for the thread; EP0 and OUT copy. It is loaded through `ZEPHYR_EXTRA_MODULES` in
  `CMakeLists.txt`. The zephyr tree stays unmodified: keep it that way.
- `boards/ch32v307v_evt_r1_ch32v307_ram_64k.overlay`: the USBHS node (`0x40023400`, IRQ 85,
  clock `CH32V20X_V30X_CLOCK_USBHS`), labeled `zephyr_udc0`. The SoC dtsi has no USB node.
  App board overlays only match the full qualified name (`zephyr_build_string()`), not the bare
  board name: a build for another variant fails with `zephyr_udc0` undeclared.
- `src/main.c`: USB device setup (VID/PID, strings, HS and FS configurations).
- `src/rtl2832u.c`: vendor class with the librtlsdr control protocol and bulk IN streaming.
- `src/r820t2.c`: R820T2 on I2C. Forwards host reads and writes to the chip and keeps a register
  shadow (regs 0 to 4 as last read, 5 to 31 as last written) for the shell and the LO/gain
  logs. Also the wiring checks, the bus scan and the standalone tune for the shell test.
- `src/si_fmt.c`: `si_fmt()` formats frequencies and sample rates with k/M/G prefixes, all
  digits kept (`435.544958 MHz`), for the shell and the logs.
- `modules/adc_dvp_wch/`: out-of-tree module with the AD9280 capture driver on the DVP
  (`wch,dvp-adc` binding, `include/dvp_adc.h` API: start/stop, get/release block, stats).
  Loaded through `ZEPHYR_EXTRA_MODULES` like the UDC module. Node `adc_dvp` in the overlay
  (DVP at `0x50050000`, IRQ 86, clock `CH32V20X_V30X_CLOCK_DVP`, DCLK from `&pwm1` on PA8 with
  `&tim1` prescaler 0, HSYNC/VSYNC from PA0/PA1). README "ADC test" lists the DVP behavior
  the driver depends on (sync edges need a running PCLK, VSYNC before HSYNC, second block
  interrupt without buffer switch under load).
- `src/sample_src.c`: stream source. Test mode: byte counter pulled by `rtl_stream_kick()`.
  Otherwise a thread (stack 2048: 1024 overflowed silently and reset the MCU) takes ADC blocks,
  runs `ddc_process()` into `rtl2832u_alloc_xfer()` buffers and submits them with
  `rtl2832u_submit()`; it re-plans when the host changes rate, IF, inversion or mode. In raw
  mode it frames the ADC blocks instead (one frame per transfer, a new transfer always starts
  a frame: drop the partial transfer on any format or ADC rate change) and ends the mode when
  `rtl2832u_xfers()` stops moving for `CONFIG_WELLE_SDR_RAW_IDLE_MS`, then flushes the queued
  frames with `rtl2832u_flush()`.
- `include/welle_sdr_raw.h`: raw mode protocol (knock rates, frame header), plain C for host
  software such as welle.io: keep it free of Zephyr includes and the version in step with any
  format change. `src/raw_knock.c` decodes the knock from the rates `rtl_demod_written()`
  decodes; `src/raw_frame.c` writes the frames and plans the raw ADC rate (only exact
  divisors of 144 MHz: `dvp_adc_start()` rejects others). Both pure C, host tests in
  `tests/raw_mode`.
- `src/ddc.c`: down-converter (fs / 4 mixer, CIC2, rotation) and rate plan with a CPU load
  model. Pure C, built with `-O2` (2.2x faster than `-Os`), host tests in `tests/ddc`
  (`west twister -p native_sim -T apps/welle-sdr/tests/ddc`), including a bit-exact
  per-sample reference: keep it in sync when the arithmetic changes.
- `src/testsig.c`: square wave on PA7 (TIM3_CH2) for AD IN, registers programmed directly
  (Zephyr's `pwm_wch_gptm.c` does not build with a GPTM and an ADTM instance enabled).
- `src/if_agc.c`: PWM on PA15 (TIM2_CH1, partial remap 1, programmed directly for the same
  reason) into the tuner board's IF_AGC low-pass. Node `if_agc` (`welle,if-agc`, binding in
  `dts/bindings/`). The loop runs in the producer thread from `producer_block()` while the
  host's RTL AGC and the PWM are on, and only writes `CH1CVR`. The PWM is off after boot
  (`CONFIG_WELLE_SDR_IF_AGC_PWM`, `welle-sdr agc pwm on`), PA15 then stays at 0 V and 0x0c bit 4
  is cleared in every write. The PWM edges break the bit-banged tuner
  I2C, so `r820t2.c` floats PA15 around every transfer with `if_agc_bus_hold()`; keep that
  call around any new tuner bus access. `r820t2_set_vga_pin_mode()` keeps register
  0x0c bit 4 set in host writes meanwhile. `src/if_agc_ctl.c` holds the arithmetic (pure C,
  host tests in `tests/if_agc`).
- `tools/iq_peaks.py`: strongest spectral lines of an `rtl_sdr` recording, `--real` for raw
  samples.
- `tools/wsdr_raw.py`: raw mode knock, capture and frame check through librtlsdr (ctypes) or
  rtl_tcp (`--tcp`). `tools/raw2iq.py`: host conversion of raw samples to u8 I/Q (reference
  for welle.io). An `rtl_tcp` that got SIGTERM can keep running and hold the device
  (`usb_claim_interface error -6`), or keep port 1234 with a stale handle: check `pgrep -a
  rtl_tcp` before a test.
- `src/welle_sdr_shell.c`: `welle-sdr` shell commands, using the accessors in
  `src/rtl2832u.h`, `src/r820t2.h` and `dvp_adc.h` (`welle-sdr adc ...`).
- `src/status_led.c`: USB state on `led0`, polled every 50 ms from `rtl2832u_get_status()` and
  `usbd_is_suspended()`. Off while not configured or suspended (an unplug reads as a suspend),
  one blink and then on when the host configures the device (also after a resume), 5 Hz blink
  while streaming.

## Hardware and host setup

- USB data goes over USBHS connector P6 (PB6 DM, PB7 DP, built-in HS PHY, no pinctrl). The
  WCH-Link debugger shows up as `/dev/ttyACM0`; its serial port carries the USART2 console at
  115200 baud (PA2 TX, PA3 RX, jumper wires from the WCH-Link header J2 pins 3/5; PA9/PA10
  are ADC data inputs, so `&usart1` is disabled in the overlay).
- R820T2: SCL on PB10, SDA on PB11, GND, 3.3 V, external pull-ups to 3.3 V, 28.8 MHz crystal
  on the tuner side. 7-bit address 0x1a. PA15 drives the RC low-pass on the tuner board's
  IF_AGC pin.
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
- `rtl_test -p`: reports the rate every 10 s. It runs in test mode (unpaced counter, about
  4.57 MS/s: the USB throughput benchmark), so it says nothing about the I/Q stream; check that with `rtl_sdr` and
  `tools/iq_peaks.py` (README "I/Q stream").
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
  - Page 0 reg 0x19: `0x03` test mode (counter output), `0x05` normal, `0x25` normal with RTL
    AGC (`rtlsdr_set_agc_mode()`). Test is bit 1, RTL AGC bit 5; RTL AGC turns on the IF AGC.
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
  Any tuner bus error is passed on as a STALL, so a missing tuner reads as `No supported tuner
  found`, as on a real dongle.

## Tuner I2C

- The bus is Zephyr's `gpio-i2c` bit-bang driver on PB10/PB11 (node `r820t2_i2c`), 100 kHz.
  The I2C2 controller on the same pins is not used: with the in-tree `i2c_wch.c` the firmware
  hung within a few `rtl_sdr` runs. Details in the README ("Tuner I2C is bit-banged"):
  `CTLR2.FREQ` overflow at the 144 MHz APB1 clock, a race in reads of 3 or more bytes that
  leaves SCL held low, and `wch_i2c_finish_transfer()` spinning on `STAR2.BUSY` without a
  timeout. A thread spinning there at a higher priority starves the shell, so the board looks
  dead; attach with `minichlink -A -G` (halt without reset) and GDB on port 3333 to see it.
- Before each transfer `bus_guard()` checks the pull-ups (re-checked until they are found) and
  that both lines are high. Pull-up check: input with the internal pull-down, a line stays high
  only with an external pull-up; then the pins go back to open-drain output high.
- PLL lock: after a host write to regs 0x14..0x16, the next host read polls until reg 2 bit 6 is
  set, at most `R820T2_LOCK_POLLS` reads (at least 5 ms). librtlsdr checks lock with that read,
  and without the wait it reads before the PLL has locked (up to about 3.5 ms measured). The
  polls are counted, not timed: `sys_clock_cycle_get_32()` in `wch_systick_ch32v00x.c` can jump
  back by up to one 10 ms tick (CNT reloads before the ISR adds the tick), so cycle-based loops
  such as `k_busy_wait()` and the bit-bang delays can end early. Sleeps last a whole 10 ms tick
  (`CONFIG_SYS_CLOCK_TICKS_PER_SEC=100`), so the poll uses `k_busy_wait()`.
- librtlsdr programs the PLL four times per open: power-on values (0x14 = 0x0f, lock not
  checked), filter calibration at 56 MHz (0x14 = 0x84), the IF alone with center frequency 0
  (0x10 = 0x04, 0x14 = 0x7e, outside the VCO range, locks only by chance: this is the
  `PLL not locked!` message), then the requested frequency.
- `CONFIG_I2C_SHELL` is enabled; its device name is `r820t2-i2c`. Its commands bypass
  `bus_guard()` and `tuner_lock`: do not use them while a host tool runs.
- `tuner_lock` serializes bus access between the usbd thread (host requests) and the shell.
  Shadow readers do not lock.
- The standalone power-on register set (`init_regs`) is the commonly published one (as in
  librtlsdr and the Linux driver); it has not been checked against the Rafael datasheet. With it
  the standalone tune locked from 28 to 1700 MHz.

## Shell and logs

- USART2 runs the Zephyr shell (prompt `welle-sdr:~$`), and logs go through the shell backend.
  `welle-sdr status`, `welle-sdr tuner regs [live]` (hex dump and decoded fields of the shadow
  or of a chip read, logical values, not bit-reversed) and `welle-sdr demod <page> <reg> [len]`
  are read-only. Field decoding is `r820t2_decode()` in `r820t2.c`, shared with the LO and gain
  values in status and logs. It decodes only fields whose meaning librtlsdr's register use
  confirms; registers 0x0a/0x0b (filter bandwidth), reg 3 and the standby bits are not decoded.
  `welle-sdr tuner test` reads the chip: wiring report, scan, chip ID, a hint on failure.
  `welle-sdr tuner test <lo_hz>` writes the tuner and returns -EBUSY while the host has the
  device open (`DEMOD_CTL` power on). `SHELL_CMD_REGISTER()` cannot register a
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

- Throughput: 9.15 MB/s in `rtl_test -p`, about 7.5 MB/s with the raw producer. The IN DMA
  reads straight from the net_buf and the ISR starts the next queued transfer, so neither a
  copy nor a thread sits between packets or transfers (4.75 MB/s before). What is left: the
  endpoint NAKs while the ISR re-arms it after each packet (`USBHS_UC_INT_BUSY`, 4 us ISR),
  and the host retries within 20..40 us or only one or two microframes later (measured). A
  hardware ping-pong without INT_BUSY would need the ISR to re-arm a buffer before the
  controller resends it, else the host gets stale data with a valid toggle: not done. The
  test-mode counter is filled word-wise; with the byte loop it limited `rtl_test -p` to
  6.5 MB/s (the usbd thread is cooperative, priority -8, and fills in its context).
- The counter stream in test mode is unpaced; the I/Q stream is paced by the ADC.
- CPU: the down-converter fits 1 MS/s (55 % load), not 2 MS/s; 2.048 MS/s is not an exact
  plan. See README "I/Q stream" for the cost model and limits. `dvp_adc_start()` takes the PWM
  clock over an integer period rounded to 1 Hz (`DDC_CLOCK_HZ / period` truncated is
  rejected when it differs). Inexact plans are chosen by rate error, not load: 1.024 and
  2.048 MS/s at librtlsdr's R820T IF run above the CPU budget and lose ADC blocks.
- Raw mode (README "Raw mode") is capped at 6 MS/s (DAB at librtlsdr's IF of 1.625 MHz),
  measured lossless; 8 MS/s loses about 6 %. The ADC level is too low for welle-cli to sync
  on 5C (analog front end, README "IF AGC").
- Timing on the target: `k_cycle_get_32()` jumps back by one 10 ms tick, so time only
  intervals shorter than a tick with it (one call per measurement); a 10-call loop over
  1.7 M cycles read as 0.3 M and hid that the first DDC was 6x too slow.
- The driver has not been tested with Linux `testusb` (loopback build) or with OUT endpoints
  other than EP0.
- Later phases: 2 MS/s and above (faster general path, or a lower-cost architecture), CIC
  droop compensation, test with the R820T2 (spectrum inversion direction to be confirmed).
