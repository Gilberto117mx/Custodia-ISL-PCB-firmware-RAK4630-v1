/*
 * ISL Board (RAK_feather) - Battery voltage read (ADC on AIN7 / P0.31)  [test #2]
 * FINAL, CALIBRATED (v4). Supersedes the original 2.4 V-FS attempt - see README.
 *
 * WHAT WAS WRONG (original):
 *   The original test assumed analogReference(AR_INTERNAL) = 2.4 V full scale
 *   and converted raw*4800/4095. On THIS RUI3 core AR_INTERNAL is
 *   actually ~3.67 V FS (0.6 V band-gap ref, gain 1/6), so every reading was
 *   mis-scaled, and without conditioning the reference the reading collapsed near
 *   the RT9080 LDO dropout (a "cliff" at ~3.3 V). Direct-register SAADC access was
 *   also tried and returns 0 on this core (the core's own driver owns the SAADC).
 *
 * THE FIX (validated over two 6-point sweeps, 3.2-3.7 V):
 *   - one-time SAADC OFFSET CALIBRATION at boot conditions the internal reference
 *     so analogRead() reads it cleanly and linearly (no cliff),
 *   - read via analogRead(AR_INTERNAL), median-of-31,
 *   - THROUGH-ORIGIN single-constant calibration:  Vbat_mV = raw * 1795 / 1000
 *     (1795 absorbs the true ~3.67 V FS, the 10k/10k divider x2, and gain error).
 *   Accuracy: <=25 mV (<0.8%) across 3.2-3.7 V incl. run-to-run repeatability.
 *   Repeatability floor of the nRF internal ref is ~+/-18 mV/boot - inherent,
 *   not removable by a constant; irrelevant for battery state-of-charge.
 *   Full data + fit in README.md / calibration_data.csv.
 *
 *   Serial (USB-C CDC, COM50) -> PC (115200)   |   ADC -> P0.31 / AIN7 (BATT_LEVEL)
 *   Output CSV: raw,Vbat   (raw kept so the one constant can be re-trimmed if ever needed)
 */

#include <Arduino.h>
#include <nrf.h>

#define BATT_ADC_PIN    P0_31
#define ADC_SAMPLES     31
#define WARMUP_MS       3000

// Through-origin calibration: Vbat_mV = raw * VBAT_CAL_NUM / VBAT_CAL_DEN
#define VBAT_CAL_NUM    1795UL       // two-sweep best fit (3.2-3.7 V)
#define VBAT_CAL_DEN    1000UL

void say(const char *s) { Serial.println(s); Serial.flush(); }
static int cmp_int(const void *a, const void *b){ return (*(const int*)a)-(*(const int*)b); }

// One-time SAADC offset calibration - conditions the internal reference so the
// core's analogRead(AR_INTERNAL) reads it cleanly (no dropout cliff).
static bool waitEvt(volatile uint32_t *evt) {
    for (uint32_t n = 0; n < 2000000UL; n++) { if (*evt) { *evt = 0; return true; } }
    return false;
}
static void saadcOffsetCalibrate()
{
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos;
    NRF_SAADC->CH[0].CONFIG =
        (SAADC_CH_CONFIG_GAIN_Gain1_6    << SAADC_CH_CONFIG_GAIN_Pos)   |   // matches AR_INTERNAL (~3.6 V FS)
        (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
        (SAADC_CH_CONFIG_TACQ_40us       << SAADC_CH_CONFIG_TACQ_Pos)   |
        (SAADC_CH_CONFIG_MODE_SE         << SAADC_CH_CONFIG_MODE_Pos);
    NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
    NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
    waitEvt(&NRF_SAADC->EVENTS_CALIBRATEDONE);
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;  // hand back to core
}

// The production reader (identical to what production/v2 uses).
uint16_t readVbat_mV()
{
    int v[ADC_SAMPLES]; int m = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) { v[m++] = analogRead(BATT_ADC_PIN); delay(2); }
    qsort(v, m, sizeof(int), cmp_int);
    uint16_t raw = v[m/2];
    return (uint16_t)(((uint32_t)raw * VBAT_CAL_NUM) / VBAT_CAL_DEN);
}

enum Phase { WARMUP, INIT, RUN };
Phase phase = WARMUP;

void setup()
{
    Serial.begin(115200);
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL Battery ADC (test #2, FINAL): analogRead(AR_INTERNAL) + raw*1795/1000.");
    say("Sweep the supply; each point reads back within ~25 mV. CSV: raw,Vbat");
}

void loop()
{
    static uint32_t t0 = millis();
    static uint32_t lastHb = 0;

    if (phase == WARMUP) {
        if (millis() - lastHb >= 500) { lastHb = millis(); Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis()-t0)); Serial.flush(); }
        if (millis() - t0 >= WARMUP_MS) phase = INIT;
        return;
    }

    if (phase == INIT) {
        pinMode(BATT_ADC_PIN, INPUT);
        analogReadResolution(12);
#ifdef AR_INTERNAL
        analogReference(AR_INTERNAL);
#endif
        say("STEP: SAADC offset calibration (conditions internal ref) ...");
        saadcOffsetCalibrate();
        say("STEP done. Reading:");
        phase = RUN;
        return;
    }

    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();

    int v[ADC_SAMPLES]; int m = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) { v[m++] = analogRead(BATT_ADC_PIN); delay(2); }
    qsort(v, m, sizeof(int), cmp_int);
    uint16_t raw = v[m/2];
    uint32_t mv  = ((uint32_t)raw * VBAT_CAL_NUM) / VBAT_CAL_DEN;

    Serial.printf("%4u,%u.%03u\r\n", raw, mv / 1000, mv % 1000);
    Serial.flush();
}
