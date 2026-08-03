# v12 bench run — 2026-07-28 (analysis)

Files: `Tracker_v12_run1.txt` (collar), `Receiver_v12_run1.txt` (reference drone),
`ppk_v12_run1.csv` (PPK2, ~102 min, did not freeze).
Config: `SIMULATE_FIX=1`, `GNSS_PERIOD_MIN=3`, `FIX_MAX_SEC=60`, `SIMULATE_WUR` drone
pass ≈ every ~9–10 min, `ACCEL_RING_RECORDS=16`, `OFFLOAD_DELETE_AFTER_SEND=1`.

## Verdict: ✅ v12 works exactly as designed — NEW data every pass

| What | Result |
|---|---|
| Each pass transmits **only new records** (cleared after send) | ✅ 1st pass 16 (leftover ring from prior run), then **1, 2, 2, 2 …** — never a re-send |
| Receiver validates every record, every pass | ✅ across the run **new = 35, bad = 0, corrupt = 0** |
| Cross-pass dups | ✅ **none** — the only `dup` is the in-pass 2nd blast (`BLE_BLAST_REPEATS`) |
| `[RING] cleared (delivered)` after each pass | ✅ every pass |
| Collar froze? | ✅ **never** — ~13 passes over ~1.5 h, all reboot-to-sleep clean |
| LoRa | ✅ 100 % (seq 87→106+ all ACKed, RSSI −37…−39 dBm) |

The receiver counters climb monotonically `new=16→17→19→21→23…35` with `bad=0`
throughout. This is the cleanest run yet.

### Nice side effect: passes are now SHORT

Because v12 only sends the new batch, a typical pass is **2 records ≈ 9 s** of BLE
(e.g. 00:26:54→00:27:03) versus v11's always-16 ≈ 72 s. Shorter air-time = less energy
and a much easier target for a real, brief drone fly-by. (The one 16-record pass at
the start was the leftover ring from the previous v11 run being flushed once.)

## Two cosmetic artifacts (NOT firmware faults)

1. **Duplicated / out-of-order serial segments in the collar log** (e.g. lines
   ~184–232 replay seq 89/90 with an older RTC stamp; doubled `[TX pass]` lines;
   `[alive]` counters out of order). The flash state proves the firmware is fine — at
   every real boot `[FLASH] Loaded: nextSeq` is **monotonic** (87,88,89,91,93,95,…) and
   `delivered` never regresses. These are USB-serial capture glitches, not re-executed
   cycles.
2. **The receiver dropped a few printed lines** (pass 1: `seq=77`/`78` per-record lines
   missing, `seq=79` printed without its header). But the END counter still reads
   `new=16` and the 2nd blast marks 77/78 as `dup` — so they *were* received and
   validated; only the receiver's verbose `Serial.printf` fell behind. Data integrity
   was 100 %.

## Power (PPK2, 102 min — full capture this time)

No freeze — the rig fix held. **But USB was still connected for logging, so the trace
cannot show the real sleep floor:** 89 % of samples sit pinned at **~1.93 mA** and
**0 %** ever drop below 1 mA. That ~1.9 mA is the **USB-CDC / VBUS artifact** (the
nRF52840 USB peripheral stays up while plugged in), *not* the 34 µA battery floor
already validated in v7. GPS/TX bursts are real (p99 ≈ 44 mA, peak ≈ 87 mA), mean
≈ 3.1 mA is dominated by the USB-sleep baseline.

➡ **To get a real floor/endurance number you must measure battery-only, headless
(no USB serial).** Over USB the floor is unobservable — this is a measurement-setup
item, not something firmware can fix (on battery, no VBUS, it is 34 µA).

## The one real gap this run exposed → v13

**The per-record TIMESTAMP is not transmitted.** Each accel record stores a `ts`
(the collar log shows `[RING] stored seq=82 ts=1784059200 n=100`), but the BLE header
is `R <seq> <n> <ck16>` — **no `ts`**. It was dropped in the v11 line-shortening to
keep every line ≤ 20 B. So the drone receives motion bursts ordered by `seq` but with
**no absolute time reference**, and the two seq spaces (accel ring vs LoRa packet) are
not cross-mapped over the air. For behaviour science the accel data needs to be
time-stamped. **Restoring `ts` is the top v13 item** — see `../README.md` / the v13
proposal (send it as its own short `T <ts>` line so it still fits one notification).
