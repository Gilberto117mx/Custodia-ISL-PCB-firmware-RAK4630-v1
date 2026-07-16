# v6 Test 2 — delivery guarantee (#5) + newest-first backlog drain (SIMULATE_FIX)

Bench validation of the never-abandon delivery guarantee using `SIMULATE_FIX=1`
(fabricated fixes, no sky needed) with the receiver toggled **off → on**. Clean
pass: real fixes are held when un-ACK'd, the oldest overflow to the archive, and
when the link returns the backlog drains **newest-first, ≥30 s apart**.

## As-run configuration
```
SIMULATE_FIX        = 1      // fabricate a canned fix each cycle (no GPS powered)
GNSS_PERIOD_MIN     = 2      // ~2-min cycles (time to toggle the receiver)
TX_PULSE_GAP_SEC    = 30
NOFIX_BACKOFF_AFTER = 999    // backoff out of the way
PENDING_SLOTS       = 5      // (built-in) newest un-ACK'd fixes kept for retry
```
Note: flash is **non-volatile**, so the RTC reset at power-up but the flash state
did **not** — the run inherited `nextSeq=77 delivered=69 undelivered=1 pend=5`
from the tail of Test 1 (5 real fixes left un-ACK'd there). That carry-over is
itself a useful result: **un-ACK'd fixes survive a reboot** and stay in the queue.

## Files
| File | What it is |
|---|---|
| `Node_test2.txt` | ISL node serial — the off/on toggle, backlog drain, `[GAP]` naps |
| `Receiver_test2.txt`| Receiver serial — confirms the newest-first arrival order |
| `PowerProfile_test2.csv` | Current trace, **10 samples/s** (see caveat) |

## What validated ✅
- **Fixes held, never abandoned.** Receiver OFF for cycles 1-8: every cycle sent
  only the newest packet, got `RECEIVE TIMEOUT → not-ACK`, and kept the fix.
- **"Don't blast a dead link."** While OFF it made **one TX per cycle** and did
  **not** try to drain the backlog — energy conserved when the link is down.
- **Overflow → archive.** With `pending` already full at 5, each new fix pushed
  the **oldest** off to the `undelivered` flash archive (`undelivered` 1→9).
- **Newest-first drain, 30 s apart (the headline).** Receiver ON at cycle 9:
  ```
  [TX] seq=85 → ACK OK
  [GAP] 30 s at floor → seq=84 → ACK
  [GAP] 30 s → seq=83 → ACK
  [GAP] 30 s → seq=82 → ACK
  [GAP] 30 s → seq=81 → ACK
  [GAP] 30 s → seq=80 → ACK
  [TX pass] done: delivered=75 lastSeq=80 pending=0
  ```
  The **receiver log confirms the same order** (85, 84, 83, 82, 81, 80).
- **Strategy C.** `bootTimeSync` grabbed **real** GPS time (`2026-07-15 08:16:10`),
  so timestamps are genuine and the canned SIM seed was correctly bypassed.
- **`napSleep` during the gaps.** The 30 s `[GAP]` waits sit at the sleep floor —
  power trace p90 = 1.78 mA (USB floor), i.e. the gaps don't burn awake current.
- **Sleep/wake.** P0.21 RTC wake solid: `[WAKE] RTC P0.21 after ~120002 ms`.

## Known bound (flagged for later — no code change yet)
Retention is bounded to the **newest `PENDING_SLOTS` (5)** fixes. In this stress
run the outage lasted long enough that **9 older fixes rolled into the
`undelivered` archive, which is currently fire-and-forget (never re-transmitted)**.
So a receiver outage longer than ~`PENDING_SLOTS` cycles drops the oldest fixes
from the over-air guarantee (they remain saved in flash for post-mortem, but are
not auto-resent). At a 2 h cadence that's a ~10 h outage window before the oldest
starts rolling off. If a longer guarantee is wanted: raise `PENDING_SLOTS`, or add
an "undelivered replay" drain after `pending`. Deferred pending evaluation.

## Power-profile caveat (same as Test 1)
CSV logged at **10 samples/s**, which under-samples the LoRa TX burst. This run
happened to catch one TX peak at **~102 mA** — corroborating that the **true TX
draw is ~90 mA** (the ~40 mA seen in Test 1 was a sampling artifact). Run was
**USB-attached**, so the ~1.78 mA floor is the VBUS artifact, not the ~155 µA
battery floor; not usable for battery-life estimates. Observed (10 sps): floor
≈ 1.78 mA (USB, incl. the 30 s gaps), TX peak (caught) ≈ 102 mA.

## Bottom line
The #5 delivery guarantee is fully validated: hold on no-ACK, conserve on a dead
link, overflow the oldest to archive, and drain newest-first with 30 s spacing on
link recovery — plus persistence of un-ACK'd fixes across a reboot. The only open
design question is the retention depth (`PENDING_SLOTS`) vs. archive replay.
