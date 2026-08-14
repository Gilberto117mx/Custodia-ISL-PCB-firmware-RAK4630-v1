// ============================================================================
// XIAO_ESP32S3_Uploader - user config
// ============================================================================
// Fill in your WiFi credentials before flashing. The Custodia endpoint,
// bearer token and repeater_id below are the working values from the
// GatewayAPI spec - edit REPEATER_ID per deployed gateway.
// ============================================================================

#pragma once

// -------- WiFi --------
#define WIFI_SSID          "YOUR_SSID"
#define WIFI_PASSWORD      "YOUR_PASSWORD"
#define WIFI_CONNECT_MS    20000

// -------- Cloud endpoint (HTTPS POST JSON, see GatewayAPI.txt) --------
//   POST https://custodia.world/api/locations
//   Authorization: Bearer <CLOUD_AUTH_BEARER>
//   Body: { repeater_id, timestamp, packets:[{device_id,timestamp,latitude,longitude,voltage},...] }
#define CLOUD_URL          "https://custodia.world/api/locations"
#define CLOUD_AUTH_BEARER  "YOUR_CLOUD_BEARER_TOKEN"

// Identifier the cloud stores against each batch. The API calls this
// "repeater_id" but in our topology it is really the gateway id. Give each
// deployed gateway a unique value.
#define REPEATER_ID        "RPT-001"

// TLS: 1 = client.setInsecure() (bench). Set to 0 and provide a CA cert in
// XIAO_ESP32S3_Uploader.ino via client.setCACert(...) for production.
#define INSECURE_TLS       1

// -------- UART link to Solar Node Gateway --------
// Wiring on the Solar Node P1-Pro side (from SenseCAP block diagram):
//   Grove D0 (yellow) = P0.10   <- Solar Node TX  -> XIAO D7 (this RX)
//   Grove D1 (white)  = P0.09   <- Solar Node RX  <- XIAO D6 (this TX)
//   GND common (mandatory) ; 3V3 optional (only if the XIAO is fed from the SN).
#define GATEWAY_BAUD       115200
#define GATEWAY_UART_RX_PIN  44   // XIAO D7 (GPIO 44)  <-  Solar Node Grove D0 (yellow)
#define GATEWAY_UART_TX_PIN  43   // XIAO D6 (GPIO 43)  ->  Solar Node Grove D1 (white)

// -------- Batching / upload policy --------
// The cloud accepts many packets in one POST. Batch until either limit trips.
#define BATCH_MAX_PACKETS       20      // hard cap on one POST body
#define BATCH_FLUSH_MS          15000   // ms - flush a partial batch after this idle time
#define UPLOAD_QUEUE_SIZE       256     // pending packets kept while WiFi/cloud is down
#define HTTP_TIMEOUT_MS         8000
#define UPLOAD_RETRY_MS         5000    // between failed POST attempts

// -------- Debug --------
#define DEBUG_SERIAL       Serial
#define DEBUG_BAUD         115200
