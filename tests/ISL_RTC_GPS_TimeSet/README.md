# ISL_RTC_GPS_TimeSet — test #7 ✅ PASS

**Goal:** seed the RV-3028's time from the **GNSS UTC timestamp** instead of a
hardcoded compile-time date, so the node self-recovers correct time after any
power loss.

## Result (bench, near window)
- GPS UTC acquired in **17.7 s** on a cold run and **3.8 s** on the next run —
  both with `sats=0`, i.e. **time arrives before a position fix** (decoded from
  the nav message), which is what makes this cheap.
- Sanity guard (`year >= 2025`, age < 2 s) correctly rejected the module's
  power-on default date (year 2000) until real time arrived.
- RTC set to `2026-07-13 08:12:57 UTC` and **kept ticking with GPS powered off**,
  unix epoch matching.

## Second bug caught (first indoor run of production v3)
A **corrupted-but-parsed RMC sentence** right after GPS power-on produced
`2088-31-19 00:00:01` (month 31!), and the original *year-only* guard
(`year >= 2025`) let it through — **TinyGPSPlus checksums sentences but does
not range-check fields.** Hardened everywhere: full range check (year window
2025–2044, month/day/h/m/s in range) here, plus in `production/v3` a
**two-sample confirmation** (second sane reading ≥2 s later must advance
consistently with wall time) and a **boot self-heal** (insane stored RTC time
clears the sync flag and forces a re-seed).

## Bug found & fixed here (affects other code!)
`Melopero_RV3028::setTime()` argument order is **`(year, month, WEEKDAY, DATE,
hh, mm, ss)`** — weekday **before** date. The old hardcoded calls passed
`(…, date, weekday, …)` swapped, which silently stored the wrong day-of-month
(first run here read 2026-07-**01** instead of **13**). Fixed in this test and
in `production/v3`; the older hardcoded placeholders were low-impact (only the
first-boot default) but the order matters anywhere real dates are set.

## Why the RTC loses time on unplug (normal for this board)
Schematic v2: RV-3028 **VBACKUP (pin 6) is tied to the same 3.3 V rail as VDD**
(only a 100 nF cap on it) — **no coin cell / supercap**. So a full power removal
resets the RTC to 2000-01-01; a *reset* (power stays) retains time, and in the
field the LDO is always on through deep sleep (the 155 µA floor), so time
survives every sleep/wake cycle. GPS re-seeding at boot is the designed recovery
for true power loss. Next-rev hardware option: supercap/coin cell + isolation
diode on VBACKUP (+ enable trickle charge) for retention across battery swaps.

## Production integration
`production/v3`: on boot, if the RTC was never GPS-synced (USER_RAM flag), run a
time-only GPS session (no fix needed) and seed the RTC; if it fails (indoors),
re-seed opportunistically during the normal COLLECT fix sessions until it
succeeds. Packets then carry true UTC epochs.
