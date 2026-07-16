# Production v1 — run logs

## Run 1 (2026-07-09, ~3.6 h, indoor)
Serial logs for this run were **not captured** — the operator confirmed behaviour
live but could not save the streams:

- **Node (emitter):** first flash showed the expected P2P-switch reboot, then the
  normal `COLLECT → GPS → TX → ACK → deep-sleep` cycle on COM50. Native USB drops
  at each `[SLEEP]`, and the operator ran on **battery with USB disconnected** to
  reach the true sleep floor, so no continuous node log exists.
- **Receiver:** operator visually confirmed **packets were received successfully**
  in the receiver monitor for the whole run (LoRa TX + ACK round-trip working on
  the ISL board's SX1262). Receiver log not saved.

Quantitative validation for this run is therefore the **power profile**
(`../PowerProfile_Production.csv`), analysed in `../README.md` §Power profile.

> TODO next run: capture both serial streams (node COM + receiver) to a file for
> a matched delivered/undelivered count.

## Run 2 (2026-07-10, min/sec variant, 15-min period)
Serial logs again not captured (operator's laptop closed mid-run; board kept
running on power). Operator confirmed at the receiver that packets arrived with
**correct, in-order sequence numbers**. Quantitative record is the power capture
`../PowerProfile_run2_15min.csv` (41 s: one full cycle incl. the board's **first
GNSS fix**, ~21 s) — analysed in `../README.md` §Run 2.
