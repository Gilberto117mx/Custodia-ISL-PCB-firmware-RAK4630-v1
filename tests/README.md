# ISL Board — Test Status

Hardware-in-the-loop bring-up of the ISL Board (RAK_feather, RAK4630 + RV-3028 RTC
+ L76K GNSS + AS3933 wake-up receiver). Each test is a focused sketch; we flash it,
capture the USB-C serial output, and iterate. All sketches use the **alive-first**
structure (tiny `setup()`, real init deferred into `loop()`) — see `../README.md`.

Pin map of record: **`../docs/ISL_Pinout.md`** (schematic v2).

## Status matrix

| # | Test | Subsystem | Status | Result / notes |
|---|------|-----------|--------|----------------|
| 0 | `ISL_I2C_Scan` | MCU + I2C + debug | ✅ **PASS** | MCU alive; RV-3028 @ 0x52; debug = native USB-C `Serial` (COM50). |
| 1 | `ISL_RTC_Read` | RV-3028 RTC | ✅ **PASS** | Keeps time, persists across reset (VBACKUP). INT/wake pin = **P0.21** (unused in this read test). |
| 2 | `ISL_Battery_ADC` | Battery ADC (AIN7/P0.31) | ✅ **CALIBRATED** | `AR_INTERNAL` is ~3.67 V FS on this core (not 2.4 V); direct SAADC returns 0. Reader = offset-cal + `analogRead` + **`raw×1795/1000`**, **≤25 mV over 3.2–3.7 V**. Folded into `production/v2`. See its README + `calibration_data.csv`. |
| 3 | `ISL_GNSS_Serial0` | L76K GNSS | ✅ **PASS** | NMEA streaming on **`Serial0`/UART1** (P0.19/P0.20), EN=**P1.02** active-low, `RAK_CUSTOM_MODE` @ 9600. `ANTENNA OK`. **No position fix yet** (indoor, 0 sats). |
| 3′| `ISL_GNSS_Monitor` | (exploratory) | 🗄️ **SUPERSEDED** | Early monitor that probed Serial1/Serial2 + EN on P0.04. Kept for history; use `ISL_GNSS_Serial0`. |
| 4 | `ISL_WUR_AS3933` | AS3933 wake-up RX | ⚠️ **PARTIAL** | SPI + config + RC-cal **PASS** (R5=0x69/R6=0x96; R14 RC_CAL_OK, taps=60; R0–R8 read back == written). **Real wake pending** an LF transmitter. Bit-banged SPI, active-HIGH CS, WAKE=P1.04. |
| 5 | `ISL_DeepSleep_Baseline` | Deep-sleep floor | ✅ **PASS** | **157 µA @ 3.6 V** (σ=2.4 µA). ⚠️ Cause is **unexplained** — the real divider is **1 MΩ/1 MΩ+C17** (~1.8 µA), *not* 10k/10k, so ~150 µA is un-accounted (see `../docs/ISL_DeepSleep_Notes.md` correction). **P0.21 RTC wake validated** (30 s blips). See its README + `PowerProfile_baseline.csv`. |
| 6 | `ISL_GNSS_DutyCycle` | GPS duty cycle + sleep | ✅ **PASS** | 30 s search / 60 s sleep × 4 clean cycles; sleep floor **157–159 µA @ 3.6 V = baseline** (GPS adds ~0). Two bugs fixed en route (instant-wake; ~600 µA phantom-power via UART pins) — see its README. Fix itself still needs an outdoor run. |
| 7 | `ISL_RTC_GPS_TimeSet` | RTC ← GNSS UTC time | ✅ **PASS** | GPS UTC acquired in **3.8–17.7 s with sats=0** (time before fix); sanity guard rejects the pre-sync year-2000 date; RTC set + free-runs. Found the Melopero `setTime` **weekday/date order bug**. Also established: **VBACKUP has no battery** → clock resets on full power loss (by design; GPS re-seed is the recovery). In `production/v3`. |

Legend: ✅ pass · ⚠️ partial · 🔬 measuring · ⛔ not run yet · 🗄️ superseded.

## Confirmed on hardware
- **Debug** = native USB-C **`Serial`** @ 115200 (single COM port = SDFU bootloader
  when idle / running app after reset). `Serial0`/RAKDAP is **not** the debug link here.
- **GPS** = **`Serial0` = UART1 = P0.19/P0.20**, opened `RAK_CUSTOM_MODE` @ 9600.
  (`Serial1` = UART2 @ P0.15/P0.16 = wrong pins; `Serial2` hangs this core.)
- **`L76K_EN` = P1.02, active-LOW** (Q1 AO3407 P-FET high-side, 10k gate pull-up).
- **RV-3028 @ 0x52** on primary I2C (P0.13/P0.14).
- **AS3933 WUR** SPI OK on v2 pins (CLK=P0.03, MOSI=P0.30, MISO=P0.29, CS=P0.26,
  WAKE=P1.04); RC-osc calibrates; pattern-mode config (0x9669) accepted.
- **Deep-sleep floor = 157 µA @ 3.6 V** (cause UNEXPLAINED — the 1 MΩ divider is
  only ~1.8 µA; see the `ISL_DeepSleep_Notes.md` correction); **RTC wake = P0.21**
  works; GPS duty-cycling adds ~0 µA when torn down per
  `../docs/ISL_DeepSleep_Notes.md` (cut EN → quiet → `end()` → drive P0.19/P0.20 LOW).

## Pending to fully evaluate the board
| Item | What's needed | Blocks |
|------|---------------|--------|
| ~~GPS position fix~~ | **DONE (production run 2):** fix in ~21 s (16 s TTFF + 5 s settle), TX'd + ACK'd — see `../production/v1/README.md` §Run 2. | — |
| ~~GPS driver port~~ | **DONE (test #6):** TinyGPSPlus + validated teardown on `Serial0`/EN=P1.02. | — |
| **AS3933 WUR wake** | SPI/config/RC-cal already PASS. Remaining: trigger a real wake from a `reference/AS3933_wakeup/WuTx*` transmitter (433 MHz, 19 kHz OOK/Manchester, 0x9669) and confirm the P1.04 rising-edge IRQ fires + R13 pattern-match. *(assigned: second engineer)* | On-demand wake |
| ~~Battery ADC~~ | **DONE:** calibrated `raw×1795/1000` (≤25 mV, 3.2–3.7 V); in `production/v2`. | — |
| ~~RTC wake / deep sleep~~ | **DONE (tests #5/#6):** 157 µA @ 3.6 V floor; P0.21 wake validated; GPS duty-cycle adds ~0. Rules in `../docs/ISL_DeepSleep_Notes.md`. | — |
| **Sleep-floor cause** | **MEASURED: 157 µA @ 3.6 V**, but the real 1 MΩ/1 MΩ+C17 divider is only ~1.8 µA → **~150 µA unexplained** = deep-sleep headroom. Needs a headless battery-only teardown (`docs/ROADMAP.md` §6). | floor optimization |
| ~~Production FW validation~~ | **DONE (v1 runs 1+2):** 3.6 h stable, floor 155 µA, GNSS fix, receiver-confirmed delivery. Robustness checklist still open (`../production/v1/README.md`). | — |
| ~~LoRa TX/ACK on ISL~~ | **DONE:** receiver-confirmed both production runs. | — |
| ~~RTC time from GNSS~~ | **DONE (test #7):** UTC seed in 3.8–17.7 s, sats=0; in `production/v3` (boot sync + opportunistic re-seed). | — |
| ~~Long cadence (>68 min)~~ | **DONE (v6 test3):** 2 h sleep on the **1/60 Hz tick**, RTC-INT wake on P0.21 at **+0.02 %** (7201.6 s vs 7200 s, not the backstop), clock held across sleep. See `../production/v6/tests/test3_longwake_2h/`. Full multi-wake endurance still pending. | — |

## Upload notes (recurring gotchas)
- **Close the Serial Monitor before Upload** — a monitor holding the COM port open
  causes the DFU `No ping response` failure.
- **Manual DFU entry:** `SW1` is the hardware **RESET** (wired to NRF_RESET).
  **Double-tap** it to stay in the bootloader (a single tap just restarts the app);
  needed if a previous sketch wedged the USB stack.
