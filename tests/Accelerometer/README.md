# Accelerometer over BLE — working emitter/receiver pair

Proven-working pair that streams **LIS3DHTR** accelerometer data from the ISL
board to a second nRF52840 over **BLE (Nordic UART Service)**. These are the
reference implementations the production BLE path is built from — **do not edit
them**; they are kept verbatim as the known-good baseline.

| Sketch | Role | Board it runs on |
|--------|------|------------------|
| `Accel_BLE_Emitter/` | Peripheral — reads the accelerometer, advertises as `Custodia-Tracker`, streams samples on connect | ISL board (RAK4630) |
| `Accel_BLE_Receiver/` | Central — scans, connects, prints the stream over USB serial | any nRF52840 (e.g. RAK4631 / XIAO nRF52840) |

## ⚠ Board package — Adafruit / WisBlock BSP, NOT RUI3

Both sketches use the **Adafruit nRF52 (WisBlock) Arduino core** (`bluefruit.h`,
`Adafruit_TinyUSB.h`). In Arduino IDE select the **WisBlock Core** board — they
will **not** compile under the "RAKwireless RUI3" package. This is the whole
reason the link works: only the Bluefruit stack puts the Nordic UART Service
UUID into the *advertising packet* (`Bluefruit.Advertising.addService(bleuart)`)
and connects without pairing, which is what the receiver's
`Scanner.filterUuid(clientUart.uuid)` + `useActiveScan(true)` discovery needs.
RUI3's `api.ble.uart` advertises the name only and cannot be found this way.

## Accelerometer wiring (Grove LIS3DHTR → ISL board)

Bit-bang (software) I2C on the exposed secondary pins — RUI3's hardware `Wire1`
is unsupported, but bit-bang is pure GPIO and works under any core:

| LIS3DHTR | ISL pin |
|----------|---------|
| SDA | P0.24 |
| SCL | P0.25 |
| VCC | 3V3 |
| GND | GND |

I2C address `0x19`, `WHO_AM_I` (0x0F) = `0x33`. Emitter config: 100 Hz, ±2 g,
high-resolution; 5 s of samples (max 60) at 100 ms period per cycle.

## BLE payload format

```
ACC cycle=<n> count=<N>
s01 <x>,<y>,<z>
s02 <x>,<y>,<z>
 …
END
```

Values are raw LIS3DHTR counts. Receiver prints them straight to USB serial.

## Expected receiver serial

```
=== Custodia Receiver — BLE Central ===
[BLE] Scanning for emitter...
[BLE] Found Custodia Tracker! Connecting...
[BLE] Connected! Discovering BLE UART service... Discovered.
[BLE] Subscribed to incoming data stream.
ACC cycle=1 count=50
s01 ...
 …
END
```
