/*
 * ISL Flash Probe - measure the usable api.system.flash user-area size (RUI3).
 * (alive-first structure - this board's native-USB CDC wedges on heavy setup().)
 *
 * Walks the flash offset space in STEP-byte increments, writing a known 16-byte
 * pattern at each offset and reading it back. The first offset that either fails
 * to write (set() == false) or does NOT read back correctly marks the ceiling.
 * Prints the max usable size, then how many accel records of various sizes fit.
 *
 * ⚠ This OVERWRITES the RUI3 user flash (it's a diagnostic). The production
 *   firmware re-initialises its flash on a magic mismatch, so it self-heals -
 *   but re-flash production afterwards to start clean.
 */

#include <Arduino.h>

#define STEP        256           // probe granularity (bytes)
#define MAX_OFFSET  (256 * 1024)  // stop here even if it keeps succeeding
#define PAT_LEN     16

enum Phase { WARMUP, RUN, DONE };
Phase phase = WARMUP;

void say(const char *s) { if (Serial) { Serial.println(s); Serial.flush(); } }

static void fillPat(uint8_t *b, uint32_t off) {
    for (int i = 0; i < PAT_LEN; i++) b[i] = (uint8_t)(off + i * 7 + 0xA5);
}

void probe() {
    say("ISL flash probe: walking api.system.flash ...");
    uint8_t w[PAT_LEN], r[PAT_LEN];
    uint32_t lastGood = 0; bool any = false;
    for (uint32_t off = 0; off <= MAX_OFFSET; off += STEP) {
        fillPat(w, off);
        bool okSet = api.system.flash.set(off, w, PAT_LEN);
        memset(r, 0, PAT_LEN);
        bool okGet = api.system.flash.get(off, r, PAT_LEN);
        bool match = (memcmp(w, r, PAT_LEN) == 0);
        if (!okSet || !okGet || !match) {
            Serial.printf("  ceiling at offset 0x%04lX (%lu B): set=%d get=%d match=%d\r\n",
                          (unsigned long)off, (unsigned long)off, okSet, okGet, match);
            break;
        }
        lastGood = off; any = true;
        if ((off % 4096) == 0) { Serial.printf("  ok up to 0x%04lX (%lu B)\r\n",
                                  (unsigned long)off, (unsigned long)off); Serial.flush(); }
    }
    uint32_t usable = any ? (lastGood + PAT_LEN) : 0;
    Serial.printf("\r\n=== USABLE api.system.flash ~= %lu bytes (%lu KB) ===\r\n",
                  (unsigned long)usable, (unsigned long)(usable / 1024));

    // How many accel records fit? record = 12 B header + samples*6.
    say("accel records that fit (record = 12 + samples*6 bytes):");
    const int cfgs[][3] = { {10,10,100},{10,5,50},{5,10,50},{5,5,25},{5,4,20},{3,5,15} };
    for (auto &c : cfgs) {
        int secs=c[0], hz=c[1], samp=c[2];
        int recB = 12 + samp*6;
        long fit = usable > 32 ? (long)(usable - 32) / recB : 0;   // minus ~meta
        Serial.printf("  %2ds @ %2d Hz = %3d samp -> %4d B/rec -> %4ld records fit"
                      "  (~%.1f days @ 6 h cadence)\r\n",
                      secs, hz, samp, recB, fit, fit * 6.0 / 24.0);
        Serial.flush();
    }
    say("\r\nDone. Re-flash production firmware to start with clean flash.");
}

void setup() {
    Serial.begin(115200);
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    say("ISL Flash Probe (alive-first). Heartbeat, then probe...");
}

void loop() {
    static uint32_t t0 = millis(), hb = 0;
    if (phase == WARMUP) {
        if (millis() - hb >= 500) { hb = millis(); Serial.printf("[alive] %lu ms\r\n",
                                     (unsigned long)(millis() - t0)); Serial.flush(); }
        if (millis() - t0 >= 3000) phase = RUN;
        return;
    }
    if (phase == RUN) { probe(); phase = DONE; return; }
    delay(1000);   // idle
}
