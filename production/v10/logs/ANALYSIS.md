# v10 bench run — 2026-07-28 (analysis)

Files in this folder:
- `Tracker_v10_run1.txt` — the collar (RUI3) serial log, ran clean the whole time.
- `Receiver_v10_run1.txt` — the reference drone/central serial log.
- `ppk_v10_run1.csv` — Nordic PPK2 current capture (froze partway — see below).

Config: `SIMULATE_FIX=1`, `GNSS_PERIOD_MIN=2`, `SIMULATE_WUR_HOURS=0.25`,
`ACCEL_RING_RECORDS=16`, `DEVICE_ID=51`, `OFFLOAD_DELETE_AFTER_SEND=0`.

## Verdict: the important half works

| Result | Status |
|---|---|
| **Collar never froze** — ~10 GNSS/LoRa cycles + **6+ drone passes**, all recovered | ✅ |
| Reboot-to-sleep clears BLE deterministically every pass | ✅ |
| LoRa position packets seq 58–77 — **every one ACKed** (RSSI −37…−60 dBm, SNR 12–14) | ✅ |
| GPS time-sync, battery reads (3.87–3.98 V), accel 100-sample collect, flash ring rotation | ✅ |
| Receiver **connected + subscribed + saw the framing** (id 051 + 16 record headers, correct seq/ts) | ✅ |
| Receiver **validated the accel records** | ❌ `new=0 dup=0 bad=0` |
| PPK2 power capture | ❌ froze ~11.6 min in |

The freeze that dominated v9 / early-v10 (the `api.ble.custom` + `notify()` path) is
**gone** — this was the #1 requirement and it held across the whole run, including
two passes where the collar logged `Connected.` → `Pairing procedure fail.` and still
recovered.

## Why no data validated — root cause

`api.ble.uart` fragments each write into ~20-byte NUS notifications and gives the
application **no flow control**, so on a lossy/unencrypted link a notification can be
dropped. v10's per-record header is 30+ bytes = **two** notifications:

```
R 051 42 1785165983 100 <sum>
└──────── frag 1 (20 B) ───────┘└─ frag 2: "100 <sum>" ─┘
    seq, ts  — always arrived        count, checksum — routinely LOST
```

The receiver therefore parsed a garbage sample-count (`n=4294952064` — the checksum
bytes bleeding into the count field once the real `100 ` was dropped), so `curGot`
never reached `curN` and **no record ever completed**. Every one of the 16 headers
shows this, including the two freshly-stored records (seq 56/57) whose collar-side
count was verified `= 100` — proving it is a *transport/framing* artifact, not stale
or corrupt ring data.

`Pairing procedure fail` is a **red herring** for the data loss: notifications flowed
anyway (the receiver got the id + all headers), so the NUS TX characteristic is not
encryption-gated on this RUI3 build. The loss is fragmentation, not encryption.

→ **Fixed in v11:** every line ≤ 20 B (one notification), 16-bit checksum, and the
whole ring blasted twice with seq-dedup. See `../../v11/`.

## PPK2 capture — not usable

11.6 min span, then the readout **flat-lines at a constant 1.92 mA** for the tail
(real current is never flat to 3 decimals) → the PPK2 locked up while still powering
the collar, exactly as observed on the bench. Mean/percentiles from this file
(mean ≈ 8.7 mA, p50 ≈ 1.9 mA, p90 ≈ 43 mA) reflect the **GPS-heavy `SIM_FIX` duty
cycle during a truncated capture**, not deployment behaviour or the 34 µA sleep floor
— do not quote them. Re-capture with the PPK on its own USB port (not shared with the
receiver) and known-good cables.

## Bench-rig issues (not firmware)

- **Receiver interfered with nearby BT (speaker/mouse) and its RST was sometimes
  unresponsive.** Unresponsive RST = classic **USB brown-out**; power the receiver
  from a powered hub / different port, keep it slightly away from the collar so the
  two 2.4 GHz radios don't desense each other during the blast.
- **PPK2 froze** (above) — put it on its own host port.

## Next test (with v11)

Same recipe, but watch the receiver for `seq=… OK (100 samples, checksum match)` and
an `END … new=k` count > 0, with the 2nd blast pass showing `(dup, skipping)`. Fix
the rig power first (PPK + receiver on separate, powered USB) before trusting any
current numbers.
