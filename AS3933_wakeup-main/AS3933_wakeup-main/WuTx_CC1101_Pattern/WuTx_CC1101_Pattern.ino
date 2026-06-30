#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <math.h>

// --- Matched timings & pattern to support AS3933 pattern mode ---
// CC1101 will transmit OOK at a data rate that matches AS3933 T_HBIT selection.

const unsigned long CARRIER_FREQUENCY_MHZ = 450; // unchanged
const float DATA_RATE_KBPS = 1000; // <-- set to 8.192 kbps to match AS3933 T_HBIT=4
// choose burst bits to safely exceed min burst for band 15-23 kHz (datasheet)
const int BURST_BITS = 40;    // ~30 bits of '1' at 8.192 kbps -> burst ≈ 30 * 122.07us ≈ 3.66 ms
const int SEPARATION_BITS = 1; // one 0 bit (half Manchester symbol)
const int PREAMBLE_BITS = 20;   // 8 alternating bits (>= datasheet min 6; meets R3=000 min 0.8 ms)
                                // note: with our bit period preamble ~ 8 * 122us = 976 us (~0.976 ms)
const byte PATTERN_BYTE1 = 0x96; // R6 (must match receiver R6)
const byte PATTERN_BYTE2 = 0x69; // R5 (must match receiver R5)

#define PACKET_BUF_MAX 128
uint8_t packetBuf[PACKET_BUF_MAX];

int gdo0;
#ifdef ESP32
  #define DEFAULT_GDO0 2
#elif defined(ESP8266)
  #define DEFAULT_GDO0 5
#else
  #define DEFAULT_GDO0 6
#endif

// compute bit period and derived numbers (for diagnostics)
float bitPeriodUs() {
  return 1.0f / (DATA_RATE_KBPS * 1000.0f) * 1e6f; // µs
}

// helper: push single bit (MSB-first packing into bytes)
void pushBit(uint8_t bit, uint8_t *buf, int &bitPtr) {
  int byteIndex = bitPtr >> 3;
  int bitInByte = 7 - (bitPtr & 0x07);
  if (byteIndex >= PACKET_BUF_MAX) return;
  if (bit) buf[byteIndex] |= (1 << bitInByte);
  bitPtr++;
}

// build WuC: burst(1s) + sep(0) + preamble(1010...) + pattern (PATTERN_BYTE1 then PATTERN_BYTE2 MSB-first)
int buildWakePacket() {
  // clear
  for (int i = 0; i < PACKET_BUF_MAX; ++i) packetBuf[i] = 0x00;

  int totalBits = BURST_BITS + SEPARATION_BITS + PREAMBLE_BITS + 16;
  int totalBytes = (totalBits + 7) >> 3;
  if (totalBytes > PACKET_BUF_MAX) return 0;

  int bitPtr = 0;
  // 1) burst: series of ones
  for (int i = 0; i < BURST_BITS; ++i) pushBit(1, packetBuf, bitPtr);
  // 2) separation: single zero
  for (int i = 0; i < SEPARATION_BITS; ++i) pushBit(0, packetBuf, bitPtr);
  // 3) preamble: alternating starting with 1 => 1010...
  for (int i = 0; i < PREAMBLE_BITS; ++i) {
    pushBit((i & 1) ? 0 : 1, packetBuf, bitPtr);
  }
  // 4) pattern: PATTERN_BYTE1 then PATTERN_BYTE2 (MSB-first)
  for (int b = 7; b >= 0; --b) pushBit( (PATTERN_BYTE1 >> b) & 0x01, packetBuf, bitPtr );
  for (int b = 7; b >= 0; --b) pushBit( (PATTERN_BYTE2 >> b) & 0x01, packetBuf, bitPtr );

  return totalBytes;
}

void transmitWakeOnce() {
  int packetLen = buildWakePacket();
  if (packetLen <= 0) {
    Serial.println("Packet build error / too large.");
    return;
  }
  Serial.print("Sending WuC single-packet (bytes): "); Serial.println(packetLen);
  ELECHOUSE_cc1101.SendData(packetBuf, packetLen);
}

void setup() {
  gdo0 = DEFAULT_GDO0;
  Serial.begin(9600);
  while (!Serial && millis() < 2000);

  Serial.println("--- TX: building single-packet WuC (burst+sep+preamble+pattern) ---");
  Serial.print("Bit period (us): "); Serial.println(bitPeriodUs());
  Serial.print("BURST_BITS: "); Serial.println(BURST_BITS);
  Serial.print("PREAMBLE_BITS: "); Serial.println(PREAMBLE_BITS);
  Serial.print("SEPARATION_BITS: "); Serial.println(SEPARATION_BITS);
  Serial.print("Pattern (R6,R5): 0x"); Serial.print(PATTERN_BYTE1, HEX); Serial.print(",0x"); Serial.println(PATTERN_BYTE2, HEX);

  if (ELECHOUSE_cc1101.getCC1101()) Serial.println("CC1101 SPI OK"); else { Serial.println("CC1101 ERROR"); while(true); }

  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setGDO0(gdo0);
  ELECHOUSE_cc1101.setCCMode(1);
  ELECHOUSE_cc1101.setModulation(2);             // ASK/OOK
  ELECHOUSE_cc1101.setMHZ(CARRIER_FREQUENCY_MHZ);
  ELECHOUSE_cc1101.setDRate(DATA_RATE_KBPS);     // IMPORTANT: match AS3933 T_HBIT
  ELECHOUSE_cc1101.setSyncMode(0);               // disable radio preamble/sync (we send our own)
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setPA(12);

  Serial.println("TX init complete.");
}

void loop() {
  transmitWakeOnce();
  delay(100); // your original retransmission delay
}
