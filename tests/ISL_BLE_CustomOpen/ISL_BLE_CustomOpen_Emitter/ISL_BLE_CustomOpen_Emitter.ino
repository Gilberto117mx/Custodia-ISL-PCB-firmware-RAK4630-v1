/*
 * ============================================================================
 * ISL BLE Custom-Open Emitter  ·  RUI3 api.ble.custom, OPEN (no pairing) notify
 * ============================================================================
 * PROOF-OF-CONCEPT for the v9 BLE offload rework. It exposes the standard Nordic
 * UART Service (same UUIDs the receiver already discovers) but via api.ble.custom
 * with setPermission(RAK_SET_OPEN) — the pattern from your BLE_Custom_Service.ino
 * / Environment_Detect.ino examples — so notifications flow with NO pairing (no
 * "Pairing procedure fail" hang). It advertises as "Custodia-Tracker" and streams
 * one fixed-size (20 B) frame every 500 ms.
 *
 * GOAL: confirm the matching receiver (ISL_BLE_CustomOpen_Receiver) prints the
 * FRAME lines — i.e. open notify reaches it, discovered by NAME, no pairing.
 * If it works, we fold this into v9 (one-directional blast + dedup-by-seq).
 *
 * Board: RAKwireless RUI3 (this is the tracker side).
 * NUS UUIDs:  service 6E400001-…, RX(write) 6E400002-…, TX(notify) 6E400003-…
 * ============================================================================
 */

#include <Arduino.h>

#define FRAME_LEN     20                 // custom notify is fixed-length
#define BLE_NAME      "Custodia-Tracker"
#define STREAM_MS     500                // one frame every 500 ms

// NUS 128-bit base UUID, MSB-first (bytes [2],[3] = the 16-bit field, like the
// RAK heart-rate example). 6E400001-B5A3-F393-E0A9-E50E24DCCA9E:
static uint8_t nus_base[16] = {
    0x6E, 0x40, 0x00, 0x01, 0xB5, 0xA3, 0xF3, 0x93,
    0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E
};

// Characteristics must be GLOBAL so the callbacks can reach them (as in the RAK
// example). RUI3 combines the 16-bit UUID with the service's base -> 6E400002/3.
RAKBleCharacteristic txc = RAKBleCharacteristic(0x0003);   // TX: NOTIFY (tracker -> drone)
RAKBleCharacteristic rxc = RAKBleCharacteristic(0x0002);   // RX: WRITE (drone -> tracker)

static bool subscribed = false;

void tx_cccd_cb(uint16_t uuid, uint8_t *cccd)
{
    subscribed = txc.notifyEnabled();
    Serial.println(subscribed ? "[BLE] central SUBSCRIBED (notify enabled) - streaming"
                              : "[BLE] notify disabled");
    Serial.flush();
}

// Optional: prove the drone->tracker path too (not needed for one-directional).
void rx_write_cb(uint16_t uuid, uint8_t *val)
{
    Serial.print("[BLE RX] central wrote: ");
    Serial.write(val, FRAME_LEN);
    Serial.println();
    Serial.flush();
}

enum Phase { WARMUP, INIT, RUN };
Phase phase = WARMUP;
static uint32_t t0, lastHb, lastFrame;
static uint32_t n = 0;

void initBLE()
{
    api.ble.custom.init();

    RAKBleService nus = RAKBleService(nus_base);
    nus.begin();

    txc.setProperties(RAK_CHR_PROPS_NOTIFY);
    txc.setPermission(RAK_SET_OPEN);          // <-- OPEN: no pairing/encryption
    txc.setFixedLen(FRAME_LEN);
    txc.setCccdWriteCallback(tx_cccd_cb);
    txc.begin();

    rxc.setProperties(RAK_CHR_PROPS_WRITE);
    rxc.setPermission(RAK_SET_OPEN);
    rxc.setFixedLen(FRAME_LEN);
    rxc.setWriteCallback(rx_write_cb);
    rxc.begin();

    char nm[] = BLE_NAME;
    api.ble.settings.broadcastName.set(nm, sizeof(nm) - 1);

    api.ble.custom.start();
    Serial.printf("[BLE] custom NUS up (OPEN), advertising '%s'. Waiting for a central...\r\n", nm);
    Serial.flush();
}

void sendFrame()
{
    uint8_t frame[FRAME_LEN];
    memset(frame, ' ', FRAME_LEN);
    char tmp[24];
    int m = snprintf(tmp, sizeof(tmp), "FRAME %05lu", (unsigned long)n++);
    if (m > FRAME_LEN - 1) m = FRAME_LEN - 1;
    memcpy(frame, tmp, m);
    frame[FRAME_LEN - 1] = '\n';              // delimiter for the receiver
    txc.notify(frame);
    Serial.printf("[BLE TX] frame %lu\r\n", (unsigned long)(n - 1));
    Serial.flush();
}

void setup()
{
    Serial.begin(115200);
    { uint32_t t = millis(); while (!Serial && (millis() - t) < 4000) delay(10); }
    delay(300);
    t0 = millis();
    Serial.println("ISL BLE Custom-Open Emitter (alive-first). Heartbeat, then init...");
}

void loop()
{
    if (phase == WARMUP) {
        if (millis() - lastHb >= 500) { lastHb = millis(); Serial.printf("[alive] %lu ms\r\n", (unsigned long)(millis() - t0)); Serial.flush(); }
        if (millis() - t0 >= 3000) phase = INIT;
        return;
    }
    if (phase == INIT) { initBLE(); phase = RUN; lastFrame = millis(); return; }

    // RUN: stream a frame every STREAM_MS (notify only works once a central is
    // connected + subscribed; before that it's a harmless no-op).
    if (millis() - lastFrame >= STREAM_MS) {
        lastFrame = millis();
        sendFrame();
    }
}
