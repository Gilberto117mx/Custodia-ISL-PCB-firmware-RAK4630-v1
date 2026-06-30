/****************************************************************************************
 * AS3933 ESP32-S3 Configuration & Register Dump
 * * This version is configured for MAXIMUM SENSITIVITY on a single channel.
 * * It is specifically adapted for the ESP32-S3 Super Mini board.
 * * It now includes an interrupt to monitor the WAKE pin and prints register
 * * values in binary.
 * * It also blinks the onboard LED for 0.2ms when a wake-up is detected.
 * * * Hardware Setup: ESP32-S3 Super Mini Pinout (MODIFIED)
 * +---------------------------+----------------+
 * | ESP32-S3 Super Mini Pin   | AS3933 Pin     |
 * +---------------------------+----------------+
 * | 3V3                       | VCC            |
 * | GND                       | GND            |
 * | G12 (MOSI)                | SDI            |
 * | G13 (MISO)                | SDO            |
 * | G11 (SCLK)                | SCL            |
 * | G10 (CS)                  | CS             |
 * | G7  (Wake Interrupt)      | WAKE           |
 * | G21 (Onboard LED)         | -              |
 * +---------------------------+----------------+
 * ****************************************************************************************/

#include <SPI.h>

// === PIN DEFINITIONS (MODIFIED) ===
const int SPI_SCLK_PIN = 11; // ** NEW: SCLK is now on G11 **
const int SPI_MISO_PIN = 13; // MISO remains on G13
const int SPI_MOSI_PIN = 12; // ** NEW: MOSI is now on G12 **
const int AS3933_CS_PIN = 10; 
const int AS3933_WAKE_PIN = 8;   // ** NEW: Interrupt is now on G7 **
const int ONBOARD_LED_PIN = 48;  // Onboard LED pin for ESP32-S3 Super Mini

// === SPI SETTINGS ===
SPISettings spiSettings(2000000, MSBFIRST, SPI_MODE1);

// === INTERRUPT FLAG ===
// A volatile boolean flag to be set by the ISR
volatile bool wakeUpDetected = false;

//=======================================================================================
//==                           USER CONFIGURABLE PARAMETERS                          ==
//==   These values are set to maximize sensitivity on a single channel.           ==
//=======================================================================================
// Changes in CONFIG constants
const byte CONFIG_R0 = 0b00000010; // Channel 1 only
const byte CONFIG_R1 = 0b00100000; // No pattern, no Manchester, freq-only
const byte CONFIG_R2 = 0b00100000; // +3dB gain, relaxed tolerance (R2<1:0> = 00)
const byte CONFIG_R3 = 0b00100000; // Slow env=000, Fast env=100 (burst-optimized)
const byte CONFIG_R4 = 0b00000000; // Max gain
const byte CONFIG_R5 = 0x69;       // Default, not used for wake-up
const byte CONFIG_R6 = 0x96;       // Default, not used for wake-up
const byte CONFIG_R7 = 0b00100000; // Bit rate irrelevant in freq-only mode
const byte CONFIG_R8 = 0b11100000; // Band 5 (15–23kHz)


//=======================================================================================

// AS3933 Register Addresses
const byte R0  = 0x00; const byte R1  = 0x01; const byte R2  = 0x02; const byte R3  = 0x03;
const byte R4  = 0x04; const byte R5  = 0x05; const byte R6  = 0x06; const byte R7  = 0x07;
const byte R8  = 0x08; const byte R9  = 0x09; const byte R10 = 0x0A; const byte R11 = 0x0B;
const byte R12 = 0x0C; const byte R13 = 0x0D; const byte R14 = 0x0E; const byte R15 = 0x0F;
const byte R16 = 0x10; const byte R17 = 0x11; const byte R18 = 0x12; const byte R19 = 0x13;

// AS3933 Direct Commands
const byte PRESET_DEFAULT = 0x04;
const byte CLEAR_WAKE     = 0x00;

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
 */
void writeRegister(byte address, byte value) {
  digitalWrite(AS3933_CS_PIN, HIGH);
  SPI.beginTransaction(spiSettings);
  SPI.transfer(address & 0x3F); 
  SPI.transfer(value);
  SPI.endTransaction();
  digitalWrite(AS3933_CS_PIN, LOW);
}

/**
 * @brief Reads a byte from a specific register on the AS3933.
 */
byte readRegister(byte address) {
  byte readValue;
  digitalWrite(AS3933_CS_PIN, HIGH);
  SPI.beginTransaction(spiSettings);
  SPI.transfer(address | 0x40); 
  readValue = SPI.transfer(0x00); 
  SPI.endTransaction();
  digitalWrite(AS3933_CS_PIN, LOW);
  return readValue;
}

/**
 * @brief Sends a direct command to the AS3933.
 */
void sendDirectCommand(byte command) {
  digitalWrite(AS3933_CS_PIN, HIGH);
  SPI.beginTransaction(spiSettings);
  SPI.transfer(command | 0xC0);
  SPI.endTransaction();
  digitalWrite(AS3933_CS_PIN, LOW);
}

/**
 * @brief Helper function to print a byte as an 8-bit binary string.
 * @param value The byte to print.
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
  while (!Serial);
  Serial.println("\nAS3933 Configuration Sketch Initializing for ESP32-S3...");
  Serial.println("Mode: Maximum Sensitivity / Channel 1 / Freq-Only Wake-Up");

  // Initialize Onboard LED
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, LOW); // Start with the LED off

  // Initialize SPI with custom pins
  pinMode(AS3933_CS_PIN, OUTPUT);
  digitalWrite(AS3933_CS_PIN, LOW);
  // ** NEW: Initialize SPI with custom SCLK/MOSI pins. Pass -1 for CS because we handle it manually. **
  SPI.begin(SPI_SCLK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, -1); 

  // Initialize Interrupt Pin
  // The WAKE pin is active-high, so a pulldown keeps it from floating.
  pinMode(AS3933_WAKE_PIN, INPUT_PULLDOWN);
  // Attach interrupt to trigger on the RISING edge (LOW to HIGH)
  attachInterrupt(digitalPinToInterrupt(AS3933_WAKE_PIN), onWakeUp, RISING);

  delay(100);

  // --- 1. Verify SPI Communication ---
  Serial.println("\nStep 1: Verifying SPI communication...");
  sendDirectCommand(PRESET_DEFAULT);
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
  Serial.println("\nStep 2: Configuring registers with max-sensitivity values...");
  writeRegister(R0, CONFIG_R0);
  writeRegister(R1, CONFIG_R1);
  writeRegister(R2, CONFIG_R2);
  writeRegister(R3, CONFIG_R3);
  writeRegister(R4, CONFIG_R4);
  writeRegister(R5, CONFIG_R5);
  writeRegister(R6, CONFIG_R6);
  writeRegister(R7, CONFIG_R7);
  writeRegister(R8, CONFIG_R8);
  Serial.println("Configuration complete.");

  // --- 3. Dump Registers for Verification ---
  dumpRegisters();

  Serial.println("\nSetup finished. The AS3933 is configured.");
  // ** NEW: Updated message to reflect new interrupt pin **
  Serial.println("Waiting for a Wake-Up Call (WuC) on Pin G7...");
}

void loop() {
  // Check if the interrupt flag has been set
  if (wakeUpDetected) {
    // Blink the onboard LED for 0.2ms (200 microseconds)
    digitalWrite(ONBOARD_LED_PIN, HIGH);
    delayMicroseconds(200);
    digitalWrite(ONBOARD_LED_PIN, LOW);

    Serial.println("\n---------------------------------");
    Serial.println(">>> Wake-Up Call (WuC) Detected! <<<");

    // Read the RSSI value from the active channel (Channel 1)
    byte rssi_val = readRegister(R10);
    Serial.print("Channel 1 RSSI: ");
    Serial.print(rssi_val);
    Serial.print(" (0b");
    for (int i = 4; i >= 0; i--) { // RSSI is a 5-bit value
      Serial.print(bitRead(rssi_val, i));
    }
    Serial.println(")");

    // IMPORTANT: Clear the wake-up condition to reset the WAKE pin
    // and allow the chip to detect the next event.
    Serial.println("Sending CLEAR_WAKE command...");
    sendDirectCommand(CLEAR_WAKE);
    
    Serial.println("---------------------------------");
    Serial.println("\nWaiting for next Wake-Up Call...");

    // Reset the flag
    wakeUpDetected = false;
  }

  // The main loop can perform other non-blocking tasks here.
  // delay(10); // A small delay can be added if needed.
}
