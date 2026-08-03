#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <SPI.h>
#include <bluefruit.h>

// ---- LIS3DHTR (LIS3DH-compatible) bit-bang I2C ----

#define SDA_PIN        24
#define SCL_PIN        25
#define I2C_DELAY_US   5
#define LIS_ADDR       0x19
#define REG_WHOAMI     0x0F     // -> 0x33
#define REG_CTRL1      0x20
#define REG_CTRL4      0x23
#define REG_OUT_X_L    0x28

// ---- schedule ----
#define ACCEL_COLLECT_MS  5000UL
#define ACCEL_PERIOD_MS   100
#define MAX_SAMPLES       60
#define DEVICE_ID         "050"

// ---- BLE ----
BLEUart bleuart;

struct Sample { int16_t x, y, z; };
Sample accelBuf[MAX_SAMPLES];
int    accelCount = 0;
uint32_t cycle = 0;

// ---- I2C bit-bang functions ----
void i2c_start() {
  pinMode(SDA_PIN, OUTPUT);
  pinMode(SCL_PIN, OUTPUT);
  digitalWrite(SDA_PIN, HIGH);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(I2C_DELAY_US);
  digitalWrite(SDA_PIN, LOW);
  delayMicroseconds(I2C_DELAY_US);
  digitalWrite(SCL_PIN, LOW);
  delayMicroseconds(I2C_DELAY_US);
}

void i2c_stop() {
  pinMode(SDA_PIN, OUTPUT);
  digitalWrite(SDA_PIN, LOW);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(I2C_DELAY_US);
  digitalWrite(SDA_PIN, HIGH);
  delayMicroseconds(I2C_DELAY_US);
}

void i2c_write_byte(uint8_t byte) {
  pinMode(SDA_PIN, OUTPUT);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(SDA_PIN, (byte >> i) & 1);
    delayMicroseconds(I2C_DELAY_US);
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(I2C_DELAY_US);
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(I2C_DELAY_US);
  }
  // ACK clock pulse
  pinMode(SDA_PIN, INPUT);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(I2C_DELAY_US);
  digitalWrite(SCL_PIN, LOW);
  delayMicroseconds(I2C_DELAY_US);
  pinMode(SDA_PIN, OUTPUT);
}

uint8_t i2c_read_byte(bool ack) {
  uint8_t byte = 0;
  pinMode(SDA_PIN, INPUT);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(I2C_DELAY_US);
    byte = (byte << 1) | digitalRead(SDA_PIN);
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(I2C_DELAY_US);
  }
  pinMode(SDA_PIN, OUTPUT);
  digitalWrite(SDA_PIN, ack ? LOW : HIGH);
  delayMicroseconds(I2C_DELAY_US);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(I2C_DELAY_US);
  digitalWrite(SCL_PIN, LOW);
  delayMicroseconds(I2C_DELAY_US);
  return byte;
}

uint8_t lis_read_reg(uint8_t reg) {
  i2c_start();
  i2c_write_byte((LIS_ADDR << 1) | 0); // Write
  i2c_write_byte(reg);
  i2c_start();
  i2c_write_byte((LIS_ADDR << 1) | 1); // Read
  uint8_t val = i2c_read_byte(false);
  i2c_stop();
  return val;
}

void lis_write_reg(uint8_t reg, uint8_t val) {
  i2c_start();
  i2c_write_byte((LIS_ADDR << 1) | 0); // Write
  i2c_write_byte(reg);
  i2c_write_byte(val);
  i2c_stop();
}

// ---- Accelerometer functions ----
bool lis_init() {
  uint8_t whoami = lis_read_reg(REG_WHOAMI);
  if (whoami != 0x33) {
    Serial.printf("[ACCEL] WHOAMI error: 0x%02X (expected 0x33)\n", whoami);
    return false;
  }

  lis_write_reg(REG_CTRL1, 0x57); // 100Hz, normal mode, all axes
  lis_write_reg(REG_CTRL4, 0x08); // High resolution, ±2g

  delay(10);
  return true;
}

void read_accel(int16_t &x, int16_t &y, int16_t &z) {
  uint8_t data[6];
  i2c_start();
  i2c_write_byte((LIS_ADDR << 1) | 0);
  i2c_write_byte(REG_OUT_X_L | 0x80); // Auto-increment bit set
  i2c_start();
  i2c_write_byte((LIS_ADDR << 1) | 1);
  for (int i = 0; i < 6; i++) {
    data[i] = i2c_read_byte(i < 5);
  }
  i2c_stop();

  x = (int16_t)(data[1] << 8) | data[0];
  y = (int16_t)(data[3] << 8) | data[2];
  z = (int16_t)(data[5] << 8) | data[4];
}

void collectAccel() {
  if (!lis_init()) {
    Serial.println("[ACCEL] init failed");
    return;
  }

  accelCount = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < ACCEL_COLLECT_MS && accelCount < MAX_SAMPLES) {
    int16_t x, y, z;
    read_accel(x, y, z);
    accelBuf[accelCount].x = x;
    accelBuf[accelCount].y = y;
    accelBuf[accelCount].z = z;
    accelCount++;
    delay(ACCEL_PERIOD_MS);
  }
  Serial.printf("[ACCEL] %d samples gathered\n", accelCount);
}

void connect_cb(uint16_t h) {
  Serial.println("  >> receiver connected!");
}

void disconnect_cb(uint16_t h, uint8_t r) {
  Serial.println("  >> receiver disconnected!");
}

void configAdv() {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setInterval(32, 244);
}

void bleTX() {
  Bluefruit.Advertising.start(0);
  Serial.println("[BLE] advertising — waiting for receiver...");

  uint32_t t0 = millis();
  while (!Bluefruit.connected() && millis() - t0 < 20000UL) delay(50);
  if (!Bluefruit.connected()) {
    Serial.println("[BLE] no receiver connected, skipping TX");
    Bluefruit.Advertising.stop();
    return;
  }

  // Delay gives Central time to enable TXD notifications
  delay(1000);

  char line[64];
  int n;

  n = snprintf(line, sizeof(line), "ACC cycle=%lu count=%d\n", (unsigned long)cycle, accelCount);
  bleuart.write((uint8_t*)line, n);
  delay(50);

  for (int i = 0; i < accelCount; i++) {
    n = snprintf(line, sizeof(line), "s%02d %d,%d,%d\n", i + 1, accelBuf[i].x, accelBuf[i].y, accelBuf[i].z);
    bleuart.write((uint8_t*)line, n);
    delay(50);
  }

  bleuart.write((uint8_t*)"END\n", 4);
  bleuart.flush();
  delay(500);

  Serial.println("[BLE] data successfully transmitted.");

  // Disconnect central after sending so loop can cycle cleanly
  Bluefruit.disconnect(Bluefruit.connHandle());
  Bluefruit.Advertising.stop();
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  Serial.println("=== Custodia tracker — BLE Emitter ===");

  pinMode(SDA_PIN, OUTPUT);
  pinMode(SCL_PIN, OUTPUT);
  digitalWrite(SDA_PIN, HIGH);
  digitalWrite(SCL_PIN, HIGH);

  Bluefruit.begin();
  Bluefruit.autoConnLed(false);
  Bluefruit.setTxPower(4);
  Bluefruit.setName("Custodia-Tracker");
  Bluefruit.Periph.setConnectCallback(connect_cb);
  Bluefruit.Periph.setDisconnectCallback(disconnect_cb);
  bleuart.begin();

  // Configure advertising ONCE during setup
  configAdv();

  sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
  Serial.println("Setup done.");
}

void loop() {
  cycle++;
  Serial.printf("\n===== CYCLE %lu =====\n", (unsigned long)cycle);

  collectAccel();
  bleTX();

  delay(5000);
}
