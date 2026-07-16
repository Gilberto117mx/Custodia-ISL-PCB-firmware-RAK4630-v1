# ISL Board — Production Firmware **v6** (GNSS field strategy)

v6 turns the v3→v5 field findings (`../../docs/GNSS_FieldStrategy.md`) into
behaviour. It keeps everything validated in v1–v5 (GPS teardown/isolation,
calibrated battery `raw×1795/1000`, alive-first structure, LoRa P2P + ACK, flash
persistence, deep sleep, the `SV=N` diagnostic) and changes **four** things.

## What changed vs v5

### A — SV-gated adaptive GPS timeout (the centerpiece)
The GPS window is no longer fixed. `gpsAcquire()` watches **satellites-in-view**
live:
- **Fix** (hot ~7 s / warm ~30 s) → return immediately.
- **Fewer than `SV_MIN` sats by `NO_SKY_ABORT_SEC`** → **abort** (no sky) and
  send a timestamp-only packet. Saves the ~60–100 s of ~45 mA that a blind cycle
  used to burn (dens, canopy, the sealed-transport window).
- **`SV_MIN`+ sats visible but no fix yet** → **extend** to `FIX_MAX_SEC` — the
  satellites are up, so the wait is worth it.

### B — No-sky backoff
After **`NOFIX_BACKOFF_AFTER` (K)** consecutive no-fix cycles the node stretches
its cadence to **`BACKOFF_PERIOD_HOURS`** (e.g. 2 h → 6 h), turning a long no-sky
stretch into a few short probes instead of dozens of wasted GPS bursts. It snaps
straight back to `GNSS_PERIOD_MIN` on the **first real fix**. Both knobs are at
the very top of the sketch with plain-language comments.

### C — RTC re-sync on every real fix (fixes the 11-day-behind bug)
v5 synced the clock **once** and could seed it from the module's unreliable
backup RTC when there was no position fix (that's what put it ~11 days behind).
v6 re-syncs the RV-3028 from the **GPS UTC of every real position fix** — a
position fix means the time is genuine satellite time. The packet timestamp on a
fix cycle is taken **directly from that fix**; no-fix cycles extrapolate from
this GNSS-disciplined clock. **Every timestamp is GNSS-derived**, and RV-3028
drift is corrected for free on each fix. (The hardened two-sample time-only seed
is kept only to give a plausible clock *before the first-ever fix*.)

### #5 — Successful fixes are never abandoned (delivery guarantee)
A real fix that gets **no ACK is not dropped** — it is retained in `pending` and
re-sent on later cycles. Ordering follows *"newest matters most"*:
1. Send the **freshly-collected** packet first.
2. **Only if it ACKs** (link proven up) do we drain the older un-ACK'd **real
   fixes**, newest → oldest, each **`TX_PULSE_GAP_SEC` (≥30 s) apart** so one
   packet's ACK window can't collide with the next TX. Stop at the first miss
   (link dropped) — the rest stay pending.
3. If the **freshest packet itself** fails to ACK, the link is down: we don't
   waste energy blasting the backlog, we just retain everything for next cycle.

So your example works exactly as described: cycle 1 fix gets no ACK → held; move
to cycle 2; if cycle 2 ACKs, cycle 1's fix is then sent (30 s later). No-fix
heartbeats are best-effort (sent to test/keep the link, never retained). If the
backlog outgrows `PENDING_SLOTS`, the **oldest** fix rolls off to the flash
undelivered archive (a physical bound; newest are always kept).

**D (low-battery GPS lockout) is intentionally omitted** — the LiSOCl₂ primary
cell has a near-flat discharge curve, so a voltage threshold can't distinguish
"healthy" from "nearly dead." Strategy A already bounds wasted GPS energy, and
the MS621FE GPS backup is rechargeable (covered by A + "fix ASAP at sealing").

## Also fixed
- **Flash layout overlap (latent since v1).** `buffer[3]` (72 B at 0x40) ran into
  the pending region at 0x80. v6 widens the offsets (buffer→0x40, pending→0x100,
  undelivered→0x200) and enlarges `pending` to `PENDING_SLOTS`. `FLASH_SCHEMA_VER`
  is bumped to **2**, so an old board re-inits its flash cleanly on first boot.
- Removed the v4/v5 intra-collect GPS retry-with-sleep and the multi-sleep TX
  retry loop — strategy A (adaptive within one session) and B (macro backoff)
  replace them, and #5 carries un-ACK'd fixes forward across cycles.

## Knobs (top of the sketch)
| Knob | Meaning | Default |
|---|---|---|
| `GNSS_PERIOD_MIN` | normal deep-sleep between cycles | 5 (bench; deploy ~120 = 2 h) |
| `SV_MIN` | sats-in-view that make the long wait worth it | 4 |
| `NO_SKY_ABORT_SEC` | abort if `<SV_MIN` sats seen by here (no sky) | 25 |
| `FIX_MAX_SEC` | hard cap once sats **are** visible | 120 |
| `NOFIX_BACKOFF_AFTER` (**K**) | consecutive no-fix cycles before backoff | 3 |
| `BACKOFF_PERIOD_HOURS` | stretched cadence while backed off | 6 |
| `TX_PULSE_GAP_SEC` | gap between backlog TX pulses | 30 |
| `PENDING_SLOTS` | un-ACK'd real fixes kept for retry | 5 |

## Bench test hooks (default OFF — a deployment build never touches them)
Indoors there is no fix, so two of v6's features can't fire on their own. Two
opt-in hooks let you validate them on the bench; both are at the top of the sketch.

| Hook | What it does |
|---|---|
| `BACKOFF_BENCH_MIN` | Override strategy B's stretched cadence with a value in **minutes** (0 = use `BACKOFF_PERIOD_HOURS`). Set e.g. `3` so backoff is testable in minutes, not hours. |
| `SIMULATE_FIX` | Fabricate a canned GPS fix without powering the GPS, so the **delivery guarantee (#5)** and **strategy-C re-sync** can be exercised with no sky. On the first sim fix the RTC is seeded from `SIM_UTC_*` (you'll see `[RTC] SET FROM GPS`); toggle the receiver on/off between cycles to watch un-ACK'd fixes held in `pending` and re-sent newest-first, ≥30 s apart. Set to `0` for any real GPS test. |

> **⚠ Bench tip.** With the hooks OFF and no sky, after **K** no-fix cycles the
> node sleeps the (possibly multi-hour) backoff period and looks *frozen* — raise
> `NOFIX_BACKOFF_AFTER` or set `BACKOFF_BENCH_MIN`. The `SV=N` field in each packet
> still tells you headless whether a no-fix is sky or hardware.

## What to watch when you test
- **Outdoors:** first fix hot-starts in seconds → `[GPS] FIX ... IN-VIEW peak=NN`;
  packet timestamp should decode to the **real** UTC (strategy C), and the
  receiver ACKs. Leave the receiver **off** for a cycle to prove #5: the fix goes
  to `pending` and is re-sent (30 s after a later ACK).
- **Indoors / no sky:** `[GPS] no fix ... (no-sky abort)` after ~25 s (not 90–120
  s), then backoff after K cycles — this is the battery-saving behaviour working.

## Field/bench tests run (see `tests/`)
| Test | What it exercised | Result |
|---|---|---|
| `tests/test1_adaptive_indoor` | overnight, 70 cycles: strategy C (real-UTC re-sync, 11-day bug gone), strategy A **extend** branch, P0.21 wake 70/70, TX/ACK incl. a correctly-dropped no-fix ACK-loss | **PASS** |
| `tests/test2_delivery_guarantee` | `SIMULATE_FIX`, receiver off→on: hold un-ACK'd fixes, conserve on a dead link, overflow oldest→archive, **newest-first drain ≥30 s apart**, persistence across reboot | **PASS** |
| `tests/test3_longwake_2h` | 2 h cadence (`SIMULATE_FIX`): **1/60 Hz tick** auto-selected, **RTC-INT wake on P0.21 at +0.02 %** (7201.6 s vs 7200 s, not the backstop), clock held across sleep. 2 cycles (full-night postponed) | **PASS** |

Still untested: **no-sky abort** (needs a genuine `SV < 4` spot — Test 1 always
had ≥4 sats near the window), **strategy B backoff** (was disabled, `K=999`), a
**headless battery-floor run** (all runs USB-attached), and a longer multi-wake
2 h confirmation.

## Status
Current field-strategy build; supersedes v5. Core behaviours (A-extend, C, #5)
validated on hardware. Remaining before deployment: no-sky abort + backoff
confirmation, the retention-depth question (`PENDING_SLOTS` vs. undelivered
replay), the robustness checklist (brownout/blackout, >68 min/1-60 Hz cadence,
headless battery-floor run), and the AS3933 WUR real-wake test.
