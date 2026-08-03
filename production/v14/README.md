# ISL Board — Production firmware v14 (COLLAR, cold / first-fix GPS strategy)

**v14 = v13 + a COLD / FIRST-FIX strategy.** The GPS now gets a long, patient window
*exactly* when it needs one — the first fix after programming, and the first fix after
being moved to a new place ("new city") — while normal wakes keep v13's frugal budget.
Everything else (two accel/BLE timers, per-record timestamp, the proven no-freeze BLE
offload, LoRa + delivery guarantee, 34 µA sleep, reboot-to-sleep) is **identical to
v13** (bench-verified — see `../v13/`).

> ✅ **Status: cold-fix VALIDATED (overnight outdoor run 2026-07-30, `logs/`).** The
> first fix used the COLD window and locked (SV=9), seq 2 fixed (SV=11), and the collar
> cycled + slept normally all night with **no hang** — a complete turnaround from the
> v13 `SV=0` run. Follow-up tuning from that run is now baked in (see below): the WARM
> no-sky abort was too tight and missed seq 3/4, so **`NO_SKY_ABORT_SEC` 25→45 s** and
> **`COLD_AFTER_NOFIX` 2→1**. See `logs/OVERNIGHT_ANALYSIS.md`.

## The problem this fixes

A **cold** GPS receiver (fresh power, or moved far while away) can take **minutes**,
and won't even *report satellites* for the first 30–60 s. v13's `NO_SKY_ABORT_SEC = 25`
therefore **killed the cold first fix before the receiver had woken up** — which is
exactly what silenced the outdoor headless run (`../v13/logs/OUTDOOR_ANALYSIS.md`): the
25 s no-sky abort fired every cycle, `SV=0` every time, and after 3 no-fix cycles the
device backed off to a 6 h sleep.

## What v14 does — two budgets, picked per fix

| Budget | No-sky abort | Max window | Used when |
|---|---|---|---|
| **WARM** (common) | `NO_SKY_ABORT_SEC` = **45 s** | `FIX_MAX_SEC` = 120 s | normal wakes with a recent fix — frugal |
| **COLD** (rare) | **none** (grace = full window) | **`COLD_FIX_MAX_SEC` = 180 s** | first fix / new city — patient |

On a COLD fix the no-sky early abort is disabled (the internal `noSkyMs == maxMs`), so
the receiver gets the whole ≥3-minute window to wake up and lock. Normal wakes are
untouched, so the energy cost is paid **only** on the rare cold moments.

## How "cold" is detected (and how it differs from a normal wake)

A **normal deep-sleep wake** resumes with `rtcSynced == true` **and** a recent
`lastFixUnix`. Anything else is cold. A fix is classified **COLD** when ANY of:

| Signal | Meaning | Scenario it catches |
|---|---|---|
| `!rtcSynced` | the RV-3028 lost its clock → **main power was lost** | fresh flash / battery reconnect / literally first-ever |
| `header.lastFixUnix == 0` | never had a **position** fix yet | first run, before it has ever locked |
| `now − lastFixUnix ≥ EPHEMERIS_STALE_HOURS` (12 h) | long time since the last fix | **new city** with the battery kept on / long unfixed gap |
| `consecutiveNoFix ≥ COLD_AFTER_NOFIX` (**1**) | any miss → next fix is patient | **new city** via boxed/in-transit no-fix cycles; also recovers a single warm miss |

### Why the "new city" case really works (both ways)

`lastFixUnix` lives in the **flash header**, so it survives deep sleep, reboots, **and
full power loss**. So the arrival fix in a new city is caught no matter how it was
transported:

- **Battery stayed connected:** the clock survives, but `now − lastFixUnix` is large
  after hours away → *stale* → COLD. And while boxed in transit it racked up no-fix
  cycles → `consecutiveNoFix ≥ 2` → COLD as a second, independent catch.
- **Battery was disconnected:** the RV-3028 loses its clock → `!rtcSynced` → COLD
  immediately on power-up.

On a successful fix we record `lastFixUnix`, so the **next** fix is warm/short again —
only the *arrival* fix is patient, not every wake in the new location.

## Config knobs (v14 additions, top of `ISL_Production_v14.ino`)

| Knob | Default | Meaning |
|------|---------|---------|
| `COLD_FIX_MAX_SEC` | 180 | The patient cold-fix window (your "3 minutes"), no early abort. |
| `COLD_AFTER_NOFIX` | 1 | This many consecutive no-fix cycles → next fix is COLD (1 = the first miss). |
| `EPHEMERIS_STALE_HOURS` | 12 | A fix older than this → COLD. **Keep it > `GNSS_PERIOD_MIN/60`** so normal wakes stay warm. |

(v13 knobs, `NO_SKY_ABORT_SEC=45` (was 25 — tuned after the overnight run), `FIX_MAX_SEC=120`, `GNSS_PERIOD_MIN`,
`ACCEL_PERIOD_HOURS`, `SIMULATE_WUR_HOURS`, `ACCEL_RING_RECORDS`, …)

> **Want it *literally* ≥3 min every cold fix even under no sky?** It already is —
> COLD disables the early abort, so a cold fix runs the full `COLD_FIX_MAX_SEC` unless
> it locks sooner (it returns immediately on a fix). Raise `COLD_FIX_MAX_SEC` for a
> longer window.

## Flash schema note

`FlashHeader` gained `lastFixUnix`, so **`FLASH_SCHEMA_VER` was bumped 2 → 3**. The
first v14 boot on a device previously running v13 does a one-time `[FLASH] Fresh init
(schema v3)` — LoRa `nextSeq` and any pending fixes reset (expected on a firmware
upgrade). The accel ring (separate region) is unaffected.

## What to check on the bench

1. Boot prints `[CFG-GPS] warm=no-sky<25s/max<120s  COLD=<180s (no early abort) …
   lastFix=NONE (first fix will be COLD)`.
2. **First fix is COLD:** the first COLLECT logs `[GPS] COLD-fix budget (no-sky<180s,
   max<180s) reason=power-lost/first …` and the GPS stays on patiently (not a 25 s
   abort).
3. After a real fix, later wakes log `[GPS] warm-fix budget (no-sky<25s, max<120s) …`.
4. **New-city test (real GPS):** get a fix, then either (a) power-cycle the board
   (→ `reason=power-lost/first`), or (b) block the sky for ≥`COLD_AFTER_NOFIX` cycles
   then re-expose it (→ `reason=no-fix-streak`), or (c) leave it unfixed > 12 h
   (→ `reason=stale/new-city`) — each should show the COLD budget on the next attempt.
5. Everything else behaves exactly like v13 (accel timer, drone-pass offload, no freeze).

## Known / deferred (unchanged)

- **Keep the MS621FE GPS backup cell charged** — with it, most wakes are warm ~30 s
  starts and only the true first-ever fix is cold. A flat cell makes *every* wake cold.
- Real AS3933 WUR wake; accel-module switchable rail; battery-only headless power floor.
