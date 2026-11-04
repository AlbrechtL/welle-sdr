<!--
Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# welle-sdr

Zephyr firmware that makes a WCH CH32V307V-EVT-R1 enumerate as an RTL-SDR dongle
(RTL2832U with an R820T2 tuner, USB `0bda:2838`) on the USBHS connector P6. The
stock librtlsdr tools (`rtl_test`, `rtl_sdr`, `rtl_tcp`) work without a new driver.
The host's tuner requests go to a real R820T2 on I2C; the samples are still dummy
data: a byte counter in `rtl_test` test mode, otherwise an I/Q tone at fs/16.

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

## Tuner wiring

| R820T2 | CH32V307V-EVT-R1 |
|--------|------------------|
| SCL    | PB10             |
| SDA    | PB11             |
| GND    | GND              |
| VCC    | 3.3 V            |

- SCL and SDA need external pull-ups to 3.3 V (4.7 kOhm), unless the tuner board has them.
- GND of the tuner board must be connected to GND of the EVT board.
- The R820T2 needs its 28.8 MHz crystal or reference clock.
- 7-bit I2C address 0x1a (librtlsdr uses the 8-bit form 0x34), 100 kHz.

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
VCO:          3484359667 Hz
LO:           435544958 Hz (VCO / 8)
```

The LO is the center frequency plus the IF (here 1.625 MHz). Gains are librtlsdr's gain steps
for the programmed indices. After a host tool closes the device, librtlsdr puts the tuner in
standby: the PLL reads unlocked and the gains read as auto.

While samples stream, the shell can drop typed characters; type slowly or paste line by line.

## PLL lock with librtlsdr

librtlsdr checks the PLL lock bit with the first read after programming the PLL. That read
reaches the chip sooner than through a real RTL2832U, before the PLL has locked (measured: up
to about 3.5 ms). The firmware therefore polls the lock bit for at least 5 ms before it answers
that read.

`[R82XX] PLL not locked!` still appears in about every second `rtl_sdr` or `rtl_test` run. It
comes from a tune during open, before the requested frequency is set, where librtlsdr programs
the PLL for the IF alone, outside the VCO range. The requested frequency locks. A real dongle
gets the same request from librtlsdr.
