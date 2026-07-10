# ISL Board (RAK_feather)

A **RAK4630-based board**, extracted into its own **standalone repository** from the
original [Custom-PCB-RAK4630-RUI-based](https://github.com/Gilberto117mx/Custom-PCB-RAK4630-RUI-based)
monorepo (where it lived under `ISL_Board/`, alongside the Tracker 6.0 (Custom PCB) work).
It is kept separate so the two boards **never mix**, while still reusing the validated
lessons and example sketches from Tracker 6.0 (same MCU + same RV-3028 RTC).

> Bring-up follows the same hardware-in-the-loop loop we used for Tracker 6.0:
> flash a focused test, capture the serial/power output, iterate. Send me each
> test's output and we refine from there.

## What's different from Tracker 6.0
| | Tracker 6.0 (Custom PCB) | **ISL Board (RAK_feather)** |
|---|---|---|
| MCU | RAK4630 | RAK4630 (same) |
| RTC | RV-3028-C7 | RV-3028-C7 (same chip) |
| RTC INT / wake pin | P1.04 | **P1.03** |
| Power tree | raw VBAT everywhere | **RT9080-33 LDO → 3.3 V** for nRF/SX1262 |
| Battery divider | 470k/470k (~3.8 µA, noisy AIN7) | **10k/10k (~180 µA, clean AIN7)** |
| GPS UART | Serial1 @ P0.15/P0.16 | **P0.19/P0.20 (instance TBD)** |
| GPS power EN | P1.01 (NPN+PMOS) | **P0.04 (P-FET, polarity TBD)** |
| **Extra: wake-up radio** | — | **AS3933 LF wake-up receiver** on SPI, wake → **P1.04** |

The headline feature is the **AS3933 wake-up receiver (WUR)**: an ultra-low-power
LF receiver that listens for a 433 MHz carrier modulated with a 19 kHz OOK/
Manchester 16-bit pattern (default `0x9669`) and asserts a wake pin (P1.04) on a
match — so the board can be woken **on demand** by a beacon, in addition to the
scheduled RTC wake (P1.03).

## Layout
```
.                             <- repository root
├── README.md                 <- this file
├── docs/
│   └── ISL_Pinout.md         <- authoritative pin map + hardware notes (READ FIRST)
├── hardware/
│   └── iteration1/           <- RAK_feather schematic, PCB PDF, BOM (as uploaded)
├── reference/
│   └── AS3933_wakeup/        <- vendor AS3933 WUR repo, UNMODIFIED (ESP32 + MATLAB/SDR TX)
└── tests/
    ├── ISL_I2C_Scan/         <- #0 bring-up: MCU alive, RTC @ 0x52
    ├── ISL_RTC_Read/         <- #1 RV-3028 keeps time (INT on P1.03)
    ├── ISL_Battery_ADC/      <- #2 battery volts (AIN7, 2.4 V ref, ÷2)
    ├── ISL_GNSS_Monitor/     <- #3 raw NMEA (resolves GPS UART instance + EN polarity)
    └── ISL_WUR_AS3933/       <- #4 AS3933 config + RC-cal + wake IRQ (the new subsystem)
```

## Suggested bring-up order
0. **`ISL_I2C_Scan`** — confirms the toolchain, the debug serial, and that the RTC
   answers on I2C (0x52). If nothing prints, the board may log over native USB-C
   (`Serial`) instead of the RAKDAP UART (`Serial0`) — see the note in each sketch.
1. **`ISL_RTC_Read`** — RV-3028 runs and keeps time. (Reuses the Tracker RV-3028 flow;
   only the INT pin moved to P1.03.)
2. **`ISL_Battery_ADC`** — reads battery volts; low-impedance divider means no
   settling/noise workaround needed. Confirm against a known supply.
3. **`ISL_GNSS_Monitor`** — resolves the two GPS unknowns (which `Serial` instance,
   and `L76K_EN` polarity) by dumping raw NMEA. Once confirmed, we port the full
   TinyGPSPlus + low-power/deep-sleep GPS logic from Tracker 6.0.
4. **`ISL_WUR_AS3933`** — brings up the wake-up receiver: SPI comms check, pattern-mode
   config, RC-oscillator calibration, register dump, and wake-on-interrupt reporting.
   Needs a WUR transmitter (`reference/AS3933_wakeup/WuTx*`) to trigger a real wake.

## Reused-from-Tracker lessons already baked in
- **Deep sleep:** `api.ble.stop()`, `NVIC_DisableIRQ(FPU_IRQn)` + `clearFPU()`,
  float-free sleep path, external-INT wake (here P1.03 for RTC **and** P1.04 for WUR).
- **RTC:** Melopero_RV3028 init + single-shot periodic-timer wake pattern
  (`RAK4630_RTC_SleepWake_v3`).
- **GPS:** power-cut-first / let-UART-quiet-then-end teardown, hot start via V_BCKP.
- **Battery ADC:** 12-bit, `AR_INTERNAL` = 2.4 V FS, ÷2 divider (calibration transfers directly).

## Open unknowns to confirm on hardware (tracked in `docs/ISL_Pinout.md`)
1. GPS `Serial` instance on P0.19/P0.20 (Serial1 vs Serial2).
2. `L76K_EN` (P0.04) polarity for the Q1 P-FET.
3. ~~Debug path~~ **RESOLVED (test #0): native USB-C `Serial`.**
4. Sleep-floor budget — the **~180 µA battery divider** likely dominates and may
   warrant a next-rev change for a WUR-class low-power board.

## Board quirk: alive-first sketch structure (IMPORTANT)
On this board the debug port is the nRF52840 **native USB CDC**, and there is a
single COM port (e.g. COM50) that is the **SDFU bootloader** when idle and the
**running app** after reset - use it for both upload and monitor. Doing long
blocking work or early peripheral init inside `setup()` wedges the app before it
prints (it faults back to the bootloader). The fix, used by all ISL test sketches:
keep `setup()` tiny (just `Serial.begin` + a short wait + one banner), then run a
short heartbeat and do the real init a couple seconds into `loop()`. This was
proven with ISL_RTC_Read (v1 all-in-setup faulted; v2 alive-first works and even
survives resets).

## Status
- **Test #0 (`ISL_I2C_Scan`) PASSED** - MCU alive, RV-3028 @ 0x52, debug = native
  USB-C `Serial` on COM50.
- **Test #1 (`ISL_RTC_Read`) PASSED** - RV-3028 keeps time and persists across MCU
  reset (VBACKUP). Established the alive-first structure (above).
- Next up: #2 battery, #3 GNSS (resolve UART instance + EN polarity), #4 AS3933 WUR
  - all now restructured alive-first. Proceeding test-by-test.
