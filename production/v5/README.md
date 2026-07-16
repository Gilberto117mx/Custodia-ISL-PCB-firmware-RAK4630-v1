# ISL Board — Production Firmware **v5** (GPS reception diagnostic)

v5 = v4 + a **reception diagnostic that rides in the packet**, so GPS health can
be read **headless from the receiver**.

## What changed vs v4
- Parses **satellites-in-view** from the GSV sentences (GP/BD/GL/GA) and appends
  the peak to the packet as a **7th field `SV=N`**. The v2 receiver still ACKs
  (uses fields 0/1) and prints the full raw line — no receiver change needed.
- Per-session log now prints `used=U  IN-VIEW peak=V`.
- `GNSS_FIX_TIMEOUT_SEC` back to **90 s**.

## Field result — outdoors (`logs/Node_outdoors_FIX.txt`, `logs/Receiver_outdoors_FIX.txt`)
🎉 **First full production position fix on the ISL board:**
```
[GPS] FIX in 6977 ms  used=14  IN-VIEW peak=19   lat=22.528619 lon=113.940480
[TX] seq=61: 051,61,22.528619,113.940480,3.92,1783081357,SV=19
receiver: Raw : 051,61,22.528619,113.940480,3.92,...,SV=19  (ACK OK)
```
- **7-second hot fix, 19 satellites in view, real coordinates delivered + ACK'd.**
- Confirms the v4 "no fix" was the **cold-start** problem: once the module had
  almanac/ephemeris in its backup battery (populated by a prior continuous run),
  production hot-starts in seconds.
- The ACK/retry path passed again headlessly: first ACK timed out (receiver off)
  → packet to PENDING → re-sent 300 s later → ACK OK.

## ⚠ Bug found in this run (drives v6): RTC re-sync
Boot showed `GPS-synced flag present  current: 2026-07-03`, and the packet
timestamp decodes to **~July 4** while the real GPS date was **July 14** — the
clock is **~11 days behind**. Cause: we **sync the RTC once and never again**, and
that seed can come from the module's **backup-RTC time**, which is unreliable when
there's no position fix (we've seen it emit 2000, 2066, and 11-days-off). Fix
(v6): **re-sync the RTC from GPS on every *real position fix*** (then the time is
genuine GPS time), keeping the sane+two-sample guard. See
`../../docs/GNSS_FieldStrategy.md`.

## Status
Latest field build. Superseded by the upcoming **v6** (adaptive SV-gated GPS
timeout + no-sky backoff + fix-only RTC re-sync + low-battery lockout — the field
strategy). The `SV` field added here is v6's control input.
