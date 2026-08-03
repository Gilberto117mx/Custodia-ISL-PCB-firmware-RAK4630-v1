# ISL_Flash_Probe — usable `api.system.flash` size (RUI3)

Walks the RUI3 flash offset space writing + verifying a pattern to find the
ceiling, then prints how many accelerometer records fit.

## Result (this board, RUI3 nRF52 4.2.4) — `probe_result.txt`

**Usable `api.system.flash` ≈ 132 KB** (writes succeed through 0x20000, fail at
0x21000). Per-call limit is still ≤255 B, so large records are chunked.

That is **plenty** — storage is not a constraint for this application:

| accel record | bytes/rec | records fit | days @ 6 h cadence |
|---|---|---|---|
| 10 s @ 10 Hz | 612 | 220 | ~55 |
| 5 s @ 10 Hz | 312 | 432 | ~108 |
| 5 s @ 5 Hz | 162 | 832 | ~208 |

Production uses offsets 0x0000–~0x2C00 for tracker state + a 16-record accel ring;
raising `ACCEL_RING_RECORDS` well into the tens/hundreds is safe (e.g. 64 records
@ 612 B ends ~0x9E00, far below the 0x21000 ceiling).

> Diagnostic only — it overwrites user flash. Production self-heals on a magic
> mismatch, but re-flash production afterwards to start clean.
