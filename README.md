# ISL Board (RAK_feather)

A **RAK4630-based custom board** built around the Nordic nRF52840 + Semtech SX1262
(the RAK4630 module), an RV-3028-C7 RTC, an L76K multi-GNSS receiver, an onboard
**LIS3DHTR accelerometer**, and an **AS3933 LF wake-up receiver**. This repository
is the standalone home for the ISL board: hardware of record, bring-up tests, and
production firmware for the animal-tracking collar (the "Custodia-Tracker" node) —
together with the companion **SolarNodeSystem** (repeater + gateway + cloud uploader)
that carries collar packets to the Custodia cloud.

> ## 🚚 This repo is the handover for the first medium-scale pilot
> **Start with [`HANDOVER.md`](HANDOVER.md)** — the end-to-end delivery snapshot
> (collar + repeater + gateway + cloud), pilot readiness, power/lifespan, and the two
> items the **ISL lab engineers** must still integrate:
> 1. the **AS3933 WUR has never been integrated in any version** — the drone pass is
>    currently **faked with a timer** (`SIMULATE_WUR_HOURS`; `ENABLE_WUR_WAKE=0`) and must
>    be integrated + validated on real hardware;
> 2. the **BLE offload protocol** is to be updated by the ISL team — that work is
>    **already done by Omar Khalfa and is pending integration.**

> Bring-up is hardware-in-the-loop: flash a focused test, capture the serial/power
> output, iterate. Each test's output feeds the next refinement.

> **📍 Current status & what to test next: [`docs/ROADMAP.md`](docs/ROADMAP.md)** —
> the single source of truth. Hardware / schematic-of-record:
> [`hardware/README.md`](hardware/README.md) (**iteration3** — the onboard-accelerometer
> board — is authoritative; iteration2/1 are historical).

## Final status (2026-08-11) — **v17 FIELD-VALIDATED (pilot build)**
- **`production/v17` is the definitive, field-validated firmware version** — v16's full
  feature set ported to **PCB iteration3** (onboard LIS3DHTR over SPI + GPS on an
  active-HIGH TPS22918 load switch). Full detail, power/lifespan model, and the
  optimization items: `production/v17/README.md`; run evidence: `production/v17/logs/`.
- **Field-validated end-to-end (real 2-day out-of-range trip, 2026-08-11):** the collar
  buffered fixes for ~2 days, then on first gateway contact delivered an **83-packet
  burst newest-first (seq 194→94), `emit=83 bad=0`**, ~15 s/packet — the durable backlog
  proven on real hardware, real trip, real gateway (`production/v17/logs/REALWORLD_BACKLOG.md`).
- **Earlier bench validation on the iteration3 board (open-sky run, 2026-08-03):**
  - **GNSS (TPS22918 active-HIGH):** 6+ consecutive **hot fixes, TTFF 6–7 s, 21–22
    sats, `CELL=OK`** — powers/fixes/tears down cleanly.
  - **Accelerometer over SPI** (onboard LIS3DHTR, CS P0.28): 100-sample records,
    checksum-matched on the drone, `bad=0`.
  - **LoRa + the v16 durable 1-week backlog, with *real* fixes:** repeater absent for
    8 cycles → **9 real fixes buffered**, then drained **newest-first on reconnect,
    all ACK'd, `delivered=10 pending=0 undelivered=0`, zero loss.**
  - **RTC / deep-sleep / battery / flash:** clean 120 s wakes, GNSS-disciplined RTC,
    backlog persisted across reboots, 3.93–3.99 V.
- **Power & lifespan (measured, PPK 2026-08-03):** deep-sleep floor **~190 µA** on
  iteration3; modelled **~3.3 years** on a 9600 mAh LiSOCl₂ cell at the deployment
  cadence (GNSS/1 h). Two **optimization-only** items (neither a blocker, both
  documented in `production/v17/README.md` → *Pending optimizations*):
  1. the **~190 µA floor** is a fixable P1 input-buffer crowbar (P1.03/P1.01/P1.04
     left as connected `INPUT`; the v7 fix parked only the P0 pins) → parking them
     drops it to **~35–45 µA ⇒ ~7 years**;
  2. `TTFF`/`CELL` on *backlogged* packets read transmit-time state (position data is
     always correct) — an optional per-packet fix (flash schema 4→5).
- ⚠️ **Two items are NOT integrated and are owned by the ISL lab engineers** (full
  detail in [`HANDOVER.md`](HANDOVER.md) and the *Handoff* section below):
  1. the **AS3933 WUR has never been integrated in any version** — the drone pass that
     triggers the BLE offload is currently **faked with a timer** (`SIMULATE_WUR_HOURS`;
     `ENABLE_WUR_WAKE=0`); a real LF wake must be integrated + validated on hardware;
  2. the **BLE offload protocol** is to be updated by the ISL team — that work is
     **already done by Omar Khalfa and is pending integration.**

## Board at a glance
| Subsystem | ISL Board (RAK_feather, iteration3) |
|---|---|
| MCU | RAK4630 (nRF52840 + SX1262) |
| RTC | RV-3028-C7, INT/wake on **P0.21** |
| Power tree | **RT9080-33 LDO → 3.3 V** for nRF/SX1262; VBAT (3.6 V nominal) elsewhere |
| Battery sense | **1 MΩ/1 MΩ divider + C17** on AIN7 (~1.8 µA), calibrated reader |
| GPS | L76K on **`Serial0`/UART1 (P0.19/P0.20)**, EN = **P1.02**, active-HIGH TPS22918 (iteration3) |
| Accelerometer | **onboard LIS3DHTR (U5)** on SPI shared with the WuR (accel CS **P0.28**) |
| Wake-up radio | **AS3933 LF wake-up receiver** on SPI, wake → **P1.04** |
| Deep-sleep floor | **34 µA** on iteration2 (v7 fix); **~190 µA** measured on iteration3 — a fixable P1 input-buffer crowbar → ~35–45 µA (v17 opt #1) |

Two wake sources are the point of this board: the scheduled **RTC wake (P0.21)** and
the on-demand **AS3933 WUR wake (P1.04)** — an ultra-low-power LF receiver that
listens for a 433 MHz carrier modulated with a 19 kHz OOK/Manchester 16-bit pattern
(default `0x9669`) and asserts P1.04 on a match, so a beacon can wake the collar.

## Layout
```
.                             <- repository root
├── HANDOVER.md               <- ⭐ pilot handover: end-to-end delivery + WUR/BLE items (READ FIRST)
├── README.md                 <- this file
├── docs/
│   ├── ROADMAP.md            <- STATUS + what to test next (single source of truth)
│   ├── ISL_Pinout.md         <- authoritative pin map + hardware notes (READ FIRST)
│   ├── ISL_DeepSleep_Notes.md<- deep-sleep rules (teardown, floor, wake)
│   └── GNSS_FieldStrategy.md <- GNSS field findings → the v6 strategy
├── hardware/
│   ├── README.md             <- schematic-of-record (iteration3 current; 2/1 historical)
│   ├── iteration1/           <- RAK_feather schematic v1, PCB PDF, BOM (SUPERSEDED)
│   ├── iteration2/           <- RAK_feather schematic v2 (prior; firmware v1–v16 validated here)
│   └── iteration3/           <- RAK_feather schematic v3 (CURRENT: onboard LIS3DHTR/SPI, GPS on TPS22918)
├── reference/
│   └── AS3933_wakeup/        <- vendor AS3933 WUR repo, UNMODIFIED (ESP32 + MATLAB/SDR TX)
├── production/               <- collar firmware v1 → v17 (v17 = pilot build; see ROADMAP §2)
│                                incl. production/v17/POWER_PROFILE.md
├── tests/                    <- subsystem bring-up sketches + BLE/accel/flash probes (see tests/README.md)
└── SolarNodeSystem/          <- companion system: LoRa repeater + gateway + XIAO ESP32 cloud uploader
    └── v1/                      (SenseCAP Solar Node P1-Pro + XIAO ESP32-S3) — see SolarNodeSystem/v1/README.md
```

## Suggested bring-up order (subsystem tests)
0. **`ISL_I2C_Scan`** — toolchain + debug serial + RTC on I2C (0x52). If nothing
   prints, the board logs over native USB-C (`Serial`), not the RAKDAP UART.
1. **`ISL_RTC_Read`** — RV-3028 runs and keeps time. INT/wake pin is P0.21.
2. **`ISL_Battery_ADC`** — battery volts; calibrated reader (`raw×1795/1000`).
3. **`ISL_GNSS_Serial0`** — GPS on **`Serial0`/UART1** (P0.19/P0.20). **PASSED** (NMEA).
4. **`ISL_WUR_AS3933`** — AS3933 SPI + pattern config + RC-cal + wake IRQ. Needs a WUR
   transmitter (`reference/AS3933_wakeup/WuTx*`) to trigger a real wake.

## Design rules baked in
- **Deep sleep:** `api.ble.stop()`, `NVIC_DisableIRQ(FPU_IRQn)` + `clearFPU()`,
  float-free sleep path, external-INT wake (P0.21 for RTC **and** P1.04 for WUR), and
  **keep the AIN7 input buffer disconnected** (the v7 crowbar fix → 34 µA).
- **RTC:** Melopero_RV3028 init + single-shot periodic-timer wake; re-synced from GNSS
  UTC on every real fix (VBACKUP has no cell, so the clock is GNSS-disciplined).
- **GPS:** power-cut-first / let-UART-quiet-then-end teardown, then drive the UART pins
  LOW to isolate the external module during sleep.
- **Battery ADC:** on this RUI3 core `analogReference(AR_INTERNAL)` is **~3.67 V FS**
  and direct-register SAADC access returns 0, so the reader is calibrated
  (`raw×1795/1000`, test #2). See `docs/ISL_Pinout.md`.

## Board quirk: alive-first sketch structure (IMPORTANT)
The debug port is the nRF52840 **native USB CDC** on a single COM port (e.g. COM50)
that is the **SDFU bootloader** when idle and the **running app** after reset — used
for both upload and monitor. Heavy work or early peripheral init in `setup()` wedges
the app before it prints (it faults to the bootloader). The fix used by all ISL
sketches: keep `setup()` tiny (`Serial.begin` + a short wait + one banner), then run a
heartbeat and do the real init a couple seconds into `loop()`.

## Subsystem bring-up — status
See **`tests/README.md`** for the full matrix. In short:
- **#0 `ISL_I2C_Scan` PASSED** — MCU alive, RV-3028 @ 0x52, debug = native USB-C `Serial`.
- **#1 `ISL_RTC_Read` PASSED** — RV-3028 keeps time, persists across reset.
- **#2 `ISL_Battery_ADC` CALIBRATED** — `AR_INTERNAL` ≈ 3.67 V FS; `raw×1795/1000`, ≤25 mV.
- **#3 `ISL_GNSS_Serial0` PASSED** — GPS on `Serial0`/UART1, EN=P1.02; NMEA, antenna OK.
- **#4 `ISL_WUR_AS3933` PARTIAL** — AS3933 SPI + config + RC-cal PASS; **real LF wake
  pending** (ISL lab engineers — see handoff below).
- **#5 `ISL_DeepSleep_Baseline` PASSED** — floor validated; P0.21 RTC wake validated.
  Floor later **solved**: an AIN7 input-buffer crowbar (our own `pinMode`), fixed in
  **v7 → 34 µA** (`docs/ISL_DeepSleep_Notes.md`).
- **#6 `ISL_GNSS_DutyCycle` PASSED** — GPS duty-cycle + isolation teardown adds ~0 µA.
- **#7 `ISL_RTC_GPS_TimeSet` PASSED** — RTC seeded from GNSS UTC in 3.8–17.7 s (time
  before fix). Established VBACKUP-has-no-battery → GPS re-seed is the recovery.

## Production firmware — v1 → v17 (summary)
Full detail per version in `production/vN/README.md`; roadmap table in `docs/ROADMAP.md` §2.
- **v1–v5** — full node loop (GNSS + LoRa TX/ACK + persistence + deep sleep), calibrated
  battery, GPS→RTC time seed + long wakes, SV reception diagnostic; **first outdoor fix**
  in v5 (7 s hot-start, 19 sats, delivered + ACK'd headless).
- **v6** — GNSS field strategy (SV-gated adaptive timeout, no-sky backoff, RTC re-sync on
  every fix) + the **delivery guarantee** (un-ACK'd fixes held, re-sent newest-first).
  Bench-validated.
- **v7** — **deep-sleep floor fix**: the ~120 µA overage was an AIN7 input-buffer crowbar
  (our `pinMode`), not the divider/LDO. **Verified 34 µA on battery** (~4.5× idle life).
- **v8–v13** — **accelerometer over BLE**: onboard motion capture to a flash ring, offloaded
  to a passing drone over BLE. v11 is the last known-good "whole ring" build; v12 switches to
  new-data-per-pass; v13 adds two independent timers + per-record timestamps. All bench-verified
  (0 bad records, collar never freezes, LoRa 100 %).
- **v14** — cold/first-fix GPS strategy (patient window on a cold or new-city fix).
- **v15** — GPS backup-cell (MS621FE) health + charge-on-cold; every packet carries
  `TTFF=<s>,CELL=<OK|LOW|DEAD>`. **Field-validated** (a drained cell self-charged into hot starts).
- **v16** — durable **1-week** LoRa backlog (`PENDING_SLOTS` 5→84) so a multi-day out-of-range
  excursion replays every fix on return. **Bench-validated** (42 fixes, zero loss).
- **v17 (DEFINITIVE / VALIDATED)** — v16 ported to **PCB iteration3**: onboard LIS3DHTR
  over SPI + GPS on an active-HIGH TPS22918. Feature-identical to v16; hardware port only.
  **Validated on the iteration3 board (2026-08-03):** hot GPS fixes (TTFF 6–7 s, 21–22 sats),
  accel over SPI (`bad=0`), and the durable backlog drained 10/10 with zero loss. Measured
  power/lifespan: floor ~190 µA → **~3.3 yr** as-is (~7 yr with opt #1). Evidence in
  `production/v17/logs/`; full write-up in `production/v17/README.md`.

## Handoff to ISL lab engineers — the two items to integrate (WUR + BLE)
These two on-demand items are **not integrated** in the delivered firmware and are owned
by the **ISL lab engineers**. Full context in [`HANDOVER.md`](HANDOVER.md).

1. **AS3933 wake-up receiver (WUR) — never integrated in any version.** The drone pass
   that triggers the BLE offload is currently **faked with a timer** (`SIMULATE_WUR_HOURS`,
   default 0.25 h) and the wake pin is disarmed (`ENABLE_WUR_WAKE = 0`). SPI comms,
   pattern-mode config, and RC-oscillator calibration are proven (`tests/ISL_WUR_AS3933/`),
   and the collar is wired for the wake on **P1.04**. Remaining: trigger a **real LF wake**
   from a transmitter (`reference/AS3933_wakeup/WuTx*`: 433 MHz, 19 kHz OOK/Manchester,
   pattern `0x9669`), confirm the **P1.04 rising-edge IRQ + pattern match**, then set
   `SIMULATE_WUR_HOURS = 0` and `ENABLE_WUR_WAKE = 1` so a real beacon drives the offload —
   and validate it end-to-end.
2. **BLE offload protocol — to be updated by the ISL team.** The collar advertises as
   **`Custodia-Tracker`** and streams accelerometer records over the proven fire-and-forget
   NUS path (`production/v17/ISL_v17_Drone_Receiver`, plus the `tests/ISL_BLE_CustomOpen/` +
   `tests/Accelerometer/` reference sketches). The **updated BLE protocol is already
   developed by Omar Khalfa and is pending integration** by the ISL team — treat the current
   path as a working placeholder to be replaced with his protocol, then validated with the
   WUR-triggered pass from item 1.

Everything needed to continue both — pin map, tests, reference emitters/receivers, and the
firmware hooks — is in this repository.
