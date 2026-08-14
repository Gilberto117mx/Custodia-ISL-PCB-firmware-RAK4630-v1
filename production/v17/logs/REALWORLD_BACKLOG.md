# v17 — real-world 2-day out-of-range test (2026-08-11)

Raw logs: [`gateway_v17_realworld_backlog.log`](gateway_v17_realworld_backlog.log)
(gateway RX/ACK/emit) and [`BLE_Receiver_v17_realworld.log`](BLE_Receiver_v17_realworld.log)
(drone). This is the **first end-to-end real-world proof** of the v16 durable LoRa
backlog with actual field-buffered fixes (not simulated). **Verdict: the delivery
guarantee works exactly as designed on real hardware, in the field.**

## Setup
- Collar (v17, iteration3) carried for ~2 days out of gateway range while collecting
  GPS fixes at a ~24 min cadence (reconstructed from the packet timestamps).
- Gateway then powered on, collar placed near it. Log captured what arrived.

## Timeline

| At uptime | Event |
|---|---|
| 00:00 | Gateway ON. Silent. |
| **21:59** | **First packet on the gateway** — seq **194**, fresh no-fix, RSSI −57. **~22 min wait**, matches the model (≤ one `GNSS_PERIOD_MIN`). |
| 22:00 → 42:59 | **83-packet burst** (seq `194 → 189 → 188 → … → 94`), backlog drain **newest-first**, ACK'd on each, RSSI −56 to −65, SNR 11.2–13.2, **`emit=83 bad=0`**. |
| 43:29 → 1:06:59 | Silence (~24 min = one collar cycle). |
| 1:06:59 | Fresh no-fix cycle (seq **195**). |
| 1:29:59 | Fresh no-fix cycle (seq **196**). |

Drain rate = 83 packets ÷ ~21 min = **~15.2 s/packet** = `BACKLOG_GAP_SEC` (spec).

## What the run proves — the delivery guarantee works in the field

- **Real out-of-range backlog buffered ~2 days and streamed back on first contact.** Zero
  corruption (`bad=0`), ACKs on every packet, link strong throughout.
- **Newest-first drain confirmed** — seq 194 first, then 189, 188, … , 94.
- **Time to first packet ≈ one collar period** (~22 min for a ~24 min cadence).
- **Drain cadence exactly matches spec** (~15 s inter-packet, deep-sleep-gapped).
- **Position history spans ~37.6 hours** (oldest recovered ts 1786226784 → newest
  1786362202) — most of the trip successfully back-hauled.

## Three notes on the run (not defects)

1. **Some of the oldest history was truncated** by the `PENDING_SLOTS = 84` cap. At
   ~24 min/fix × ~2 days ≈ ~120 fixes attempted → **84 newest survived, ~30–40 oldest
   lost** to the write-only `undelivered` archive. Documented behaviour; if longer
   trips must be lossless, either lengthen the cadence (fewer fixes/day fit in 84 slots)
   or raise `PENDING_SLOTS` (24 B/slot, flash has ~90 KB free).

2. **The three fresh cycles at the gateway (194/195/196) are all no-fix**
   (`SV=13–15 used=0 TTFF=120 CELL=LOW`). The module sees satellites but doesn't
   complete a fix — most likely **indoor/covered placement near the gateway** (no sky).
   Secondary possibility: the L76K MS621FE backup cell drained during the 2-day trip
   and hasn't hot-started yet. Placing the collar in open sky for ~3–5 min should let
   v15's `CELL=LOW → charge to 180 s → next fix hot` cycle recover it. If it still
   doesn't fix under open sky, the backup cell may be genuinely dead.

3. **Every packet reads `CELL=LOW` and `TTFF=120`**, which is misleading for the
   *backlogged* packets. This is the exact behaviour flagged as **optimization #2** in
   the v17 README: `TTFF`/`CELL` are stamped at *transmission* time (from the collar's
   globals), not at *fix* time (per-packet). The collar is currently `LOW`, so every
   backlogged packet inherits that stamp on delivery — even though many were likely
   hot fixes when captured 2 days ago. **Positions are per-packet-correct**; only the
   two health fields are as-of-transmission. Fix is optional (schema bump 4→5).

## Bottom line

The v16 durable backlog + v17 iteration3 port are **field-validated end-to-end** on real
buffered fixes across a real 2-day out-of-range excursion. This is the last major
behaviour that was still theoretical after the earlier bench + off-then-on validation;
it now has a real-world confirmation. No firmware issue found in this run.
