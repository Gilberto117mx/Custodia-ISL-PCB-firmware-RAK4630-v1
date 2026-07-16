# ISL Board (RAK_feather) — Pin Map

Extracted from `hardware/iteration2/RAK_feather_schematic_v2.pdf` (latest) +
`hardware/iteration1/RAK_feather.csv`.

> **Schematic v2 pin changes (vs iteration1):** `L76K_EN` moved **P0.04 → P1.02**,
> and `RTC_INT` moved **P1.03 → P0.21**. P0.04 is now only broken out to a header.

Board = **RAK4630 (U1) + RV-3028-C7 RTC (U2) + RT9080-33 3.3 V LDO (U3) + USB-C**,
with external **L76K GPS** and **AS3933 wake-up receiver** modules on the headers.

> The verified nets below are specific to this board's schematic v2 — always use
> them rather than a generic RAK4630 pin assumption.

## Core peripherals

| Function | Net | RAK4630 pin | nRF52840 GPIO | Notes |
|----------|-----|-------------|---------------|-------|
| I2C SDA (RTC) | `I2C_SDA` | 4 | **P0.13** | primary I2C = `Wire` |
| I2C SCL (RTC) | `I2C_SCL` | 5 | **P0.14** | RV-3028 @ 0x52 (CONFIRMED present) |
| RTC INT (wake) | `RTC_INT` | 11 | **P0.21** | RV-3028 ~INT; 10k pull-up (R7). Moved from P1.03 in schematic v2 (now on P0.21/UART1_DE). |

- ⚠️ **RTC VBACKUP has NO battery**: schematic v2 ties RV-3028 pin 6 (VBACKUP) to
  the **same 3.3 V rail as VDD** (only a 100 nF cap). Time survives resets and
  deep sleep (LDO stays on, part of the 155 µA floor) but **any full power loss
  resets the clock to 2000-01-01**. Mitigation: **GPS time-seed on cold boot**
  (test #7, `production/v3`) — the RV-3028 backup/trickle registers can't help
  with nothing connected to VBACKUP. Next-rev option: supercap/coin cell +
  isolation diode on VBACKUP.
- `Melopero_RV3028::setTime()` argument order is **(year, month, WEEKDAY, DATE,
  hh, mm, ss)** — weekday BEFORE date (found in test #7; older calls had them
  swapped → wrong day-of-month).
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
| GPS UART RX (← GPS TX) | `UART1_RX` | 9 | **P0.19** | UART1 = RUI3 **`Serial0`** |
| GPS UART TX (→ GPS RX) | `UART1_TX` | 10 | **P0.20** | |
| GPS power enable | `L76K_EN` | 26 | **P1.02** | schematic v2 (was P0.04); drives Q1 AO3407 P-FET |
| GPS 1PPS | `L76_1PPS` | — | header only | not on a dedicated MCU pin |
| GPS supply (switched) | `L76K_SUPPLY` | — | Q1 drain | |

- **RESOLVED (test `ISL_GNSS_Serial0`, NMEA streaming):** on RAK4630/RUI3, UART1
  (P0.19/P0.20) is **`Serial0`** — NOT `Serial1` (that's UART2 @ P0.15/P0.16) and
  NOT `Serial2` (not a real instance; it *hangs* this core). Open it with
  **`Serial0.begin(9600, RAK_CUSTOM_MODE)`** so the AT interpreter doesn't eat the
  NMEA. On the ISL board, PC debug is the native USB-C **`Serial`** (see below), so
  `Serial` and `Serial0` are used together (this differs from the RAKDAP-based
  wiring assumed in the general pin guide).
- **RESOLVED:** `L76K_EN` = **P1.02**, **active-LOW** (drive LOW = GPS ON). Q1 is a
  P-FET high-side switch with a 10k gate pull-up (R4) to 3.3 V.
- Confirmed L76K is **multi-GNSS** (GPS + BeiDou: GN/GP/BD talkers) and reports
  `$GPTXT ... ANTENNA OK`. A real position fix still needs sky view (validated next).

## Battery / ADC

| Function | Net | RAK4630 pin | nRF52840 GPIO |
|----------|-----|-------------|---------------|
| Battery sense | `BATT_LEVEL` | 39 | **P0.31** (AIN7) |

- Divider **R9 = R10 = 1 MΩ** from **3.6 V (VBAT)** → tap at AIN7 → ratio **2.0**,
  with **C17 = 100 nF** filtering the `BATT_LEVEL` node. (⚠️ **CORRECTION:** earlier
  docs said 10k/10k — that was a *wrong diagram*, never the real board. Confirmed on
  hardware. See the deep-sleep correction note in `ISL_DeepSleep_Notes.md`.)
- ⚠️ **Do NOT assume a 2.4 V full scale.** On this RUI3 core,
  `analogReference(AR_INTERNAL)` is **~3.67 V full scale** (0.6 V band-gap ref,
  gain 1/6), and **direct-register SAADC access returns 0** (the core owns the
  SAADC). **CALIBRATED reader (test #2 final):** one-time SAADC offset calibration
  → `analogRead(AR_INTERNAL)` median-of-31 → **`Vbat_mV = raw × 1795 / 1000`**
  (through-origin). Accuracy **≤25 mV (<0.8 %) over 3.2–3.7 V**; ~±18 mV/boot
  repeatability floor from the internal ref. Details: `../tests/ISL_Battery_ADC/`.
  The divider **ratio (2.0) is identical** to the wrong 10k/10k assumption, so this
  calibration is **unaffected**. Source impedance is now ~500 kΩ (high) — which is
  exactly what **C17** settles for the SAADC.
- ✅ **The 1 MΩ/1 MΩ divider draws only ~1.8 µA** (3.6 V / 2 MΩ), so it is **NOT**
  the sleep floor. The measured **~155 µA @ 3.6 V floor (test #5) is therefore
  unexplained by the divider** and is genuine optimization headroom — see the
  correction note in `ISL_DeepSleep_Notes.md`.

## Power tree

| Rail | Source | Feeds |
|------|--------|-------|
| 3.6 V | VBAT (battery) | `VBAT_NRF` (pin 44), battery divider, LDO input |
| 3.3 V | **RT9080-33 LDO** (U3) | `VDD_NRF` (43), `VBAT_SX`/`VBAT_IO_SX` (20/21), RTC, AS3933, pull-ups |

- nRF core + SX1262 run from the **3.3 V LDO** (not raw VBAT).
- Sleep-floor contributors: LDO quiescent + AS3933 listening (~2.7 µA typ) + nRF/RTC
  + the **~1.8 µA** divider. These sum to **far less than the measured ~155 µA**
  (test #5), so most of that floor is currently **unaccounted for** (LDO Iq, a leak
  path, or a peripheral not fully asleep) — the deep-sleep improvement target. Always
  quote the floor with the supply voltage.

## Buttons / misc
- `SW1` (P1.01), `SW2` (P1.02) — user buttons / tactile (TS-1088). `P1.03`/`P1.04`
  are the module's LED1/LED2 pins, here **repurposed** for RTC_INT and WuR_WAKE.
- USB-C (J1) → USBLC6 ESD → `RAK_USB±` (native nRF USB). SWD on headers (`SWD_CLK`/
  `SWD_IO`).

## Two wake sources (the point of this board)
- **P0.21** ← RTC periodic timer (scheduled wake). *(was P1.03 in schematic v1)*
- **P1.04** ← AS3933 (on-demand wake from an LF wake-up beacon).
Production firmware should arm **both** as deep-sleep wake sources.

## Hardware notes / next-rev candidates
1. ~~Battery divider ~180 µA always-on~~ **ALREADY DONE in this rev:** the divider is
   **1 MΩ/1 MΩ + C17** (~1.8 µA), not 10k/10k. The real next-rev task is to **find the
   unexplained ~150 µA** in the ~155 µA floor (LDO quiescent / leakage / a peripheral
   not fully off) — see `ISL_DeepSleep_Notes.md`.
1b. **RTC VBACKUP has no storage element** — add a supercap or coin cell with an
   isolation diode from 3.3 V (+ enable RV-3028 trickle charge) so time survives
   battery swaps without needing a GPS re-fix. (Firmware mitigation in place:
   GPS time-seed, `production/v3`.)
2. ~~Confirm `L76K_EN` polarity and the GPS `Serial` instance~~ **RESOLVED:** GPS =
   `Serial0`/UART1 (P0.19/P0.20), `L76K_EN` = P1.02 active-LOW (see GPS section).
3. ~~Debug path~~ **RESOLVED (native USB-C `Serial`, single COM port e.g. COM50 =
   SDFU bootloader when idle / app after reset; sketches must be 'alive-first' -
   tiny setup(), init deferred into loop()).** Originally: **RESOLVED: native USB-C CDC = `Serial`** (board flashes via USB
   serial DFU on its COM port; `Serial0`/RAKDAP is not wired). Confirmed test #0.
