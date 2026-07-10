/*
 * ISL Board (RAK_feather) - AS3933 Wake-Up Receiver bring-up  [test #4]
 * (alive-first structure - see ISL_RTC_Read: this board's native-USB CDC wedges
 *  if setup() does long blocking work / early peripheral init, so setup() is tiny
 *  and the real init runs a few seconds into loop() with flushed markers.)
 *
 * Ported from reference/AS3933_wakeup/WuRx_Pattern_blink (ESP32) to RAK4630 (RUI3).
 * 16-bit PATTERN mode (pattern 0x9669, ~244 bps, 23-40 kHz), RC-osc calibration,
 * register dump, then reports each wake event. SPI is BIT-BANGED (AS3933 uses an
 * active-HIGH CS and needs manual clocking for RC calibration anyway).
 *
 * ISL wiring (docs/ISL_Pinout.md):
 *   Serial (USB-C CDC, COM50) -> PC (115200)
 *   SPI CLK=P0.03  MOSI=P0.30->SDI  MISO=P0.29<-SDO  CS=P0.26 (ACTIVE-HIGH)
 *   WAKE=P1.04 <- AS3933 WAKE (rising edge on detect)
 * TX side: reference/AS3933_wakeup/WuTx*.  First check: PRESET -> R5=0x69,R6=0x96.
 */

#include <Arduino.h>

#define PIN_SCLK   P0_03
#define PIN_MOSI   P0_30
#define PIN_MISO   P0_29
#define PIN_CS     P0_26
#define PIN_WAKE   P1_04
#define SPI_HALF_US  5
#define WARMUP_MS    3000

enum { R0=0x00,R1,R2,R3,R4,R5,R6,R7,R8,R9,R10,R11,R12,R13,R14 };

static const uint8_t CFG[9] = {
  0b00000010, 0b01101010, 0b00100000, 0b10111111, 0b00000000,
  0x69,       0x96,       0b11111111, 0b11100000
};

volatile bool wakeUpDetected = false;
void onWakeUp() { wakeUpDetected = true; }

enum Phase { WARMUP, INIT, RUN };
Phase phase = WARMUP;

void say(const char *s) { Serial.println(s); Serial.flush(); }

// ---- bit-banged SPI, Mode 1 (CPOL=0, CPHA=1), MSB first ----
static uint8_t spiXfer(uint8_t out)
{
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--)
  {
    digitalWrite(PIN_MOSI, (out >> i) & 1);
    digitalWrite(PIN_SCLK, HIGH); delayMicroseconds(SPI_HALF_US);
    digitalWrite(PIN_SCLK, LOW);
    in = (in << 1) | (digitalRead(PIN_MISO) & 1);
    delayMicroseconds(SPI_HALF_US);
  }
  return in;
}
// AS3933 CS is ACTIVE-HIGH: assert HIGH for a frame, release LOW when idle.
void    writeReg(uint8_t a, uint8_t v){ digitalWrite(PIN_CS,HIGH); spiXfer(a&0x3F); spiXfer(v); digitalWrite(PIN_CS,LOW); }
uint8_t readReg (uint8_t a){ digitalWrite(PIN_CS,HIGH); spiXfer(a|0x40); uint8_t v=spiXfer(0); digitalWrite(PIN_CS,LOW); return v; }
void    directCmd(uint8_t c){ digitalWrite(PIN_CS,HIGH); spiXfer(c|0x80); digitalWrite(PIN_CS,LOW); }

void calibrateRC()
{
  say("STEP: RC-osc calibration (fRC ~ 33.25 kHz)...");
  digitalWrite(PIN_CS, HIGH); delayMicroseconds(10);
  spiXfer(0xC2); delayMicroseconds(100);
  for (int i = 0; i < 65; i++) { digitalWrite(PIN_SCLK,HIGH); delayMicroseconds(15); digitalWrite(PIN_SCLK,LOW); delayMicroseconds(15); }
  digitalWrite(PIN_CS, LOW); delay(5);
  uint8_t r14 = readReg(R14);
  Serial.printf("  R14=0x%02X RC_CAL_OK=%d RC_CAL_KO=%d taps=%u\r\n",
                r14, (r14&0x80)?1:0, (r14&0x40)?1:0, r14&0x3F); Serial.flush();
}

void initWUR()
{
  say("STEP: GPIO + interrupt setup ...");
  pinMode(PIN_SCLK, OUTPUT); digitalWrite(PIN_SCLK, LOW);
  pinMode(PIN_MOSI, OUTPUT); digitalWrite(PIN_MOSI, LOW);
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_CS,   OUTPUT); digitalWrite(PIN_CS, LOW);  // idle low (active-high CS)
  pinMode(PIN_WAKE, INPUT);
  attachInterrupt(PIN_WAKE, onWakeUp, RISING);
  delay(50);

  say("STEP: PRESET + comms check ...");
  directCmd(0x3C); delay(10);
  uint8_t r5 = readReg(R5), r6 = readReg(R6);
  Serial.printf("  PRESET check: R5=0x%02X R6=0x%02X -> %s\r\n",
                r5, r6, (r5==0x69 && r6==0x96) ? "AS3933 OK" : "NO (check SPI wiring)");
  Serial.flush();

  say("STEP: write pattern-mode config ...");
  for (uint8_t a = R0; a <= R8; a++) writeReg(a, CFG[a]);

  calibrateRC();

  say("--- register dump R0..R8 ---");
  for (uint8_t a = R0; a <= R8; a++) { Serial.printf("  R%-2u = 0x%02X\r\n", a, readReg(a)); Serial.flush(); }
  say("Ready. Waiting for a wake-up pattern on P1.04 ...");
}

void setup()
{
  Serial.begin(115200);
  { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
  delay(300);
  say("ISL AS3933 WUR (test #4, alive-first). Heartbeat, then init...");
}

void loop()
{
  static uint32_t t0 = millis();
  static uint32_t lastHb = 0;

  if (phase == WARMUP)
  {
    if (millis() - lastHb >= 500) { lastHb = millis(); Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis()-t0)); Serial.flush(); }
    if (millis() - t0 >= WARMUP_MS) phase = INIT;
    return;
  }
  if (phase == INIT) { initWUR(); phase = RUN; return; }

  // RUN
  if (!wakeUpDetected) { delay(5); return; }
  wakeUpDetected = false;
  uint8_t rssi = readReg(R10) & 0x1F;
  uint8_t why  = readReg(R13);
  Serial.printf("\r\n>>> WAKE-UP  RSSI(ch1)=%u  R13=0x%02X%s%s\r\n", rssi, why,
                (why&0x80)?" [pattern-match]":"", (why&0x40)?" [freq-detect]":"");
  directCmd(0x00);   // CLEAR_WAKE
  say("  cleared, waiting for next...");
}
