<!--
Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# Si2141 tuner

Notes on the Skyworks (formerly Silicon Labs) Si2141-A10 as a replacement for the R820T2 on a
welle-sdr board. Collected 2026-11-08 from the public data short, the Linux `si2157` driver,
distributor listings and user reports. The full datasheet, the programming guide and the
evaluation board documents are under NDA; everything below comes from public sources, and
"unknown" marks what those sources do not answer.

## Summary

- Low-IF DTV tuner, 42 to 1002 MHz, 3 mm x 3 mm QFN-24, I2C control (7-bit address 0x60),
  differential analog LIF output (LIF_P, LIF_N). The output is analog: the channel filter runs
  in an internal DSP, the result leaves the chip as an analog low IF ("DLIF", digital low-IF).
- 1.7 MHz channel bandwidth mode, a good fit for the 1.536 MHz DAB block. Band III NF 2.3 dB
  (104 to 198 MHz) and 3.0 dB (199 to 470 MHz).
- No balun, no LNA, no SAW filter, no tracking filter needed; single 3.3 V supply possible
  (internal 1.8 V LDO).
- Boots from ROM. A firmware patch (1.6 KB or 2.1 KB, from the Windows driver) can be loaded
  over I2C; the current Linux driver treats it as optional for this chip.
- No public Si2141 schematic. The pin-compatible Si2151 appears in a public TV service manual
  schematic and in distributor design notes, which give the external components per pin
  (24 MHz / 8 pF crystal, RF input network, decoupling, AGC and LIF RC stages). Two consumer
  DVB-T2 sticks use the Si2141 (MyGica T230C family, August DVB-T210 v2).
- At JLCPCB only 20 pieces in stock; DigiKey holds the bulk of the distributor stock.

## Sourcing

| Source                        | Part number      | Code / note       | Stock      | Price            |
|-------------------------------|------------------|-------------------|------------|------------------|
| JLCPCB / LCSC                 | SI2141-A10-GMR   | C2984260          | 20         | $0.83 @ 1        |
| JLCPCB / LCSC                 | SI2141-A10-GM    | C430209           | 0          | $1.88 @ 1, $0.73 @ 200 |
| JLCPCB / LCSC                 | SI2141-A10-GMR   | C3824186          | 0          | $1.51 @ 1        |
| DigiKey                       | SI2141-A10-GM    | 8 weeks lead time | about 27,000 | $1.63 @ 1, $1.22 @ 1000 |
| Mouser                        | SI2141-A10-GM    | restricted availability | -    | -                |
| Mouser                        | SI2141-A-EVB     | 634-SI2141-A-EVB, NDA | -      | -                |

JLCPCB and LCSC figures from their part APIs on 2026-11-07; DigiKey and Mouser figures from the
tuner list of 2026-11-07. Parts not stocked at JLCPCB can be brought in from DigiKey through
JLCPCB Global Sourcing or consigned. Skyworks lists the part as active.

## Electrical data (data short)

| Parameter                         | Condition                         | Typ                       |
|-----------------------------------|-----------------------------------|---------------------------|
| RF input range                    |                                   | 42 to 1002 MHz            |
| Channel bandwidths                |                                   | 1.7, 6, 7, 8, 10 MHz      |
| Supply                            |                                   | 1.8 V and 3.3 V, or 3.3 V only |
| Power                             | external 1.8 V supply             | 494 mW                    |
| NF, terrestrial                   | VHF-L1 42 to 103 MHz              | 3.6 dB                    |
|                                   | VHF-L2 104 to 198 MHz             | 2.3 dB                    |
|                                   | VHF-H 199 to 470 MHz              | 3.0 dB                    |
|                                   | UHF 471 to 870 MHz                | 3.2 dB                    |
| Return loss                       | terrestrial mode / cable mode     | 3 dB / 8 dB               |
| Out-of-band IIP3                  | VHF-H, N +/-18, +/-36             | +5 dBm                    |
| In-band IIP3                      | VHF-H, N +/-1, +/-2               | -6 dBm                    |
| LO phase noise at 860 MHz         | 1 kHz / 10 kHz / 100 kHz / 1 MHz  | -100 / -100 / -105 / -132 dBc/Hz |
| LO integrated phase noise, 860 MHz | DSB, 125 Hz to 4 MHz             | 0.25 deg rms (-47 dBc)    |
| LIF center frequency              | channel BW 6 / 7 / 8 MHz          | 4 to 7 / 4.5 to 6.5 / 5 to 6 MHz |
| LIF differential output voltage   | programmable                      | 0.5 to 2.0 Vppd           |

The LIF output voltage row is in the 2014 Silicon Labs data short, not in the 2021 Skyworks
version. NF and IIP3 include the losses of the evaluation board front end. No LIF center
frequency is given for the 1.7 MHz bandwidth; see "IF frequency".

Band III (174 to 240 MHz) spans the VHF-L2 and VHF-H rows. The terrestrial return loss of 3 dB
is poor; cable mode (software-selectable) improves it to 8 dB at an unknown NF cost.

## Pinout (QFN-24, 3 mm x 3 mm, 0.4 mm pitch, exposed pad GND)

| Pin | Name    | Pin | Name    | Pin | Name    | Pin | Name    |
|-----|---------|-----|---------|-----|---------|-----|---------|
| 1   | GPIO2   | 7   | VDDL    | 13  | XTAL_I  | 19  | RF_REF  |
| 2   | SDA     | 8   | GND     | 14  | XTAL_O  | 20  | RF_IP   |
| 3   | SCL     | 9   | VDDH    | 15  | VDDH    | 21  | GND     |
| 4   | AGC2    | 10  | LIF_N   | 16  | GND     | 22  | RF_IN   |
| 5   | AGC1    | 11  | LIF_P   | 17  | VDDH    | 23  | ADDR    |
| 6   | GND     | 12  | XOUT    | 18  | NC      | 24  | GPIO1   |

Package: A 0.80 to 0.90 mm, D/E 3.00 mm, e 0.40 mm, exposed pad J/K 1.60 to 1.80 mm.

The Si2151 (the hybrid analog/digital sister chip) has the same pinout and package per its data
short, so its application circuits apply. No public Si2141 circuit exists; the external
components below come from two Si2151 sources:

- [TV] the Si2151-A10 tuner page of a TV mainboard service manual (Mstar platform, MSB3400
  demodulator, "Technical Document" page 32/44, page title "Page5 Tuner"), designators as in
  that schematic. Its note 2 says that all values not marked otherwise have to be confirmed
  with the FAE and can be adapted during tuning.
- [SK] Sekorm design notes on the Si2151 reference design (designators of the Silicon Labs
  reference schematic, which is not public).

### External components per pin

| Pin | Name | Function | External components (source) | welle-sdr recommendation |
|-----|------|----------|------------------------------|--------------------------|
| 1 | GPIO2 | general-purpose I/O | none, left open [TV] | leave open, optional test pad |
| 2 | SDA | I2C data | 100 Ohm series (R1T62) and 47 pF to GND (C8T7), both close to the tuner [TV]; pull-up at the master | 100 Ohm series + 47 pF at the tuner, 4.7 kOhm pull-up to 3.3 V (as on the R820T2 bus); route with ground guard and vias [SK] |
| 3 | SCL | I2C clock, 400 kHz max. [SK] | 100 Ohm series (R1T63) and 47 pF to GND (C8T8), close to the tuner [TV] | as SDA |
| 4 | AGC2 | second AGC input | none, left open [TV]. AGC1 and AGC2 are identical; firmware assigns each to ATV, DTV or both (property 0x12 per [SK]) | leave open |
| 5 | AGC1 | IF AGC control voltage from the demodulator | two RC stages [TV note 1]: 1 kOhm series (R1T2) + 100 nF to GND (C3T2) at the tuner; at the demodulator 10 kOhm pull-up to 3.3 V (R1T21, open-drain AGC output), 0 Ohm (R0T22) + 22 nF to GND (C0T13). "The number of RC stages depends on the demodulator" [TV] | PA15 PWM -> RC at the MCU -> 1 kOhm + 100 nF at the tuner, the same PWM principle as the R820T2 IF_AGC; keep the trace away from RF and LIF, ground guard [SK] |
| 6 | GND | ground | direct to the ground plane | - |
| 7 | VDDL | 1.8 V core, output of the internal LDO | internal LDO: 1 uF (C2T78, 6.3 V) + 1 nF (C2T86) [TV], 1.0 uF + 1.2 nF [SK]. External 1.8 V: only 1.2 nF, the 0 Ohm links R20/R21 of the reference fitted, and the internal LDO switched off by firmware at power-up, otherwise the two supplies fight [SK] | internal LDO: 1 uF + 1.2 nF (or 1 nF) next to the pin |
| 8 | GND | ground | direct to the ground plane | - |
| 9 | VDDH | 3.3 V | 1 nF (C2T79) + 100 nF (C2T84) [TV] | 1 nF + 100 nF, one set per VDDH pin, as close to the pin as possible [TV PCB note 2] |
| 10 | LIF_N | low-IF output, negative | one RC stage close to the tuner is mandatory [TV]; capacitors C64/C65 at LIF_P/N must be fitted, they low-pass signals coupled back from the demodulator [SK]; IF filter capacitors 22 pF in the reference (CT23/CT24/CT9/CT8) [SK]. At the demodulator: 100 Ohm series (R0T1) + 47 pF to GND (C0T1) [TV] | 100 Ohm + 47 pF (about 34 MHz) per leg at the tuner, then the differential to single-ended stage and the anti-alias low-pass in front of the MS9280 |
| 11 | LIF_P | low-IF output, positive | as LIF_N (R0T2, C0T2) | as LIF_N |
| 12 | XOUT | buffered crystal clock output, enabled by firmware | none, left open [TV] | leave open, or use as 24 MHz reference only after checking its level and spurs |
| 13 | XTAL_I | crystal | 24 MHz crystal (X0T10), ESR < 120 Ohm, CL = 8 pF [TV]; +/-25 ppm, a dedicated crystal for the tuner [SK]. Load capacitors C2T89/C2T90 22 pF to GND marked NC: for an 8 pF crystal they stay unpopulated [TV FAE note]; on-chip programmable capacitors set the load and trim the frequency [SK] | 24 MHz, CL 8 pF, ESR < 120 Ohm, +/-25 ppm or better; pads for the two load capacitors, unpopulated; vias around the crystal [SK] |
| 14 | XTAL_O | crystal | see XTAL_I | see XTAL_I |
| 15 | VDDH | 3.3 V | 1 nF (C2T82) [TV]; PCB note 1: one capacitor each at pin 15 and pin 17 | 1 nF + 100 nF |
| 16 | GND | ground | direct to the ground plane | - |
| 17 | VDDH | 3.3 V | 1 nF (C2T77) + 100 nF (C2T88) [TV] | 1 nF + 100 nF |
| 18 | NC | not connected | none [TV] | leave open |
| 19 | RF_REF | RF input reference | 1 nF to GND (C2T87); RF_IP returns to this node through two 270 nH in series (L2T8, L6T0) [TV] | as [TV] |
| 20 | RF_IP | RF input | 39 pF series (C2T83) from the antenna node; 2 x 270 nH (L2T8, L6T0) to RF_REF [TV] | as [TV] |
| 21 | GND | ground | direct to the ground plane | - |
| 22 | RF_IN | RF input | 39 pF series (C2T80) from the antenna node; 2 x 270 nH in series to GND (L5T8, L5T2) [TV]. Each 270 nH pair can be replaced by one 560 nH inductor [SK] | as [TV]; high-Q RF inductors, no ferrite types [SK] |
| 23 | ADDR | I2C address select | tied to GND [TV]. Addresses (7-bit / 8-bit write): GND 0x60 / 0xC0, 220 kOhm to GND 0x61 / 0xC2, 220 kOhm to 3.3 V 0x62 / 0xC4, 3.3 V 0x63 / 0xC6 [SK]. If not tied directly to GND, a capacitor (C63) close to the pin is mandatory: the pin is next to the RF input [SK] | tie to GND: address 0x60 as in Linux, no capacitor needed |
| 24 | GPIO1 | general-purpose I/O | none, left open [TV] | leave open, optional test pad |
| EP | GND | exposed pad, ground | to the ground plane; 5 vias (4 at the corners plus one in the center) [SK] | 5 or more vias |

Antenna input network in [TV], from the connector to the two 39 pF capacitors:

| Part | Value | Connection | Purpose |
|------|-------|------------|---------|
| D2T2 | TVS | connector to GND | surge protection |
| C7T2 | 1 nF, 50 V | series | DC block |
| D1T7 | TT0521SB | to GND | ESD protection, at the RF entry point [SK] |
| L6T1 | 330 nH | shunt to GND | with C1T89 a high-pass, about 40 MHz; also drains ESD charge (L5 in the reference, "consider carefully before removing" [SK]) |
| C1T89 | 47 pF, 50 V | series | high-pass |
| L2T9 + C1T88 | 180 nH + 150 pF | series to GND | series trap at about 31 MHz, the CB radio rejection (26 to 30 MHz) of the reference [SK] |
| C1T87 | 0 Ohm | series | placeholder for matching or for reducing conducted emissions (R1 in the reference [SK]) |

Supply in [TV]: +3.3V_TUNER from the 3.3 V rail through a ferrite bead, 60 Ohm at 100 MHz
(FB0T7). [SK] recommends a ferrite bead on the tuner 3.3 V feed and 0.1 uF in place of the
reference's CT6.

Layout notes [SK]: ESD device at the RF entry; split ground around the RF section as in the
Silicon Labs application note AN813 (not public); vias along both sides of the IF traces,
around the crystal and the supply; guard I2C and AGC with ground and vias.

For DAB on welle-sdr the [TV] network is a starting point: the high-pass and CB trap do no harm
in Band III. Strong FM broadcast (87.5 to 108 MHz) passes it unattenuated; an FM band-stop or a
Band III band-pass in front of it is an option that this network does not include.

## Signal path

RF -> LNA with tracking filter and RF AGC -> I/Q mixer (0/90 deg LO from the synthesizer) ->
PGA (IF AGC) -> I and Q ADCs -> DSP channel filter -> shift to the programmed low IF -> output
interface -> LIF_P/LIF_N.

The internal ADCs let the DSP do the image rejection (with I/Q mismatch correction), the
channel filter, DC offset and LO leakage removal and the level detection for the IF AGC. The
output interface converts back to analog, because TV demodulators expect an analog IF.
The internal I/Q data is not accessible.

For welle-sdr this means the MS9280 sees one channel-filtered block: adjacent DAB blocks are
already suppressed, so aliasing and AGC are less critical than with the R820T2.

## Control interface (from Linux `si2157.c`)

Command/response protocol over I2C at 0x60 (maximum clock rate not in the public documents):

1. Write the command: command byte plus arguments.
2. Read the response until bit 7 of the first byte (CTS, clear to send) is set; Linux polls
   for up to 80 ms. Bit 6 set means error.

Startup sequence used by Linux for the Si2141:

| Step | Bytes written                        | Read | Purpose                                         |
|------|--------------------------------------|------|-------------------------------------------------|
| 1    | `c0 00 0d 0e 00 01 01 01 01 03`      | 1    | POWER_UP: clock mode crystal, XOUT enabled (may return an error, Linux ignores -EAGAIN here) |
| 2    | `c0 08 01 02 00 00 01`               | 1    | second power-up/wake command the Si2141 needs before it answers |
| 3    | `02`                                 | 13   | PART_INFO: byte 2 part (41), bytes 1/3/4 revision ("A10"), byte 12 ROM id (0x60 or 0x61) |
| 4    | firmware patch records               | 1 each | optional, see "Firmware patch"                |
| 5    | `01 01`                              | 1    | start the firmware                              |
| 6    | `11`                                 | 10   | GET_REV: firmware version in bytes 6, 7, 8 (1.1.11 reported with the patch) |
| 7    | `14 00 01 05 01 00`, `14 00 01 06 01 00`, `14 00 01 07 01 00` | 1 each | set properties 0x0501, 0x0601, 0x0701 to 1 (Linux comment: enable tuner status flags) |

Property access: `14 00 <prop lo> <prop hi> <value lo> <value hi>` sets a property,
`15 00 <prop lo> <prop hi>` reads one (value in response bytes 2, 3). Linux reads 0x0402
(crystal trim) to check whether the tuner is still powered and initialized: a value below 16
means warm.

Tuning a digital channel:

| Bytes written                          | Purpose                                                    |
|----------------------------------------|------------------------------------------------------------|
| `14 00 03 07 <sys|bw> <inv>`           | property 0x0703: delivery system in bits 7:4 (0x20 DVB-T/T2, 0x30 DVB-C, 0x00 ATSC, 0x10 DVB-C annex B, 0x40 ISDB-T, 0x60 DTMB), bandwidth code in bits 3:0 (9 = 1.7 MHz, 6, 7, 8 = MHz, 10 = 6.1 MHz); `inv` 1 = spectral inversion |
| `14 00 02 07 <port> 00`                | property 0x0702: IF port / AGC source; the MyGica T230C family uses 0 for the Si2141 |
| `14 00 06 07 <kHz lo> <kHz hi>`        | property 0x0706: LIF center frequency in kHz               |
| `41 00 00 00 <f0> <f1> <f2> <f3>`      | tune to the RF frequency in Hz, 32-bit little endian       |

After the tune command Linux polls single status bytes until `(status & 0x81) == 0x81`
(command done), for up to 40 ms; bit 2 signals digital lock.

Other commands: `42 00` (read 12 bytes) returns the RSSI as a signed byte in dBm in response
byte 3; `16 00` puts the tuner into standby.

The command and property names come from the Linux code and its comments; the Silicon Labs
names and the valid property ranges are in the NDA programming guide.

## Firmware patch

- Files: `dvb-tuner-si2141-a10-01.fw` (old name) or `dvb_driver_si2141_rom60.fw` /
  `dvb_driver_si2141_rom61.fw` (current Linux names, chosen by ROM id).
- Format: records of 17 bytes, byte 0 the length n, bytes 1 to n written as one I2C write,
  each followed by a 1-byte status read. A record holds one or more bootloader commands, see
  "Analysis of `dvb-tuner-si2141-a10-01.fw`".
- Two versions circulate: 1666 bytes (98 records) in `osmc/dvb-firmware-osmc` and 2159 bytes
  (127 records) in `LibreELEC/dvb-firmware`. Users report version 1.1.11 after loading.
- Origin: extracted from the MyGica Windows driver. Neither repository includes a license for
  it, and it is not in linux-firmware. Redistribution in a welle-sdr flash image is therefore
  not cleared.
- Since commit 48dde945e7f8 ("media: si2157: Add optional firmware download", 2021) Linux
  marks the patch as not required for the Si2141 (ROM 0x60 and 0x61) and runs from ROM when
  the file is missing. What the patch fixes is unknown; test reception with and without it.

### Analysis of `dvb-tuner-si2141-a10-01.fw`

The file in this directory is the OSMC version: 1666 bytes, SHA-256
`29cf0b0d20a3040f8e59b0ed548e9d3d54eda0b2d2869708f7bcee9d7cbf7e34`. It was compared with the
LibreELEC version (2159 bytes, SHA-256
`6b8d5005325c5115920827c68ab8c8e3d2e78cb5c210b61109a435b702f7adcb`).

Result: it is code for the tuner's internal controller, a RAM patch on top of the ROM firmware,
in the form of encrypted bootloader commands. It is not an EEPROM image.

Record structure:

- 98 records of 17 bytes: one length byte (8 to 16) and up to 16 bytes, zero-padded. Each
  record is one I2C write to 0x60 in Linux.
- A record carries one or more bootloader commands back to back. The first byte of a command
  determines its length:

  | First byte | Length                  | Count (this file) | Count (LibreELEC) |
  |------------|-------------------------|-------------------|-------------------|
  | 0x05       | 8, only as command 0    | 1                 | 1                 |
  | 0x08, 0x09, 0x0C | 8                 | 56                | 72                |
  | 0x30 to 0x3F | 9 + (byte & 7)        | 54                | 75                |
  | 0x40 to 0x4F | 1 + (byte & 7)        | 18                | 22                |

  With this rule both files split completely into commands, 129 and 170, with no bytes left
  over. Example, record 2: `4a 4d 15 | 0c fb 5e 06 b2 72 fe e3 | 4a 9b ec`. The rule is
  derived from the two files only; what each opcode does is not documented publicly.
- The first command is a header: `05 11 e2 0c 2c e5 01 c7` here, `05 11 ff 58 2c e5 01 c7` in
  the LibreELEC version. Only bytes 2 and 3 differ, probably a checksum or size (unverified).

Why it is a controller patch:

- The Si2141 has no EEPROM interface. Its only digital pins are the I2C slave (SDA, SCL,
  ADDR) and GPIO1/GPIO2. The patch goes into RAM between POWER_UP and the start command
  (`01 01`), and is lost at power-down. Linux loads it on every cold start; after the start,
  GET_REV reports the patched version (1.1.11 in user logs).
- 1.3 KB of command payload (1327 bytes here, 1806 in the LibreELEC version) is far too small
  for a complete tuner firmware. The main firmware is in ROM (ROM id 0x60 or 0x61) and the
  patch modifies it.
- An EEPROM image, such as the Cypress FX2 boot EEPROM of the T230C, is a flat byte image with
  a header (VID/PID, load addresses). This file has neither addresses nor a flat layout.

Why the content cannot be read:

- The command bytes after the opcode look random: 7.79 bits/byte entropy over 1229 bytes
  (random data of this size gives about 7.85), and zlib cannot compress them (ratio 1.01).
  Machine code compresses noticeably.
- The two versions share no command, although large parts of a patch for the same ROM would
  normally be identical. The payload is therefore encrypted or scrambled per version, and the
  chip decrypts it.
- The processor architecture of the tuner and the content of the patch cannot be determined
  from the file.

### Purpose of the patch

Silicon Labs publishes no release notes for the Si2141 patches; the following is the general
practice for tuner, demodulator and radio controller chips (Si2168, Bluetooth and Wi-Fi
controllers, CPU microcode), not a documented statement about this chip.

- The main firmware is in mask ROM, fixed with the photomasks. Changing it needs a new silicon
  revision (for example A10 to A20): new masks, requalification, new part numbers, months of
  delay.
- Bugs and weak spots often show up only after release, in the field with many standards,
  countries and interference conditions. They can only be fixed in software.
- The ROM code calls its routines and tables through hooks (jump tables, patch points) that
  can be redirected to RAM. The patch replaces single routines or tables there, which is why
  it is small (about 1.3 KB) compared with a full firmware.
- Typical content of such patches: AGC behavior (loop speed, thresholds with strong adjacent
  channels), calibration routines (image rejection, DC offset, I/Q balance), the LO/PLL
  frequency plan (avoiding spurs from internal clock harmonics in the channel), filter
  coefficients and per-standard settings (for example the 1.7 MHz mode, DTMB, ISDB-T),
  robustness against LTE and Wi-Fi interference, and fixes in the command interface
  (properties, standby, wake-up).
- The encryption protects the vendor's code and the internal ROM interfaces, and prevents
  modified code from running: the chip accepts only patches it can decrypt and check.

What is visible for the Si2141:

- Two ROM versions (0x60, 0x61), so fixes also went into new masks.
- At least two patch builds (1666 and 2159 bytes). With the patch, GET_REV reports 1.1.11,
  probably "FW 1.1 build 11" in Silicon Labs' naming.
- Linux treats the patch as optional: the ROM firmware alone tunes, but in an older state.

To find out whether welle-sdr needs the patch, read GET_REV (`11`) with and without it, and
compare DAB reception both ways: lock, SNR, and behavior next to strong adjacent blocks. If
the ROM firmware is good enough, the license question does not arise.

### Loading the patch in welle-sdr

Loading the patch means storing the 98 records (1666 bytes) in flash and
sending them as 98 I2C writes, each followed by the 1-byte status read until CTS is set. The
file can be used only as an opaque blob, and its license remains unclear (see above), so it
should not be committed to the repository or built into a published image until that is
settled.

## Integration into welle-sdr

- IF frequency. `src/ddc.c` mixes by fs / 4 and rotates the residual, so the ADC rate follows
  the IF: fs is about 4 x LIF, and must be 144 MHz divided by an integer (README "ADC test").
  Candidate pairs: LIF 3.6 MHz at 14.4 MS/s, LIF 4.5 MHz at 18 MS/s, LIF 5.14 MHz at
  20.57 MS/s. The DVP takes up to 24 MS/s and the MS9280 32 MS/s; the CPU load of
  `ddc_process()` at the higher rates needs checking with the load model in `ddc_plan()`.
  The data short gives no LIF range for the 1.7 MHz bandwidth. Linux sets 5 MHz for DVB-T/T2
  and 3.25 MHz for ATSC (6 MHz bandwidth), so values below the data short ranges are accepted
  at least in 6 MHz mode. Test how low the 1.7 MHz mode goes before fixing the rate plan.
- Level. 0.5 to 2.0 Vppd programmable differential output. The MS9280 has a single-ended
  input, so a differential to single-ended stage (op-amp or transformer) with an anti-alias
  low-pass at about fs / 2 sits in between, as on the R820T2 board.
- AGC. In the Si2151 TV design AGC1 takes the RC-filtered PWM/open-drain AGC output of the
  demodulator, the same principle as the R820T2 IF_AGC from PA15 (see "External components
  per pin"). Polarity, voltage range and whether the tuner can regulate the LIF level without
  it are unknown; property 0x0702 selects the AGC source. Route PA15 through the RC stages to
  AGC1.
- Host interface. librtlsdr drives an R820T2 by register writes. With an Si2141 the firmware
  has to answer the R820T2 probe and translate the PLL and gain register writes into Si2141
  tune and gain commands, instead of forwarding them as `src/r820t2.c` does now. The R820T2 LO
  sits above the RF by the IF; the Si2141 takes the RF frequency directly.
- Crystal. 24 MHz, CL 8 pF, +/-25 ppm (Si2151 sources, see "External components per pin").
  The ppm correction through `rtlsdr_set_freq_correction()` must be mapped onto the Si2141
  tune frequency or its crystal trim property 0x0402.
- Supply. 3.3 V only works with the internal LDO; plan for about 0.5 W plus the LDO loss.

## Reference schematics

No public Si2141 schematic, reference design or application note was found. The closest public
circuits are for the pin-compatible Si2151:

- TV mainboard service manual with an Si2151-A10 tuner page (single and dual RF connector
  variants and an EWBS variant, MSB3400 demodulator), page 32 of 44, document title "调试说明",
  created 2019 with WPS Office; the TV model is not named in the file:
  https://manuals.plus/m/a3816a0c76164722c741d56b67c8890364315499fba4246eec39b31d23e30677.pdf.
  The component table in "External components per pin" is read from its single-connector
  variant.
- Sekorm (Silicon Labs distributor in China) design notes on the Si2151 reference design:
  application design (supply, RF input filter, ADDR, LIF, crystal), PCB and schematic tips,
  common design problems, and FAQs on the crystal, I2C address and AGC pins. The pages block
  automated access; their content here comes from search excerpts.

Further:

- Skyworks offers the SI2141-A-EVB "under NDA", with GUI software and an Si2183 DVB-T2/T/C and
  ISDB-T demodulator. Mouser lists it (634-SI2141-A-EVB); RX Electronics lists an
  SI2141-B-EVB.
- No open hardware design with the Si2141 or its sisters (Si2144, Si2151) was found on GitHub,
  OSHWLab/EasyEDA or in KiCad projects.
- The data short has the application block diagram (Si2141 + demodulator + SoC, crystal,
  3.3 V / 1.8 V supply options) and the pinout above, but no component values.
- The Silicon Labs layout application note AN813, referenced by the Sekorm notes, is not public.

The consumer sticks below show an Si2141 layout in practice; photos of a T230C v2 board would
confirm whether the Si2151 values carry over to the Si2141.

## Products with the Si2141

| Product                                   | USB ID      | Demodulator | Source of the chip identification       | Availability (2026-11-08)                      |
|-------------------------------------------|-------------|-------------|-----------------------------------------|------------------------------------------------|
| MyGica (Geniatech) T230C                  | 0572:c689   | Si2168-D60  | Linux `dvbsky.c`, patch cover letter    | Geniatech shop out of stock ($28); AliExpress "MYGICA Official Store"; cntronic.com $40.60; Amazon UK unavailable |
| MyGica T230C v2                           | 0572:c68a   | Si2168-D60  | Linux `dvbsky.c`, dmesg logs            | sold as "T230C", hardware version not stated in listings |
| MyGica T230C Lite                         | 0572:c699   | Si2168      | Linux `dvbsky.c`                        | not found                                      |
| MyGica T230C v2 Lite                      | 0572:c69a   | Si2168      | Linux `dvbsky.c`                        | not found                                      |
| MyGica T230A                              | 0572:689a   | Si2168-D60  | Linux `dvbsky.c`, LibreELEC forum logs  | not checked                                    |
| August DVB-T210 v2                        | -           | Si2168-D60  | firmware files and dmesg in user reports | Amazon.de/.it/.fr ASIN B008D8K50Q; listings do not state v1 or v2 |

The original MyGica T230 (0572:c688) uses an Si2157, not the Si2141. Linux registers the Si2141
on these sticks with `dvb_module_probe("si2157", "si2141", ...)` at 0x60 and `if_port = 0`.
The Si2141 support was traced from the Windows driver of the T230C and added in commit
3a2824c72ab5 ("[media] si2157: Add support for Si2141-A10", 2017).

No SDR product with the Si2141 was found.

## Open questions

1. Lowest LIF center frequency in the 1.7 MHz mode (decides the ADC rate).
2. AGC1 voltage range and polarity, and whether the internal AGC suffices without it.
3. Whether the Si2151 component values (crystal, RF input network, LIF and AGC RC stages)
   carry over to the Si2141 unchanged; a T230C v2 board would show it.
4. Reception with ROM firmware only versus the patch, and the license of the patch.
5. LIF output voltage setting (property number unknown).

## Sources

- Si2141 data short, Skyworks, 2021:
  https://www.skyworksinc.com/-/media/SkyWorks/SL/documents/public/data-shorts/Si2141-short.pdf
- Si2141 data short, Silicon Labs, 2014:
  https://www.mouser.com/datasheet/2/368/Si2141-short-1365891.pdf
- Si2151 data short (same pinout), Silicon Labs, 2014:
  https://cdn.promelec.ru/upload/items/2025/11/24/6_all_pdf/Si2151-short-1365899.pdf
- Si2151 TV tuner schematic, service manual page 32:
  https://manuals.plus/m/a3816a0c76164722c741d56b67c8890364315499fba4246eec39b31d23e30677.pdf
- Sekorm Si2151 design notes: https://www.sekorm.com/news/31233666.html (application design),
  https://www.sekorm.com/news/44595386.html (PCB and schematic),
  https://www.sekorm.com/news/84516786.html (common design problems),
  https://www.sekorm.com/faq/2170.html (I2C address), https://www.sekorm.com/faq/18370.html
  (AGC pins), https://www.sekorm.com/faq/44360.html (Si2141)
- Si2141 product page and evaluation kit:
  https://www.skyworksinc.com/en/Products/TV-and-Video/Si2151-41-6th-Generation-Silicon-TV-Tuner-ICs/Si2141,
  https://www.skyworksinc.com/en/Products/TV-and-Video/Evaluation-Kits/si2141-evaluation-kit
- SI2141-A-EVB at Mouser:
  https://www.mouser.com/ProductDetail/Skyworks-Solutions-Inc/SI2141-A-EVB?qs=p9T7GgSe1IFAiEyqELd27A%3D%3D
- Linux driver: `drivers/media/tuners/si2157.c`, `si2157_priv.h`,
  `drivers/media/usb/dvb-usb-v2/dvbsky.c`, `include/media/dvb-usb-ids.h` (torvalds/linux master)
- MyGica T230C patch series, cover letter:
  https://lkml.iu.edu/hypermail/linux/kernel/1702.1/03094.html,
  https://patchwork.kernel.org/patch/9578575/
- T230C v2 support patch:
  https://www.mail-archive.com/linux-media@vger.kernel.org/msg147755.html
- Firmware: https://github.com/osmc/dvb-firmware-osmc/blob/master/dvb-tuner-si2141-a10-01.fw,
  https://github.com/LibreELEC/dvb-firmware
- User reports: https://forum.libreelec.tv/thread/23142-mygica-t230a/?pageNo=3,
  https://tvheadend.org/boards/5/topics/10864?r=22863,
  https://owenashurst.com/post/setting-up-live-tv-with-jellyfin-with-a-usb-august-t210-v2-tv-receiver
- Products: https://shop.geniatech.com/product/pc-tv-tuner/,
  https://www.aliexpress.us/item/2251801697283219.html,
  https://cntronic.com/dvb-t2-geniatech-mygica-usb-tv-tuner-stick-t230c-dvb-c-dvb-t-hd-tv-for-russia-thailand-colombia-europe-win10-android-os-1565,
  https://www.amazon.de/August-DVB-T210-TV-Tuner-Fernbedienung-kompatibel-Schwarz/dp/B008D8K50Q
- Distributors: LCSC C2984260
  https://www.lcsc.com/product-detail/Video-Interface-ICs_SKYWORKS-SILICON-LABS-SI2141-A10-GMR_C2984260.html,
  https://www.tme.com/us/en-us/details/si2141-a10-gm/rtv-audio-integrated-circuits/skyworks-solutions/
