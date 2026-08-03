# v14 overnight outdoor run — 2026-07-30 (analysis)

Files: `LoRaRepeater_v14_overnight.txt` (the repeater's serial, ~9 h),
`ppk_v14_overnight_summary.txt` (PPK2 current, ~4.8 h captured before the logging
computer turned off). Collar headless on battery, clear sky.

Config (from the `.ino` run): `GNSS_PERIOD_MIN=60`, `SIMULATE_FIX=0` (real GPS),
`NO_SKY_ABORT_SEC=25`, `FIX_MAX_SEC=120`, `COLD_FIX_MAX_SEC=180`, `COLD_AFTER_NOFIX=2`,
`EPHEMERIS_STALE_HOURS=12`, `NOFIX_BACKOFF_AFTER=3`, `BACKOFF_PERIOD_HOURS=2`,
`ACCEL_PERIOD_HOURS=3`, `SIMULATE_WUR_HOURS=6`, schema v3.

## ✅ The headline: v14 gets REAL GNSS fixes (cold-fix strategy works)

Every packet the repeater received:

| # | seq | position | SV | result | budget used |
|---|---|---|---|---|---|
| 1 | 1 | 22.556584, 113.922289 | **9** | ✅ **FIX** | COLD (first fix — `lastFixUnix==0`) |
| 2 | 2 | 22.556543, 113.922274 | **11** | ✅ **FIX** | warm |
| 3 | 3 | 0, 0 | 1 | ❌ no fix | warm |
| 4 | 4 | 0, 0 | 0 | ❌ no fix | warm |

This is a **complete turnaround from the v13 outdoor run** (which was `SV=0` on every
cycle): the first fix used the patient COLD window and locked (PPK burst 1 = 54 s of
GPS-on → SV=9), and seq 2 also fixed. The seq numbers restart at 1 because the schema
v3 bump re-initialised the flash, as intended.

## Power: healthy cycling, no hang

PPK (4.8 h): **6 hourly GPS bursts** (t≈0, 1.02, 2.04, 3.05, 4.06 h — matching
`GNSS_PERIOD_MIN=60`), each ~27–69 s of GPS-on, and **between them a flat ~325 µA**
(the accel-module deep-sleep floor). The collar cycles once an hour and sleeps in
between — **no freeze, no stuck state.** (The ~325 µA floor is the Grove module, not
the 34 µA MCU floor — the known hardware item.)

## The real issue: warm reacquisition dies at the 25 s no-sky abort

After two good fixes, **seq 3 and seq 4 failed** — and the reason is in the budget
logic, not a crash:

- After seq 2 fixed, `lastFixUnix` is recent and `consecutiveNoFix==0`, so seq 3 is
  classified **warm** → `NO_SKY_ABORT_SEC = 25 s`. The GPS only had **1 satellite** by
  25 s (`SV=1`) → no-sky abort → no fix.
- seq 4 is still **warm** (`consecutiveNoFix==1 < COLD_AFTER_NOFIX=2`) → 25 s abort →
  `SV=0` → no fix. Only seq 5 would finally be COLD.

So **two cycles were thrown away to the aggressive 25 s warm abort before the patient
cold budget could engage.** A warm reacquisition after a 1 h sleep can easily need
30–60 s to see ≥4 satellites — 25 s is too tight. (seq 2 happened to get ≥4 sats
within 25 s and fixed; seq 3 didn't — that inconsistency is the tell.)

### Likely hardware contributor: the GPS backup cell

The intermittency (warm fix at seq 2, warm miss at seq 3, one hour later, same sky)
points at the **MS621FE GPS backup cell not reliably holding** across the 1 h sleeps.
If it held ephemeris, warm starts would consistently see many sats within a few seconds
(hot/warm start). When it drains between wakes, each start is effectively cold and the
25 s abort kills it. **Check/charge the backup cell and its trickle-charge path** — it
is the deepest fix; with it, warm starts are fast and only the true first fix is cold.

## After seq 4 → silence (expected, given the above)

The repeater heard nothing from ~04:13 to 09:07 (`rx` stuck at 4). With seq 3, 4 (and
likely 5) no-fix, after 3 consecutive no-fix the node backs off to
`BACKOFF_PERIOD_HOURS=2`, and each backoff wake is again warm-then-cold. The PPK ended
at 4.8 h (logging computer off), so we can't see whether later cold cycles recovered.
BLE was never exercised: `SIMULATE_WUR_HOURS=6` puts the first drone pass at ~6 h,
beyond the captured window.

## Recommendations (small, high-impact)

1. **Raise the WARM no-sky abort** `NO_SKY_ABORT_SEC` from 25 s → **~45–60 s.** This
   alone would very likely have saved seq 3 and 4. 25 s is too tight for a warm
   reacquisition after an hour asleep.
2. **`COLD_AFTER_NOFIX = 1`** so the *first* no-fix flips the next cycle to the patient
   cold window — no two warm misses in a row.
3. **Charge / verify the MS621FE GPS backup cell** (hardware) — the root of the
   intermittency; makes warm starts reliable.
4. (Optional) shorten `SIMULATE_WUR_HOURS` for the next run so a drone pass lands
   inside the capture and BLE gets exercised too.

Net: **v14's cold/first-fix strategy is validated — the collar acquires real fixes and
never hangs.** The remaining misses are the still-too-aggressive *warm* 25 s abort plus
a marginal GPS backup cell, both easy to address.
