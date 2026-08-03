# ISL Board — Hardware (schematics / PCB / BOM)

The board has gone through three schematic iterations. **Only `iteration3` is
current** — the go-forward firmware (`../production/v17`), the pin map
(`../docs/ISL_Pinout.md`), and new tests are written against it. `iteration2` is the
prior board that firmware v1–v16 were validated on; `iteration1` is history only.

## ✅ Current / authoritative
```
iteration3/
└── RAK_feather_schematic_v3.pdf   <- THE schematic of record (use this one)
```
**Schematic v3 (iteration3)** = iteration2 **plus the accelerometer brought ONBOARD**.
This is the board the project refers to as "PCB v2 with onboard accelerometer." It
changes exactly **two** firmware-relevant things vs iteration2; everything else (RTC,
GPS UART, battery, LoRa, BLE, power) is identical:

| Signal | iteration2 | **iteration3 (current)** | Firmware impact |
|---|---|---|---|
| Accelerometer | external **Grove LIS3DHTR over I²C** (P0.24/P0.25) | **onboard LIS3DHTR (U5) over 4-wire SPI**, shared with the WuR: CLK P0.03, MOSI P0.30, MISO P0.29, **accel CS P0.28**; INT1 P1.03, INT2 P1.01 | driver rewritten I²C→**SPI** |
| `L76K_EN` (GPS power) | P1.02, **active-LOW** (AO3407 P-FET high-side) | P1.02, **active-HIGH** (**TPS22918** load switch `ON`; `VOUT`=`L76K_SUPPLY`) | GPS enable **polarity flipped** |

> ⚠️ **The GPS-power polarity flip is easy to miss** — the board looks "the same" but
> the P-FET became a TPS22918 load switch, so `L76K_EN` is now **HIGH = ON**. Firmware
> v17 handles it via `GPS_EN_ACTIVE_HIGH` (=1). If a given physical board still has the
> P-FET, set it to 0.

### Full component delta (exhaustive schematic diff, 2026-08-03)

Every reference-designator change iteration2 → iteration3. Only the first two rows
touch firmware; the rest are passive and need **no** code change:

| Ref | iteration2 | iteration3 | Meaning / impact |
|---|---|---|---|
| **Q1 → U4** | AO3407 P-FET (+R3 1k, R4 10k gate) | **TPS22918** load switch | GPS switch, **polarity flip** → firmware `GPS_EN_ACTIVE_HIGH` |
| **U5** | — (external Grove on I²C) | **LIS3DHTR** (+C18 10µ, C19 100n) | onboard accel on **SPI** → firmware driver I²C→SPI |
| R9, R10 | 10k, 10k | **1M, 1M** | battery divider — *schematic correction* to match the real board (ratio still 2.0, calibration unchanged) |
| R5, R6 | 2.2k | **4.7k** | primary-I²C (RTC) pull-ups — weaker standard value, no impact |
| C17 | — | **100n** | `BATT_LEVEL` (AIN7) sense filter — no impact |
| C20, C21 | — | **10µ, 100n** | TPS22918 input/output decoupling — no impact |
| D2 | ESD5651N | ESD5651N | ESD diode (unchanged) — no impact |

**Verified unchanged (so firmware is untouched here):** U1 RAK4630, U2 RV-3028 RTC
(I²C P0.13/14, INT P0.21), U3 RT9080-33 3.3 V LDO, GPS UART (`Serial0` P0.19/20),
battery AIN7 (P0.31). **RTC `VBACKUP` is still tied to 3.3 V with no coin cell/supercap**
— time still resets on a full power loss, so the cold-boot GPS time-seed is still
required (this rev did **not** add the backup element from the next-rev wishlist).

Everything below was already true on iteration2 and is unchanged on iteration3:

| Signal | value (unchanged) |
|---|---|
| `RTC_INT` (RTC wake) | **P0.21** |
| GPS UART | **Serial0 / UART1 = P0.19 (RX) / P0.20 (TX)** |
| RTC I²C (RV-3028) | **P0.13 (SDA) / P0.14 (SCL)** |
| WuR wake (AS3933) | **P1.04** |
| Battery divider R9/R10 | **1 MΩ / 1 MΩ** (~1.8 µA) + **C17 = 100 nF** on AIN7 (`BATT_LEVEL`) |

> **Battery divider = 1 MΩ/1 MΩ + C17** is the value of record, **confirmed on the
> physical board**. Some earlier text docs said 10k/10k — that was a *wrong diagram*.
> The ratio is 2.0 either way, so the battery calibration is unaffected; but the
> ~155 µA sleep floor is therefore **not** the divider (~1.8 µA) and its cause is an
> open item — see `../docs/ISL_DeepSleep_Notes.md`.

Full pin map with every peripheral: **`../docs/ISL_Pinout.md`**.

## 🗄️ Prior / historical (do not design new work against)
```
iteration2/
└── RAK_feather_schematic_v2.pdf   <- prior board: firmware v1–v16 were validated here
                                      (external Grove accel on I²C; GPS on an active-LOW P-FET)
iteration1/
├── RAK_feather_schematic.pdf      <- schematic v1 (superseded)
├── RAK PCB.pdf                    <- PCB layout as originally fabricated
└── RAK_feather.csv               <- BOM (bill of materials)
```
`iteration2` is kept because **firmware v1–v16 were validated on it** — if you flash
v16 or earlier, it targets iteration2 (Grove-I²C accel, active-LOW GPS). `iteration1`
is kept so the early pin reassignments (`L76K_EN` P0.04→P1.02, `RTC_INT` P1.03→P0.21)
stay traceable. For any new wiring, probing, or firmware, use **`iteration3`**.

> If a newer schematic revision is ever added, create `iteration4/`, move the
> "current" pointer here to it, and update `../docs/ISL_Pinout.md` + the firmware in
> the same commit so there is always exactly **one** authoritative schematic.
