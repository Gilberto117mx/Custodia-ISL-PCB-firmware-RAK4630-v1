# ISL Board — Production Firmware **v3** (GPS time-seed + long wake periods)

Same validated operate loop and calibrated battery reader as **v2**, plus the two
features needed for real deployment cadence:

## 1) RTC time comes from GNSS (no hardcoded date) — test #7
On this board the RV-3028's **VBACKUP is tied to the 3.3 V rail** (no coin
cell/supercap), so **any full power loss resets the clock to 2000-01-01**. v3
makes that self-healing:

- **At boot**, if the RTC was never GPS-synced (USER_RAM flag absent), run a
  **time-only GPS session** (up to `BOOT_TIME_SYNC_TIMEOUT_SEC` = 120 s). GPS
  time is decoded from the nav message **before a position fix** — test #7
  measured **3.8–17.7 s with `sats=0`**.
- **Hardened sanity guard** (after the first indoor v3 run caught a corrupted
  RMC — `2088-31-19` — passing a year-only check; TinyGPSPlus checksums
  sentences but does **not** range-check fields):
  1. **Full range check** — year window 2025–2044, month 1–12, day 1–31,
     h/m/s in range, age < 2 s.
  2. **Two-sample confirmation** — a second sane reading ≥ 2 s later must have
     advanced consistently with wall time (±2 s). Garbage doesn't tick
     coherently; a real GPS clock does.
  3. **Self-heal at boot** — if the sync flag is set but the stored RTC time is
     itself insane (a past bad seed), the flag is cleared and the node re-seeds.
- If boot sync fails (indoors), the node operates anyway and **re-seeds
  opportunistically inside every COLLECT fix session** until it succeeds
  (`maybeSeedRtc()` runs in the NMEA loop at zero extra power).
- The RTC holds **UTC**; packet `ts` fields are true unix epochs after sync.
- Fixes the `Melopero_RV3028::setTime()` **argument-order bug** found in test #7:
  the order is `(year, month, WEEKDAY, DATE, hh, mm, ss)` — weekday *before*
  date. (Older hardcoded calls had them swapped → wrong day-of-month.)

## 2) Wake periods > 68 min (e.g. 2 h) — robust dual-mode RTC timer
The RV-3028 countdown timer is 12-bit (max preset 4095). `rtcSetNextWake()` now
**auto-selects the tick**:

| Requested sleep | Tick mode | Preset unit | Max | Granularity |
|---|---|---|---|---|
| ≤ 4095 s (~68 min) | **1 Hz** | seconds | 4095 s | 1 s |
| > 4095 s | **1/60 Hz** | minutes (rounded) | 4095 min (**~2.8 days**) | 1 min |

- Default `GNSS_PERIOD_MIN = 120` (the 2 h deployment cadence) → 120-min preset
  on the 1/60 Hz tick.
- The deep-sleep **backstop is now dynamic**: `requested sleep + 5 min` margin
  (v1/v2's fixed 1 h backstop would have cut a 2 h sleep short — it exists only
  to catch a *missed* RTC INT).
- Known countdown-timer property: the **first period can be short/long by up to
  one tick** (~60 s in 1/60 Hz mode) — inherent to the RV-3028, negligible at
  multi-hour cadence.
- `[RTC] timer: preset=… s/min` and `[CFG] … (1/60Hz tick)` lines show which
  mode is active.

## Knobs (new in v3, on top of v2's set)
| Constant | Default | Role |
|---|---|---|
| `GNSS_PERIOD_MIN` | **120** | wake cadence — any value up to 4095 min works now |
| `BOOT_TIME_SYNC_TIMEOUT_SEC` | 120 | boot time-only GPS session limit |
| `MIN_VALID_YEAR` | 2025 | GPS time sanity guard |

## Expected first-boot serial (cold power-up, sky view)
```
[RTC] NOT GPS-synced (will seed from GNSS)  current: 2000-01-01 ...
[TIME] boot GPS time-sync (up to 120 s, no fix needed)...
[RTC] SET FROM GPS: 2026-07-13 08:12:57 UTC (unix~1783930377)
[CFG] id=051  period=120 min (1/60Hz tick)  ...  rtcSynced=1
== COLLECT ==  ...  == IDLE deep-sleep 7200 s ==
[RTC] timer: preset=120 min (1/60 Hz tick)
```
Then the COM port drops; on the meter, expect a **2 h flat floor at ~155 µA**
between cycles. A warm reboot (power kept) skips the time sync (`flag present`).

## How to test (suggested)
1. **Time-seed**: cold power-up near a window → watch `SET FROM GPS` with today's
   UTC date; unplug/replug → it re-seeds again (VBACKUP loses time by design).
2. **Long sleep**: to verify the 1/60 Hz path without waiting 2 h, temporarily set
   `GNSS_PERIOD_MIN = 70` (70 min > 68 min forces the 1/60 Hz tick) and confirm
   `[WAKE] RTC P0.21 after ~4200000 ms` — or just leave 120 and check the meter
   shows the wake 2 h later.
3. Everything else behaves exactly like the validated v1/v2 runs.

## Status
**NOT yet run on hardware** (scaffold stage, like v1 was). The building blocks
are individually validated: time-seed = test #7 PASS; sleep/wake machinery =
tests #5/#6 + v1's 3.6 h run (1 Hz tick only — **the 1/60 Hz path is the new
thing to validate**); battery = test #2 CALIBRATED. The v1 robustness checklist
(`../v1/README.md`) still applies.
