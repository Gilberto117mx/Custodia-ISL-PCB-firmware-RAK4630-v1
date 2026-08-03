# v13 outdoor headless run — 2026-07-29 (analysis)

Files: `LoRaRepeater_v13_outdoor.txt` (the SolarNode repeater's serial),
`ppk_v13_outdoor_summary.txt` (PPK2 current, decile summary — raw ~14 MB CSV not
committed). **The collar was headless** (powered via PPK2, no USB serial host); the
BLE receiver and the repeater were on separate computers.

Firmware config (from the `.ino` that was run): `GNSS_PERIOD_MIN=15`,
**`SIMULATE_FIX=0` (real GPS)**, `ACCEL_PERIOD_HOURS=0.33`, `SIMULATE_WUR_HOURS=1`,
`FIX_MAX_SEC=120`, `NO_SKY_ABORT_SEC=25`, **`NOFIX_BACKOFF_AFTER=3`,
`BACKOFF_PERIOD_HOURS=6`**, `ENABLE_WUR_WAKE=0`.

## Question asked: does the collar work with NO serial connection?

**Yes — headless works, and this run proves it.** Two independent pieces of evidence:

1. **The headless collar transmitted LoRa and the repeater received it.** The repeater
   logged `[RX] #16 … "051,122,…,SV=0"` and `[RX] #17 … "051,123,…,SV=0"` — packets
   from collar id 51, seq 122 & 123, ACKed and relayed. A collar blocked on `Serial`
   could not have transmitted anything.
2. **The power trace shows normal cycling for the first ~43 min** (GPS/TX bursts to
   ~91 mA), not a boot hang.

Code-level confirmation (the `.ino`): every serial call is guarded/bounded, so a
missing host cannot block:
- `#define DBG(...) do { if (Serial) { Serial.printf(...); Serial.flush(); } } while(0)`
  — no-ops when no host.
- `void say(...) { if (Serial) { … } }` — same.
- `setup()`: `while (!Serial && (millis()-t) < 4000) delay(10);` — bounded 4 s wait,
  then proceeds regardless.
(The GPS uses `Serial0`/UART1, a hardware UART independent of the USB CDC.)

## So why did "nothing work"? The GPS never fixed → backoff → the device went quiet

The chain, all **working as designed**:

1. **No GPS fix (real cause).** Every packet is `SV=0`, `lat/lon = 0.000000` — the L76K
   never acquired satellites (and, from the missing timestamp field in the relayed
   packet, likely never even time-synced). That is an **antenna / sky-view /
   cold-start / GPS-backup-battery** issue — hardware/environment, **not firmware and
   not related to serial.**
2. **No fix → Strategy B backoff.** With `SIMULATE_FIX=0` and no sky, each cycle is a
   no-fix cycle. **Each GPS attempt was ~25 s, NOT 120 s:** `gpsAcquire()` runs a
   120 s (`FIX_MAX_SEC`) loop, but Strategy A's **no-sky early abort** returns at
   `NO_SKY_ABORT_SEC=25 s` whenever fewer than `SV_MIN=4` sats have *ever* been seen
   (`peakInView < 4`). The packets are `SV=0`, so every attempt aborted at ~25 s; the
   full 120 s only applies when ≥4 sats ARE visible ("sky, no fix"). Between attempts
   the device deep-sleeps `GNSS_PERIOD_MIN=15 min`. After **`NOFIX_BACKOFF_AFTER=3`
   consecutive no-fix cycles (~43 min)** the cadence stretches to
   **`BACKOFF_PERIOD_HOURS=6`**.

   > ⚠ **This "3 cycles" is INFERRED, not directly logged** — the collar was headless,
   > so there is no `[GPS] no fix … (no-sky abort)` / `[BACKOFF]` trace to read. The
   > inference rests on: the config (`NOFIX_BACKOFF_AFTER=3`), the PPK shape (~43 min
   > active → 6 h flat sleep), and the repeater receiving **2** no-fix packets
   > (seq 122, 123, ~15 min apart); the 3rd (seq 124) was most likely transmitted but
   > lost at the −110 dBm link, and its cycle tripped the backoff. To *see* the attempts
   > you need the collar's serial (or flash-logging) — see the note below.
   >
   > **Also worth checking:** 25 s may be too short for a **cold GPS start**. If the
   > MS621FE GPS backup cell is flat, the L76K can take 30–60 s just to start reporting
   > sats-in-view — in which case the 25 s no-sky abort fires *before* the GPS has woken
   > up, and it can never fix even under open sky. If cold-starting, raise
   > `NO_SKY_ABORT_SEC`, or ensure the backup cell is charged for a hot start.
3. **The flat ~325 µA for the rest of the capture is that 6-hour backoff deep-sleep —
   NOT a hang.** The PPK deciles show mean ≈ 7 mA → 1.6 mA → 1.8 mA for the first
   ~43 min (active), then a solid **~325 µA with no wake bursts** to the end. ~325 µA
   is the **known Grove accel-module floor** (~335 µA wired vs ~34 µA unplugged — a
   hardware item, the switchable rail on the next PCB). A *hung* MCU would sit at
   milliamps; ~325 µA flat is the MCU genuinely in deep sleep at the module floor.
   The 144-min capture only caught the first ~100 min of the 6 h sleep.
4. **BLE never fired** because `SIMULATE_WUR_HOURS=1` (the 1 h drone-pass mark) fell
   **inside** the 6 h backoff sleep — the collar was asleep and never woke to trigger
   an offload. (Also, the v13 accel/drone-pass path is gated on `rtcSynced`; if the
   GPS never time-synced, no accel record was collected, so there was nothing to
   offload either.)
5. **The 2 packets that did arrive were at RSSI −110 dBm** (SF7/250 kHz sensitivity is
   ~−123 dBm, so −110 is ~13 dB of margin — very marginal). Most no-fix packets in the
   active window were likely transmitted but lost to range; the repeater caught 2.

## Bottom line

- ✅ **Headless operation is fine.** No serial-related freeze; the collar ran and
  transmitted with no host attached.
- ❌ **The GPS didn't fix (SV=0)** — the real problem to chase (antenna / sky /
  GPS backup cell / warmup).
- ⓘ The long silent, flat-current tail is the **6 h no-sky backoff working**, not a
  crash; the ~325 µA level is the accel-module draw, not the 34 µA floor.

## Recommendations for the next outdoor/profiling run

- **Fix the GPS acquisition first** (clear sky, check the active antenna + the MS621FE
  GPS backup cell; allow a longer cold-start). A real fix stops the backoff entirely.
- **For a profiling run you want to stay active**, temporarily raise
  `NOFIX_BACKOFF_AFTER` or lower `BACKOFF_PERIOD_HOURS` (e.g. 0.25) so the device keeps
  cycling instead of dropping into a 6 h sleep after 45 min — otherwise the capture is
  mostly one long backoff sleep.
- **If you want BLE to exercise without a GPS fix**, note the drone-pass + accel path
  needs `rtcSynced`; get at least a GPS *time* sync (sats=0 is enough for time), or
  we can add a fallback clock so accel/BLE run even before the first fix.
- The **~325 µA floor is the accel module** (hardware); the true ~34 µA floor needs
  the module on a switchable rail (deferred) and a battery-only, headless capture.
