# v17 / iteration3 — validation run (2026-08-03)

Raw logs: [`Tracker_v17_iter3_validation.log`](Tracker_v17_iter3_validation.log) (collar),
[`SolarNode_repeater_v17.log`](SolarNode_repeater_v17.log) (repeater),
[`BLE_Receiver_v17.log`](BLE_Receiver_v17.log) (drone), power
[`ppk_v17_iter3_summary.txt`](ppk_v17_iter3_summary.txt).

Setup: v17 on the iteration3 board (same code, no changes). Walked outside (fixes), came
back indoors without interrupting, then plugged in the SolarNode repeater and the BLE
drone. **Verdict: iteration3 + v17 validated.** Two optimization-only items surfaced
(documented in the [v17 README → Pending optimizations](../README.md)); neither is a blocker.

## What passed

| Subsystem | Result |
|---|---|
| **GNSS (iteration3, TPS22918 active-HIGH)** | ✅ open-sky: **6+ consecutive hot fixes, TTFF 6–7 s, 21–22 sats in view, 13–16 used, `CELL=OK`**. Powers/fixes/tears down cleanly. |
| **Accel over SPI (onboard LIS3DHTR, CS P0.28)** | ✅ 100-sample records, checksum-match on the drone, `bad=0`. |
| **LoRa + v16 durable backlog (REAL fixes)** | ✅ repeater absent for the first 8 cycles → **9 real fixes buffered**; on reconnect the fresh packet ACK'd (−40 dBm) and the backlog drained **newest-first (seq 11→1), all ACK'd, `delivered=10 pending=0 undelivered=0`, zero loss.** Repeater received/ACK'd/relayed all 10, dedup clean. |
| **RTC / deep-sleep / battery / flash** | ✅ clean 120 s wakes, RTC-from-GPS, backlog persisted across reboots, 3.93–3.99 V. |

The earlier "no-fix" runs were **no sky at that spot** (confirmed: same module gave 22
sats once outdoors; a bad test location, not the board/firmware). See the GNSS-monitor
debugging chain that isolated it to reception, then to open sky.

## Two observations feeding the optimization backlog (README)

1. **Deep-sleep floor ~190 µA** (PPK; 119 s continuous run). ~6× the v7 34 µA — a fixable
   P1 input-buffer crowbar (P1.03/P1.01/P1.04 left as connected `INPUT`; `accelPinsPark`
   disconnects only P0). Modelled life at GNSS/1 h on 9600 mAh: **~3.3 yr as-is**, **~7 yr
   if the floor is parked**. See README *Pending opt #1* + the PPK summary.
2. **`TTFF`/`CELL` stale on backlogged packets.** Cross-check the same fix in both logs —
   `seq=9` was **staged `TTFF=7,CELL=OK`** (collar, outdoors) but **delivered/relayed
   `TTFF=120,CELL=LOW`** (repeater `[GPS-CELL] id=51 SV=22 TTFF=120s CELL=LOW`). Position
   is per-packet-correct; only the health fields read transmit-time state. See README
   *Pending opt #2* (optional per-packet fix, schema 4→5).

## Deferred (other engineer / next step)
- WUR real LF wake + arming P1.04; BLE-integration robustness (a dropped record + reset
  disconnecting peers). Final **battery-only** PPK (USB detached) for the true floor, and
  a real 1 h-cadence endurance run to confirm the hot-fix (lifespan) assumption.
