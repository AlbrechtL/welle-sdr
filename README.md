<!--
Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# welle-sdr

Zephyr firmware that makes a WCH CH32V307V-EVT-R1 enumerate as an RTL-SDR dongle
(RTL2832U with an R820T2 tuner, USB `0bda:2838`) on the USBHS connector P6. The
stock librtlsdr tools (`rtl_test`, `rtl_sdr`, `rtl_tcp`) work without a new driver.
The host's tuner requests go to a real R820T2 on I2C. The samples come from an AD9280 ADC
on the DVP, converted to I/Q on the MCU (see "I/Q stream"); in `rtl_test` test mode the
stream is a byte counter.

The USBHS device controller driver is an out-of-tree module in
`modules/udc_wch_usbhs`.

```
west build -b ch32v307v_evt_r1/ch32v307/ram_64k apps/welle-sdr
west flash
rtl_test
```

The firmware needs the 256 KB flash / 64 KB SRAM split in the chip's option bytes.

The console is on USART2 (PA2 TX, PA3 RX) at 115200 baud, available through the WCH-Link
serial port; this needs two wires on the EVT board, see "Board modifications on the EVT
board". It runs
the Zephyr shell: `welle-sdr status` shows the stream state, sample rate, center frequency and
tuner gain, and changes made by the host are logged, as are USB connection, enumeration and
host driver open/close.

LED1 shows the USB state once a jumper wire connects J3 LED1 to PD0: it is off while no host
has configured the device or the cable is removed, blinks once and then stays on when the host
configures the device, and blinks at 5 Hz while samples are streamed.

## Tuner wiring

| R820T2                     | CH32V307V-EVT-R1 |
|----------------------------|------------------|
| SCL                        | PB10             |
| SDA                        | PB11             |
| IF_AGC (RC low-pass input) | PA15             |
| GND                        | GND              |
| VCC                        | 3.3 V            |

- SCL and SDA need external pull-ups to 3.3 V (4.7 kOhm), unless the tuner board has them.
- GND of the tuner board must be connected to GND of the EVT board.
- The R820T2 needs its 28.8 MHz crystal or reference clock.
- 7-bit I2C address 0x1a (librtlsdr uses the 8-bit form 0x34), 100 kHz.
- PA15 drives the RC low-pass in front of the tuner's IF_AGC pin (pin 14) with a PWM, see
  "IF AGC". Without the low-pass the VGA sees the 3.3 V square wave.

Without a responding tuner the host reports `No supported tuner found`, as a real dongle
without a tuner would.

## Tuner I2C is bit-banged

The tuner bus runs on Zephyr's GPIO bit-bang I2C driver (`gpio-i2c`, node `r820t2-i2c` in the
board overlay) on PB10/PB11. PB10/PB11 are also the pins of the I2C2 controller, but its in-tree
driver (`drivers/i2c/i2c_wch.c`) is not reliable on this board:

- APB1 runs undivided at 144 MHz. The driver writes this clock in MHz into the 6-bit
  `CTLR2.FREQ` field, which then reads 16.
- Reads of 3 or more bytes, which librtlsdr does all the time, can stop before the last byte:
  the driver changes the acknowledge setting from the interrupt handler after the controller
  may already be receiving the next byte. The controller then holds SCL low.
- After a failed or timed-out transfer, `wch_i2c_finish_transfer()` waits for `STAR2.BUSY` to
  clear without a timeout. With SCL held low that never happens: the calling thread spins
  forever and the device drops off USB.

With the I2C2 controller the tuner answered, but the firmware hung within a few `rtl_sdr` runs.
The bit-bang driver has no such wait. It costs CPU time: the calling thread is busy for the
whole transfer, about 0.1 ms per byte at 100 kHz.

To try the controller again after a driver fix: enable `&i2c2` in the board overlay with
pinctrl group `<I2C2_SCL_PB10_0>, <I2C2_SDA_PB11_0>` and `output-high; drive-open-drain;`
(without `output-high` the CH32V20x/30x pinctrl driver leaves the pins as analog inputs), and
point `r820t2.c` at it.

## Tuner test

`welle-sdr tuner test` checks the wiring and reads the chip:

```
Pull-ups:     SCL ok, SDA ok
Idle levels:  SCL high, SDA high
Bus scan:     0x1a
Chip ID:      ok (raw 0x69)
PLL lock:     yes
VCO fine:     2 (nominal 2)
Filter cal:   6
Registers read from the chip (not bit-reversed):
00000000: 96 b3 c5 ff e6 83 32 75  c0 40 d6 6c f0 63 75 6c |......2u .@.l.cul|
00000010: 8c 83 80 31 84 72 1c 30  48 cc 60 00 54 86 4a 00 |...1.r.0 H.`.T.J.|
```

- Pull-ups: each line is read with the internal pull-down on; it stays high only with an
  external pull-up.
- Idle levels: both lines must be high while the bus is idle.
- Bus scan: addresses that acknowledge.
- If the chip does not answer, a `Hint:` line names the likely fault (missing pull-up, line
  held low, no answer: swapped or open lines, tuner power, GND).

`welle-sdr tuner test <lo_hz>` initializes the tuner and tunes the PLL to `lo_hz` without a host
(27.66 to 1770 MHz), then reports PLL lock. It is refused while a host tool has the device
open; the host re-initializes the tuner when it opens the device.

Zephyr's I2C shell is enabled for cross-checks: `i2c scan r820t2-i2c` and
`i2c direct_read r820t2-i2c 1a 5` (plain read, as the R820T2 is read). Do not use it while a
host tool has the device open.

`welle-sdr tuner regs` dumps and decodes the register shadow (registers 0 to 4 as last read
from the chip, 5 to 31 as last written); `welle-sdr tuner regs live` reads the registers from
the chip instead. Reading is safe while a host tool streams. Example, read while `rtl_sdr -f
433.92e6 -g 19.7` runs:

```
Chip ID:      ok
PLL lock:     yes
VCO fine:     2 (nominal 2)
Filter cal:   4
LNA gain:     manual, index 6: 14.4 dB
Mixer gain:   manual, index 5: 5.3 dB
VGA code:     8
PLL:          Nint 60, SDM 32267/65536 (on), reference 28.8 MHz
VCO current:  4
Autotune:     8 kHz
VCO:          3.484359667 GHz
LO:           435.544958 MHz (VCO / 8)
```

The VCO runs at 1.77 to 3.54 GHz and is divided down to the LO. The LO is the center frequency
plus the IF (here 1.625 MHz). Gains are librtlsdr's gain steps
for the programmed indices. After a host tool closes the device, librtlsdr puts the tuner in
standby: the PLL reads unlocked and the gains read as auto.

While samples stream, the shell can drop typed characters; type slowly or paste line by line.

## IF AGC

The R820T2 has an RF AGC loop for the LNA and the mixer, but none for the IF VGA. librtlsdr
sets the VGA to a fixed code (26.5 dB in auto gain mode, 16.3 dB in manual mode), so the
level at the 8-bit ADC follows the signal. The firmware closes that loop itself: register
0x0c bit 4 makes the VGA gain follow the voltage on the IF_AGC pin (0.6 V to 2.5 V gives
about -1.5 dB to +45.5 dB in the datasheet), and a PWM on PA15 sets that voltage through
the RC low-pass on the tuner board.

- PWM: TIM2_CH1 on PA15 (TIM2 partial remap 1), 144 MHz / 4096 = 35.2 kHz, 12 bits. TIM2 is
  programmed directly in `src/if_agc.c`, like TIM3 for the test signal.
- The PWM is off after boot: PA15 stays at 0 V, the VGA gain comes from its register (0x0c
  bit 4 cleared in every write, also over librtlsdr's register init, which selects pin
  control) and the loop does not run. `welle-sdr agc pwm on` starts it;
  `CONFIG_WELLE_SDR_IF_AGC_PWM=y` starts it at boot.
- With the PWM on, the host's RTL AGC switch turns the loop on (`rtlsdr_set_agc_mode()`, rtl_tcp command 0x08:
  demod page 0 reg 0x19 bit 5). librtlsdr writes 0x05 to that register on every open, so
  each session starts with the loop off. The switch is otherwise unused, as the RTL2832U's
  digital AGC is not emulated.
- With the loop on, the firmware sets 0x0c bit 4 in every host write, so librtlsdr's gain
  changes do not take the VGA back. LNA and mixer gain stay with the host (manual or RF AGC).
  With the loop off, the host's last 0x0c value is written back and PA15 returns to the fixed
  voltage (`CONFIG_WELLE_SDR_IF_AGC_FIXED_MV`, 1.55 V, about +22 dB).
- Loop: every 16th ADC sample (`CONFIG_WELLE_SDR_IF_AGC_STRIDE`) over 20 ms
  (`CONFIG_WELLE_SDR_IF_AGC_RATE_HZ`) gives the mean absolute deviation (MAD) of the codes
  from their mean. More than 1 in 1024 samples at code 0 or 255 steps the gain down by 3 dB.
  Otherwise the gain moves by half the level error in dB, at most 3 dB, with a dead band of
  1 dB. The target MAD is 24 codes (`CONFIG_WELLE_SDR_IF_AGC_TARGET`), an RMS of about 30
  codes and about 12 dB below full scale. The voltage stays within
  `CONFIG_WELLE_SDR_IF_AGC_MIN_MV` and `_MAX_MV`.
- PA15 is a floating input during every tuner I2C transfer and returns to the PWM afterwards;
  the low-pass capacitor holds the voltage meanwhile. The PWM edges couple into the
  bit-banged I2C on the jumper wiring: with PA15 always driven, 2 to 5 of 10 standalone tunes
  failed with a NACK and librtlsdr reported `i2c wr failed` on almost every open. With a
  constant level on PA15, and with the floating, 0 failures (30 tunes, 8 librtlsdr opens).
  The loop level does not change while the shell reads the registers 45 times during
  streaming. Keep the PA15 wire away from SCL and SDA and run a ground wire next to it.
- Measured on 5C (178.352 MHz) with an antenna, through the module front end: PA15 from 0 V
  to 3.3 V takes the ADC signal from 3 to 64 codes peak to peak (standalone tune, wide IF
  filter). With librtlsdr at 1 MS/s the level is a MAD of 1.6 codes at 2500 mV, so the
  default target is out of reach; with `welle-sdr agc target 1` the loop settles at 2.0 V
  to 2.35 V with a MAD of 0.9 to 1.1 codes.
- The step size assumes the datasheet's linear curve of 47 dB over 1.9 V and a low-pass that
  settles within 20 ms. Neither is measured on this tuner board yet.
- Through the AD9280 module's front end (`V_adc = 0.99 V + V_in / 5.14`, see "ADC test") the
  tuner's 2 Vpp maximum IF reaches about 50 codes peak to peak, a MAD of about 16 for a sine.
  The default target is then out of reach and the loop stays at the upper end of the range.
  Lower the target to about 8 codes with this path, or feed the ADC directly
  (`docs/ad9280-adc-module.md`, "Limits of this path").

`welle-sdr agc` shows the mode, the voltage and the approximate VGA gain, the PWM compare
value, and with the loop on the measured MAD, mean code, clip rate and update count.
`welle-sdr agc fixed <mV>` sets the voltage used while the loop is off, to measure the
voltage to gain curve. `welle-sdr agc target <codes>` sets the target MAD.
`welle-sdr agc pwm off` stops the PWM: PA15 stays at 0 V, the VGA gain comes from its register
(0x0c bit 4 cleared in every write) and the loop pauses; `on` runs it again.
`welle-sdr tuner vga off` powers the VGA off (0x0c bit 6 cleared in every write, also by the
standalone tune); `on` follows the written value again.

Noise check on 5C at 1 MS/s (averaged spectrum of `rtl_sdr` recordings, RTL AGC off): the
noise floor is 46 dB with the PWM on and off alike, and 11 dB with the VGA off, so the
noise comes through the tuner's IF chain, not from the PWM or the ADC. With the VGA on the
IF_AGC pin (RTL AGC on, 2500 mV) the lines at +/-35.16 kHz and +/-70.3 kHz around the
strongest line are at the noise floor: no PWM ripple on the gain. At 2500 mV the level is
about 13 dB lower than with librtlsdr's register code (26.5 dB), less than the datasheet
curve gives; the voltage at pin 14 is not measured. `welle-sdr status` shows the IF AGC line,
and `welle-sdr tuner regs` shows the VGA mode and the gains the RF AGC picked (register 3).

The loop's arithmetic has host tests: `west twister -p native_sim -T apps/welle-sdr/tests/if_agc`.

## PLL lock with librtlsdr

librtlsdr checks the PLL lock bit with the first read after programming the PLL. That read
reaches the chip sooner than through a real RTL2832U, before the PLL has locked (measured: up
to about 3.5 ms). The firmware therefore polls the lock bit for at least 5 ms before it answers
that read.

`[R82XX] PLL not locked!` still appears in about every second `rtl_sdr` or `rtl_test` run. It
comes from a tune during open, before the requested frequency is set, where librtlsdr programs
the PLL for the IF alone, outside the VCO range. The requested frequency locks. A real dongle
gets the same request from librtlsdr.

## ADC module wiring (AD9280)

The AD9280 ADC module (`docs/ad9280-adc-module.md`) is meant to sample the R820T2 IF. The
CH32V307 digital video port (DVP) captures its 8-bit parallel data: it latches D0..D7 on
DVP_PCLK and writes them to SRAM by DMA, at up to 144 MHz pixel clock (datasheet table
4-37). The MCU generates DCLK with TIM1 and feeds it both to the ADC and back into
DVP_PCLK. Status: the capture driver (`modules/adc_dvp_wch`) and the `welle-sdr adc` shell
commands implement variant B below, see "ADC test". The USB stream carries the down-converted
ADC data, see "I/Q stream".

Pins are taken from the
[CH32V303/305/307 datasheet](https://ch32-riscv-ug.github.io/CH32V307/datasheet_en/CH32V20x_30xDS0.PDF)
(table 3-4) and the
[EVT board schematic](https://github.com/openwch/ch32v307/blob/main/SCHPCB/CH32V307V-R1-1v0/SCH_PCB/CH32V307V-R1.pdf).

### Pin assignment

Module J2 to the EVT board:

| Module J2 | Signal | EVT pin | MCU function            | Notes                                     |
|-----------|--------|---------|-------------------------|-------------------------------------------|
| 1         | +5V    | 5V      | -                       | See "Power and ground"                    |
| 2         | GND    | GND     | -                       |                                           |
| 3         | DB0    | PA9     | DVP_D0                  | USART1_TX by default, see modifications   |
| 4         | DB1    | PA10    | DVP_D1                  | USART1_RX by default, see modifications   |
| 5         | DB2    | PC8     | DVP_D2                  | Ethernet TXP by default, remove R7        |
| 6         | DB3    | PC9     | DVP_D3                  | Ethernet TXN by default, remove R7        |
| 7         | DB4    | PC11    | DVP_D4                  |                                           |
| 8         | DB5    | PB3     | DVP_D5 (remapped)       | See "DVP_D5 and USBHS"                    |
| 9         | DB6    | PB8     | DVP_D6                  |                                           |
| 10        | DB7    | PB9     | DVP_D7                  |                                           |
| 11        | DCLK   | PA6     | DVP_PCLK, see below     | Variant A: PA6; variant B: PA8            |

### Clock and sync signals (PCLK, HSYNC, VSYNC)

The DVP is made for cameras, which deliver a pixel clock, a line signal and a frame signal
together with the data. On the DVP these are inputs: DVP_PCLK (PA6), DVP_HSYNC (PA4) and
DVP_VSYNC (PA5). The DVP latches data on PCLK, stores it only while HSYNC is active and
starts a frame on VSYNC. The AD9280 delivers none of them: it needs DCLK from outside and
has no line or frame signals. So the MCU has to generate all three and the DVP has to read
them back. The driver holds both active for the whole capture and runs the DVP in JPEG mode,
in which it stores a continuous byte stream in fixed-size DMA blocks; see "ADC test".

Variant A (no jumper wires, untested, try first): the pins drive their own DVP inputs.

| Pin | Configured as                      | Function                                     |
|-----|------------------------------------|----------------------------------------------|
| PA6 | TIM3_CH1 PWM output                | DCLK to module J2 pin 11, read back as DVP_PCLK |
| PA4 | GPIO push-pull output              | DVP_HSYNC                                    |
| PA5 | GPIO push-pull output              | DVP_VSYNC                                    |

On STM32-style GPIO, which the CH32V307 follows, the input buffer stays enabled while a pin
is an output or an alternate function output, so peripherals on that pin see the driven
level. Whether the DVP reads PCLK, HSYNC and VSYNC from the pad in that state is not
documented for the CH32V307 and not tested. TIM3 is on APB1, which runs undivided at
144 MHz, so it reaches the same DCLK rates as TIM1. Pinctrl: `TIM3_CH1_PA6_0`.

Test: drive PA4/PA5 and the PA6 clock, enable the DVP with its frame interrupts, and check
that frame start and row done interrupts arrive and that the DMA buffer fills with data.
With AD IN at a fixed voltage the buffer must hold a constant code. If no interrupts
arrive, the DVP does not see its own pins driven, and variant B is needed.

Variant B (fallback, three jumper wires on the EVT board): other pins generate the signals
and the DVP pins stay plain inputs.

| From | To   | Function                                                        |
|------|------|-----------------------------------------------------------------|
| PA8  | PA6  | DCLK (TIM1_CH1 PWM output) into DVP_PCLK                        |
| PA0  | PA4  | DVP_HSYNC, driven by firmware (PA0 is also TIM2_CH1)            |
| PA1  | PA5  | DVP_VSYNC, driven by firmware (PA1 is also TIM2_CH2)            |

Module J2 DCLK then goes to PA8 instead of PA6. Pinctrl: `TIM1_CH1_PA8_0`.

The tuner (PB10, PB11), USBHS (PB6, PB7) and the LED1 jumper (PD0) are not affected. PB3,
PB8, PB9, PC11 and PA0..PA8 go only to the EVT pin headers.

### Board modifications on the EVT board

Two changes free the pins for DB0..DB3. Both are reversible.

#### 1. Console from USART1 (PA9/PA10) to USART2 (PA2/PA3)

DB0 and DB1 need PA9 and PA10, which carry the USART1 console to the WCH-Link. The
connection runs through J2 on the EVT board: the 2 x 7 pin header between the WCH-Link
(CH549, USB-C P9) and the CH32V307, block "SWD&UART" in the schematic. Odd pins are the
WCH-Link side, even pins the CH32V307 side; a jumper connects each pair:

| EVT J2 pins | WCH-Link side (odd) | CH32V307 side (even) | Jumper                      |
|-------------|---------------------|----------------------|-----------------------------|
| 1-2         | 5V                  | 5V                   | not needed                  |
| 3-4         | UART0_RX (RX0)      | TX, to PA9           | remove                      |
| 5-6         | UART0_TX (TX0)      | RX, to PA10          | remove                      |
| 7-8         | GND                 | GND                  | not needed                  |
| 9-10        | CLK0                | CLK, to PA14 (SWCLK) | keep, needed for flashing   |
| 11-12       | DIO0                | DIO, to PA13 (SWDIO) | keep, needed for flashing   |
| 13-14       | 3V3                 | 3V3                  | not needed                  |

Steps:

1. Remove the jumpers 3-4 and 5-6. PA9 and PA10 are then free for DB0 and DB1.
2. Wire EVT J2 pin 3 (WCH-Link RX) to PA2 (USART2_TX) and EVT J2 pin 5 (WCH-Link TX) to
   PA3 (USART2_RX). The console stays on the same `/dev/ttyACM0` at 115200 baud.
3. The app overlay sets `zephyr,console` and `zephyr,shell-uart` to `&usart2` (pinctrl
   `USART2_TX_PA2_0` / `USART2_RX_PA3_0`) and disables `&usart1`, so PA9 is not driven.
   Flashing is not affected by the jumpers.

To go back to USART1, refit the two jumpers, remove the two wires and drop the USART
settings from the overlay.

#### 2. Ethernet off PC8/PC9

DB2 and DB3 need PC8 and PC9, which are the TXP/TXN pins of the built-in 10 Mbit/s Ethernet
PHY. They go through R7, a 4 x 0 Ohm resistor array (block "ETH" in the schematic), to the
transmit winding of the transformer in the RJ45 jack P2. The center tap of that winding
is on 3.3 V. With R7 fitted, PC8 and PC9 are pulled to 3.3 V and shorted to each other
through the winding, so the ADC cannot drive them.

Steps:

1. Remove R7, next to the RJ45 jack P2. It also carries RXP/RXN (PC6/PC7), which this
   wiring does not use.
2. The Ethernet port no longer works; the app does not use it. Solder R7 back (or bridge
   its four pads) to restore it.

### DVP_D5 and USBHS

DVP_D5 defaults to PB6, which is also USBHS_DM, the USB data line this app uses. Datasheet
note: DVP_D5 moves to PB3 automatically when DVPEN (bit 13) and USBHSEN (bit 11) in
RCC_HBPCENR are set and RB_UC_RST_SIE (bit 2) in R8_USB_CTRL is 0, that is, while both
peripherals are clocked and the USBHS controller is out of reset. This holds only for chips
whose lot number has a fifth-from-last digit greater than 1 or a sixth-from-last digit
other than 0. On an older chip the DVP cannot be used together with USBHS, and the capture
needs a different path.

The CH32V307 on the board used here has lot number 31452ff12: fifth from last is 2 (greater
than 1) and sixth from last is 5 (not 0), so DVP_D5 is on PB3 while USBHS runs. Check the
lot number again when using another board.

### Clock and latch edge

- DCLK is a timer PWM output: TIM3_CH1 on PA6 (variant A) or TIM1_CH1 on PA8 (variant B).
  This is option A in "Sample clock (DCLK)" in `docs/ad9280-adc-module.md`, which covers
  the choice of rate. Both timers run at 144 MHz; the clock is high for half the divider,
  rounded down (50 % duty for even dividers), for example 8 MHz (144 / 18) or at most
  24 MHz (144 / 6) within the 14.7 ns pulse width limit.
- The DVP latches on DVP_PCLK, which carries DCLK itself (variant A) or DCLK fed back
  through the PA8-PA6 jumper (variant B), so the capture follows the clock whatever its
  source. With an external clock (options B to D), connect it to module J2 DCLK and PA6
  and use neither timer output.
- The ADC outputs a new word after each rising edge, valid at most 25 ns (t_OD) later.
  Below 20 MHz, latch on the falling edge (DVP PCLK polarity inverted): at 8 MHz the word
  is then stable for 37.5 ns before the edge, against the DVP setup time of 2.5 ns.
- From 20 MHz to 24 MHz the falling edge falls into the output transition. Latch on the
  rising edge instead, which reads the word output after the previous edge: setup margin
  is the period minus 25 ns (16.7 ns at 24 MHz). Hold then depends on how long the old word
  stays after the edge (not checked against the datasheet) against the DVP hold time of
  1 ns. Verify with a scope or logic analyzer before relying on it.
- The DVP writes one byte per DCLK cycle into SRAM, 8 MB/s at 8 MHz. Mixing and decimation
  to the host rate run on the MCU.

### Logic levels

- PA9, PA10, PC8, PC9, PC11, PB3, PB8 and PB9 are 5 V tolerant (FT) inputs, so the data
  lines are safe even if the module drives them from a 5 V DRVDD. PA4, PA5 and PA6 are not
  5 V tolerant; they are driven only by the MCU at 3.3 V.
- DCLK from the MCU swings 0 V to 3.3 V. The CLK threshold is AVDD / 2: with AVDD = 3.3 V
  the pin drives CLK directly; with AVDD = 5 V (not traced) the threshold is 2.5 V, and an
  HCT/ACT buffer from the module's 5 V belongs between the timer pin and DCLK. PA6 then
  stays on the 3.3 V side of the buffer, and the buffer delay (several ns) shifts the ADC
  clock against the DVP latch edge.

### Power and ground

- Feed module J2 pin 1 from the EVT board's 5 V, which comes from USB through a 500 mA
  fuse. The module's MC34063A inverter draws its input current from this rail; measure the
  module's supply current before relying on the fuse margin.
- Module J2 has a single GND pin for nine fast signals. Add a second ground wire from
  module J1 GND to the EVT board GND.
- Keep the wires short (10 cm or less) and of equal length, and run DCLK next to a ground
  wire. A 22 Ohm to 47 Ohm series resistor at the timer pin (PA6 or PA8) damps ringing on
  DCLK. In variant A, place it on the module side of PA6, so the DVP input sees the
  undamped pin.

## ADC test

The capture driver is the out-of-tree module `modules/adc_dvp_wch` (compatible
`wch,dvp-adc`, node `adc_dvp` in the board overlay, wiring variant B). The DVP runs in JPEG
mode: it writes the samples alternately into two DMA buffers of
`CONFIG_ADC_DVP_WCH_BLOCK_SIZE` bytes (default 2048) and raises an interrupt per block. A
ring of `CONFIG_ADC_DVP_WCH_BLOCK_COUNT` blocks (default 4) holds the completed blocks until
the consumer releases them; if it is too slow, the oldest block is reused (overrun).

Shell commands:

- `welle-sdr adc start [rate_hz]`: start the capture, default 8 MS/s. The rate must be 144 MHz
  divided by an integer of at least 6: 24, 20.57, 18, 16, 14.4, 12, ... MS/s. Odd dividers
  give a clock high for (divider - 1) / 2 cycles. The DVP latches on the falling DCLK edge
  below 20 MS/s, on the rising edge above.
- `welle-sdr adc stop`
- `welle-sdr adc status`: blocks, overruns, FIFO overflows, frame starts, spurious block
  interrupts and whether the guard area after the ring is intact.
- `welle-sdr adc capture [blocks]`: take 1 to 64 blocks (default 4) and print a hex dump of the
  first 32 bytes, min/max/mean, the most frequent codes and the statistics. Starts and stops
  the capture at 8 MS/s if it is not running.

Expected codes with AD IN at a fixed voltage, if the front end maps +/-5 V to the ADC's
0 V to 2 V (see `docs/ad9280-adc-module.md`): about 0x80 at 0 V and about 0xD4 at 3.3 V,
a constant code within a few LSB.

Measured with the module, DC at AD IN, `adc capture 64` (131072 samples):

| Rate     | AD IN 0 V: mean, range | AD IN 3.3 V: mean, range |
|----------|------------------------|--------------------------|
| 8 MS/s   | 126.3, 0x7B..0x82      | 208.4, 0xCF..0xD2        |
| 12 MS/s  | -                      | 208.6, 0xCE..0xD3        |
| 18 MS/s  | -                      | 208.7, 0xCE..0xD3        |
| 24 MS/s  | 121.8, 0x74..0x7F      | 206.9, 0xCD..0xD2        |

- At 8 MS/s: `V_adc = 0.99 V + V_in / 5.14`, the full code range covers about -5.1 V to
  +5.2 V at AD IN (details in `docs/ad9280-adc-module.md`).
- No bit errors at any rate: the spread stays within a few LSB around the mean.
- 24 MS/s (rising-edge latch) reads 1.5 to 4.5 LSB lower and shows a repeating pattern of
  period 4 (a tone at fs/4, about +/-1 LSB), possibly from the DVP DMA, which moves 4 bytes
  per request; not verified. Use 18 MS/s or less.

Measured without the ADC module (data lines open, DCLK looped back through the PA8-PA6
jumper):

- All codes read 0x0C (DB2/DB3 on PC8/PC9 float high with R7 removed).
- 8, 18 and 24 MS/s: the block count matches the rate, no FIFO overflows, every block fully
  written and the guard intact (`COL_NUM` counts bytes).
- `rtl_test` streaming is unaffected by a running 24 MS/s capture (0 lost, about 2.34 MS/s).
- `adc capture` reports about 63 overruns: the hex dump of the first block takes longer than
  the ring holds. The blocks after it are contiguous.

DVP behavior found while testing, handled in the driver:

- VSYNC and HSYNC are sampled with PCLK. TIM1 starts with the reset period 0xFFFF, and the
  new period is preloaded, so PCLK only runs after up to 455 us; the driver waits 1 ms after
  starting the clock and checks that the DVP saw the frame start.
- Data is stored only after HSYNC goes active inside the frame; an HSYNC that is already
  active at the frame start does not count. The driver raises VSYNC first, then HSYNC.
- While the CPU is busy, the block interrupt can fire a second time for the same block, with
  no buffer switch until the next block. The driver takes a block as complete only when
  `RB_DVP_BUF_TOG` shows the switch and counts the others as spurious.

## I/Q stream

Outside `rtl_test` test mode, a thread in `src/sample_src.c` converts the ADC blocks to I/Q at
the host's sample rate and IF and submits them as bulk IN transfers. The ADC paces the stream.
The down-converter is `src/ddc.c`:

1. The ADC rate fs is chosen so that fs / 4 lies near the IF. Mixing by fs / 4 is the
   sequence 1, -j, -1, +j: each sample is added to or subtracted from I or Q, without a
   multiplication.
2. A second-order CIC decimates by D = fs / host rate, with additions only.
3. A complex rotation per output sample removes the residual |IF| - fs / 4.

The IF is the one the host writes into the demodulator (page 1, regs 0x19..0x1b), the spectrum
inversion bit is page 1 reg 0x15 bit 0. Without a tuner librtlsdr switches to direct sampling
and writes the center frequency as the IF, so the stream shows the spectrum at AD IN around
the tuned frequency, as a real RTL2832U in direct sampling mode does.

### Rate plan

`ddc_plan()` takes fs = 144 MHz / p and D with fs = D * host rate. Host rates that divide
144 MHz work exactly: 1.0, 1.2, 2.0, 2.4 MS/s, ... 2.048 MS/s (DAB, welle.io) is not
reachable; the nearest rate is streamed and a warning gives the error in ppm.

Among the exact plans it takes the smallest residual whose estimated CPU load is at most
60 %, else the cheapest. The load model is fitted to measured times of `ddc_process()`
(`-O2`, 144 MHz): 7.9 cycles per input and 47 per output sample when D is a multiple of 4
(the outputs fall on the ends of the four-sample groups), 13.3 and 86.5 cycles otherwise.
`welle-sdr status` shows the plan, the residual, the measured and the estimated load:

```
DDC:          running, IF 1099999 Hz
ADC rate:     4 MS/s, decimation 4, residual 99999 Hz
Output rate:  1 MS/s (exact)
DDC load:     55 % (estimated 54 %)
DDC output:   2945 transfers, 0 bytes dropped
```

### Test with the square-wave test signal

`welle-sdr testsig <hz>` drives a square wave on PA7 (TIM3_CH2, `src/testsig.c`), `off` stops
it. Wire PA7 to AD IN of the module (J3) through about 1 kOhm, GND to GND. TIM3 is programmed
directly: with TIM1 (`wch,adtm-pwm`, the ADC clock) and TIM3 (`wch,gptm-pwm`) both enabled,
Zephyr's `pwm_wch_gptm.c` defines `pwm_wch_gptm_0_config` twice and does not build.

```
welle-sdr testsig 1200000
rtl_sdr -f 1100000 -s 1000000 -n 8000000 out.bin
tools/iq_peaks.py out.bin -s 1000000
```

`tools/iq_peaks.py` lists the strongest spectral lines relative to the center frequency.
Measured without tuner (direct sampling), 1 MS/s, ADC 4 MS/s, D 4:

| Line         | Level    | Origin                                                   |
|--------------|----------|----------------------------------------------------------|
| +100006 Hz   | 0 dB     | 1.2 MHz test signal (IF register rounds to 1099999 Hz)   |
| +300003 Hz   | -15.9 dB | 3rd harmonic 3.6 MHz, aliased to 0.4 MHz at 4 MS/s       |
| -300003 Hz   | -30.6 dB | image of the test signal (-1.2 MHz - IF, folded)         |
| -99487 Hz    | -53.7 dB | mirror of the test signal                                |

Tuned to 1.3 MHz the line moves to -99991 Hz (sign check). 8 M samples arrive in real time,
with no dropped transfers and no lost ADC blocks.

The down-converter has host tests (`native_sim`), including a bit-exact comparison with a
per-sample reference: `west twister -p native_sim -T apps/welle-sdr/tests/ddc`.

### Limits

- CPU: 1 MS/s with fs = 4 MS/s needs about 55 % of the CPU. 2 MS/s (estimated 109 % at the
  best plan) does not fit: ADC blocks are lost and the spectrum shows sidebands at the
  block rate. Lost blocks show up as `Spurious IRQs` in `welle-sdr adc status`, as their
  interrupts arrive late and merge.
- The CIC droops towards the band edges (-1.1 dB at 0.2 x rate from the band center,
  -7.8 dB at 0.5 x rate) and folds signals around multiples of the output rate into the band
  with limited suppression (about 30 dB for the image of a tone near the center). The band
  center is fs / 4, so the residual shifts the host's band against it.
- DC at AD IN lands on a CIC null only when D is a multiple of 4; otherwise it leaks into the
  band edge.
- `rtl_test -p` runs in test mode, so it measures the unpaced counter stream (about
  2.37 MS/s), not the I/Q rate.

