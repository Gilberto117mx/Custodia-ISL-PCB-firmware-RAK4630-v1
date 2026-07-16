# ISL Board — Hardware (schematics / PCB / BOM)

The board has gone through two schematic iterations. **Only `iteration2` is
current** — all firmware, the pin map (`../docs/ISL_Pinout.md`), and every test in
this tree are written against it. `iteration1` is kept for history only.

## ✅ Current / authoritative
```
iteration2/
└── RAK_feather_schematic_v2.pdf   <- THE schematic of record (use this one)
```
**Schematic v2** is what the hardware actually is. Key differences it introduced
over v1 (all confirmed on hardware during bring-up):

| Signal | v1 (old) | **v2 (current)** |
|---|---|---|
| `L76K_EN` (GPS power) | P0.04 | **P1.02**, active-LOW (P-FET high-side) |
| `RTC_INT` (RTC wake) | P1.03 | **P0.21** |
| GPS UART | (ambiguous) | **Serial0 / UART1 = P0.19 (RX) / P0.20 (TX)** |
| WuR wake (AS3933) | — | **P1.04** |
| Battery divider R9/R10 | 10k/10k | **1 MΩ / 1 MΩ** (~1.8 µA) |
| Battery-sense filter | — | **C17 = 100 nF on AIN7 (`BATT_LEVEL`)** |

> **Battery divider = 1 MΩ/1 MΩ + C17** is the value of record, **confirmed on the
> physical board**. Some earlier text docs said 10k/10k — that was a *wrong diagram*.
> The ratio is 2.0 either way, so the battery calibration is unaffected; but the
> ~155 µA sleep floor is therefore **not** the divider (~1.8 µA) and its cause is an
> open item — see `../docs/ISL_DeepSleep_Notes.md`.

Full pin map with every peripheral: **`../docs/ISL_Pinout.md`**.

## 🗄️ Historical (superseded — do not design against)
```
iteration1/
├── RAK_feather_schematic.pdf      <- schematic v1 (superseded by v2)
├── RAK PCB.pdf                    <- PCB layout as originally fabricated
└── RAK_feather.csv               <- BOM (bill of materials)
```
Kept so the pin reassignments (e.g. `L76K_EN` P0.04→P1.02, `RTC_INT` P1.03→P0.21)
are traceable. If you are wiring, probing, or writing firmware, ignore v1 and use
`iteration2`.

> If a newer schematic revision is ever added, create `iteration3/`, move the
> "current" pointer here to it, and update `../docs/ISL_Pinout.md` in the same
> commit so there is always exactly **one** authoritative schematic.
