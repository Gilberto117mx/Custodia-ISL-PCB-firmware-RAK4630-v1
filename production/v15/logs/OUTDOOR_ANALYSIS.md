# v15 outdoor field run — analysis (2026-08-02, SIM_FIX=0, real sky)

Raw logs: [`Tracker_v15_outdoor.txt`](Tracker_v15_outdoor.txt) (collar) and
[`BLE_Receiver_v15_outdoor.txt`](BLE_Receiver_v15_outdoor.txt) (drone).
This is the **first run that actually exercises the v15 feature** — the earlier
bench run was `SIM_FIX=1`, which hard-codes `TTFF=1,CELL=OK`. Here `SIM_FIX=0`,
so real GPS acquisition drives the real TTFF → HOT/LOW/DEAD classifier and the
charge-on-cold hold loop. **Verdict: the self-charging cell strategy works in the
field, precisely as designed.**

## Headline: the cell self-healed via the charge-hold

A previously-idle module started with a **drained backup cell** (long cold start),
and the patient charge window recharged it into **hot starts** within two cycles:

| seq | budget | TTFF | SV used/in-view | CELL | coldStreak | gps-on | what happened |
|----:|--------|-----:|:---------------:|:----:|:----------:|:------:|---------------|
| 10 | **COLD** `reason=stale/new-city` | **110 s** | 5 / 6 | **LOW** | 1 | 180 s | flat cell → cold fix; held GPS on to 180 s to charge |
| 11 | warm | **35 s** | 5 / 6 | **LOW** | 1\* | 180 s | still slow → charged again (\*reset by the reboot between, see note 1) |
| 12 | warm | **14 s** | 11 / 12 | **OK** | 0 | 14 s | **HOT — cell recovered**, powered off in 14 s |
| 13 | warm | 9 s | 7 / 12 | OK | 0 | 9 s | hot |
| 14 | warm | 9 s | 7 / 12 | OK | 0 | 8 s | hot |
| 15 | warm | 11 s | 7 / 12 | OK | 0 | 11 s | hot |
| 16 | warm | 11 s | 8 / 12 | OK | 0 | 10 s | hot |
| 17 | warm | 12 s | 7 / 12 | OK | 0 | 12 s | hot |

The arc **110 s → 35 s → 14 s → 9 s (and stays hot)** is the whole v15 thesis
proven on real hardware: a low cell is detected from a slow TTFF, the patient
window doubles as the charge, and after ~two 180 s holds the cell holds ephemeris
and every subsequent fix is a hot start. Sats-in-view also jumps 6 → 12 once the
cell is charged (module has ephemeris to work with). Each packet's `TTFF=`/`CELL=`
telemetry matched the measured behaviour exactly, e.g.:

```
051,10,22.530686,113.937077,3.99,1785656444,SV=6,TTFF=110,CELL=LOW
051,12,22.531256,113.937790,3.97,1785657271,SV=12,TTFF=14,CELL=OK
```

The COLD-fix budget also earned its keep: the first fix took **110 s**, which a
warm 45 s no-sky abort would have killed. Because it was classified COLD
(`reason=stale/new-city`, patient 180 s, no early abort) it rode it out and fixed.

## LoRa delivery guarantee — repeater absent 33 min, then full backfill

The LoRa peer (repeater/receiver) was **not in range / not powered until ~16:11**.
seq 10–16 all got `RXP2P RECEIVE TIMEOUT` (no ACK). The delivery guarantee behaved
exactly as specified:

- Un-ACK'd fixes were held as **pending**; the queue grew 1 → 5 and then capped at
  `pend_slots=5`. The two oldest (**seq 10, 11**) overflowed → `undelivered` 1 → 2.
- At **16:11** seq 17 got `ACK OK RSSI=-48`. The guarantee then drained the pending
  ring newest-first — seq 17, 16, 15, 14, 13, 12 — each ACK'd, 30 s apart.
  `delivered 9 → 15`, `pending 0`, `undelivered 2`.

So of the 8 position fixes taken while the link was down, **6 were recovered** and
only the **2 oldest were lost** (buffer overflow, by design). Once the peer was up,
RSSI −47…−54 dBm / SNR 12–14 dB — a strong link. **The ACK failures were the peer
being absent, not a radio-config fault.**

## BLE drone offload — 7 records, 0 bad, no freeze

6 drone passes, all clean (connect → blast ×2 → ring cleared → reboot-to-sleep,
never a freeze):

| pass | records | seqs | result |
|------|:-------:|------|--------|
| 15:36 | 1 | 13 | OK (leftover from the prior indoor run — ts `1784059995`, July 14; the flash ring survived the reprogram) |
| 15:44 | 1 | 14 | OK |
| 15:51 | 1 | 15 | OK |
| 15:59 | 1 | 16 | OK |
| 16:08 | 2 | 17, 18 | OK |
| 16:16 | 1 | 19 | OK |

Receiver totals: **new=7, bad=0**, checksum match on every record; the ×2 blast's
duplicates were correctly skipped every pass (`dup=7`). `clear_after_send` held —
each pass carried only records collected since the previous pass.

## Everything else intact (no regression)

- **RTC ← real GPS:** seeded at boot from live sky (`SET FROM GPS 2026-08-02
  07:36:33 UTC`); all packet timestamps are genuine.
- **Flash persistence** across all 6 reboots: `nextSeq`/`delivered`/`pending`/ring
  all carried correctly (incl. the seq-13 leftover record).
- **Two independent timers:** accel collected on its own 180 s cadence
  (seq 14/15/16/17/18/19), *not* every GNSS cycle — cycles at 15:56 and 16:04 fixed
  but did not collect. Decoupling from v13 preserved.
- **Battery:** 3975–3995 mV, stable/climbing across two 180 s charge holds — the
  9 Ah 26500 absorbs the charge-on-cold cost with no droop.

## Two things to consider (design notes, not bugs)

1. **`coldStreak` is RAM, so DEAD can't accumulate across a drone-pass reboot.**
   Each reboot-to-sleep (a drone pass) resets `coldStreak` to 0 — that's why both
   LOW fixes here read `coldStreak=1`. In the real deployment (drone passes rare,
   many fixes between them) the streak accumulates across fixes and `CELL=DEAD`
   would fire correctly; but a cell that only ever looks bad right after a pass
   could keep charging 180 s forever without ever being flagged DEAD. If we want
   the DEAD verdict to be robust regardless of pass frequency, **persist
   `coldStreak` (and `lastTTFF`) in the flash header.** Recommended for a future rev.
   (Already listed under "Known / deferred" in the v15 README.)
2. **`pend_slots=5` bounded the position backlog.** With the repeater absent 33 min
   at a 2-min cadence, only the last 5 fixes survived; seq 10/11 were lost. Position
   is secondary here (the accel ring is 64 deep and was fully delivered over BLE),
   and a real fixed repeater wouldn't be absent that long — but if long repeater
   outages are expected, bump the pending buffer. No change made now.

## Bottom line

v15 is **field-validated.** The new feature (charge-on-cold + TTFF/CELL health
telemetry) did exactly what it was built to do — a drained cell was detected and
self-charged into hot starts — with **zero regression** in the proven v14/v13/v12
behaviour (BLE offload 0-bad/no-freeze, LoRa delivery guarantee, two-timer accel,
real-GPS RTC, flash persistence). Ready to proceed.
