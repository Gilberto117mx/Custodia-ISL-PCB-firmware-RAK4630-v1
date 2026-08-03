# ISL Board — Production firmware v16 (COLLAR, durable long-excursion LoRa backlog)

**v16 = v15 + a durable, week-deep LoRa position backlog for animals that roam out of
repeater range.** Everything in v15 (GPS backup-cell health + charge-on-cold, TTFF/CELL
telemetry) and below (cold/first-fix budget, two accel/BLE timers, per-record timestamp,
the proven no-freeze BLE offload, reboot-to-sleep) is **unchanged**. v16 only changes
**how many un-delivered position fixes the collar keeps**, where it stores them, and how
fast it replays them when the link returns.

> ✅ **Status: BENCH-VALIDATED (2026-08-03, repeater off→on).** The repeater was powered
> off to emulate an out-of-range excursion: `pending` climbed **1 → 42** (past v15's old cap
> of 5), survived **every drone-pass reboot**, and on reconnect the backlog drained
> **newest-first 42→1**, `delivered=42 pending=0 undelivered=0` — **all 42 fixes, zero
> loss**. The accel ring shared the flash concurrently with **0 bad** records. Full trace:
> [`logs/BACKLOG_TEST_ANALYSIS.md`](logs/BACKLOG_TEST_ANALYSIS.md). (Power capture was
> USB-attached, so its ~1.67 mA floor is an artifact, not sleep current — a clean
> battery-only run is the one remaining follow-up.)

## Why (the deployment reality)

At a **~2 h fix cadence**, animals **very often leave repeater range for days**. GPS keeps
fixing the whole time — that needs no repeater — but each un-ACK'd fix is held in the
`pending[]` backlog and only sent once the link is back. **v15 kept just 5 slots (~10 h),
then silently dropped the oldest into a write-only archive.** So a 2-day excursion (≈24
fixes) delivered only the last 5 on return and **lost ~19 positions**. Not robust for the
real animals.

**v16 sizes the backlog to a full week** so the whole excursion replays, losing nothing
until an outage exceeds a week:

| Out-of-range | Fixes taken | v15 (5 slots) delivered on return | **v16 (84 slots)** |
|---|---:|---:|---:|
| ≤ 10 h | ≤ 5 | all | **all** |
| 1 day | ~12 | last 5 (~10 h) | **all** |
| 2 days | ~24 | last 5 | **all** |
| 1 week | ~84 | last 5 | **all** |
| > 1 week | >84 | last 5 | last **84** (oldest overflow to the write-only archive) |

## How it works (mechanism is v15-identical, only bigger + relocated)

- **Depth:** `PENDING_SLOTS` **5 → 84** = 1 week of 2 h fixes.
- **Durable:** `pending[]` is written to flash every TX pass and reloaded at boot, so it
  **survives the drone-pass reboots** (already true in v15 — just deeper now). An animal
  can be out of range *and* get drone passes; the LoRa backlog rides through both.
- **Ordering unchanged:** on a pass, the freshest fix goes first; if it ACKs, the link is
  proven up and the backlog drains **newest → oldest**. So on return you learn **where the
  animal is now first**, and the historical trail fills in behind.
- **Interrupt-safe:** the drain **stops at the first miss** (link dropped) and keeps the
  rest; a partial catch-up simply **resumes on the next pass**. A brief pass through range
  progressively empties the backlog across passes.
- **Faster catch-up:** a week-deep backlog drains on a shorter `BACKLOG_GAP_SEC` (15 s)
  instead of the 30 s fresh-packet gap, so a full week replays in **minutes** (the gap is a
  deep-sleep nap, so the cost is wall-clock, **not battery**). Still well above the 8 s ACK
  window, so ACK windows can't collide.

## Shared flash — proven safe at compile time

The accel ring and the position backlog share the one ~132 KB RUI3 flash. v16 makes
non-collision a **compile-time invariant** (`static_assert`), so you can never silently
corrupt one with the other:

| Region | Base | Size | Ends |
|---|---|---|---|
| Metadata (header/buffer/undeliv/sleepcmd/accel-meta) | `0x0000` | ~1.3 KB | `0x0500` |
| **Accel ring** — 64 × 612 B (100 samples/rec) | `0x0500` | ~38.3 KB | `0x9E00` |
| **Position backlog** — 84 × 24 B | `0xA000` | ~2.0 KB | `0xA7E0` |
| Free | | **~90 KB** | |

```c
static_assert(accel_ring_end   <= FLASH_OFF_PENDING, "accel runs into the backlog");
static_assert(backlog_end       <= 130000,           "backlog exceeds the flash budget");
```

Grow `ACCEL_RING_RECORDS` (longer drone interval) or `PENDING_SLOTS` (longer excursion)
too far and **the build fails** rather than corrupting data at runtime. There is a 512 B
guard gap between the accel ring and the backlog today, and ~90 KB free overall.

## Config knobs (v16 additions / changes)

| Knob | Default | Meaning |
|------|---------|---------|
| `PENDING_SLOTS` | **84** | Un-ACK'd fixes retained for retry. 84 = 1 week @ 2 h. Raise for longer excursions (watch the budget assert). |
| `FLASH_OFF_PENDING` | `0xA000` | Flash base of the backlog, above the accel ring. |
| `BACKLOG_GAP_SEC` | 15 | Inter-packet gap while draining the backlog (deep-sleep nap; > 8 s ACK window). |
| `FLASH_SCHEMA_VER` | 4 | Bumped from 3 → one-time flash re-init on upgrade. |

(v15 knobs unchanged: `HOT_TTFF_SEC=15`, `GPS_CHARGE_SEC=180`, `CELL_DEAD_AFTER=4`; v14:
`COLD_FIX_MAX_SEC=180`, `NO_SKY_ABORT_SEC=45`, `COLD_AFTER_NOFIX=1`, `EPHEMERIS_STALE_HOURS=12`.)

## What to check on the bench

1. Boot prints `[CFG-BACKLOG] pending=84 fixes (~168 h out-of-range @ 120 min) held in
   flash @0xA000, drain gap=15 s newest-first`.
2. Simulate an outage: let several fixes fail to ACK (repeater off). `pending` should climb
   past 5 (where v15 capped), `[FLASH] Loaded: … pend=N` should persist N across the
   drone-pass reboots, and **no** early `undelivered` until N > 84.
3. Bring the repeater back: the freshest fix ACKs, then the backlog drains newest-first
   ~15 s apart until empty (or the link drops), `delivered` jumps by N.
4. Accel path and cell-health telemetry behave exactly as v15 (unchanged).

## Known / deferred (unchanged from v15)
- Real AS3933 WUR wake; accel-module switchable rail (~325 µA sleep floor).
- `coldStreak`/cell-health is RAM (resets on a reboot-to-sleep); persist it if you want the
  `CELL=DEAD` flag to survive reboots.
- The overflow (`undelivered`) archive is still **write-only** (forensic count only). If you
  ever need >1-week lossless retention, raise `PENDING_SLOTS` (flash has ~90 KB free — a
  month is ~360 fixes ≈ 8.6 KB) or make the archive re-transmittable.
