# ISL Board (RAK_feather) — Pin Map

Extracted from `hardware/iteration1/RAK_feather_schematic.pdf` + `RAK_feather.csv`.
Board = **RAK4630 (U1) + RV-3028-C7 RTC (U2) + RT9080-33 3.3 V LDO (U3) + USB-C**,
with external **L76K GPS** and **AS3933 wake-up receiver** modules on the headers.

> This board is RAK4630-based like Tracker 6.0, but the **pinout is different** —
> do not assume Tracker 6.0 pins. Verified nets below.

## Core peripherals

| Function | Net | RAK4630 pin | nRF52840 GPIO | Notes |
|----------|-----|-------------|---------------|-------|
| I2C SDA (RTC) | `I2C_SDA` | 4 | **P0.13** | primary I2C = `Wire` |
| I2C SCL (RTC) | `I2C_SCL` | 5 | **P0.14** | RV-3028 @ 0x52 (CONFIRMED present) |
| RTC INT (wake) | `RTC_INT` | 27 | **P1.03** | RV-3028 ~INT; 10k pull-up (R7). *Tracker used P1.04!* |
| RTC EVI | `RTC_EVI` | — | header only | event input, not on a dedicated MCU pin |
| RTC CLKOUT | `RTC_CLKOUT` | — | header only | 10k pull-up (R8) |

## Wake-Up Receiver (AS3933) — SPI + wake

| Function | Net | RAK4630 pin | nRF52840 GPIO |
|----------|-----|-------------|---------------|
| SPI CLK | `SPI_CLK` | 29 | **P0.03** (QSPI_CLK) |
| SPI MOSI → AS3933 SDI | `SPI_MOSI` | 33 | **P0.30** (QSPI_DIO0) |
| SPI MISO ← AS3933 SDO | `SPI_MISO` | 32 | **P0.29** (QSPI_DIO1) |
| SPI CS | `SPI_CS` | 34 | **P0.26** (QSPI_CS) |
| **WuR wake IRQ** | `WuR_WAKE` | 28 | **P1.04** (LED2) |

- AS3933 CS is **active-HIGH** (idle LOW), SPI **Mode 1**, MSB-first.
- WAKE asserts **HIGH** on pattern/frequency detect → drives P1.04. Clear with the
  `CLEAR_WAKE` (0x00) direct command.
- SPI is bit-banged in `tests/ISL_WUR_AS3933` (avoids RUI3 SPI-instance ambiguity
  and the RC-calibration needs manual clocking anyway).

## GPS (L76K) — external module on J6

| Function | Net | RAK4630 pin | nRF52840 GPIO | Notes |
|----------|-----|-------------|---------------|-------|
| GPS UART RX (← GPS TX) | `UART1_RX` | 9 | **P0.19** | module "UART1" |
| GPS UART TX (→ GPS RX) | `UART1_TX` | 10 | **P0.20** | |
| GPS power enable | `L76K_EN` | 41 | **P0.04** (AIN2) | drives Q1 AO3407 P-FET |
| GPS 1PPS | `L76_1PPS` | — | header only | not on a dedicated MCU pin |
| GPS supply (switched) | `L76K_SUPPLY` | — | Q1 drain | |

- **UNKNOWN to verify on bench:** which RUI3 `Serial` instance maps to P0.19/P0.20.
  Tracker 6.0's GPS was on P0.15/P0.16 = `Serial1`; here it's P0.19/P0.20. The raw
  GNSS monitor test will show which instance receives NMEA.
- **UNKNOWN to verify:** `L76K_EN` polarity. Q1 is a P-FET high-side switch with a
  10k gate pull-up (R4) to 3.3 V, so **most likely active-LOW** (drive P0.04 LOW =
  GPS ON). The test has a `GPS_EN_ACTIVE_LOW` flag — flip it if there's no power/data.

## Battery / ADC

| Function | Net | RAK4630 pin | nRF52840 GPIO |
|----------|-----|-------------|---------------|
| Battery sense | `BATT_LEVEL` | 39 | **P0.31** (AIN7) |

- Divider **R9 = R10 = 10 kΩ** from **3.6 V (VBAT)** → tap at AIN7 → ratio **2.0**.
- Same nRF ADC as Tracker 6.0 → **12-bit, `AR_INTERNAL` = 2.4 V FS, ÷2** calibration
  applies directly. Source impedance is only ~5 kΩ (vs 235 kΩ on Tracker), so **no
  AIN7 settling/noise issue** here — the median/retry logic isn't needed.
- ⚠️ **The 10k/10k divider draws ~180 µA continuously** from VBAT (3.6 V / 20 kΩ).
  For a WUR ultra-low-power board this will **dominate the sleep floor**. Candidate
  next-rev change: higher-value or switched divider (see hardware notes below).

## Power tree (differs from Tracker 6.0)

| Rail | Source | Feeds |
|------|--------|-------|
| 3.6 V | VBAT (battery) | `VBAT_NRF` (pin 44), battery divider, LDO input |
| 3.3 V | **RT9080-33 LDO** (U3) | `VDD_NRF` (43), `VBAT_SX`/`VBAT_IO_SX` (20/21), RTC, AS3933, pull-ups |

- nRF core + SX1262 run from the **3.3 V LDO**; Tracker ran everything off raw VBAT.
- Expect a **different (higher) sleep floor** than Tracker's ~94 µA: LDO quiescent +
  the ~180 µA divider + AS3933 listening (~2.7 µA typ) + nRF/RTC. Quantify with a
  deep-sleep baseline test, and always quote the floor with the supply voltage.

## Buttons / misc
- `SW1` (P1.01), `SW2` (P1.02) — user buttons / tactile (TS-1088). `P1.03`/`P1.04`
  are the module's LED1/LED2 pins, here **repurposed** for RTC_INT and WuR_WAKE.
- USB-C (J1) → USBLC6 ESD → `RAK_USB±` (native nRF USB). SWD on headers (`SWD_CLK`/
  `SWD_IO`).

## Two wake sources (the point of this board)
- **P1.03** ← RTC periodic timer (scheduled wake, like Tracker).
- **P1.04** ← AS3933 (on-demand wake from an LF wake-up beacon).
Production firmware should arm **both** as deep-sleep wake sources.

## Hardware notes / next-rev candidates
1. **Battery divider ~180 µA always-on** — biggest floor item; consider 1–2 MΩ
   resistors (accepting the AIN7 settling tradeoff Tracker hit) or a switched divider.
2. Confirm `L76K_EN` polarity and the GPS `Serial` instance (see GPS section).
3. ~~Debug path~~ **RESOLVED (native USB-C `Serial`, single COM port e.g. COM50 =
   SDFU bootloader when idle / app after reset; sketches must be 'alive-first' -
   tiny setup(), init deferred into loop()).** Originally: **RESOLVED: native USB-C CDC = `Serial`** (board flashes via USB
   serial DFU on its COM port; `Serial0`/RAKDAP is not wired). Confirmed test #0.
