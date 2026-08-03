# ISL Board — Production firmware v13 (COLLAR, two timers + per-record timestamp)

**v13 = v12 + two additions:** (1) **two independent timers** — accel collection now
has its own cadence, separate from the drone pass; and (2) the **per-record
timestamp** is back on the BLE wire. Everything else — the proven, no-freeze
`api.ble.uart` fire-and-forget transport, the short single-notification wire format,
the x2 blast, new-data-per-pass (`clear-after-send`), GNSS A/B/C, LoRa, 34 µA sleep,
reboot-to-sleep — is **identical to v12** (bench-verified: new-data-per-pass, 35
records, 0 bad / 0 corrupt — see `../v12/`).

> ✅ **Status: BENCH-VERIFIED (2026-07-29, `logs/`).** Accel collected on its own
> 324 s timer (not every GNSS cycle); each drone pass sent everything since the last
> pass and cleared; per-record `ts` cross-checked exactly with the collar; `bad=0`, no
> cross-pass dup, collar never froze. The LoRa repeater being off for the first ~3
> cycles just exercised the delivery guarantee (fixes held, then drained newest-first).
> See `logs/ANALYSIS.md`.

## ✅ Bench result — 2026-07-29 (`logs/`)

Run config (from the boot `[CFG-ACCEL]` line): `accel_collect_every=324 s`,
`drone_pass_every=720 s`, GNSS wake `180 s`, `ring=64`, `clear_after_send=1`,
`SIMULATE_FIX=1`.

**Feature 1 — two timers decoupled (confirmed).** Accel fired on its own 324 s
cadence, *not* every GNSS cycle:

| Collar cycle | fix ts | accel collected? |
|---|---|---|
| 1 | …9200 | ✅ `stored seq=1` (first record) |
| 2 / 3 / 4 | …9220 / …9244 / …9433 | — (< 324 s since last) |
| 5 | …9736 | ✅ `stored seq=2` (536 s ≥ 324) |
| 6 | …9928 | — |

Each drone pass (720 s) then offloaded **everything since the last pass** and cleared:
seq `{1,2} → {3,4} → {5,6} → {7,…}` — every pass new, `ring_now` back to 0 after each.

**Feature 2 — per-record timestamp (confirmed, cross-checked).** The receiver printed a
non-zero `ts` on every record, matching the collar's stored `ts` exactly:

| seq | collar `[RING] stored ts` | receiver `… OK` ts |
|---|---|---|
| 1 | 1784059200 | 1784059200 ✅ |
| 2 | 1784059736 | 1784059736 ✅ |
| 3 | 1784060310 | 1784060310 ✅ |
| 4 | 1784060683 | 1784060683 ✅ |

**Integrity & robustness.** Receiver totals climbed `new=2 → 4 → 6`, **`bad=0`**, no
cross-pass `dup` (the only dups are each pass's own 2nd redundancy blast). The collar
ran many cycles + drone passes, every one recovering via reboot-to-sleep — **no freeze.**

**LoRa outage handled correctly (not a fault).** The repeater was off for cycles 1–3:
`RECEIVE TIMEOUT → not-ACK`, so the **#5 delivery guarantee** *held* the un-ACK'd fixes
(`pending` 1→2→3→4, nothing lost). When it returned (cycle 4) the backlog drained
**newest-first** with the 30 s guard (seq 111,110,109,108,107 all ACK'd → `pending=0`),
then normal ACKs resumed. Position data fully intact.

**Note — the receiver does not list the raw `x,y,z` samples, by design.** All 100
samples per record *are* received; the reference receiver folds each into a running
16-bit checksum and prints only a summary (`seq=… ts=… OK (100 samples, checksum
match)`). `checksum match` proves every sample arrived. Dumping 100+ lines/record would
flood USB serial (and drop console lines, as the mild formatting artifacts in the log
show). To *see/store* the raw samples, add a print/append in the receiver's sample
branch — the data is already there, only the display is suppressed.

---

## Feature 1 — two independent timers (accel collection vs. drone pass)

Until v12 the accelerometer was collected **once per GNSS fix** — its cadence was
tied to the position cadence. v13 gives it **its own timer**, fully decoupled:

| Timer | Knob | What it controls |
|---|---|---|
| **Accel collection** | `ACCEL_PERIOD_HOURS` (v13, new) | How often a 10 s / 100-sample record is collected and appended to the flash ring. |
| **Drone pass** | `SIMULATE_WUR_HOURS` (real: AS3933 WUR) | How often the drone passes → offload the **whole** ring (everything since the last pass), then clear it. |
| *(Position)* | `GNSS_PERIOD_MIN` | The GNSS fix + LoRa cadence — the wake heartbeat (unchanged). |

**The behaviour you asked for:** say accel every 6 h → 4 records/day → ~40 records
over 10 days. When the drone passes on day 10, **all 40 are sent in that one pass**
(the drone waits for the full transfer — that's the BLE engineer's side; the collar
just streams it reliably, exactly as v11/v12 proved). In deployment `SIMULATE_WUR` is
replaced by the real WUR wake, but the accel timer is identical.

**How it works (implementation):** the accel timer is time-gated off the
GNSS-disciplined RTC via a new persisted `accelMeta.lastAccelUnix`. On each wake, if
`now − lastAccelUnix ≥ ACCEL_PERIOD`, a record is collected (on **fix or no-fix**
cycles alike) and the timer is re-armed. Because the check runs on GNSS wakes, keep
`ACCEL_PERIOD_HOURS` a whole multiple of the wake period (`GNSS_PERIOD_MIN/60`) for an
exact cadence — the record lands on the first wake at/after each accel-period
boundary. The `[CFG-ACCEL]` line printed at boot shows both timers and the ring size.

### Memory safety — the ring drops the oldest when full

The accel ring is a **bounded flash ring**: when it reaches `ACCEL_RING_RECORDS` it
**overwrites the oldest** record (`accelRingAppend`). So the device can **never**
exhaust flash — if a drone pass is missed long enough that more records accumulate
than the ring holds, the oldest simply roll off. **Size the ring for your deployment:**
it must cover the records between passes = `ceil(drone_interval / ACCEL_PERIOD)`.
2-week drone + 6 h accel = 56 records; the default is now **64** (with margin), and a
compile-time `static_assert` guarantees the ring fits the ~132 KB flash budget
(64 × 612 B ≈ 39 KB). `BLE_STREAM_MAX_MS` was also raised to 10 min so a *full* ring
still streams in one pass without being capped.

## Feature 2 — per-record timestamp on the wire

### Why: v11/v12 sent accel data with no absolute time

Each 10 s accel record already **stores** a unix timestamp — the collar logs
`[RING] stored seq=82 ts=1784059200 n=100`. But when v11 shortened every wire line to
≤ 20 B (the fix that made the offload reliable), it **dropped `ts` from the header**.
So v11/v12 delivered motion bursts ordered only by `seq`, with **no absolute time**,
and the accel-ring seq space is never cross-mapped to the LoRa-position seq space over
the air. For behaviour science each burst must be time-referenced — hence v13.

### How: `ts` as its own short line (keeps the reliability rule intact)

The rule that made v11/v12 reliable is **"one logical line ≤ 20 B = one NUS
notification"** (so a dropped packet loses a whole line, never corrupts its
neighbour). Rather than lengthen the header, v13 sends the timestamp on **its own
line**, right after the record header:

```
I <dev> <nrecs>          announce (DEVICE_ID once)
R <seq> <n> <ck16>       per-record header (seq, sample-count, 16-bit checksum)
T <ts>                   ← v13: this record's unix timestamp  ("T 1784059200" = 13 B)
<x>,<y>,<z>              a sample   × n
E <dev> <nrecs>          end
```

- `T <ts>` is ≤ 14 B → one notification. The reliability property is unchanged.
- It is sent **after `R` and before the samples**, so the receiver associates it with
  the current record. It is **not** part of the checksum.
- Cost: one extra line per record (~+22 ms/record) — negligible vs the 90 s stream
  cap. Everything else is byte-identical to v12.

The reference receiver parses `T` and now prints `seq=<seq> ts=<ts> OK (…)`. If a `T`
line is ever lost, that record simply reports `ts=0` (and can be retried next pass /
re-derived); nothing else is affected.

## Config knobs (top of `ISL_Production_v13.ino`)

| Knob | Default | Meaning |
|------|---------|---------|
| **`ACCEL_PERIOD_HOURS`** | **6.0** | **v13: accel-collection timer — how often a 10 s record is stored. Keep it a whole multiple of `GNSS_PERIOD_MIN/60`.** |
| `SIMULATE_WUR_HOURS` | 0.25 | Drone-pass timer: fake a pass every N h (0 = real AS3933 WUR on P1.04). |
| `OFFLOAD_DELETE_AFTER_SEND` | 1 | 1 = new data every pass (clear-after-send); 0 = v11 keep + dedup. |
| `ACCEL_RING_RECORDS` | 64 | Flash ring capacity. Size ≥ `ceil(drone_interval / ACCEL_PERIOD)`; drops oldest when full. |
| `DEVICE_ID` | 51 | Collar id, sent in the `I`/`E` lines (`051`). |
| `BLE_BLAST_REPEATS` | 2 | How many times the batch is sent per pass (loss resilience). |
| `BLE_SETTLE_MS` | 6000 | Fixed wait for the central to connect+subscribe before streaming. |
| `BLE_LINE_GAP_MS` | 22 | Per-line pacing (≥ conn interval). Lower it to shorten the fly-by window. |
| `BLE_STREAM_MAX_MS` | 600000 | Safety ceiling on the whole blast (fire-and-forget can't hang; this only caps a runaway). Covers a full 64-record ring ×2. |
| `GNSS_PERIOD_MIN` | 120 | GNSS fix + LoRa cadence = the wake heartbeat (set small for bench). |
| `SIMULATE_FIX` | 0 | 1 = fabricate fixes to run indoors. |

Fast bench recipe: `SIMULATE_FIX 1`, `GNSS_PERIOD_MIN 2`, `ACCEL_PERIOD_HOURS 0.1`
(≈ 6 min, i.e. every ~3rd wake), `SIMULATE_WUR_HOURS 0.3`, and a small
`ACCEL_RING_RECORDS` (e.g. 8) so the ring visibly fills and rolls over quickly.

## What to check on the bench

1. Collar boots `PRODUCTION v13`; `[CFG-ACCEL]` shows both timers + ring size.
2. **Accel on its own timer:** `== ACCEL collect … (accel timer, every N s) ==` fires
   on the accel period, *not* every GNSS cycle — some COLLECT cycles have no accel.
3. **Each pass sends everything since the last pass:** the drone announces however
   many records accumulated in that window, each validating with a **non-zero
   timestamp**: `seq=84 ts=1784060044 OK (100 samples, checksum match)`, `END … bad=0`.
4. **Ring overflow is safe:** with a small `ACCEL_RING_RECORDS`, once it's full the
   collar log shows the oldest slot being reused (`… -> slot X (N/N)`), and the device
   keeps running — no freeze, no flash exhaustion.
5. Cross-check: the `ts` the receiver prints for a record equals the `ts` the collar
   logged when it stored that same `seq`.
6. Same v12 signatures otherwise: all-new seqs across passes (no cross-pass dup),
   collar never freezes.

## Known / deferred (unchanged)

- **Real AS3933 WUR wake** pending the LF transmitter; `SIMULATE_WUR` stands in. On
  real WUR, add a fast-path (advertise ASAP, skip GNSS/LoRa) to fit the fly-by window.
- **Accel module sleep current** (~296 µA on 3V3) — hardware switchable rail on the
  next PCB (`ACCEL_PWR_PIN` hook ready).
- **On-collar retention/replay** — intentionally not here; owned by the other
  engineer's BLE approach.
- **True sleep-floor / endurance number** — must be measured **battery-only, headless**
  (over USB the floor reads a ~1.9 mA USB-CDC artifact, not 34 µA).
