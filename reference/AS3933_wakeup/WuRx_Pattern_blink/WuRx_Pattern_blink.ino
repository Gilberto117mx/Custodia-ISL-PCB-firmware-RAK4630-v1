/****************************************************************************************
 * AS3933 ESP32-S3 Configuration & Register Dump - PATTERN MODE
 *
 * This version is configured for MAXIMUM RELIABILITY on a single channel using
 * a 16-bit wake-up pattern at the lowest possible data rate (~244 bps).
 * It is specifically adapted for the ESP32-S3 Super Mini board.
 *
 * --- KEY CHANGES ---
 * 1.  MODE: Switched from "Frequency-Only" to "Pattern Correlation" for reliability.
 * 2.  DATA RATE: Configured for the slowest possible data rate (~244 bps) to maximize range.
 * 3.  PATTERN: Listens for a 16-bit pattern of 0xAAAA.
 * 4.  REGISTERS: Settings in R1, R5, R6, and R7 have been changed to support this new mode.
 * 5.  RC-CALIBRATION: Added automatic RC-oscillator calibration via SPI (fRC = 33250 Hz).
 *
 * --- Hardware Setup: ESP32-S3 Super Mini Pinout ---
 * +---------------------------+----------------+
 * | ESP32-S3 Super Mini Pin   | AS3933 Pin     |
 * +---------------------------+----------------+
 * | 3V3                       | VCC            |
 * | GND                       | GND            |
 * | G12 (MOSI)                | SDI            |
 * | G13 (MISO)                | SDO            |
 * | G11 (SCLK)                | SCL            |
 * | G10 (CS)                  | CS             |
 * | G8  (Wake Interrupt)      | WAKE           |
 * | G48 (Onboard LED)         | -              |
 * +---------------------------+----------------+
 ****************************************************************************************/

#include <SPI.h>

// === PIN DEFINITIONS ===
const int SPI_SCLK_PIN = D8;
const int SPI_MISO_PIN = D9;
const int SPI_MOSI_PIN = D10;
const int AS3933_CS_PIN = 40;
const int AS3933_WAKE_PIN = 39;
const int ONBOARD_LED_PIN = 21;

// === SPI SETTINGS ===
SPISettings spiSettings(2000000, MSBFIRST, SPI_MODE1);

// === INTERRUPT FLAG ===
volatile bool wakeUpDetected = false;

//=======================================================================================
//==                      USER CONFIGURABLE PARAMETERS (PATTERN MODE)                  ==
//== These values are set for maximum reliability using a 16-bit pattern at the        ==
//== lowest possible data rate.                                                      ==
//=======================================================================================
// Recommended AS3933 Configuration for 32kHz Carrier / ~500us Bit Duration
const byte CONFIG_R0 = 0b00000010; // PATT32=0 (16-bit), DAT_MASK=0, ON_OFF=0, MUX_123=0, CH1 Enabled.
// The correct one have to be const byte CONFIG_R1 = 0b01101010, Antenna dumper have to be disabled
const byte CONFIG_R1 = 0b01101010; // Dynamic slicer, AGC Up/Down, Manchester Decoder Enabled, Pattern Correlation Enabled, Crystal oscillator Disabled.
const byte CONFIG_R2 = 0b00100000; // +3dB Gain Boost, Relaxed frequency detection tolerance (16±6).
const byte CONFIG_R3 = 0b10111111; // FS_SCL=111 (3.5 ms preamble is required) (Slow Slicer), FS_ENV=111 (for ~512 symbols/s).
//Maybe we can enable R3<7> if HY_20m = 1 then comparator hysteresis = 20mV 
// 2.3 ms preamble is reqired at minimum R3<5:3> Data slicer time constant (see Figure 45) 

const byte CONFIG_R4 = 0b00000000; // No gain reduction (highest sensitivity), No antenna damper.
const byte CONFIG_R5 = 0x69;       //0b01,10,10,01(0110) PATT2B: Second byte of the 16-bit pattern (User Defined).
const byte CONFIG_R6 = 0x96;       //0b10,01,01,10(1001) PATT1B: First byte of the 16-bit pattern (User Defined).
const byte CONFIG_R7 = 0b11111111; // T_OUT=350ms, T_HBIT=18 -> ~500us bit duration with 36kHz clock. (R7<4:0>= 11111 Bit Duration in RTC Clock Periods is 32 that means 512 symbols per second )
const byte CONFIG_R8 = 0b11100000; // BAND_SEL=011 (23-40kHz band), Artificial Wake-up disabled.

// AS3933 Register Addresses
const byte R0  = 0x00; const byte R1  = 0x01; const byte R2  = 0x02; const byte R3  = 0x03;
const byte R4  = 0x04; const byte R5  = 0x05; const byte R6  = 0x06; const byte R7  = 0x07;
const byte R8  = 0x08; const byte R9  = 0x09; const byte R10 = 0x0A; const byte R11 = 0x0B;
const byte R12 = 0x0C; const byte R13 = 0x0D; const byte R14 = 0x0E; const byte R15 = 0x0F;
const byte R16 = 0x10; const byte R17 = 0x11; const byte R18 = 0x12; const byte R19 = 0x13;

// AS3933 Direct Commands
const byte PRESET_DEFAULT = 0x96; // This command code is 0x96, not 0x04.
const byte CLEAR_WAKE     = 0x00; // This is a command code, sent via sendDirectCommand.
const byte CALIB_RCOSC    = 0x02; // RC-oscillator calibration command (0x02 + 0x80 OR = 0x82 = 10000010, but you said 11000010=0xC2)

// Structure to hold register information
struct RegisterInfo {
  byte address;
  const char* name;
};

const RegisterInfo registers[] = {
  {R0, "R0 (CONF)"}, {R1, "R1 (WUP/PWR)"}, {R2, "R2 (GAIN/CLK)"}, {R3, "R3 (SLICER)"},
  {R4, "R4 (DAMP/GR)"}, {R5, "R5 (PATT2B)"}, {R6, "R6 (PATT1B)"}, {R7, "R7 (T_OUT/BIT)"},
  {R8, "R8 (BAND/AUTO)"}, {R9, "R9 (BLOCK_AGC)"}, {R10, "R10 (RSSI1)"}, {R11, "R11 (RSSI2)"},
  {R12, "R12 (RSSI3)"}, {R13, "R13 (F_WAKE)"}, {R14, "R14 (RC_CAL)"}, {R15, "R15 (LC_OSC)"},
  {R16, "R16 (CLK_MUX)"}, {R17, "R17 (CAP_CH1)"}, {R18, "R18 (CAP_CH2)"}, {R19, "R19 (CAP_CH3)"}
};
const int NUM_REGISTERS = sizeof(registers) / sizeof(RegisterInfo);

/**
 * @brief Interrupt Service Routine called when the WAKE pin goes HIGH.
 */
void IRAM_ATTR onWakeUp() {
  // Set the flag. Keep ISR as short as possible.
  wakeUpDetected = true;
}

/**
 * @brief Writes a byte to a specific register on the AS3933.
 * Address bits are 00AAAAAA for a write command.
 */
void writeRegister(byte address, byte value) {
  digitalWrite(AS3933_CS_PIN, HIGH);
  SPI.beginTransaction(spiSettings);
  SPI.transfer(address & 0x3F); // Mask to ensure top two bits are 00
  SPI.transfer(value);
  SPI.endTransaction();
  digitalWrite(AS3933_CS_PIN, LOW);
}

/**
 * @brief Reads a byte from a specific register on the AS3933.
 * Address bits are 01AAAAAA for a read command.
 */
byte readRegister(byte address) {
  byte readValue;
  digitalWrite(AS3933_CS_PIN, HIGH);
  SPI.beginTransaction(spiSettings);
  SPI.transfer(address | 0x40); // Set bit 6 to indicate a read command
  readValue = SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(AS3933_CS_PIN, LOW);
  return readValue;
}

/**
 * @brief Sends a direct command to the AS3933.
 * Address bits are 10CCCCCC for a direct command.
 */
void sendDirectCommand(byte command) {
  digitalWrite(AS3933_CS_PIN, HIGH);
  SPI.beginTransaction(spiSettings);
  // Per datasheet, direct commands have address bits 10CCCCCC.
  // So we OR the command with 0x80 (or 0b10000000).
  // The library example might be different, but datasheet is the source of truth.
  // Let's use the standard format: 10CCCCCC. PRESET_DEFAULT is 0x3C -> 10111100
  // CLEAR_WAKE is 0x00 -> 10000000
  SPI.transfer(command | 0x80);
  SPI.endTransaction();
  digitalWrite(AS3933_CS_PIN, LOW);
}

/**
 * @brief Calibrates the AS3933 RC-oscillator via SPI to target frequency fRC = 33250 Hz
 * According to datasheet: Set CS high, send Calib_RCosc command, provide 65 clock pulses, then CS low
 * Reference clock frequency should be calculated based on target fRC
 * For fRC = 33250 Hz, reference clock = fRC/4 = 8312.5 Hz (use ~8.3 kHz)
 */
void calibrateRCOscillator() {
  Serial.println("\nStarting RC-oscillator calibration (fRC = 33250 Hz)...");
  
  // Correct calculation based on carrier frequency (19 kHz)
  // fRC = fcarrier × (14/8) = 19000 × (14/8) = 33250 Hz
  // Period = 1/fRC = 1/33250 = 30.075 microseconds
  // Half period = 30.075/2 = 15.0375 microseconds
  const int halfPeriod_us = 15; // Half period for 33.25 kHz reference clock
  
  // Step 1: Set CS high before sending the calibration command
  digitalWrite(AS3933_CS_PIN, HIGH);
  delayMicroseconds(10); // Small delay to ensure CS is stable
  
  // Step 2: Send the Calib_RCosc direct command
  SPI.beginTransaction(spiSettings);
  // Send the exact command as specified: 11000010 = 0xC2
  SPI.transfer(0xC2); // Direct RC calibration command
  SPI.endTransaction();
  
  Serial.println("Sent RC calibration command: 0xC2 (11000010)");
  
  // Step 2.5: Wait a bit after command before starting clock pulses
  delayMicroseconds(100); // Allow AS3933 to process the command
  
  // Step 3: Generate 65 clock pulses on SCLK while keeping CS high
  // We need to manually toggle the SCLK pin to provide the reference clock
  
  // First, end the current SPI transaction and take manual control
  SPI.end();
  pinMode(SPI_SCLK_PIN, OUTPUT);
  digitalWrite(SPI_SCLK_PIN, LOW); // Start with SCLK low
  
  // Ensure clean initial state
  delayMicroseconds(50);
  
  Serial.println("Providing 65 reference clock pulses at 33.25 kHz...");
  for (int i = 0; i < 65; i++) {
    // Rising edge
    digitalWrite(SPI_SCLK_PIN, HIGH);
    delayMicroseconds(halfPeriod_us);
    
    // Falling edge  
    digitalWrite(SPI_SCLK_PIN, LOW);
    delayMicroseconds(halfPeriod_us);
  }
  Serial.println(" Done!");
  
  // Step 4: Ensure SCLK ends in a clean low state
  digitalWrite(SPI_SCLK_PIN, LOW);
  delayMicroseconds(100);
  
  // Step 5: Pull CS low to complete the calibration
  digitalWrite(AS3933_CS_PIN, LOW);
  
  // Step 6: Properly restore SPI functionality
  // Give more time for the calibration to settle and complete
  delay(50); // Increased settling time
  
  // Reinitialize SPI completely
  SPI.begin(SPI_SCLK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, -1);
  
  // Ensure CS is in the correct state for normal operation
  digitalWrite(AS3933_CS_PIN, LOW);
  delay(1); // Small delay to ensure SPI is ready
  
  Serial.println("RC-oscillator calibration completed!");
  
  // Test SPI communication before reading R14
  Serial.println("Testing SPI communication after calibration...");
  
  // Try reading a known register first (R0) to verify SPI is working
  byte r0_test = readRegister(R0);
  Serial.print("SPI Test - R0 value: 0x");
  Serial.println(r0_test, HEX);
  
  // Read R14 to check calibration result
  Serial.println("Reading R14 calibration result...");
  byte r14_val = readRegister(R14);
  Serial.print("Calibration result (R14): 0x");
  Serial.print(r14_val, HEX);
  Serial.print(" (");
  printBinary(r14_val);
  Serial.println(")");
  
  // Decode R14 register according to datasheet
  bool rc_cal_ok = (r14_val & 0x80) != 0;        // R14<7> RC_CAL_OK
  bool rc_cal_ko = (r14_val & 0x40) != 0;        // R14<6> RC_CAL_KO  
  byte rc_osc_taps = r14_val & 0x3F;             // R14<5:0> RC_OSC_TAPS
  
  Serial.println("\n--- RC Calibration Status ---");
  Serial.print("RC_CAL_OK (R14<7>): ");
  Serial.println(rc_cal_ok ? "SUCCESS - Calibration completed successfully" : "FAILED - Calibration not successful");
  
  Serial.print("RC_CAL_KO (R14<6>): ");
  Serial.println(rc_cal_ko ? "ERROR - Unsuccessful calibration" : "OK - No calibration error");
  
  Serial.print("RC_OSC_TAPS (R14<5:0>): ");
  Serial.print(rc_osc_taps);
  Serial.println(" - RC-Oscillator taps setting");
  
  // Overall calibration assessment
  if (rc_cal_ok && !rc_cal_ko) {
    Serial.println("\n✓ RC-OSCILLATOR CALIBRATION: SUCCESS");
    Serial.print("  Oscillator trimmed with taps value: ");
    Serial.println(rc_osc_taps);
  } else if (rc_cal_ko && rc_cal_ok) {
    Serial.println("\n⚠ RC-OSCILLATOR CALIBRATION: CONTRADICTORY STATE");
    Serial.println("  Both RC_CAL_OK and RC_CAL_KO are set - this indicates:");
    Serial.println("  - Reference clock frequency may be incorrect");
    Serial.println("  - Clock timing/quality issues");
    Serial.println("  - Try adjusting reference clock frequency");
    Serial.print("  Current taps value: ");
    Serial.println(rc_osc_taps);
  } else if (rc_cal_ko) {
    Serial.println("\n✗ RC-OSCILLATOR CALIBRATION: FAILED");
    Serial.println("  Error: Unsuccessful calibration detected");
    Serial.println("  Check reference clock frequency and timing");
  } else {
    Serial.println("\n⚠ RC-OSCILLATOR CALIBRATION: UNCERTAIN");
    Serial.println("  Warning: RC_CAL_OK not set, calibration may not have completed properly");
  }
}

/**
 * @brief Helper function to print a byte as an 8-bit binary string.
 */
void printBinary(byte value) {
  Serial.print("0b");
  for (int i = 7; i >= 0; i--) {
    Serial.print(bitRead(value, i));
  }
}

/**
 * @brief Prints all register values in a formatted table with binary values.
 */
void dumpRegisters() {
  Serial.println("\n--- AS3933 Register Dump ---");
  Serial.println("==============================================================");
  Serial.println("| Addr | Register Name  |   Value (Hex) | Value (Binary)   |");
  Serial.println("==============================================================");

  for (int i = 0; i < NUM_REGISTERS; i++) {
    byte value = readRegister(registers[i].address);
    char buffer[100];
    sprintf(buffer, "| 0x%02X | %-14s |     0x%02X      | ", registers[i].address, registers[i].name, value);
    Serial.print(buffer);
    printBinary(value);
    Serial.println(" |");
  }
  Serial.println("==============================================================");
}

void setup() {
  Serial.begin(115200);
  //while (!Serial);
  Serial.println("\nAS3933 Configuration Sketch Initializing for ESP32-S3...");
  Serial.println("Mode: Pattern Correlation / Channel 1 / ~244 bps");

  // Initialize Onboard LED
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, LOW); // Start with the LED off

  // Initialize SPI with custom pins
  pinMode(AS3933_CS_PIN, OUTPUT);
  digitalWrite(AS3933_CS_PIN, LOW);
  SPI.begin(SPI_SCLK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, -1);

  // Initialize Interrupt Pin
  pinMode(AS3933_WAKE_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(AS3933_WAKE_PIN), onWakeUp, RISING);

  delay(100);

  // --- 1. Verify SPI Communication ---
  Serial.println("\nStep 1: Verifying SPI communication...");
  sendDirectCommand(0x3C); // PRESET_DEFAULT command code is 0x3C
  delay(10);
  byte r5_val = readRegister(R5);
  byte r6_val = readRegister(R6);
  Serial.print("Reading default pattern registers... R5=0x");
  Serial.print(r5_val, HEX);
  Serial.print(", R6=0x");
  Serial.println(r6_val, HEX);
  if (r5_val == 0x69 && r6_val == 0x96) {
    Serial.println("SUCCESS: AS3933 identified correctly!");
  } else {
    Serial.println("ERROR: Failed to communicate with AS3933. Check wiring. Halting.");
    while (1);
  }

  // --- 2. Configure Registers ---
  Serial.println("\nStep 2: Configuring registers for pattern mode...");
  writeRegister(R0, CONFIG_R0);
  writeRegister(R1, CONFIG_R1);
  writeRegister(R2, CONFIG_R2);
  writeRegister(R3, CONFIG_R3);
  writeRegister(R4, CONFIG_R4);
  writeRegister(R5, CONFIG_R5); // Set pattern byte 2
  writeRegister(R6, CONFIG_R6); // Set pattern byte 1
  writeRegister(R7, CONFIG_R7); // Set data rate
  writeRegister(R8, CONFIG_R8);
  Serial.println("Configuration complete.");

  // --- 3. Calibrate RC-oscillator ---
  Serial.println("\nStep 3: RC-oscillator calibration...");
  // Check if pattern detection and Manchester decoder are enabled (R1<1>=0 and R1<3>=1)
  // If both are true, calibration is needed according to datasheet
  byte r1_current = readRegister(R1);
  bool patternDetectionDisabled = (r1_current & 0x02) == 0; // R1<1>=0
  bool manchesterEnabled = (r1_current & 0x08) != 0;        // R1<3>=1
  
  if (!patternDetectionDisabled || manchesterEnabled) {
    Serial.println("Pattern detection or Manchester decoder enabled - RC calibration required");
    calibrateRCOscillator();
  } else {
    Serial.println("Pattern detection and Manchester decoder disabled - RC calibration skipped");
  }

  // --- 4. Dump Registers for Verification ---
  dumpRegisters();

  Serial.println("\nSetup finished. The AS3933 is configured.");
  Serial.println("Waiting for a Wake-Up Pattern (0xAAAA) on Pin G8...");
}

void loop() {
  if (wakeUpDetected) {
    // Blink the onboard LED for 2ms to make it more visible
    digitalWrite(ONBOARD_LED_PIN, HIGH);
    delay(2);
    digitalWrite(ONBOARD_LED_PIN, LOW);

    Serial.println("\n---------------------------------");
    Serial.println(">>> Wake-Up Pattern Detected! <<<");

    // Read the RSSI value from the active channel (Channel 1)
    byte rssi_val = readRegister(R10);
    Serial.print("Channel 1 RSSI: ");
    Serial.print(rssi_val);
    Serial.print(" (0b");
    for (int i = 4; i >= 0; i--) { // RSSI is a 5-bit value
      Serial.print(bitRead(rssi_val, i));
    }
    Serial.println(")");

    // Read wake-up reason from R13
    byte wake_reason = readRegister(R13);
    Serial.print("Wake-up reason (R13): ");
    printBinary(wake_reason);
    if(bitRead(wake_reason, 7)) Serial.print(" - Pattern match");
    if(bitRead(wake_reason, 6)) Serial.print(" - Freq detected");
    Serial.println();


    // IMPORTANT: Clear the wake-up condition to reset the WAKE pin
    // and allow the chip to detect the next event.
    Serial.println("Sending CLEAR_WAKE command...");
    sendDirectCommand(0x00); // CLEAR_WAKE command code is 0x00

    Serial.println("---------------------------------");
    Serial.println("\nWaiting for next Wake-Up Pattern...");

    // Reset the flag
    wakeUpDetected = false;
  }
}
