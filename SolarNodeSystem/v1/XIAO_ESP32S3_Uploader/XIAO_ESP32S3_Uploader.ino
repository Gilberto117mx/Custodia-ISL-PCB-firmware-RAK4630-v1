/*
 * ============================================================================
 * XIAO ESP32S3 Uploader - firmware v1
 * ============================================================================
 * Target : Seeed XIAO ESP32S3, connected to the Solar Node Gateway via the
 *          Grove UART (Serial1 on the XIAO, 115200 8N1, LF terminated).
 * Role   : read one JSON packet per line from the gateway, batch several
 *          packets into ONE POST to Custodia's /api/locations endpoint,
 *          retry the batch on failure.
 *
 * DESIGN PRINCIPLE - "collars & repeaters stay dumb, cloud stays smart".
 * The XIAO uploads EVERY packet the gateway hands it, duplicates included.
 * Dedup + policy live in the cloud/dashboard so we can tune them without
 * pushing firmware to field devices.
 *
 * Input line from the Gateway (see SolarNode_Gateway.ino):
 *   {"devID":50,"seq":42,"lat":24.713600,"lon":46.675301,
 *    "vbat":3.60,"ts":1751328000,
 *    "gwID":60,"hops":1,"srcNodeID":52,"rssi":-95,"snr":8}\n
 *
 * Outbound POST (see GatewayAPI.txt):
 *   POST https://custodia.world/api/locations
 *   Authorization: Bearer <CLOUD_AUTH_BEARER>
 *   {
 *     "repeater_id": "<REPEATER_ID>",
 *     "timestamp":   <unix seconds now>,
 *     "packets": [
 *       {"device_id":50,"timestamp":1751328000,
 *        "latitude":24.713600,"longitude":46.675301,"voltage":3.60},
 *       ...
 *     ]
 *   }
 *
 * WiFi credentials, endpoint, token and REPEATER_ID live in config.h.
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "config.h"

// ============================================================================
// One tracker packet in the pending queue. Kept as the raw text fields from
// the gateway so the POST body serializer doesn't have to reparse floats.
// ============================================================================
struct PendingPacket {
    String devID;
    String ts;
    String lat;
    String lon;
    String vbat;
};

static PendingPacket queue[UPLOAD_QUEUE_SIZE];
static uint16_t qHead = 0;       // oldest
static uint16_t qTail = 0;       // next free
static uint16_t qCount = 0;
static uint32_t lastEnqueueMs = 0;

void enqueue(const PendingPacket &p)
{
    if (qCount >= UPLOAD_QUEUE_SIZE) {
        // Drop OLDEST to make room - realtime dashboard cares about fresh.
        qHead = (qHead + 1) % UPLOAD_QUEUE_SIZE;
        qCount--;
        DEBUG_SERIAL.println("[Q] full - dropped oldest");
    }
    queue[qTail] = p;
    qTail = (qTail + 1) % UPLOAD_QUEUE_SIZE;
    qCount++;
    lastEnqueueMs = millis();
}

// ============================================================================
// Very small JSON field extractor - the gateway lines are simple and
// well-formed enough that a full parser is overkill.
// Returns "" if the key isn't found.
// ============================================================================
String jsonField(const String &line, const char *key)
{
    String needle = String("\"") + key + "\":";
    int i = line.indexOf(needle);
    if (i < 0) return "";
    i += needle.length();
    // Skip whitespace
    while (i < (int)line.length() && line[i] == ' ') i++;
    if (i >= (int)line.length()) return "";
    // Value is either a quoted string or a bare number/bool
    if (line[i] == '"') {
        int j = line.indexOf('"', i + 1);
        if (j < 0) return "";
        return line.substring(i + 1, j);
    }
    int j = i;
    while (j < (int)line.length() && line[j] != ',' && line[j] != '}' && line[j] != ' ') j++;
    return line.substring(i, j);
}

// ============================================================================
// Line accumulator on Serial1 (from the gateway)
// ============================================================================
static String lineBuf;

void pumpSerialFromGateway()
{
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (lineBuf.length() > 0) {
                if (lineBuf[0] == '{' && lineBuf[lineBuf.length() - 1] == '}') {
                    DEBUG_SERIAL.printf("[UART] %s\n", lineBuf.c_str());
                    PendingPacket p;
                    p.devID = jsonField(lineBuf, "devID");
                    p.ts    = jsonField(lineBuf, "ts");
                    p.lat   = jsonField(lineBuf, "lat");
                    p.lon   = jsonField(lineBuf, "lon");
                    p.vbat  = jsonField(lineBuf, "vbat");
                    if (p.devID.length() && p.ts.length() &&
                        p.lat.length()   && p.lon.length() && p.vbat.length()) {
                        enqueue(p);
                    } else {
                        DEBUG_SERIAL.println("[UART] missing required field");
                    }
                } else {
                    DEBUG_SERIAL.printf("[UART] skip garbled: %s\n", lineBuf.c_str());
                }
                lineBuf = "";
            }
        } else {
            lineBuf += c;
            if (lineBuf.length() > 512) {
                DEBUG_SERIAL.println("[UART] line too long, reset");
                lineBuf = "";
            }
        }
    }
}

// ============================================================================
// WiFi + time
// ============================================================================
static bool timeSynced = false;

void wifiEnsure()
{
    if (WiFi.status() == WL_CONNECTED) return;
    DEBUG_SERIAL.printf("[WiFi] connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_MS) {
        delay(200);
        DEBUG_SERIAL.print('.');
    }
    DEBUG_SERIAL.println();
    if (WiFi.status() == WL_CONNECTED) {
        DEBUG_SERIAL.printf("[WiFi] connected, IP=%s\n",
                            WiFi.localIP().toString().c_str());
        if (!timeSynced) {
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            timeSynced = true;
        }
    } else {
        DEBUG_SERIAL.println("[WiFi] connect FAILED");
    }
}

uint32_t nowUnix()
{
    time_t t = time(nullptr);
    if (t < 1700000000L) return 0;   // clock not synced yet
    return (uint32_t)t;
}

// ============================================================================
// Build the POST body for up to N pending packets, POST once, return true on
// 2xx (in which case caller pops N from the head of the queue).
// ============================================================================
bool postBatch(uint16_t n)
{
    if (n == 0) return true;

    // {"repeater_id":"...","timestamp":N,"packets":[ {...}, {...} ]}
    String body;
    body.reserve(128 + n * 128);
    body  = "{\"repeater_id\":\"";
    body += REPEATER_ID;
    body += "\",\"timestamp\":";
    body += nowUnix();
    body += ",\"packets\":[";
    for (uint16_t i = 0; i < n; i++) {
        const PendingPacket &p = queue[(qHead + i) % UPLOAD_QUEUE_SIZE];
        if (i) body += ",";
        body += "{\"device_id\":";
        body += p.devID;
        body += ",\"timestamp\":";
        body += p.ts;
        body += ",\"latitude\":";
        body += p.lat;
        body += ",\"longitude\":";
        body += p.lon;
        body += ",\"voltage\":";
        body += p.vbat;
        body += "}";
    }
    body += "]}";

    WiFiClientSecure client;
#if INSECURE_TLS
    client.setInsecure();
#endif
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, CLOUD_URL)) {
        DEBUG_SERIAL.println("[HTTP] begin() failed");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    if (strlen(CLOUD_AUTH_BEARER) > 0) {
        http.addHeader("Authorization", String("Bearer ") + CLOUD_AUTH_BEARER);
    }

    DEBUG_SERIAL.printf("[HTTP] POST %u packets, body=%u B\n",
                        (unsigned)n, (unsigned)body.length());
    int code = http.POST(body);
    String resp = (code > 0) ? http.getString() : String();
    http.end();

    if (code >= 200 && code < 300) {
        DEBUG_SERIAL.printf("[HTTP] OK %d  resp=%s\n", code, resp.c_str());
        return true;
    }
    DEBUG_SERIAL.printf("[HTTP] FAIL code=%d resp=%s\n", code, resp.c_str());
    return false;
}

// ============================================================================
// Uploader - flush when the batch is full OR when it's been idle long enough.
// ============================================================================
static uint32_t lastAttemptMs = 0;

void uploadTick()
{
    if (qCount == 0) return;

    bool batchFull   = (qCount >= BATCH_MAX_PACKETS);
    bool idleTimeout = (millis() - lastEnqueueMs) >= BATCH_FLUSH_MS;
    if (!batchFull && !idleTimeout) return;

    if ((millis() - lastAttemptMs) < UPLOAD_RETRY_MS) return;
    lastAttemptMs = millis();

    if (WiFi.status() != WL_CONNECTED) {
        wifiEnsure();
        if (WiFi.status() != WL_CONNECTED) return;
    }
    if (nowUnix() == 0) {
        DEBUG_SERIAL.println("[TIME] not synced yet, holding batch");
        return;
    }

    uint16_t n = (qCount < BATCH_MAX_PACKETS) ? qCount : BATCH_MAX_PACKETS;
    if (postBatch(n)) {
        // Pop n from the head.
        for (uint16_t i = 0; i < n; i++) {
            queue[qHead] = PendingPacket();
            qHead = (qHead + 1) % UPLOAD_QUEUE_SIZE;
        }
        qCount -= n;
        // If more is pending, try again immediately.
        lastAttemptMs = 0;
    }
}

// ============================================================================
// setup / loop
// ============================================================================
void setup()
{
    DEBUG_SERIAL.begin(DEBUG_BAUD);
    delay(200);
    DEBUG_SERIAL.println("\n=== XIAO ESP32S3 Uploader v1 ===");
    DEBUG_SERIAL.printf("[CFG] endpoint=%s  repeater_id=%s  batch=%u  queue=%u\n",
                        CLOUD_URL, REPEATER_ID,
                        (unsigned)BATCH_MAX_PACKETS,
                        (unsigned)UPLOAD_QUEUE_SIZE);

    Serial1.begin(GATEWAY_BAUD, SERIAL_8N1,
                  GATEWAY_UART_RX_PIN, GATEWAY_UART_TX_PIN);
    lineBuf.reserve(512);

    wifiEnsure();
}

void loop()
{
    pumpSerialFromGateway();
    uploadTick();
    delay(5);
}
