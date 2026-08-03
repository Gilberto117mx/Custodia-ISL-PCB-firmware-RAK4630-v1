# v13 bench run — 2026-07-29 (analysis)

Files: `Tracker_v13_run1.txt` (collar), `Receiver_v13_run1.txt` (reference drone).
Config (from the `[CFG]`/`[CFG-ACCEL]` lines): `SIMULATE_FIX=1`, `GNSS_PERIOD_MIN=3`
(180 s wake), **`accel_collect_every=324 s`**, **`drone_pass_every=720 s`**,
`ring=64`, `clear_after_send=1`.

## Verdict: ✅ both v13 features work

### 1. Two independent timers — CONFIRMED

Accel is collected on its **own** 324 s timer, NOT every GNSS cycle. Trace:

| Collar cycle | fix ts | accel? |
|---|---|---|
| 1 | …9200 | ✅ stored seq=1 (first, `lastAccelUnix==0`) |
| 2 | …9220 | — (only 20 s elapsed) |
| 3 | …9244 | — |
| 4 | …9433 | — (233 s < 324) |
| 5 | …9736 | ✅ stored seq=2 (536 s ≥ 324) |
| 6 | …9928 | — |

So some COLLECT cycles carry accel and some don't — exactly the decoupling we wanted.
The drone pass (every 720 s) then offloads **everything since the last pass** and
clears: seq {1,2} → {3,4} → {5,6} → {7,…}, every pass new. `ring_now` returns to 0
after each. Ring capacity 64; overflow/drop-oldest not exercised at 2 records/pass
(expected — few samples per pass by design of this test).

### 2. Per-record timestamp — CONFIRMED, and it cross-checks

Receiver prints `seq=1 ts=1784059200 OK`, `seq=2 ts=1784059736 OK`,
`seq=3 ts=1784060310 OK`, `seq=4 ts=1784060683 OK` … and each `ts` **exactly matches**
the `ts` the collar logged when it stored that same `seq` (`[RING] stored seq=1
ts=1784059200`, etc.). Motion bursts are absolutely time-referenced again.

### No freezes, new-data-per-pass, checksums

Collar ran many cycles + drone passes, every one recovered via reboot-to-sleep — no
freeze. Receiver: `new=2 → 4 → 6`, `bad=0`, cross-pass `dup=0` (the only dups are each
pass's own 2nd redundancy blast). All checksums matched.

## The LoRa gap (repeater was OFF for the first ~3 cycles) — correct behaviour

Cycles 1–3 show `+EVT:RXP2P RECEIVE TIMEOUT → [RX] not-ACK ("")` (the LoRa repeater
was off). This is **not a fault** — it is the v6 **#5 delivery guarantee** working:
un-ACK'd real fixes were **held** (`pending` grew 1→2→3→4, `delivered` stayed 106),
nothing was dropped. When the repeater came back (cycle 4), the backlog drained
**newest-first** with the 30 s ACK-window guard — seq 111,110,109,108,107 all ACK'd,
`pending` → 0, `delivered` → 111 — then normal 1-per-cycle ACKs resumed. Position data
is completely intact; the outage just delayed delivery, exactly as designed.

## Why the receiver doesn't list the accelerometer samples (interpretation)

**The 100 samples per record ARE received — they're just not printed.** The reference
receiver is a *verifier*, not a logger: for each `x,y,z` line it folds the value into a
running 16-bit checksum and counts it, but it does **not** `Serial.print` each sample.
It prints only a per-record summary:

```
[REC] seq=1 n=100 ck=29696 ...          <- record header (100 samples announced)
      seq=1 ts=1784059200 OK (100 samples, checksum match)   <- all 100 verified
```

`checksum match` is the proof: the receiver summed all 100×3 values it received and got
the exact `ck` the collar computed over the same 100 samples — so **every sample
arrived correctly**. They're simply not dumped to the console (printing 100+ lines/record
would flood USB serial and, as seen in earlier runs, make the receiver's `printf` drop
lines). If you want to *see/store* the raw `x,y,z`, add a print/append in the receiver's
sample branch (`sscanf(ln,"%d,%d,%d")`) — the data is already there; only the display is
suppressed. (The staggered indentation / a couple of truncated lines like `seq=5 …atch)`
are just USB-serial console drops on the receiver side — cosmetic; the `END … bad=0`
totals confirm the data validated regardless.)
