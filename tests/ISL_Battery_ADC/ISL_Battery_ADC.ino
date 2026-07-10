/*
 * ISL Board (RAK_feather) - Battery voltage read (ADC on AIN7 / P0.31)  [test #2]
 * (alive-first structure: this board's native-USB CDC wedges if setup() does long
 *  blocking work / early peripheral init, so setup() is tiny and the real init
 *  happens a few seconds into loop() - see ISL_RTC_Read for the proof of this.)
 *
 * Same nRF52840 ADC as Tracker 6.0, so the calibration carries over:
 *   analogReadResolution(12) ; analogReference(AR_INTERNAL)=2.40 V FS ; divider 2.0
 *   => Vbat_mV = raw * 2.4 * 2.0 * 1000 / 4095 = raw * 4800 / 4095
 * Divider here is 10k/10k (~5k source) so no AIN7 settling/noise issue (a plain
 * average is fine); tradeoff is ~180 uA always-on from VBAT.
 *
 *   Serial (USB-C CDC, COM50) -> PC (115200)   |   ADC -> P0.31 / AIN7 (BATT_LEVEL)
 */

#define BATT_ADC_PIN    P0_31
#define ADC_SAMPLES     16
#define CAL_GAIN        1.0f       // trim vs a known supply if needed (Vsource/Vbat)
#define WARMUP_MS       3000

enum Phase { WARMUP, INIT, RUN };
Phase phase = WARMUP;

void say(const char *s) { Serial.println(s); Serial.flush(); }

void setup()
{
  Serial.begin(115200);
  { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
  delay(300);
  say("ISL Battery ADC (test #2, alive-first). Heartbeat, then init...");
}

void loop()
{
  static uint32_t t0 = millis();
  static uint32_t lastHb = 0;

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
    pinMode(BATT_ADC_PIN, INPUT);
    analogReadResolution(12);
    analogReference(AR_INTERNAL);   // 2.4 V full scale on nRF52840
    say("ADC configured (AIN7/P0.31, 2.4 V ref, divider 2.0). rawAvg,Vbat:");
    phase = RUN;
    return;
  }

  // RUN
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  uint32_t acc = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) { acc += analogRead(BATT_ADC_PIN); delay(2); }
  uint16_t raw = acc / ADC_SAMPLES;

  // Integer math (FPU-free), matches Tracker's readVbat_mV().
  uint32_t vbat_mV = ((uint32_t)raw * 4800UL) / 4095UL;
  vbat_mV = (uint32_t)(vbat_mV * CAL_GAIN);

  Serial.printf("%4u,%u.%03u\r\n", raw, vbat_mV / 1000, vbat_mV % 1000);
  Serial.flush();
}
