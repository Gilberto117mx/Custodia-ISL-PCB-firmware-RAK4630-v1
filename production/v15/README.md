# ISL Board — Production firmware v15 (COLLAR, GPS backup-cell health + charge-on-cold)

**v15 = v14 + a self-regulating GPS backup-cell strategy and health telemetry.**
Everything in v14 (cold/first-fix patient window, two accel/BLE timers, per-record
timestamp, the proven no-freeze BLE offload, LoRa + delivery guarantee, reboot-to-
sleep) is unchanged. v15 only changes *how the GPS is powered* around a fix, and adds
`TTFF`/`CELL` to the LoRa packet.

> ✅ **Status: FIELD-VALIDATED (outdoor, 2026-08-02, `SIM_FIX=0`).** A drained backup
> cell was detected (110 s cold fix → `CELL=LOW`) and self-charged into hot starts
> (`TTFF` 110 → 35 → 14 → 9 s, `CELL=OK`) within two 180 s charge holds — the whole
> v15 thesis, on real hardware. Zero regression: BLE offload 7 records / 0 bad / no
> freeze, LoRa delivery guarantee backfilled a 33 min repeater outage, two-timer
> accel + real-GPS RTC + flash persistence all intact. Full trace:
> [`logs/OUTDOOR_ANALYSIS.md`](logs/OUTDOOR_ANALYSIS.md).

## Why (measured, `tests/ISL_GPS_Backup_CellTest`)

The L76K's **MS621FE backup cell (5.5 mAh)** preserves ephemeris for **hot starts**,
and it **only charges while the GPS is powered**. The bench test proved:

- **Charged cell → ~3–5 s hot fix even after 1 h off** (60 s / 10 min / 30 min / 60 min
  gaps all gave ~3.4–4.6 s TTFF, sats reported in ~1 ms).
- **Flat cell → ~35 s cold start**, or a *failure* (`SV=0/1`) if the almanac is lost.

Our per-cycle on-time (~5 s) **can't offset the ~1 h backup drain**, so the cell slowly
depletes and cold starts return — exactly the v14 overnight trajectory (2 good fixes,
then `SV=1/0`). The fix is to **keep the cell charged**, and the cheapest way is to let
the *patient fix window double as the charge*.

## What v15 does (self-regulating — the patient window IS the charge)

Per fix, classified by **TTFF**:

| Outcome | Meaning | Action | `CELL=` |
|---|---|---|---|
| **HOT** (fix < `HOT_TTFF_SEC` = 15 s) | cell holding ephemeris | power GPS off immediately — cheap | `OK` |
| **LOW** (slow/missed fix) | cell drained | **keep GPS on to `GPS_CHARGE_SEC` = 180 s total to recharge it** → next wake is hot | `LOW` |
| **DEAD** (non-hot for `CELL_DEAD_AFTER` = 4 cycles despite charging) | cell won't hold | stop the futile charge; still fix (just slower) | `DEAD` |

So a healthy cell costs ~5 s/cycle; a draining cell self-heals with one 3-min charge;
a dead cell is flagged and stops wasting energy. On a **9000 mAh 26500 LiSOCl₂**, even
the pathological worst case (dead cell → 3 min every cycle) is **~9–12 months**.

## Telemetry: know if the cell is good, remotely

Every position packet now ends with **`TTFF=<s>,CELL=<OK|LOW|DEAD>`**, e.g.:

```
051,42,22.556584,113.922289,4.00,1785349354,SV=9,TTFF=4,CELL=OK      <- healthy
051,43,0.000000,0.000000,3.99,1785352954,SV=0,TTFF=180,CELL=DEAD     <- cell failing
```

The **repeater relays the raw string**, so this shows up in its log with **no repeater
change**. The in-repo reference LoRa receiver (`ISL_v15_LoRa_Receiver`) parses it and
prints a health line (`GPS-CELL: TTFF=…s CELL=…`, with a `<<< GPS CELL DEAD >>>`
banner). `TTFF` climbing / `CELL=DEAD` tells you which collar's GPS cell to check.

## Operational note — pre-charge before deployment

The firmware *maintains* the cell but can't bootstrap a fully-flat one instantly. Before
sealing a collar, **keep the module powered ~30 min** (e.g. run any fixing firmware
outdoors) so the MS621FE is topped up. Then v15 keeps it charged and reports if it ever
stops holding.

## Config knobs (v15 additions)

| Knob | Default | Meaning |
|------|---------|---------|
| `HOT_TTFF_SEC` | 15 | Fix within this ⇒ cell healthy (HOT); slower ⇒ LOW (charge). |
| `GPS_CHARGE_SEC` | 180 | When LOW, hold GPS on this long *total* to recharge (the "3 min"). |
| `CELL_DEAD_AFTER` | 4 | Consecutive non-hot fixes ⇒ flag `CELL=DEAD`, stop charging. |

(v14 knobs unchanged: `COLD_FIX_MAX_SEC=180`, `NO_SKY_ABORT_SEC=45`, `COLD_AFTER_NOFIX=1`,
`EPHEMERIS_STALE_HOURS=12`, `GNSS_PERIOD_MIN`, accel/WUR timers, …)

## What to check on the bench

1. Boot prints `[CFG-CELL] hot_if_ttff<15s charge_when_low_to=180s_on dead_after=4 …`.
2. With a **charged** cell: fixes are fast → `[GPS] CELL=OK lastTTFF=4s coldStreak=0`,
   GPS powers off in a few seconds, packet ends `…,TTFF=4,CELL=OK`.
3. Force a **drained/slow** start (fresh module, or a long gap): `CELL=LOW`, the log
   shows `charging to 180s total on…`, and the *next* cycle should be `CELL=OK` again.
4. Receiver / repeater shows the `TTFF`/`CELL` fields on every packet.

## Known / deferred (unchanged)
- Real AS3933 WUR wake; accel-module switchable rail (the ~325 µA sleep floor);
  battery-only headless power floor.
- `coldStreak`/health is RAM (resets on a reboot-to-sleep = drone pass; re-learned in a
  few cycles). Persist it if you want the DEAD flag to survive reboots.
