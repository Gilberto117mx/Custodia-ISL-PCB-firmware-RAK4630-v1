/*
 * ISL Board (RAK_feather) - GNSS on Serial0 / UART1  [test #3h - rev2]
 *
 * CHANGE from rev1: the updated schematic (RAK_feather_1.pdf) moved the GPS power
 * enable L76K_EN from P0.04 -> **P1.02** (pin 26, P1.02/SW2). rev1 drove P0.04,
 * which on this board goes to a bare header and switches nothing, so the L76K was
 * never powered -> 0 bytes. Only the EN pin changes; the port was already correct.
 *
 * Port: GPS is on UART1 = P0.19(RX)/P0.20(TX), which in RUI3 is *Serial0* (NOT
 * Serial1 = UART2 @ P0.15/16, NOT Serial2 which hangs). Open in RAK_CUSTOM_MODE so
 * the AT interpreter doesn't eat the NMEA.
 *
 * ISL board wiring (verified against RAK_feather_1.pdf):
 *   Serial  = native USB-C CDC (COM50) -> PC debug
 *   Serial0 = UART1 (P0.19 RX / P0.20 TX) -> L76K GPS @ 9600, RAK_CUSTOM_MODE
 *   GPS EN  = P1.02, active-LOW (Q1 AO3407 P-FET high-side, R4 10k gate pull-up)
 *
 * Expect $GNRMC / $GNGGA (or $GP...) within ~10 s now that power is real.
 */

#include <Arduino.h>

#define GPS_EN_PIN          P1_02     // <-- was P0_04; moved per updated schematic
#define GPS_EN_ACTIVE_LOW   1         // Q1 P-FET high-side, 10k gate pull-up -> LOW = ON
#define GPS_BAUD            9600
#define WARMUP_MS           3000

void say(const char *s) { Serial.println(s); Serial.flush(); }

enum Phase { WARMUP, INIT, RUN };
Phase phase = WARMUP;

void setup()
{
  Serial.begin(115200);                        // USB-C CDC -> PC (ISL debug)
  { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
  delay(300);
  say("ISL GNSS on Serial0/UART1, EN=P1.02 (rev2). Heartbeat, then GPS init...");
}

void loop()
{
  static uint32_t t0 = millis();
  static uint32_t lastHb = 0;
  static uint32_t lastByte = 0;
  static uint32_t lastStatus = 0;
  static uint32_t bytes = 0;

  if (phase == WARMUP)
  {
    if (millis() - lastHb >= 500)
    {
      lastHb = millis();
      Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis() - t0));
      Serial.flush();
    }
    if (millis() - t0 >= WARMUP_MS) phase = INIT;
    return;
  }

  if (phase == INIT)
  {
    say("STEP 1: GPS power ON (P1.02 LOW, active-low)...");
    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, GPS_EN_ACTIVE_LOW ? LOW : HIGH);
    delay(500);
    say("STEP 1 done");

    say("STEP 2: Serial0.begin(9600, RAK_CUSTOM_MODE)  [UART1 = P0.19/P0.20]...");
    Serial0.begin(GPS_BAUD, RAK_CUSTOM_MODE);
    say("STEP 2 done - UART1 open. Raw NMEA below (expect $GxRMC/$GxGGA):");
    say("  ------------------------------------------------------------");
    lastByte = millis();
    lastStatus = millis();
    phase = RUN;
    return;
  }

  // RUN: forward every GPS byte to USB, print a status line every 3 s.
  while (Serial0.available())
  {
    Serial.write((uint8_t)Serial0.read());
    bytes++;
    lastByte = millis();
  }

  if (millis() - lastStatus >= 3000)
  {
    lastStatus = millis();
    Serial.printf("\r\n[status] %lu bytes total, %lu ms since last byte%s\r\n",
                  (unsigned long)bytes,
                  (unsigned long)(millis() - lastByte),
                  bytes == 0 ? "  (still 0 - if EN=P1.02 is right, check the J6 cable)" : "");
    Serial.flush();
  }
}
