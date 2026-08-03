# ISL Board (RAK_feather) — Pin Map

Extracted from `hardware/iteration3/RAK_feather_schematic_v3.pdf` (**current**) +
`hardware/iteration1/RAK_feather.csv`.

> **iteration3 changes (vs iteration2) — the "onboard accelerometer" board:** exactly
> two firmware-relevant deltas; everything else is identical.
> 1. **Accelerometer** moved from an external **Grove LIS3DHTR on I²C** (P0.24/P0.25)
>    to the **onboard LIS3DHTR (U5) on the SPI bus shared with the WuR** — see the new
>    *Accelerometer* section below (accel CS = **P0.28**, INT1 = P1.03, INT2 = P1.01).
> 2. **GPS power** switch changed from an active-LOW P-FET to an **active-HIGH TPS22918**
>    load switch (`L76K_EN` = P1.02, **HIGH = ON**) — see the GPS section.
>
> **Earlier schematic-v2 pin changes (vs iteration1):** `L76K_EN` moved **P0.04 → P1.02**,
> and `RTC_INT` moved **P1.03 → P0.21**. P0.04 is now only broken out to a header.

Board = **RAK4630 (U1) + RV-3028-C7 RTC (U2) + RT9080-33 3.3 V LDO (U3) + USB-C**,
with external **L76K GPS** and **AS3933 wake-up receiver** modules on the headers.

> The verified nets below are specific to this board's schematic — always use them
> rather than a generic RAK4630 pin assumption.

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

## Shared SPI bus (AS3933 WuR + onboard LIS3DHTR accelerometer)

On **iteration3** the SPI bus (P0.03/P0.30/P0.29) is shared by **two** devices, each
with its own chip-select:

| Function | Net | RAK4630 pin | nRF52840 GPIO |
|----------|-----|-------------|---------------|
| SPI CLK (shared) | `SPI_CLK` | 29 | **P0.03** (QSPI_CLK) |
| SPI MOSI (shared) | `SPI_MOSI` | 33 | **P0.30** (QSPI_DIO0) |
| SPI MISO (shared) | `SPI_MISO` | 32 | **P0.29** (QSPI_DIO1) |
| **WuR CS** | `SPI_CS_WuR` | 34 | **P0.26** (QSPI_CS) — active-HIGH, park LOW |
| **Accel CS** | `SPI_CS_LIS3D` | 31 | **P0.28** (QSPI_DIO2) — active-LOW, park HIGH |
| **WuR wake IRQ** | `WuR_WAKE` | 28 | **P1.04** (LED2) |

- AS3933 CS is **active-HIGH** (idle LOW), SPI **Mode 1**, MSB-first. WAKE asserts
  **HIGH** on detect → drives P1.04. Clear with the `CLEAR_WAKE` (0x00) command.
- Both are bit-banged (avoids RUI3 SPI-instance ambiguity). Since they share the bus,
  only one CS is asserted at a time; the other device is deselected and ignores it.

## Accelerometer — onboard LIS3DHTR (U5) over SPI  *(iteration3)*

| Function | Net | RAK4630 pin | nRF52840 GPIO | Notes |
|----------|-----|-------------|---------------|-------|
| Accel CS | `SPI_CS_LIS3D` | 31 | **P0.28** (QSPI_DIO2) | active-LOW; shares CLK/MOSI/MISO above |
| Accel INT1 | `LIS3DH_INT1` | 27 | **P1.03** (LED1) | available; firmware collection is timer-based (unused) |
| Accel INT2 | `LIS3DH_INT2` | 25 | **P1.01** (SW1) | available (unused) |

- **U5 = LIS3DHTR (LCSC C15134)** — same silicon as the old Grove part, so `WHO_AM_I`
  (0x0F) still returns **0x33** and all registers are unchanged. `VDD`/`VDD_IO` are on
  **always-on 3.3 V**; firmware puts the bare chip in **power-down (CTRL1=0x00, ~0.5 µA)**
  after each 10 s collect, so it adds ~nothing to the sleep floor (no Grove-module
  parasitic). SPI is **mode 3** (SPC idles HIGH, sample on rising edge), MSB-first;
  address byte bit7=RW, bit6=auto-increment.
- Driver: `production/v17` (`lisReadReg`/`lisWriteReg`/`lisReadXYZ`, bit-bang SPI).
  **iteration2 and earlier used bit-bang I²C on P0.24/P0.25** (Grove module) — do not
  use that path on iteration3.

## GPS (L76K) — external module on J6

| Function | Net | RAK4630 pin | nRF52840 GPIO | Notes |
|----------|-----|-------------|---------------|-------|
| GPS UART RX (← GPS TX) | `UART1_RX` | 9 | **P0.19** | UART1 = RUI3 **`Serial0`** |
| GPS UART TX (→ GPS RX) | `UART1_TX` | 10 | **P0.20** | |
| GPS power enable | `L76K_EN` | 26 | **P1.02** | **iteration3: active-HIGH** (→ TPS22918 `ON`). iteration2 was active-LOW (Q1 AO3407 P-FET) |
| GPS 1PPS | `L76_1PPS` | — | header only | not on a dedicated MCU pin |
| GPS supply (switched) | `L76K_SUPPLY` | — | U4 VOUT (iter3) / Q1 drain (iter2) | |

- **RESOLVED (test `ISL_GNSS_Serial0`, NMEA streaming):** on RAK4630/RUI3, UART1
  (P0.19/P0.20) is **`Serial0`** — NOT `Serial1` (that's UART2 @ P0.15/P0.16) and
  NOT `Serial2` (not a real instance; it *hangs* this core). Open it with
  **`Serial0.begin(9600, RAK_CUSTOM_MODE)`** so the AT interpreter doesn't eat the
  NMEA. On the ISL board, PC debug is the native USB-C **`Serial`** (see below), so
  `Serial` and `Serial0` are used together (this differs from the RAKDAP-based
  wiring assumed in the general pin guide).
- **`L76K_EN` = P1.02.** ⚠️ **Polarity differs by board rev:**
  - **iteration3 (current):** **active-HIGH** — `L76K_EN` drives the **TPS22918 (U4)**
    `ON` pin, `VOUT` = `L76K_SUPPLY`. **HIGH = GPS ON.** (Firmware v17: `GPS_EN_ACTIVE_HIGH=1`.)
  - **iteration2:** **active-LOW** — Q1 AO3407 P-FET high-side with a 10k gate pull-up
    (R4). **LOW = GPS ON.** (Firmware ≤ v16.)
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
  the sleep floor. ⚠️ But do **not** park P0.31 as a connected `INPUT` in sleep: the
  divider holds it at ~VDD/2, and a connected input buffer there draws ~118 µA of
  crowbar current (the old "155 µA floor"). **Production v7** leaves the buffer
  disconnected (`PIN_CNF[31]=2`) → **34 µA**; `analogRead()` still works via the SAADC
  mux. See the RESOLVED note in `ISL_DeepSleep_Notes.md`.

## Power tree

| Rail | Source | Feeds |
|------|--------|-------|
| 3.6 V | VBAT (battery) | `VBAT_NRF` (pin 44), battery divider, LDO input |
| 3.3 V | **RT9080-33 LDO** (U3) | `VDD_NRF` (43), `VBAT_SX`/`VBAT_IO_SX` (20/21), RTC, AS3933, pull-ups |

- nRF core + SX1262 run from the **3.3 V LDO** (not raw VBAT).
- Sleep-floor contributors: LDO quiescent + AS3933 listening (~2.7 µA typ) + nRF/RTC
  + the **~1.8 µA** divider. With the AIN7 input-buffer crowbar fixed in v7 these sum
  to the measured **~34 µA** floor. Always quote the floor with the supply voltage.

## Buttons / misc
- The module's `SW1`/`SW2`/`LED1`/`LED2` pins (P1.01/P1.02/P1.03/P1.04) are all
  **repurposed** on this board: **P1.02** = `L76K_EN` (GPS power), **P1.04** = `WuR_WAKE`,
  and on **iteration3** **P1.03** = `LIS3DH_INT1`, **P1.01** = `LIS3DH_INT2` (the accel
  interrupts; available but unused by the timer-based collection). *(On iteration2, P1.01
  was a spare button net.)* RTC_INT is on **P0.21** (not P1.03).
- USB-C (J1) → USBLC6 ESD → `RAK_USB±` (native nRF USB). SWD on headers (`SWD_CLK`/
  `SWD_IO`).

## Two wake sources (the point of this board)
- **P0.21** ← RTC periodic timer (scheduled wake). *(was P1.03 in schematic v1)*
- **P1.04** ← AS3933 (on-demand wake from an LF wake-up beacon).
Production firmware should arm **both** as deep-sleep wake sources.

## Hardware notes / next-rev candidates
1. ~~Battery divider ~180 µA always-on~~ **ALREADY DONE:** divider is **1 MΩ/1 MΩ + C17**
   (~1.8 µA), not 10k/10k. ~~Find the unexplained ~150 µA~~ **SOLVED (firmware, v7):** it
   was the AIN7 input-buffer crowbar, not hardware; fixed → 34 µA. Any further squeeze
   (34 µA → single digits) would be a low-Iq-LDO item, low priority. See `ISL_DeepSleep_Notes.md`.
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
