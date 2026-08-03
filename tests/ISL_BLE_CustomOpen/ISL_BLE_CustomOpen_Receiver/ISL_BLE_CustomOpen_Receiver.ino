/*
 * ============================================================================
 * ISL BLE Custom-Open Receiver  (DIAGNOSTIC) · matches ISL_BLE_CustomOpen_Emitter
 * ============================================================================
 * The emitter is up and streaming, but the previous receiver (name-match only)
 * never found it -> api.ble.custom.start() apparently does NOT advertise the
 * "Custodia-Tracker" NAME. This version PRINTS EVERY advertisement it sees (name,
 * RSSI, address, and whether the Nordic UART Service UUID is present) so we can
 * see exactly what the emitter advertises, and it connects on EITHER a name match
 * OR a NUS-service match.
 *
 * Read the "[scan]" lines: find the one that is your emitter (strong RSSI, and/or
 * NUS=1) and note its name. If NUS=1 it will auto-connect and print the FRAMEs.
 *
 * Board: Adafruit / WisBlock BSP (bluefruit.h).
 * ============================================================================
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#define TARGET_NAME  "Custodia-Tracker"

BLEClientUart clientUart;

void bleuart_rx_callback(BLEClientUart& uart) {
  while (uart.available()) Serial.write((char)uart.read());
}

static void addrStr(const uint8_t *a, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

void scan_callback(ble_gap_evt_adv_report_t* report) {
  uint8_t nb[32];
  uint8_t nlen = Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, nb, sizeof(nb) - 1);
  if (nlen == 0) nlen = Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, nb, sizeof(nb) - 1);
  char name[33] = "(no name)";
  if (nlen > 0) { memcpy(name, nb, nlen); name[nlen] = 0; }

  bool hasNUS = Bluefruit.Scanner.checkReportForService(report, clientUart);   // NUS service in adv?
  char addr[20]; addrStr(report->peer_addr.addr, addr);
  Serial.printf("[scan] name='%s' rssi=%d NUS=%d addr=%s\n", name, report->rssi, hasNUS, addr);

  bool match = hasNUS || (strcmp(name, TARGET_NAME) == 0);
  if (match) {
    Serial.printf("[BLE] MATCH (%s) -> connecting...\n", hasNUS ? "NUS service" : "name");
    Bluefruit.Central.connect(report);
  } else {
    Bluefruit.Scanner.resume();
  }
}

void connect_callback(uint16_t conn_handle) {
  Serial.print("[BLE] Connected! Discovering NUS... ");
  if (clientUart.discover(conn_handle)) {
    Serial.println("ok.");
    if (clientUart.enableTXD()) Serial.println("[BLE] subscribed - printing frames:\n");
    else                        Serial.println("[BLE] enableTXD FAILED");
    delay(300);
    const char *msg = "HELLO_FROM_DRONE \n";
    clientUart.write((uint8_t*)msg, 18);
  } else {
    Serial.println("NUS not found, disconnecting.");
    Bluefruit.disconnect(conn_handle);
  }
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle; (void)reason;
  Serial.println("\n[BLE] Disconnected - resuming scan...");
  Bluefruit.Scanner.start(0);
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  Serial.println("=== ISL BLE Custom-Open Receiver (DIAGNOSTIC - prints all ads) ===");

  Bluefruit.begin(0, 1);
  Bluefruit.setName("Custodia-Drone");

  clientUart.begin();
  clientUart.setRxCallback(bleuart_rx_callback);

  Bluefruit.Central.setConnectCallback(connect_callback);
  Bluefruit.Central.setDisconnectCallback(disconnect_callback);

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(true);   // needed to receive scan-response name
  Bluefruit.Scanner.start(0);

  Serial.println("[BLE] scanning - printing every advertisement seen...");
}

void loop() {
  // driven by callbacks
}
