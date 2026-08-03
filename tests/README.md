# ISL Board — Test Status (by subsystem × board)

Hardware-in-the-loop bring-up of the ISL Board (RAK_feather, RAK4630 + RV-3028 RTC
+ L76K GNSS + LIS3DHTR accel + AS3933 wake-up receiver). Each test is a focused
sketch; we flash it, capture the USB-C serial output, and iterate. All sketches use
the **alive-first** structure (tiny `setup()`, real init deferred into `loop()`).

Pin map of record: **`../docs/ISL_Pinout.md`**. Schematic of record: **`../hardware/`**.

---

## 📌 Which BOARD did a test run on? (read this first)

Every test row below carries a **Board** column. The board matters because the PCB
has iterations, and a result only applies to the board it was taken on:

| Board | What it is | Key traits that affect tests |
|---|---|---|
| **iter1** | schematic v1 (`hardware/iteration1`) | superseded pinout (`L76K_EN`=P0.04, `RTC_INT`=P1.03). History only. |
| **iter2** | schematic v2 (`hardware/iteration2`) | **firmware v1–v16 board.** Accel = external **Grove on I²C** (P0.24/25). GPS = **active-LOW P-FET**. Divider 1 MΩ/1 MΩ. |
| **iter3** | schematic v3 (`hardware/iteration3`) — **CURRENT** | **firmware v17.** Accel = **onboard LIS3DHTR on SPI** (CS P0.28). GPS = **active-HIGH TPS22918**. Everything else = iter2. |

**Carry-over rule:** iter3 changed only **three** things vs iter2 — the **accelerometer**
(I²C→SPI), **GPS power polarity** (P-FET→TPS22918), and (as a side effect) the
**deep-sleep floor** (the Grove parasitic is gone). So every iter2 ✅ result below
**still holds on iter3 EXCEPT** those three subsystems, which carry a `⛔ iter3` row
that must be re-run on the new board before it's trusted. All other subsystems (RTC,
battery ADC, flash, LoRa, BLE transport, WUR) are electrically identical on iter3.

Legend: ✅ pass · ⚠️ partial · 🔬 measuring · ⛔ not run yet · 🗄️ superseded.

---

## Results by subsystem

### MCU / debug / I²C bus
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 0 | `ISL_I2C_Scan` | iter2 | ✅ PASS | MCU alive; RV-3028 @ 0x52; debug = native USB-C `Serial` (COM50). Bus wiring (P0.13/14) identical on iter3. |

### RTC — RV-3028
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 1 | `ISL_RTC_Read` | iter2 | ✅ PASS | Keeps time, persists across reset. INT/wake = **P0.21**. `setTime()` weekday/date order bug found. Identical on iter3. |
| 7 | `ISL_RTC_GPS_TimeSet` | iter2 | ✅ PASS | GPS UTC seed in **3.8–17.7 s at sats=0** (time-before-fix); year-2000 guard; in `production/v3`. **VBACKUP has no battery → clock resets on full power loss** (still true on iter3 — not fixed this rev). |

### Battery / ADC (AIN7 / P0.31)
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 2 | `ISL_Battery_ADC` | iter2 | ✅ CALIBRATED | `AR_INTERNAL` ≈ 3.67 V FS on this core (not 2.4 V). Reader = offset-cal + `analogRead` + **`raw×1795/1000`**, **≤25 mV over 3.2–3.7 V**. In `production/v2`. Divider 1 MΩ/1 MΩ (ratio 2.0) identical on iter3 → **calibration carries over**. |

### Deep sleep / power floor
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 5 | `ISL_DeepSleep_Baseline` | iter2 | ✅ PASS | **157 µA @ 3.6 V** baseline → cause **SOLVED** (AIN7 input-buffer crowbar, not divider/LDO) → **34 µA in production v7**. P0.21 RTC wake validated. |
| — | *(sleep floor, onboard accel)* | **iter3** | ⛔ **not run yet** | iter3 removes the Grove module's ~296 µA parasitic (bare LIS3DHTR powers down in-place, ~0.5 µA). **Expect the floor to return toward 34 µA.** Needs a **clean battery-only PPK** (no USB) with firmware **v17**. This is the board's power payoff + the number still owed from the v16 test. |

### GNSS — L76K
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 3 | `ISL_GNSS_Serial0` | iter2 | ✅ PASS | NMEA on **`Serial0`/UART1** (P0.19/20), `RAK_CUSTOM_MODE` @ 9600, `ANTENNA OK`. UART is identical on iter3. |
| 6 | `ISL_GNSS_DutyCycle` | iter2 | ✅ PASS | 30 s search / 60 s sleep × 4 clean; GPS teardown adds ~0 µA (cut EN → quiet → `end()` → drive P0.19/20 LOW). |
| 3′| `ISL_GNSS_Monitor` | iter1-era | 🗄️ SUPERSEDED | Early probe (Serial1/Serial2, EN on **P0.04** = iter1 pin). Use `ISL_GNSS_Serial0`. |
| — | *(GPS power polarity)* | **iter3** | ⛔ **not run yet** | GPS enable flipped: iter2 P-FET **LOW=ON** → iter3 TPS22918 **HIGH=ON** (`GPS_EN_ACTIVE_HIGH=1`). **Confirm GPS powers + fixes on iter3** (look for `SET FROM GPS`, not stuck `SV=0`). UART/teardown unchanged. |

### Accelerometer — LIS3DHTR
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 8 | `Accelerometer/` | iter2 | ✅ PASS | Emitter/receiver pair: LIS3DHTR over **bit-bang I²C (P0.24/25)**, `WHO_AM_I`=0x33, streamed over BLE NUS. ⚠ Uses Adafruit/WisBlock BSP, not RUI3 (see folder README). **This is the I²C/Grove path — iter2 only.** |
| — | *(onboard accel over SPI)* | **iter3** | ⛔ **not run yet** | iter3 accel is the **onboard LIS3DHTR on SPI** (CLK P0.03, MOSI P0.30, MISO P0.29, **CS P0.28**), driven by **production v17** (bit-bang SPI mode 3). **Confirm `WHO_AM_I`=0x33 over SPI + clean 100-sample records.** A dedicated bring-up sketch (`ISL_Accel_SPI`, iter3) can be added here when run. |

### BLE offload (collar → drone)
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 8 | `Accelerometer/` (BLE half) | iter2 | ✅ PASS | NUS transport proven (see above). The production BLE offload (`api.ble.uart` fire-and-forget) is validated in `production/v11–v16`. |
| — | *BLE transport on iter3* | iter3 | ✅ carries over | BLE is inside the RAK4630 module — **electrically unchanged**. v17 BLE wire format is byte-identical to v16; the v16 drone-receiver logs apply. |

### Wake-Up Receiver — AS3933
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 4 | `ISL_WUR_AS3933` | iter2 | ⚠️ PARTIAL | SPI + config + RC-cal **PASS** (R14 RC_CAL_OK, taps=60; regs read-back OK). **Real LF wake pending** a transmitter. CLK=P0.03, MOSI=P0.30, MISO=P0.29, CS=P0.26, WAKE=P1.04 — **same pins on iter3**, but the bus is now **shared with the accel** (separate CS), so re-confirm no contention when both are active. *(assigned: 2nd engineer)* |

### Flash (RUI3 user flash)
| # | Test | Board | Status | Result / notes |
|---|------|-------|--------|----------------|
| 9 | `ISL_Flash_Probe` | iter2 | ✅ MEASURED | `api.system.flash` usable = **~132 KB** (fails at 0x21000); per-call cap ≤255 B. MCU-internal → identical on iter3. Sizes the v16 backlog + accel ring. |

---

## Adding a new test — keep board attribution clean
So an "accelerometer test" is never ambiguous:
1. **Name the folder with the board when the hardware differs**, e.g. `Accelerometer/`
   (iter2, I²C) vs a new `ISL_Accel_SPI/` (iter3, SPI). Don't overwrite the old one —
   the iter2 result is still the record for that board.
2. **Add a row in the matching subsystem table above** with the **Board** column filled
   (`iter2`/`iter3`) and a one-line result.
3. If a test re-validates one of the three iter3-changed subsystems, replace its
   `⛔ iter3` placeholder row with the real result (and link the log).

## Confirmed on hardware (board-tagged)
- **Debug** = native USB-C **`Serial`** @ 115200 (single COM port). *(all boards)*
- **GPS** = **`Serial0` = UART1 = P0.19/P0.20**, `RAK_CUSTOM_MODE` @ 9600. *(all boards)*
- **`L76K_EN` = P1.02** — **active-LOW P-FET on iter2**, **active-HIGH TPS22918 on iter3**. ⚠ polarity differs by board.
- **RV-3028 @ 0x52** on primary I²C (P0.13/P0.14). *(all boards)*
- **Accel `WHO_AM_I` = 0x33** — over **I²C P0.24/25 on iter2**, over **SPI CS P0.28 on iter3**.
- **AS3933 WUR** SPI OK (CLK P0.03/MOSI P0.30/MISO P0.29/CS P0.26/WAKE P1.04). *(iter2; iter3 shares the bus with the accel.)*
- **Deep-sleep floor** = 157 µA baseline → **34 µA (v7, iter2)**; **iter3 expected ≤34 µA** (no Grove parasitic) — ⛔ to be measured.

## Upload notes (recurring gotchas)
- **Close the Serial Monitor before Upload** — a held COM port causes the DFU `No ping response` failure.
- **Manual DFU entry:** `SW1` is the hardware **RESET** (NRF_RESET). **Double-tap** to stay in the bootloader.
