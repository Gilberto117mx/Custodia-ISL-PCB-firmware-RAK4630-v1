/*
 * ISL Board (RAK_feather) - GNSS monitor  [test #3 - v3 ALIVE-FIRST diagnostic]
 *
 * IMPORTANT UPLOAD NOTE: CLOSE the Serial Monitor before clicking Upload. A
 * monitor holding the COM port open causes the DFU "No ping response" failure.
 * Open the monitor again only after "Device programmed".
 *
 * setup() does NOTHING but bring up USB serial, so this sketch cannot wedge the
 * board before it proves it is alive. loop() heartbeats for 5 s, THEN powers the
 * GPS and opens the UART with flushed STEP markers, THEN streams NMEA. If
 * GPS.begin() is the hang, the last line you see will be "STEP 2: GPS.begin ...".
 *
 *   Serial (USB-C CDC) -> PC (115200)   [debug]
 *   GPS UART -> P0.19 / P0.20   |   GPS EN -> P0.04
 */

#include <Arduino.h>

#define GPS_EN_PIN          P0_04
#define GPS_EN_ACTIVE_LOW   1        // 1 = drive LOW to turn GPS ON. Flip if needed.
#define GPS_BAUD            9600
#define USE_SERIAL2         0        // 0 = Serial1, 1 = Serial2
#define GPS_CUSTOM_MODE     1        // 1 = RAK_CUSTOM_MODE (raw), 0 = plain begin(baud)
#define WARMUP_MS           5000

#if USE_SERIAL2
  #define GPS  Serial2
#else
  #define GPS  Serial1
#endif

void say(const char *s) { Serial.println(s); Serial.flush(); }

enum Phase { WARMUP, GPSINIT, STREAM };
Phase phase = WARMUP;

void setup()
{
  Serial.begin(115200);
  { uint32_t _t = millis(); while (!Serial && (millis() - _t) < 4000) delay(10); }
  delay(500);
  say("ISL GNSS v3 (alive-first). Heartbeat 5 s, then GPS init...");
}

void loop()
{
  static uint32_t t0 = millis();
  static uint32_t lastHb = 0;
  static uint32_t lastByte = 0;

  if (phase == WARMUP)
  {
    if (millis() - lastHb >= 500)
    {
      lastHb = millis();
      Serial.printf("[alive] t=%lu ms\r\n", (unsigned long)(millis() - t0));
      Serial.flush();
    }
    if (millis() - t0 >= WARMUP_MS) phase = GPSINIT;
    return;
  }

  if (phase == GPSINIT)
  {
    Serial.printf("UART=%s  EN active-%s  customMode=%d\r\n",
                  USE_SERIAL2 ? "Serial2" : "Serial1",
                  GPS_EN_ACTIVE_LOW ? "LOW" : "HIGH", GPS_CUSTOM_MODE);
    Serial.flush();

    say("STEP 1: gpsPower(true)");
    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, GPS_EN_ACTIVE_LOW ? LOW : HIGH);
    delay(300);
    say("STEP 1 done");

    say("STEP 2: GPS.begin ...");
#if GPS_CUSTOM_MODE
    GPS.begin(GPS_BAUD, RAK_CUSTOM_MODE);
#else
    GPS.begin(GPS_BAUD);
#endif
    say("STEP 2 done (UART open) - streaming / heartbeat:");
    lastByte = millis();
    phase = STREAM;
    return;
  }

  // STREAM
  while (GPS.available()) { Serial.write(GPS.read()); lastByte = millis(); }
  if (millis() - lastHb >= 3000)
  {
    lastHb = millis();
    Serial.printf("[hb] alive, %lu ms since last GPS byte\r\n",
                  (unsigned long)(millis() - lastByte));
    Serial.flush();
  }
}
