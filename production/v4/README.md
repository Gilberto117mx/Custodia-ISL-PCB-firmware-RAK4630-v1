# ISL Board — Production Firmware **v4**

v4 = the **hardened v3** (full-range + two-sample + self-heal time-seed guard)
packaged with **bench knob values** and a small **cycle counter** in the logs.

## What changed vs v3
- **Hardened GPS→RTC time-seed is the active build.** (An earlier indoor run that
  produced a `2066-31-19` garbage seed was on the *pre-hardening* v3; v4 rejects
  it — full range check + two-sample confirmation + boot self-heal.)
- Bench knobs baked in: `GNSS_PERIOD_MIN=5`, `GNSS_FIX_TIMEOUT_SEC=60`,
  `MAX_TX_RETRIES=2` (as-run).
- `cycleNum` in the `== COLLECT (cycle N) ==` / `[TX pass]` lines, so a USB-CDC
  log *replay* after wake (a cosmetic re-enumeration artifact) is distinguishable
  from a real repeat.

## Field result — outdoors, headless on battery (`logs/`, `PowerProfile_outdoors_nofix.csv`)
- ✅ **Time-seed worked outdoors**: real UTC (receiver timestamps decode to
  **2026-07-14**, not the garbage epoch) — the hardened seed validated in the field.
- ✅ **Real battery sleep floor** confirmed **161–168 µA** (USB disconnected).
- ❌ **Zero position fixes** — every packet `lat/lon=0`, `sats=0`. **Not hardware**
  (see `../../tests/ISL_GNSS_Serial0/outdoor_nmea_capture.txt`: the module gets a
  12-sat 3D fix under continuous power). The cause was **cold-start vs short,
  power-cycled windows**: 60 s of GPS-on, power-cycled, never completed the initial
  almanac/ephemeris download. Fixed conceptually in v5 (proof) and addressed by the
  v6 strategy (`../../docs/GNSS_FieldStrategy.md`).

## Status
Superseded by **v5** (adds the `SV` reception diagnostic that turned this from a
mystery into a measured cold-start problem). Kept as the field-record build.
