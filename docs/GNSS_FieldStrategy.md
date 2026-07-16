# ISL Board — GNSS Field Findings & v6 Strategy

Consolidated from the v3→v5 field runs (indoor + outdoor). Basis for **production
v6**. Deployment context: a **sealed animal collar**, GNSS roughly **every 2 h**,
that must survive **~24 h of transport with no clear sky view** before deployment,
and long stretches of poor sky (dens, dense canopy) in the field.

## What we proved

| # | Finding | Evidence |
|---|---------|----------|
| 1 | **GNSS hardware + antenna are excellent** | Outdoor raw NMEA: 3D fix, **12 sats used / ~19 in view, SNR 33–42 dB-Hz, HDOP 2.0**, real position (`tests/ISL_GNSS_Serial0/outdoor_nmea_capture.txt`). |
| 2 | **Warm/hot start is fast** | v5 outdoors: **FIX in 6977 ms**, `SV=19` (`production/v5/logs`). |
| 3 | **The "no fix" was cold-start × power-cycling** | v4 outdoors: 0 fixes in ~20×60 s power-cycled windows, but the *continuously*-powered monitor fixed. Cold acquisition (initial almanac/ephemeris) needs a **continuous** stretch the 60 s power-cycled windows never gave it. |
| 4 | **Time-seed (hardened) works in the field** | v4 receiver timestamps decode to real **2026-07-14**; garbage (`2066`, `2000`) rejected by the range + two-sample guard. |
| 5 | **Real battery sleep floor holds in the field** | v4 outdoors (headless): **161–168 µA**. |
| 6 | **BUG: RTC sync-once → stale clock** | v5 boot `current: 2026-07-03`, packet ts ≈ Jul 4, real = Jul 14 → **~11 days behind**. Sync-once + seeding from the module's unreliable backup-RTC time when there's no fix. |

**Bottom line:** the subsystem works; the open problem is a *power/timing policy* —
get a fix cheaply when there's sky, and **don't burn battery when there isn't** —
plus disciplining the clock.

## Cold vs warm vs hot (why the window can't just be "longer")
| Start | Module has | TTFF (open sky) | Cost |
|---|---|---|---|
| **Hot** | position + time + fresh ephemeris (<~2 h) | ~1–7 s (measured 7 s) | tiny |
| **Warm** | almanac + position, ephemeris stale | ~30 s (ephemeris download) | moderate |
| **Cold** | nothing | ~30 s → **minutes** (almanac download) | large / battery risk |

Almanac is valid **weeks**; ephemeris only **~2–4 h**. So after the 24 h blind
transport the deployment fix is at worst a **warm start (~30 s)** — *iff* the
module's backup keeps the almanac. If the backup drains, it's a full cold start
(the exact failure we hit). **Open hardware question:** L76X backup type
(rechargeable vs supercap) and hold-time with main power off.

## Proposed v6 strategy (layers, each a tunable knob)

### A — SV-gated adaptive GPS timeout (centerpiece)
Watch satellites-in-view live instead of a fixed window:
- Fix (hot ~7 s / warm ~30 s) → done.
- **`SV≈0` after `NO_SKY_ABORT_SEC` (~20–25 s) → abort**, timestamp-only packet
  (saves ~60–100 s of ~45 mA per blind cycle).
- **`SV≥SV_MIN` but no fix → extend** to `FIX_MAX_SEC` (~90–120 s): satellites are
  visible, the wait is worth it.

### B — No-sky backoff (24 h transport / dens)
After `NOFIX_BACKOFF_CYCLES` consecutive no-fix cycles, **stretch the GPS cadence**
(e.g. 2 h → 6–12 h) or skip GPS (still send a timestamp/battery heartbeat). Snap
back to normal cadence on the first fix. Turns ~12 wasted transport sessions into
~3 short probes.

### C — RTC re-sync on every real position fix
Change "sync once" → **re-sync whenever `hasFix` is true** (a position fix ⇒ the
RMC/GGA time is genuine GPS time, not the module's backup RTC). Fixes the 11-day
bug, corrects RV-3028 drift for free, never seeds garbage. Keep the sane-range +
two-sample guard.

### D — Low-battery GPS lockout
Below `LOW_VBAT_LOCKOUT_MV` (~3.2–3.4 V), **skip GPS** (timestamp + battery packet
only) to avoid the 40–100 mA burst browning out a dying cell.

### E — Programming/sealing procedure
At sealing, hold GPS until a **solid fix** (fresh almanac + ephemeris in the
backup). Almanac then covers the 24 h transport. Optionally top up the backup by
occasionally keeping GPS on a little longer. Resolve the backup-hold-time question.

## Battery impact (2 h cadence, one blind/no-sky day)
| Policy | Wasted GPS energy/day |
|---|---|
| Fixed 120 s windows | ~18 mAh |
| SV-gated abort (~20 s) | ~3 mAh |
| SV-gate + backoff | < 1 mAh |
(Sleep floor itself ≈ 3.7 mAh/day.) → **~5–15× less** wasted GPS energy in no-sky.

## v6 recommended default
**A + C** are must-haves; **B + D** are the sealed-collar survival layer; **E** is
procedure + the one hardware question. Knobs: `SV_MIN`, `NO_SKY_ABORT_SEC`,
`FIX_MAX_SEC`, `NOFIX_BACKOFF_CYCLES`, `LOW_VBAT_LOCKOUT_MV`.

## What shipped in production v6 (decisions)
- **A — implemented.** SV-gated adaptive timeout (`SV_MIN`, `NO_SKY_ABORT_SEC`,
  `FIX_MAX_SEC`).
- **B — implemented**, knobs `NOFIX_BACKOFF_AFTER` (K) and `BACKOFF_PERIOD_HOURS`.
- **C — implemented.** Re-sync the RTC from **every real position fix**; the
  packet timestamp on a fix cycle comes straight from that fix ⇒ **every timestamp
  is GNSS-derived**. No-fix cycles extrapolate from the GNSS-disciplined RTC.
- **D — dropped.** The LiSOCl₂ primary has a near-flat discharge curve, so a
  voltage threshold can't tell "healthy" from "nearly dead." A already bounds the
  wasted GPS energy.
- **E / backup battery** — the GPS backup is a rechargeable **MS621FE**; rather
  than depend on a hold-time spec, the field procedure is *"take a fix as soon as
  possible after sealing"* and let A absorb the occasional cold start.
- **Delivery guarantee (added on top of A–E).** A successful fix that misses its
  ACK is **never abandoned**: it's retained in `pending` and re-sent newest-first,
  ≥30 s apart (`TX_PULSE_GAP_SEC`), and only when the freshest packet's ACK proves
  the link is up. See `production/v6/`.
