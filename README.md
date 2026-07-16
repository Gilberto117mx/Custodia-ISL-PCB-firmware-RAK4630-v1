# ISL Board (RAK_feather)

A **RAK4630-based custom board** built around the Nordic nRF52840 + Semtech SX1262
(the RAK4630 module), an RV-3028-C7 RTC, an L76K multi-GNSS receiver, and an
**AS3933 LF wake-up receiver**. This repository is the standalone home for the ISL
board: hardware of record, bring-up tests, and production firmware.

> Bring-up is hardware-in-the-loop: flash a focused test, capture the serial/power
> output, iterate. Each test's output feeds the next refinement.

> **📍 For the current status and what to test next, read
> [`docs/ROADMAP.md`](docs/ROADMAP.md)** — the single source of truth. Hardware /
> schematic-of-record: [`hardware/README.md`](hardware/README.md) (schematic **v2**
> is authoritative; v1 is historical).

## Board at a glance
| Subsystem | ISL Board (RAK_feather) |
|---|---|
| MCU | RAK4630 (nRF52840 + SX1262) |
| RTC | RV-3028-C7, INT/wake on **P0.21** |
| Power tree | **RT9080-33 LDO → 3.3 V** for nRF/SX1262; VBAT (3.6 V nominal) elsewhere |
| Battery sense | **1 MΩ/1 MΩ divider + C17** on AIN7 (~1.8 µA), calibrated reader |
| GPS | L76K on **`Serial0`/UART1 (P0.19/P0.20)**, EN = **P1.02** active-LOW |
| Wake-up radio | **AS3933 LF wake-up receiver** on SPI, wake → **P1.04** |

The headline feature is the **AS3933 wake-up receiver (WUR)**: an ultra-low-power
LF receiver that listens for a 433 MHz carrier modulated with a 19 kHz OOK/
Manchester 16-bit pattern (default `0x9669`) and asserts a wake pin (P1.04) on a
match — so the board can be woken **on demand** by a beacon, in addition to the
scheduled RTC wake (P0.21).

## Layout
```
.                             <- repository root
├── README.md                 <- this file
├── docs/
│   ├── ROADMAP.md            <- STATUS + what to test next (single source of truth)
│   ├── ISL_Pinout.md         <- authoritative pin map + hardware notes (READ FIRST)
│   ├── ISL_DeepSleep_Notes.md<- deep-sleep rules (teardown, floor, wake)
│   └── GNSS_FieldStrategy.md <- GNSS field findings → the v6 strategy
├── hardware/
│   ├── README.md             <- schematic-of-record (v2 current; v1 historical)
│   ├── iteration1/           <- RAK_feather schematic v1, PCB PDF, BOM (SUPERSEDED)
│   └── iteration2/           <- RAK_feather schematic v2 (CURRENT: L76K_EN→P1.02, RTC_INT→P0.21)
├── reference/
│   └── AS3933_wakeup/        <- vendor AS3933 WUR repo, UNMODIFIED (ESP32 + MATLAB/SDR TX)
├── production/
│   ├── v1/                   <- full node firmware (GNSS + LoRa TX/ACK + persistence + deep sleep)
│   ├── v2/                   <- + calibrated battery reader (raw×1795/1000)
│   ├── v3/                   <- + GPS time-seed for the RTC + >68 min wakes (1/60 Hz tick)
│   ├── v4/                   <- hardened seed + bench knobs (field: time OK, no fix = cold start)
│   ├── v5/                   <- + SV reception diag in packet (field: FIRST FIX, 7 s, SV=19)
│   └── v6/                   <- GNSS field strategy: SV-gated GPS + no-sky backoff + fix-only
│                                 RTC re-sync + never-abandon delivery guarantee
└── tests/
    ├── README.md             <- test status matrix + what's pending (READ for status)
    ├── ISL_I2C_Scan/         <- #0 bring-up: MCU alive, RTC @ 0x52
    ├── ISL_RTC_Read/         <- #1 RV-3028 keeps time (INT on P0.21)
    ├── ISL_Battery_ADC/      <- #2 battery volts (AIN7, calibrated)
    ├── ISL_GNSS_Monitor/     <- #3 (superseded) exploratory NMEA monitor
    ├── ISL_GNSS_Serial0/     <- #3 GNSS on Serial0/UART1, EN=P1.02 — PASSED (NMEA)
    ├── ISL_WUR_AS3933/       <- #4 AS3933 config + RC-cal + wake IRQ (the new subsystem)
    ├── ISL_DeepSleep_Baseline/ <- #5 sleep floor 157 µA @ 3.6 V, P0.21 wake — PASSED
    ├── ISL_GNSS_DutyCycle/   <- #6 GPS 30 s / sleep 60 s cycle at baseline floor — PASSED
    └── ISL_RTC_GPS_TimeSet/  <- #7 RTC seeded from GNSS UTC (3.8–17.7 s, sats=0) — PASSED
```

## Suggested bring-up order
0. **`ISL_I2C_Scan`** — confirms the toolchain, the debug serial, and that the RTC
   answers on I2C (0x52). If nothing prints, the board may log over native USB-C
   (`Serial`) instead of the RAKDAP UART (`Serial0`) — see the note in each sketch.
1. **`ISL_RTC_Read`** — RV-3028 runs and keeps time. The INT/wake pin is P0.21 on
   schematic v2.
2. **`ISL_Battery_ADC`** — reads battery volts; the low-impedance divider + C17
   filter mean no settling/noise workaround is needed. Confirm against a known supply.
3. **`ISL_GNSS_Serial0`** — GPS on **`Serial0`/UART1** (P0.19/P0.20), EN=**P1.02**,
   `RAK_CUSTOM_MODE` @ 9600. **PASSED** (NMEA streaming, antenna OK). Next: bring up
   the TinyGPSPlus + low-power/deep-sleep GPS logic and validate a fix.
4. **`ISL_WUR_AS3933`** — brings up the wake-up receiver: SPI comms check, pattern-mode
   config, RC-oscillator calibration, register dump, and wake-on-interrupt reporting.
   Needs a WUR transmitter (`reference/AS3933_wakeup/WuTx*`) to trigger a real wake.

## Design rules baked in
- **Deep sleep:** `api.ble.stop()`, `NVIC_DisableIRQ(FPU_IRQn)` + `clearFPU()`,
  float-free sleep path, external-INT wake (P0.21 for RTC **and** P1.04 for WUR).
- **RTC:** Melopero_RV3028 init + single-shot periodic-timer wake pattern.
- **GPS:** power-cut-first / let-UART-quiet-then-end teardown, then drive the UART
  pins LOW to isolate the external module during sleep.
- **Battery ADC:** on this RUI3 core `analogReference(AR_INTERNAL)` is **~3.67 V FS**
  and direct-register SAADC access returns 0, so the reader is calibrated
  (`raw×1795/1000`, test #2). See `docs/ISL_Pinout.md`.

## Open unknowns to confirm on hardware (tracked in `docs/ISL_Pinout.md`)
1. ~~GPS `Serial` instance~~ **RESOLVED: `Serial0`/UART1 (P0.19/P0.20).**
2. ~~`L76K_EN` polarity~~ **RESOLVED: P1.02, active-LOW.**
3. ~~Debug path~~ **RESOLVED (test #0): native USB-C `Serial`.**
4. **Sleep-floor cause UNRESOLVED (test #5): 157 µA @ 3.6 V measured, but the real
   1 MΩ/1 MΩ+C17 divider is only ~1.8 µA** — so ~150 µA is unexplained = deep-sleep
   headroom. Next: headless battery-only teardown — see `docs/ROADMAP.md` §6.
5. GPS **position fix** validated outdoors (production v5: 7 s hot-start, 19 sats).
6. **AS3933 WUR** — SPI + config + RC-cal confirmed; a real **wake event** (LF TX) is pending.
7. ~~Battery ADC reads low~~ **CALIBRATED (test #2):** `AR_INTERNAL` ≈ 3.67 V FS here;
   reader `raw×1795/1000`, ≤25 mV over 3.2–3.7 V; in `production/v2`.

## Board quirk: alive-first sketch structure (IMPORTANT)
On this board the debug port is the nRF52840 **native USB CDC**, and there is a
single COM port (e.g. COM50) that is the **SDFU bootloader** when idle and the
**running app** after reset — use it for both upload and monitor. Doing long
blocking work or early peripheral init inside `setup()` wedges the app before it
prints (it faults back to the bootloader). The fix, used by all ISL test sketches:
keep `setup()` tiny (just `Serial.begin` + a short wait + one banner), then run a
short heartbeat and do the real init a couple seconds into `loop()`. This was
proven with ISL_RTC_Read (v1 all-in-setup faulted; v2 alive-first works and even
survives resets).

## Status
See **`tests/README.md`** for the full status matrix and the remaining work to fully
evaluate the board. In short:
- **#0 `ISL_I2C_Scan` PASSED** — MCU alive, RV-3028 @ 0x52, debug = native USB-C `Serial`.
- **#1 `ISL_RTC_Read` PASSED** — RV-3028 keeps time, persists across reset. Established
  the alive-first structure (above).
- **#3 `ISL_GNSS_Serial0` PASSED** — GPS on `Serial0`/UART1, EN=P1.02; NMEA streaming,
  antenna OK.
- **#4 `ISL_WUR_AS3933` PARTIAL** — AS3933 SPI + config + RC-cal PASS (R5/R6 ok, RC_CAL_OK,
  config verified); real wake pending an LF transmitter (assigned to second engineer).
- **#5 `ISL_DeepSleep_Baseline` PASSED** — floor **157 µA @ 3.6 V**, **P0.21 RTC wake
  validated**. ⚠️ Floor **cause unexplained** (real 1 MΩ divider is ~1.8 µA, not the
  floor) → deep-sleep headroom. Rules + correction in `docs/ISL_DeepSleep_Notes.md`.
- **#6 `ISL_GNSS_DutyCycle` PASSED** — 30 s GPS / 60 s sleep cycles with the sleep floor
  back at the 157 µA baseline (GPS adds ~0 when torn down per the notes).
- **#2 `ISL_Battery_ADC` CALIBRATED** — `AR_INTERNAL` is ~3.67 V FS on this core (not
  2.4 V); reader = offset-cal + `analogRead` + **`raw×1795/1000`**, ≤25 mV over 3.2–3.7 V.
- **`production/v2` = v1 + calibrated battery** (the only delta is `readVbat_mV()`; the
  `vbat` packet field is now trustworthy). All v1 validation carries over.
- **#7 `ISL_RTC_GPS_TimeSet` PASSED** — RTC seeded from GNSS UTC in 3.8–17.7 s (time
  before fix, `sats=0`). Found the Melopero `setTime` weekday/date order bug and the
  **VBACKUP-has-no-battery** fact (clock resets on full power loss — GPS re-seed is
  the designed recovery).
- **`production/v3` = v2 + GPS time-seed + long wakes** — boot time-sync, auto 1 Hz /
  **1/60 Hz** tick up to ~2.8 days, dynamic backstop.
- **`production/v4`/`v5` — field-tested outdoors.** v5 got the **first full production
  GPS fix: 7 s hot-start, 19 sats, real coordinates delivered+ACK'd headless**. The
  GNSS hardware is excellent (12-sat 3D fix, HDOP 2.0). Two findings drive **v6**:
  (a) production "no fix" was **cold-start × short power-cycled windows** (not hardware);
  (b) **RTC sync-once** left the clock ~11 days behind. Full analysis in
  **`docs/GNSS_FieldStrategy.md`**.
- **`production/v6` = the field strategy, coded (awaiting field test).** SV-gated
  adaptive GPS timeout (abort on no sky, extend when sats are visible), no-sky
  backoff after K no-fix cycles, **RTC re-sync on every real fix** (every timestamp
  GNSS-derived; kills the 11-day bug), and a **delivery guarantee**: a successful
  fix that misses its ACK is retained and re-sent newest-first, ≥30 s apart, only
  when the link is proven up. Also fixes a latent flash-layout overlap (schema v2).
  See `production/v6/README.md`.
- **`production/v1` VALIDATED (happy path)** — full node firmware (GNSS + LoRa TX/ACK +
  persistence + deep sleep). Run 1 (~3.6 h): floor **155 µA @ 3.6 V** over 80 plateaus,
  receiver-confirmed delivery. Run 2 (`ISL_Production_units`, min/sec knobs — **the
  current reference version**): **first GNSS fix** (~21 s) → TX → ACK → 15-min sleep at
  the floor. Remaining before deployment: the robustness checklist in
  `production/v1/README.md` (ACK-loss/retry paths, pending/undelivered lists, brownout,
  blackout, serial-log evidence, >68 min cadence, battery cal, WUR wake).
