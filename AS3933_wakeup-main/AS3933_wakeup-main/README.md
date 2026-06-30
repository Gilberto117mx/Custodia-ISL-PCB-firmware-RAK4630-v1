# AS3933 Wake-Up System

This repository contains a working implementation of a wake-up system using the AS3933 low-frequency wake-up receiver chip. The system consists of a transmitter (MATLAB) and receiver (Arduino) that communicate using a pattern-based wake-up protocol.

## 🚀 Working Code

The following files contain **fully functional and tested code, that it is working for aaround 10 meters with adalm pluto SDR**:

### Transmitter

- **`WuTx/transmitter_new.m`** - MATLAB script for Pluto SDR transmitter
  - Generates pattern-based OOK (On-Off Keying) wake-up signals
  - Uses 433 MHz HF carrier with 19 kHz LF envelope
  - Implements AS3933 protocol with Manchester encoding
  - Default wake-up pattern: [1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1]

### Receiver

- **`WuRx_Pattern_blink/WuRx_Pattern_blink.ino`** - Arduino sketch for ESP32-S3
  - Configured for AS3933 in pattern correlation mode
  - Includes automatic RC-oscillator calibration
  - Blinks onboard LED when wake-up pattern is detected

## 📋 System Overview

### Communication Protocol

- **Frequency**: 433 MHz HF carrier with 19 kHz LF envelope modulation
- **Modulation**: OOK (On-Off Keying) with Manchester encoding
- **Pattern**: 16-bit wake-up pattern ([1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1])

### Protocol Structure

1. **Carrier Burst**: 4ms continuous square wave
2. **Separation**: Half Manchester symbol duration
3. **Preamble**: 6-bit sequence (101010)
4. **Wake-up Pattern**: 16-bit Manchester encoded pattern : [1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1]

## 🔧 Hardware Requirements

### Transmitter Setup

- **Pluto SDR** (ADALM-PLUTO)
- **MATLAB** with Communications Toolbox
- **Antenna** suitable for 433 MHz

### Receiver Setup

- **ESP32-S3 Super Mini** board
- **AS3933** wake-up receiver chip
- **Antenna** tuned for 433 MHz

### Wiring (ESP32-S3 to AS3933)

| ESP32-S3 Pin | AS3933 Pin | Function       |
| ------------ | ---------- | -------------- |
| 3V3          | VCC        | Power          |
| GND          | GND        | Ground         |
| G12 (MOSI)   | SDI        | SPI Data In    |
| G13 (MISO)   | SDO        | SPI Data Out   |
| G11 (SCLK)   | SCL        | SPI Clock      |
| G10 (CS)     | CS         | Chip Select    |
| G8           | WAKE       | Wake Interrupt |
| G48          | -          | Onboard LED    |

## 🏃‍♂️ Getting Started

### Running the Transmitter

1. Open MATLAB and navigate to the `WuTx/` directory
2. Connect your Pluto SDR to your computer
3. Run the script: `transmitter_new`
4. The script will continuously transmit wake-up patterns

### Running the Receiver

1. Open Arduino IDE
2. Install ESP32 board support if not already installed
3. Open `WuRx_Pattern_blink/WuRx_Pattern_blink.ino`
4. Select "ESP32S3 Dev Module" as the board
5. Enable CDC on boot from tools for terminal output but if the led is not working you can disable it
6. Upload the sketch to your ESP32 dev S3
7. Open Serial Monitor (115200 baud) to see debug output
8. The onboard LED will blink when a wake-up pattern is detected

## ✅ Features

- **Pattern Correlation Mode**: Uses 16-bit pattern matching for reliable wake-up detection
- **Automatic RC Calibration**: Self-calibrates the AS3933 RC oscillator for optimal performance
- **Manchester Encoding**: Robust data encoding for reliable transmission
- **RSSI Reporting**: Displays received signal strength information
- **Debug Output**: Comprehensive serial output for troubleshooting
- **LED Indication**: Visual feedback when wake-up patterns are detected

## 📊 Performance

- **Operating Frequency**: 19Khz (15-23 kHz band (AS3933 band selection))
- **Data Rate**: ~244 bps (optimized for maximum range)
- **Pattern Length**: 16 bits (configurable)
- **Range**: Depends on antenna design and transmission power

## 🔧 Configuration

### Customizing Wake-up Pattern

To change the wake-up pattern, modify these values in both files:

**In `transmitter_new.m`:**

```matlab
wakeupPattern = [1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1]; % Your 16-bit pattern
```

**In `WuRx_Pattern_blink.ino`:**

```cpp
const byte CONFIG_R5 = 0x69; // Second byte of pattern  
const byte CONFIG_R6 = 0x96; // First byte of pattern
```

## 📁 Project Structure

Nothe that the other codes are not working and they are just for test

```
AS3933_wakeup/
├── README.md                           # This file
├── WuRx_Pattern_blink/
│   └── WuRx_Pattern_blink.ino         # ✅ Working receiver code
├── WuRx_NoPattern_blink/
│   └── WuRx_NoPattern_blink.ino       # Alternative receiver (frequency-only)
├── WuTx/
│   ├── transmitter_new.m              # ✅ Working transmitter code
│   ├── transmitter.m                  # Alternative transmitter
│   ├── transmitter_pattern.m          # Pattern-specific transmitter
│   ├── transmitter_carrier.m          # Carrier-only transmitter
│   ├── receiver_emulator.m            # Receiver emulation tool
│   └── clock_calculations.m           # Clock timing calculations
└── WuTx_CC1101_Pattern/
    └── WuTx_CC1101_Pattern.ino        # CC1101-based transmitter alternative
```

## 🐛 Troubleshooting

### Common Issues

1. **No wake-up detection**: Check antenna tuning and positioning
2. **SPI communication errors**: Verify wiring connections
3. **Pattern mismatch**: Ensure transmitter and receiver use the same pattern
4. **Led not blink**: Close CDC on boot from the tools section on the receiver
