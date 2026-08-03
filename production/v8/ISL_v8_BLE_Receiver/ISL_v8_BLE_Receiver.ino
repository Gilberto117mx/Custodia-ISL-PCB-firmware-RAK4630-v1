/*
 * ============================================================================
 * ISL v8 BLE Receiver  ·  Bluefruit central, matches the RUI3 tracker BY NAME
 * ============================================================================
 * Pairs with ISL_Production_v8 (the RUI3 tracker). It is your committed
 * tests/Accelerometer receiver with ONE change: the RUI3 tracker advertises the
 * device NAME only (its api.ble.uart does not put the Nordic UART Service UUID in
 * the advertising packet), so this central matches on the COMPLETE LOCAL NAME
 * "Custodia-Tracker" instead of filtering on the service UUID. After connecting
 * it still discovers the standard NUS and subscribes to TXD exactly as before.
 *
 * ⚠ Board package: Adafruit / WisBlock BSP (bluefruit.h) — same as the committed
 *   receiver. Flash this to your receiver nRF52840, not the tracker.
 *
 * If it connects but no data arrives, the likely cause is Just-Works PAIRING
 * (RUI3's NUS may require an encrypted link before it sends notifications) — see
 * the v8 README test plan; we can enable a security/pairing callback here.
 * ============================================================================
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#define TARGET_NAME  "Custodia-Tracker"

BLEClientUart clientUart;

// Match by NAME (RUI3 advertises the name, not the service UUID).
void scan_callback(ble_gap_evt_adv_report_t* report) {
  uint8_t buf[32];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, buf, sizeof(buf) - 1);
  if (len == 0) {
    len = Bluefruit.Scanner.parseReportByType(
        report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, buf, sizeof(buf) - 1);
  }

  bool match = false;
  if (len > 0) { buf[len] = 0; match = (strcmp((char*)buf, TARGET_NAME) == 0); }

  if (match) {
    Serial.println("\n[BLE] Found Custodia-Tracker (by name)! Connecting...");
    Bluefruit.Central.connect(report);
  } else {
    Bluefruit.Scanner.resume();
  }
}

void connect_callback(uint16_t conn_handle) {
  Serial.print("[BLE] Connected! Discovering BLE UART service... ");

  if (clientUart.discover(conn_handle)) {
    Serial.println("Discovered.");
    if (clientUart.enableTXD()) {
      Serial.println("[BLE] Subscribed to incoming data stream.");
    } else {
      Serial.println("[BLE] Failed to subscribe to TXD!");
    }
  } else {
    Serial.println("Failed to discover BLE UART service.");
    Bluefruit.disconnect(conn_handle);
  }
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  (void)reason;
  Serial.println("\n[BLE] Disconnected — resuming scan...");
  Bluefruit.Scanner.start(0);
}

void bleuart_rx_callback(BLEClientUart& uart) {
  while (uart.available()) {
    char c = (char)uart.read();
    Serial.print(c);
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  Serial.println("=== ISL v8 Receiver — BLE Central (name-match) ===");

  Bluefruit.begin(0, 1);                 // 0 peripheral, 1 central
  Bluefruit.setName("Custodia-Receiver");

  clientUart.begin();
  clientUart.setRxCallback(bleuart_rx_callback);

  Bluefruit.Central.setConnectCallback(connect_callback);
  Bluefruit.Central.setDisconnectCallback(disconnect_callback);

  // NOTE: no Scanner.filterUuid() here — the RUI3 tracker doesn't advertise the
  // NUS UUID, so we filter by name inside scan_callback instead.
  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);   // 0.625 ms units
  Bluefruit.Scanner.useActiveScan(true);     // needed to receive the scan-response name
  Bluefruit.Scanner.start(0);                // scan indefinitely

  Serial.println("[BLE] Scanning for 'Custodia-Tracker' by name...");
}

void loop() {
  // Driven entirely by BLE callbacks.
}
