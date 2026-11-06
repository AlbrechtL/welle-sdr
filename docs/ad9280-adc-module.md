# AD9280 ADC module

Notes on the AliExpress "AD9280 high-speed data acquisition ADC module, 8-bit, 32 MSPS"
(60 mm x 36 mm, ADC only, no DAC).

- Listing: https://de.aliexpress.com/item/1005013480134736.html
- Product photo watermark: "...Embedded" (probably the store name)

## Schematic status

No public schematic for this exact board was found (searched: AliExpress page data,
OSHWHub, JLC 硬创社, GitHub, CSDN, Zhihu, datasheet sites). The board is not the common
AD9280 + AD9708 AD/DA board (ALINX AN108, Zhengdian Atom ATK-HS-AD/DA); those schematics
do not apply directly.

To obtain it:

1. Ask the seller via AliExpress chat for the "资料" (data package) or "原理图" (schematic).
2. Otherwise reverse-engineer it from the board: about 20 passives and 3 ICs.

## Board contents (product photos and close-ups of the board)

| Ref                     | Part / function                                                      |
|-------------------------|----------------------------------------------------------------------|
| U2                      | AD9280ARSZ (SSOP-28), 8-bit 32 MSPS ADC                              |
| U1                      | AMS1117-3.3 regulator, probably the ADC digital output supply (DRVDD)|
| IC1 (SO-8)              | MC34063A switching regulator, +5 V to -5 V inverting converter       |
| D1                      | SS14 Schottky diode (1 A, 40 V), catch diode of the inverter         |
| L3                      | Power inductor, marked "101" (100 uH), inverter inductor             |
| R2, R3                  | 3.00 kOhm ("3001"), 1 kOhm ("102"): MC34063A feedback divider        |
| R1                      | Marked "0" (0 Ohm), probably the MC34063A current sense (Rsc) bridge |
| C1                      | Probably the MC34063A timing capacitor (C_T)                         |
| L2                      | Filter inductor/ferrite on the -5 V rail                             |
| U4 (SO-8)               | TL072C dual JFET op-amp (TI), input attenuation/offset and ADC driver|
| U3 (SOT-23-6)           | Marked "HRA"; function not identified (input stage)                  |
| R4, R7, R9, R10         | 10 Ohm ("100"), 2.00 kOhm ("2001"), 20.0 kOhm ("2002"), 10 kOhm ("103")|
| D2, D3                  | Input clamp diodes                                                   |
| P1 (SMA), J3 (2-pin)    | Analog input "AD IN" (SMA or pin header)                             |
| J1 (3-pin)              | Output of the generated rails: -5V, GND, +5V                         |
| J2 (pin header)         | Power and digital interface, see below                               |

## J2 pin header

Single row, 1 x 11 pins, 2.54 mm pitch, straight male header on the board edge next to the
AD9280. Pin 1 has the square pad and is at the end labeled "J2" / "+5V" (the corner near
the 100 uF capacitor and the AMS1117). The silkscreen labels +5V, GND, DB0, DB1, DB7 and
DCLK; an arrow from DB1 to DB7 marks the data bits in between as sequential.

| Pin | Label | Direction    | Function                                                  |
|-----|-------|--------------|-----------------------------------------------------------|
| 1   | +5V   | Power in     | Board supply, feeds the AMS1117-3.3 and the -5 V converter |
| 2   | GND   | Power        | Ground                                                    |
| 3   | DB0   | Output       | Data bit 0 (LSB), AD9280 D0                               |
| 4   | DB1   | Output       | Data bit 1                                                |
| 5   | DB2   | Output       | Data bit 2                                                |
| 6   | DB3   | Output       | Data bit 3                                                |
| 7   | DB4   | Output       | Data bit 4                                                |
| 8   | DB5   | Output       | Data bit 5                                                |
| 9   | DB6   | Output       | Data bit 6                                                |
| 10  | DB7   | Output       | Data bit 7 (MSB), AD9280 D7                               |
| 11  | DCLK  | Input        | Sample clock to the AD9280 CLK pin, max 32 MHz            |

Notes:

- The output code is straight binary: 0x00 at the bottom of the ADC range, 0xFF at the
  top (AD9280 datasheet).
- Data on DB0..DB7 belongs to the sample taken 3 clock cycles earlier. A new word is output
  after every rising DCLK edge and is valid at most 25 ns (t_OD) later. Which edge to latch
  on depends on the clock rate, see "ADC module wiring" in the README.
- Logic levels: the data outputs swing to the AD9280 DRVDD, most likely 3.3 V from the
  AMS1117. Not verified; measure DB0..DB7 high level before connecting to a 3.3 V-only
  input.
- OTR (out of range), STBY and THREE-STATE are not brought out on J2.
- The back side has an unpopulated 4-pad footprint "CY1" with R11, the size of an SMD
  crystal oscillator. If an oscillator is fitted there, it presumably drives the ADC clock
  and DCLK then carries that clock instead of taking it from the host. With CY1 empty, the
  host must drive DCLK. Unverified; trace the board to confirm.
- Pin 1 is the square pad (front photo); confirm by continuity to the +5V/GND capacitors
  before applying power, because the header carries no pin numbers.

## AD9280 key facts

From the AD9280 datasheet [1] and the pin-compatible 3PA9280 datasheet [2].

- 8-bit, up to 32 MSPS, single supply 2.7 V to 5.5 V, about 95 mW.
- The clock is supplied externally on CLK; no configuration interface.
- Pipeline latency: 3 clock cycles plus about 25 ns output delay.
- Input range typically 0 V to 2 V: 0 V gives code 0, 2 V gives code 255.
- OTR goes high when the input is out of range (check whether the board routes it; it is
  not on the J2 silkscreen).
- Output levels follow DRVDD; at 3.3 V they are compatible with 3.3 V logic.

## Analog front end

The board has an op-amp attenuation/offset stage that maps a bipolar input into the
ADC's 0 V to 2 V range and is powered from the on-board -5 V rail. The closest documented
design, the Zhengdian Atom ATK-HS-AD/DA module [11], uses:

    V_adc = V_in / 5 + 1        (-5 V -> 0 V, +5 V -> 2 V)

This board likely behaves similarly. The resistors around U3/U4 (R7 2 kOhm, R9 20 kOhm,
R10 10 kOhm) suggest attenuation ratios in that range.

Measured with the CH32V307 DVP capture at 8 MS/s (README "ADC test"), DC at AD IN, mean of
131072 samples:

| AD IN | Mean code | Spread        |
|-------|-----------|---------------|
| 0 V   | 126.3     | 0x7B to 0x82  |
| 3.3 V | 208.4     | 0xCF to 0xD2  |

With 1 LSB = 2 V / 256: slope 24.9 codes/V (gain 1/5.14), offset 0.99 V, so

    V_adc = 0.99 V + V_in / 5.14        (code 0x00 at about -5.1 V, 0xFF at about +5.2 V)

which confirms the assumed V_in / 5 + 1 within a few percent. Only two points were
measured; linearity and input impedance are not checked. At 24 MS/s the codes are 1.5 to
4.5 LSB lower and carry a tone at fs/4 of about +/-1 LSB.

### Block diagram

Derived from the parts on the board and their placement; the connections are not traced.
Dotted lines and "?" mark assumptions.

```
 Signal path
 ===========

  P1 SMA ----+
  (AD IN)    |   +-----------------+     +-------------------+     +-----------------+
             +-->| Input           |     | U4A TL072C        |     | U4B TL072C      |
  J3 header -+   | protection      |---->| attenuation +     |---->| ADC driver /    |---+
                 | R6 series       |     | offset            |     | buffer          |   |
                 | D2, D3 clamps   |     | R7 2k, R9 20k,    |     +-----------------+   |
                 +-----------------+     | R10 10k           |                           |
                                         +-------------------+                           |
                 +-----------------+               ^                                     |
                 | U3 "HRA"        |  offset       :                                     |
                 | SOT-23-6        |. . . . . . . .:                                     |
                 | buffer / ref?   |  voltage?                                           |
                 +-----------------+                                                     |
                                                                                         |
     +-----------------------------------------------------------------------------------+
     |
     |   +-----------------+     +-------------------+     +-------------------+
     +-->| Series R + C    |     | U2 AD9280         |     | J2                |
         | R4 10R?         |---->| VINA 0 V .. 2 V   |---->| DB0..DB7 (out)    |
         | isolation /     |     | 8 bit, 32 MSPS    |<----| DCLK (in)         |
         | anti-alias      |     +-------------------+     +-------------------+
         +-----------------+               |
                                 +-------------------+
                                 | C10 .. C15        |
                                 | reference         |
                                 | decoupling        |
                                 +-------------------+

 Supplies
 ========

                 +-------------------+
  J2 +5V ---+--->| IC1 MC34063A      |---- -5 V ----> U4 V-, J1 -5V
            |    | inverter, L3, D1  |
            |    +-------------------+
            |
            |    +-------------------+
            +--->| U1 AMS1117        |---- 3.3 V ---> U2 DRVDD (digital outputs)?
            |    +-------------------+
            |
            +------------------------------ +5 V ---> U4 V+, U2 AVDD?, J1 +5V

  ---->  path derived from the parts placement, not traced
  . . :  assumed connection, unconfirmed
```

Signal levels along the chain (assuming the ATK-style +/-5 V input range):

| Point              | Range (assumed)  | Notes                                     |
|--------------------|------------------|-------------------------------------------|
| AD IN              | -5 V to +5 V     | Clamped by D2/D3 beyond the rails         |
| U4A output         | 0 V to 2 V       | Needs the -5 V rail to reach 0 V          |
| AD9280 VINA        | 0 V to 2 V       | Code 0x00 at 0 V, 0xFF at 2 V             |

Bandwidth is limited by the TL072C to about 1 MHz to 2 MHz (see below).

### -5 V rail (IC1, MC34063A)

IC1 is an MC34063A in the inverting topology (L3, D1 SS14, output capacitors). Its output
voltage is set by the feedback divider [9]:

    V_out = -1.25 V x (1 + R_a / R_b) = -1.25 V x (1 + 3 kOhm / 1 kOhm) = -5.0 V

with R_a = R2 (3.00 kOhm, output to feedback) and R_b = R3 (1 kOhm, feedback to ground),
assuming the usual connection; not traced. The rail powers the negative supply of the
TL072C and is also brought out on J1. The MC34063A switches at roughly 20 kHz to
100 kHz (set by C1); expect ripple at that frequency on the -5 V rail and possibly in
the ADC data.

### Role of U4 (TL072C)

Estimated from the AD9280 datasheet and the board layout; not traced yet. U4 sits between
the input stage (U3, R4, R7, R9, R10) and the AD9280 input capacitors C10 to C15, which
fits the role of the ADC driver.

- Level shift and attenuation: the AD9280 only converts 0 V to 2 V (unipolar), while
  the input at AD IN is bipolar. One TL072 half most likely attenuates and adds an
  offset (summing amplifier), as in V_adc = V_in / 5 + 1.
- ADC driver: the AD9280 input is a switched-capacitor sample-and-hold that draws charge
  spikes on every clock. It needs a low-impedance source; the datasheet recommends an
  op-amp driver. The op-amp output decouples the input source from these spikes.
- High input impedance: the JFET inputs draw only picoamperes, so large divider
  resistors at the input cause no offset error ((R1 + R2) x I_B, datasheet).
- Second half: either a buffer for the offset reference, or the second stage of an
  inverting chain (two inversions give a non-inverted output).
- The -5 V rail: the TL072 is not rail-to-rail (output stays about 1.5 V to 2 V from
  each rail) and needs a split supply to process negative inputs and drive its output
  down to 0 V. That is why the board has the MC34063A -5 V converter.

Limitation: the TL072 has about 3 MHz gain-bandwidth and 13 V/us slew rate [8]. Full-power
bandwidth for a 2 Vpp output is about SR / (pi x 2 V) = 2 MHz. The analog front end, not
the ADC's 16 MHz Nyquist limit, sets the usable bandwidth to roughly 1 MHz to 2 MHz, with
rising distortion above that. The R820T2 IF band reaches 2.65 MHz for DAB at 2.048 MS/s
and 6.57 MHz at 2.56 MS/s and above (see "Required sample rate"), at or beyond that
limit. Options: replace U4 with a pin-compatible fast dual op-amp
(e.g. AD8056, OPA2690, AD8066; check supply range and stability at the board's gain) or
bypass the stage with AC coupling into the AD9280 biased at the middle of its configured
input range (1 V for the 0 V to 2 V range these boards use).

Measure before relying on it: feed known DC voltages into AD IN and read DB0..DB7.
Also determine the input impedance and the front-end bandwidth.

## Connecting an R820T2 tuner to J3

### R820T2 IF output

From the R820T datasheet [4] unless noted.

| Item                  | Value                                                          |
|-----------------------|----------------------------------------------------------------|
| Pins                  | IF_N (pin 12), IF_P (pin 13), QFN-24; differential             |
| Level                 | 1 Vpp typical, 2 Vpp maximum (differential)                    |
| Load                  | 2 kOhm // 5 pF differential                                    |
| Reference circuit     | Each pin through 220 Ohm and 100 nF to the demodulator input   |
| IF center frequency   | Low IF chosen by librtlsdr from the sample rate: 1.625 MHz at 2.048 MS/s, 3.57 MHz at 2.56 MS/s and above (see "Required sample rate") [6][7]; `welle-sdr status` shows it as `IF:` |
| Gain control          | IF VGA via register 0x0C (VGA_MODE bit 4 = 0) or the IF_AGC pin (bit 4 = 1) |

The R820T2 output is differential with a DC bias; J3 is a single-ended input to ground.
The connection therefore needs AC coupling and a differential to single-ended conversion.

### J3

J3 is a 2-pin header next to the SMA input P1 and is most likely wired in parallel with
it: one pin AD IN (SMA center), the other GND (SMA shell). Neither this nor the pin order
is printed on the board; check with a continuity tester against the SMA before
connecting. Leave nothing connected to the SMA
while J3 is used.

### Wiring, option A: transformer (recommended)

A 1:1 wideband RF transformer converts the differential IF to single-ended without loss
and blocks the DC bias. Any 1:1 part covering 1 MHz to 10 MHz works, for example
Mini-Circuits ADT1-1WT or Coilcraft WBC1-1TL.

```
  R820T2 board                         transformer 1:1             AD9280 module
                  220R    100n        +--------------+
  IF_P (13) ----[====]----||----------+ pri      sec +------------ J3 AD IN
                                      |   )||(       |
  IF_N (12) ----[====]----||----------+ pri      sec +----+------- J3 GND
                  220R    100n        +--------------+    |
  GND ----------------------------------------------------+------- J2 GND
```

- The 220 Ohm and 100 nF parts follow the R820T reference circuit; the capacitors are
  optional with a transformer but keep its primary free of DC between IF_P and IF_N.
- Signal at J3: about 1 Vpp typical, single-ended.

### Wiring, option B: single-ended pickup (quick test)

Without a transformer, use one output and terminate the other:

```
  IF_P (13) ----[220R]----||100n---------------------------------- J3 AD IN
  IF_N (12) ----[220R]----||100n----[1k]---+
  GND -------------------------------------+---------------------- J3 GND, J2 GND
```

- Only half the signal reaches J3 (about 0.5 Vpp typical).
- Common-mode noise is not rejected; ripple from the MC34063A on the ADC board and USB
  noise couple in directly. Keep the wires short and twisted with GND.

### Ground and supply

- Tie GND of the CH32V307 board, the R820T2 board and the ADC module together (J2 GND).
- The R820T2 runs from 3.3 V, the ADC module from 5 V on J2. Do not feed the tuner from
  the module's J1 rails.
- The R820T2 datasheet asks for less than 30 mVpp ripple on its 3.3 V supply; do not
  power it from the same wire that carries the MC34063A input current.

### IF gain

librtlsdr's register init sets VGA_MODE (0x0C bit 4 [5]) to pin control and switches it to
register control once the host sets a gain mode (`rtlsdr_set_tuner_gain_mode()`, done by
`rtl_sdr`, `rtl_test` and `rtl_tcp`) [6]. With pin control and IF_AGC floating the IF gain is
undefined: give IF_AGC a defined voltage (datasheet range 0.6 V to 2.5 V, 10 kOhm series
resistor in the reference circuit) or check that the tuner board already does.

The firmware drives IF_AGC from PA15 through the RC low-pass on the tuner board, at 1.55 V
from boot. With the host's RTL AGC on, it keeps pin control selected and sets the voltage
from the ADC level. See "IF AGC" in the README.

### Limits of this path

Feeding the R820T2 through J3 uses the board's analog front end, which is not designed for
this signal:

- Level: the front end is assumed to map +/-5 V to the ADC's 0 V to 2 V, a factor of
  about 0.2. A 1 Vpp IF then becomes about 0.2 Vpp at the ADC, about 26 of 256 codes
  (less than 5 bits). Raise the R820T2 IF gain to its 2 Vpp maximum, or change the
  attenuation.
- Bandwidth: the TL072C (about 3 MHz gain-bandwidth) attenuates and phase-shifts the
  upper part of the IF band. For DAB at 2.048 MS/s (IF 1.625 MHz, band 0.6 MHz to
  2.65 MHz) the band sees a sloped response; a 3.57 MHz IF is mostly outside the usable
  range.
- Better: bypass U4 and feed the IF (after the transformer) AC-coupled directly into the
  AD9280 input, biased at 1 V, the middle of its 0 V to 2 V range. A 1 Vpp to 2 Vpp IF
  then uses half to the full ADC range. This needs a board modification; trace the board
  first.

## Sample clock (DCLK)

The AD9280 has no internal clock. Every conversion is started by DCLK (J2 pin 11, the
AD9280 CLK pin), so the clock rate is the sample rate and its quality limits the SNR.

### AD9280 clock requirements

Values from the 3PA9280 datasheet [2], which is pin-compatible and repeats the AD9280
specifications [1]; check against the ADI datasheet before relying on a limit.

| Parameter                     | Value                    | Consequence                              |
|-------------------------------|--------------------------|------------------------------------------|
| Conversion rate               | max 32 MSPS              | DCLK up to 32 MHz                        |
| Clock pulse width high / low  | min 14.7 ns each         | Duty cycle must stay near 50 % at high rates |
| CLK input threshold           | AVDD / 2, CMOS levels    | 3.3 V CMOS swing; with AVDD = 5 V the threshold is 2.5 V, use an HCT/ACT buffer |
| Edges used                    | both                     | Track while CLK is low, hold (sample) on the rising edge |
| Aperture delay / jitter       | 4 ns / 2 ps              | ADC jitter is negligible; the clock source dominates |
| Pipeline latency              | 3 clock cycles           | Output belongs to the sample 3 clocks earlier |
| Data valid delay tOD          | 25 ns after rising edge  | Latch the data on the falling edge if the half period > 25 ns (fs < 20 MHz) |

The datasheet recommends HC/HCT or AC/ACT CMOS logic to drive CLK, for symmetrical
thresholds and fast edges [2].

### Required sample rate

The IF is not fixed. librtlsdr derives it from the tuner filter bandwidth, which defaults
to the host's sample rate (`rtlsdr_set_sample_rate()` calls `r82xx_set_bandwidth()` and
then programs the result as the RTL2832U IF) [6][7]. `R82XX_IF_FREQ` (3.57 MHz) only
applies until the first sample rate is set.

| Host sample rate          | IF (librtlsdr)  | R820T2 filter bandwidth | Signal band at the ADC (IF +/- BW/2) |
|---------------------------|-----------------|-------------------------|--------------------------------------|
| 1.024 MS/s                | 1.700 MHz       | 1.20 MHz                | 1.10 MHz .. 2.30 MHz                 |
| 2.048 MS/s (DAB, welle.io)| 1.625 MHz       | 2.05 MHz                | 0.60 MHz .. 2.65 MHz                 |
| 2.4 MS/s                  | 1.815 MHz       | 2.43 MHz                | 0.60 MHz .. 3.03 MHz                 |
| 2.56 MS/s .. 3.2 MS/s    | 3.570 MHz       | 6 MHz                   | 0.57 MHz .. 6.57 MHz                 |

Values computed from `r82xx_set_bandwidth()` [6]; `welle-sdr status` shows the IF the host
actually programmed.

Rules for choosing fs:

- Nyquist: fs > 2 x upper band edge. For DAB at 2.048 MS/s that is fs > 5.3 MHz. The
  board has no anti-alias filter apart from the R820T2 IF filter skirt and the TL072C
  roll-off, so leave margin: fs of 8 MHz or more.
- The host expects complex samples at its rate (2.048 MS/s for DAB). The firmware has to
  mix the real IF down and decimate. An fs that is an integer multiple of the host rate
  (8.192 MHz = 4 x 2.048 MHz) avoids a fractional resampler.
- Processing load: at 8 MS/s a 144 MHz CH32V307 has about 18 cycles per sample for
  capture, mixing and filtering. Keep fs as low as Nyquist allows.
- The USB path currently carries about 2.25 MS/s complex (see `AGENTS.md`), so the
  decimated output must stay at or below that.

### Clock source options

| Source | Achievable rates | Pros | Cons |
|--------|------------------|------|------|
| A: CH32V307 timer PWM output (timer clock 144 MHz) | 144 MHz / N, 50 % duty for even N: 24, 18, 14.4, 12, 9, 8 MHz (N = 6, 8, 10, 12, 16, 18) | No extra parts; the same timer can trigger the capture DMA, so clock and capture are synchronous | No integer multiple of 2.048 MHz; 28.8 MHz (N = 5) violates the 14.7 ns pulse width (13.9 ns low or high) |
| B: R820T2 CLK_Out (pin 10), 28.8 MHz | 28.8 MHz, or 14.4 / 7.2 MHz after a divider | Same reference as the tuner LO, like a real RTL2832U, so frequency errors track | Triangle wave, 2 Vpp typical into 16 pF // 10 MOhm [4]: needs AC coupling and a squaring buffer (e.g. self-biased 74LVC1GU04) and a divider (74LVC74); capture must follow this external clock |
| C: Crystal oscillator, e.g. 24.576 MHz / 3 or 16.384 MHz / 2 | 8.192 MHz | Exact multiple of 2.048 MS/s, low jitter | Extra parts; capture must follow the external clock |
| D: Oscillator on the module's CY1 footprint (back side) | depends on the part | On-board, short clock path | Footprint and routing not traced; DCLK would then likely be an output |

Recommended starting point: option A at 8 MHz (144 MHz / 18) with the timer also
triggering the capture on the falling edge. It needs no extra hardware; the 8 MHz to
2.048 MHz ratio (125/32) needs a rational resampler, which can later be removed by moving
to option C. Untested.

### Capture timing

- Sample n is taken on rising edge n and appears on DB0..DB7 3 clocks later, valid tOD
  (25 ns) after rising edge n+3 [2]. At 8 MHz (125 ns period), latching on the falling
  edge leaves 37.5 ns of margin.
- If DCLK comes from the MCU, generate it and the capture trigger from the same timer
  (two compare channels) so their phase is fixed.
- If DCLK comes from an external clock (options B to D), the MCU must capture with that
  clock as strobe, for example the CH32V307 DVP interface with DCLK on its pixel clock
  input. Pin mapping and limits: "ADC module wiring" in the README.

### Clock jitter budget

Jitter limits the SNR for an input at frequency f_in to

    SNR_jitter = -20 x log10(2 x pi x f_in x t_j)

An ideal 8-bit ADC reaches 6.02 x 8 + 1.76 = 49.9 dB. To keep jitter below that:

| Case                       | Highest f_in | Max jitter t_j |
|----------------------------|--------------|----------------|
| DAB, IF 1.625 MHz          | 2.65 MHz     | 190 ps rms     |
| IF 3.57 MHz, 6 MHz filter  | 6.57 MHz     | 77 ps rms      |

A timer output or crystal oscillator meets this easily. A squared triangle wave (option
B) has more jitter but should still be within the DAB budget; not measured.

## References

1. Analog Devices, AD9280 datasheet, Rev. E (specifications, reference and evaluation
   board schematics):
   https://www.analog.com/media/en/technical-documentation/data-sheets/AD9280.pdf
2. 3PEAK, 3PA9280 datasheet (pin-compatible to the AD9280; clock timing, input SHA,
   clock input section):
   https://static.3peak.com/res/doc/ds/Datasheet_3PA9280.pdf
3. Analog Devices, AD9280 product page: https://www.analog.com/en/products/ad9280.html
4. Rafael Micro, R820T datasheet, preliminary 2011-11-30 (IF output level, pinout,
   reference circuit, clock output):
   https://rtl-sdr.com/wp-content/uploads/2013/04/R820T_datasheet-Non_R-20111130_unlocked.pdf
5. Rafael Micro, R820T2 register description (0x0C VGA_MODE):
   https://www.rtl-sdr.com/wp-content/uploads/2016/12/R820T2_Register_Description.pdf
6. osmocom rtl-sdr, `src/tuner_r82xx.c` (`r82xx_set_bandwidth()`, IF selection):
   https://github.com/osmocom/rtl-sdr/blob/master/src/tuner_r82xx.c
7. osmocom rtl-sdr, `src/librtlsdr.c` (`R82XX_IF_FREQ`, `r820t_set_bw()`,
   `rtlsdr_set_sample_rate()`):
   https://github.com/osmocom/rtl-sdr/blob/master/src/librtlsdr.c
8. Texas Instruments, TL072 datasheet (gain-bandwidth, slew rate, output swing):
   https://www.ti.com/lit/ds/symlink/tl072.pdf
9. Texas Instruments, MC34063A datasheet (inverting converter, output voltage formula):
   https://www.ti.com/lit/ds/symlink/mc34063a.pdf
10. ALINX, AN108 AD/DA module manual (AD9280 with AD8056 +/-5 V attenuator):
    https://manuals.plus/alinx/an108-high-speed-ad-da-module-manual
11. Zhengdian Atom, high-speed AD/DA chapter (front-end formula V_in / 5 + 1, Verilog
    example): https://zhuanlan.zhihu.com/p/503272911,
    https://blog.csdn.net/weixin_55796564/article/details/121223853
12. gojimmypi, notes on AD9280/AD9708 modules (header numbering pitfalls):
    https://gojimmypi.github.io/ad9280-ad9708-ad-da-module-for-ax/
13. gallicchio/icepi-zero-dac-adc (KiCad adapter board, FPGA tutorials):
    https://github.com/gallicchio/icepi-zero-dac-adc
14. JLC 硬创社, AD9280/AD9708 module (paid design, SI5351 clock):
    https://www.jlc-ycs.com/platform/detail/8ea6de139dd4401db378d7e0431bb503?type=1
15. AliExpress product listing:
    https://de.aliexpress.com/item/1005013480134736.html
