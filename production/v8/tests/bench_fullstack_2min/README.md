# v8 bench test — full stack (2-min cadence, `SIMULATE_FIX`)

Indoor bench run of production **v8** with `SIMULATE_FIX 1`, `GNSS_PERIOD_MIN 2`.
Three nodes logged simultaneously (tracker on battery, no power profiler):

| Log | Node |
|-----|------|
| `Tracker_log.txt` | ISL board (RAK4630, v8) — the tracker |
| `BLE_Receiver_log.txt` | nRF52840 BLE central (`ISL_v8_BLE_Receiver`) — the drone-side accel receiver |
| `SolarNode_Repeater_log.txt` | SenseCAP Solar Node (XIAO nRF52840 + SX1262) — LoRa RX→ACK→relay repeater |

## What it validated ✅

- **Deep sleep + RTC wake:** every cycle wakes on P0.21 at **~120002 ms** (rock-solid, no backstop).
- **Reboot-to-sleep (v8 BLE teardown):** each fix cycle ends `reboot-to-sleep` → boot →
  `queued 120 s deep-sleep (BLE-clean)` → wake → next cycle. Cycles run back-to-back.
- **LoRa TX + ACK, end-to-end:** the SolarNode repeater receives each packet
  (RSSI −66…−76 dBm, SNR 10–13 dB), sends `ACK,51,<seq>`, relays onward. `dup=0 bad=0`.
- **Delivery guarantee (the highlight):** the tracker booted with **5 un-ACK'd fixes
  pending** (seq 173–177). On the first live link it sent the fresh packet (179), then
  drained the backlog **newest-first, 30 s apart** (177→176→175→174→173) — the repeater
  received and ACK'd **all of them** (`rx=1…6`), and `pending` went 5 → 0.
- **Accelerometer + BLE offload:** from cycle 3 on, `[ACCEL] 49 samples gathered` → BLE
  burst `ACC cycle=0 count=49 … END` received in full by the BLE central.
- **Receiver-independent BLE:** the BLE receiver was disconnected from USB mid-test and
  reconnected (its log restarts) — the tracker kept offloading each cycle regardless.
- **Cold-boot recovery:** a mid-run power interruption reset the RV-3028 (VBACKUP has no
  battery → clock → `2000-01-01`, by design). The node cold-booted, re-seeded the clock
  from the (simulated) fix, and continued — **flash state survived** (nextSeq/pending/
  delivered intact).

## Notes / things to watch

1. **Accelerometer `WHO_AM_I != 0x33` on cycles 1–2, then fine from cycle 3.** Intermittent
   I2C detection (likely the Grove connector seating / power settling); the 10× retry in
   `lisInit` recovered it. On a skipped cycle no accel data is produced. Re-seat/secure the
   Grove wiring; the v9 switchable rail (`ACCEL_PWR_PIN`) with a settle delay should also help.
2. **One mid-run cold boot** (power glitch). Expected behaviour given VBACKUP has no cell,
   but if it recurs it may indicate the battery can't ride the ~100 mA LoRa-TX current
   spikes — worth watching on the longer run.
3. **Battery reads drift up** ~3.89 → 3.99 V across the run (relaxation / charge source),
   not a fault.
4. **Power note:** this run had no profiler, but see `../../README.md` — with the Grove
   accel on always-on 3V3 the sleep floor is ~335 µA (module draw), not 34 µA. Functional
   here; battery-life numbers need the switchable rail.

## TX/ACK link (tracker ↔ repeater)

Tracker TX 14 dBm → repeater RX −66…−76 dBm; repeater ACK 22 dBm → tracker RX −42…−49 dBm.
The ~20 dB asymmetry is the 8 dB TX-power difference plus path — consistent, all packets ACK'd.
