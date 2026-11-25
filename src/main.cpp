/*********
    Current Sensor ACS712 with ESP32
*********/

// #include <Arduino.h>

// #define PIN_CURRENT 35

// float zeroOffset = 2.5;

// void calibrateZero() {
//   long sum = 0;
//   for (int i = 0; i < 200; i++) {
//     sum += analogRead(PIN_CURRENT);
//     delay(1);
//   }
//   zeroOffset = ((float)sum / 200.0) / 4095.0 * 3.3;
//   Serial.print("Zero offset calibrated: ");
//   Serial.print(zeroOffset, 4);
//   Serial.println(" V");
// }

// float readCurrent() {
//   int adc = analogRead(PIN_CURRENT);
//   float vsensor = (adc / 4095.0) * 3.3;
//   float current = (vsensor - zeroOffset) / 0.066;
//   return current < 0 ? 0 : current;
// }

// void setup() {
//   Serial.begin(115200);
//   analogReadResolution(12);
//   Serial.println("Calibrating zero offset — ensure no current is flowing...");
//   calibrateZero();
//   Serial.println("ACS712 test ready.");
// }

// void loop() {
//   float current = readCurrent();
//   Serial.print("Current: ");
//   Serial.print(current, 3);
//   Serial.println(" A");
//   delay(1000);
// }



/*********
  Temperature Sensor DS18B20 with ESP32
*********/

// #include <OneWire.h>
// #include <DallasTemperature.h>

// // GPIO where the DS18B20 is connected to
// const int oneWireBus = 4;     

// // Setup a oneWire instance to communicate with any OneWire devices
// OneWire oneWire(oneWireBus);

// // Pass our oneWire reference to Dallas Temperature sensor 
// DallasTemperature sensors(&oneWire);

// void setup() {
//   // Start the Serial Monitor
//   Serial.begin(115200);
//   // Start the DS18B20 sensor
//   sensors.begin();
// }

// void loop() {
//   sensors.requestTemperatures(); 
//   float temperatureC = sensors.getTempCByIndex(0);
//   float temperatureF = sensors.getTempFByIndex(0);
//   Serial.print(temperatureC);
//   Serial.println("ºC");
//   Serial.print(temperatureF);
//   Serial.println("ºF");
//   delay(5000);
// }


/*********
  Voltage Sensor with ESP32
*********/


/*********
  Solar Diagnostic Tool — unified sensor firmware
  Reads voltage, current, and temperature together each loop.
*********/

#include <Arduino.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ---------- Pin assignments ----------
const int PIN_V_PANEL   = 34; // Panel voltage module S pin   (ADC1_CH6)
const int PIN_V_BATTERY = 33; // Battery voltage module S pin (ADC1_CH5)
const int PIN_CURRENT   = 35; // ACS712 OUT via ÷2 divider    (ADC1_CH7)
const int PIN_ONEWIRE   = 4;  // DS18B20 bus (4.7k pull-up to 3.3V)

// ---------- ADC constants ----------
// Conversion goes through analogReadMilliVolts(), which applies the per-chip
// eFuse calibration. The raw (counts/4095)*3.3 formula assumes a straight line
// the ESP32 ADC does not follow at 11dB — on the bench it read ~0.14V low
// around 0.8V, a 17% error no divider-ratio calibration can correct.
const int ADC_SAMPLES = 16;         // oversampling count for the voltage channels

// The PWM controller chops the panel line on and off at a few hundred Hz, so
// the current in that wire is a square wave, not DC. 16 back-to-back samples
// finish well inside one switching cycle, which makes each reading a random
// phase snapshot rather than an average. Sample the current channel across a
// fixed window instead, long enough to cover many cycles.
const uint32_t CURRENT_WINDOW_MS = 32;

// ---------- Voltage dividers (Phase 1.1 / Phase 2) ----------
// Panel channel has an added ~22k series resistor so a 22V panel stays under
// the 3.3V ADC ceiling:  ratio = (22k + 30k + 7.5k) / 7.5k ≈ 7.93.
// Battery channel uses the unmodified 0-25V module (÷5); battery max ~14.4V.
// CALIBRATE both against a multimeter: ratio = V_multimeter / V_adc_pin.
float PANEL_DIVIDER_RATIO   = 7.93; // CALIBRATE on bench
float BATTERY_DIVIDER_RATIO = 5.0;  // CALIBRATE on bench

// ---------- ACS712 current sensor (Phase 1.2) ----------
// OUT feeds a ÷2 divider (10k/10k) into GPIO35 to keep it under 3.3V, so the
// true sensor voltage = pin voltage × 2. Zero is calibrated at boot in the
// sensor domain (captures ~2.5V idle automatically).
const float ACS712_SENSITIVITY = 0.066; // V per A (30A variant)
const float ACS712_OUT_DIVIDER = 2.0;   // external ÷2 on OUT

// One ADC count is ~24mA through this chain and the channel carries roughly
// ±0.07A of inherent noise. Readings inside the deadband report as exactly
// zero; outside it the sign is preserved, so reverse flow and night-time drain
// stay visible instead of being clamped away.
const float CURRENT_DEADBAND_A = 0.10;

// The zero is measured once under known-quiet conditions and kept in NVS. It is
// deliberately NOT re-measured at boot: the tool is powered from the system
// battery, so it can only ever start up on a live system, and in daylight
// current is already flowing through the sensor by then. A boot-time
// measurement would record a loaded reading as "zero" on every single start.
Preferences g_prefs;
const char *PREF_NAMESPACE = "solardiag";
const char *PREF_KEY_IZERO = "izero";
float g_iZeroVoltage = 2.5;   // sensor domain; loaded from NVS in setup()
bool  g_iZeroStored  = false; // false => never calibrated, readings provisional

// ---------- DS18B20 (Phase 3) ----------
// Two probes share the bus: index 0 = panel, index 1 = ambient.
// NOTE: bus index follows factory serial, not wiring order — confirm by warming
// one probe, and hard-code 64-bit addresses for the final build.
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);

// Average several calibrated ADC samples and return the pin voltage in volts.
float readPinVoltage(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogReadMilliVolts(pin);
  }
  return (sum / (float)ADC_SAMPLES) / 1000.0f;
}

// Average the ACS712 pin across CURRENT_WINDOW_MS so the PWM chopping in the
// panel line averages out instead of aliasing.
float readCurrentPinVoltage() {
  uint32_t sum = 0, n = 0;
  uint32_t start = millis();
  while ((millis() - start) < CURRENT_WINDOW_MS) {
    sum += analogReadMilliVolts(PIN_CURRENT);
    n++;
  }
  if (n == 0) n = 1;
  return (sum / (float)n) / 1000.0f;
}

// Read the ACS712 output referred back through the external ÷2 divider.
float readCurrentSensorVoltage() {
  return readCurrentPinVoltage() * ACS712_OUT_DIVIDER;
}

// Measure and store the ACS712 quiescent output. The caller must guarantee no
// current is flowing — cover the panel, or open the panel line.
void calibrateCurrentZero() {
  uint32_t sum = 0;
  for (int i = 0; i < 200; i++) {
    sum += analogReadMilliVolts(PIN_CURRENT);
    delay(1);
  }
  float avgPinV = (sum / 200.0f) / 1000.0f;
  g_iZeroVoltage = avgPinV * ACS712_OUT_DIVIDER;
  g_iZeroStored  = true;
  g_prefs.putFloat(PREF_KEY_IZERO, g_iZeroVoltage);

  Serial.print("ACS712 zero stored: ");
  Serial.print(g_iZeroVoltage, 4);
  Serial.print(" V (sensor domain), raw pin ");
  Serial.print(avgPinV, 4);
  Serial.println(" V");

  // The raw pin voltage reveals whether the external ÷2 is actually fitted:
  // a bare ACS712 idles near 2.5V, halved it lands near 1.25V. Without the
  // divider every current reading is double the truth and the pin exceeds
  // 3.3V above ~13A.
  if (avgPinV > 1.9f) {
    Serial.println("  WARNING: raw pin near 2.5V — the ÷2 divider on ACS712 OUT looks MISSING");
  } else {
    Serial.println("  ÷2 divider on ACS712 OUT looks present");
  }
}

// ---------- BLE · Nordic UART Service (Phase 5) ----------
// Streams one JSON reading packet per second to the mobile app over the TX
// characteristic. The app writes short commands (e.g. "CAL") to the RX one.
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // phone -> ESP32 (write)
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> phone (notify)

BLECharacteristic *g_txChar = nullptr;
// Written from the BLE stack's task and read from loop(), so they must not be
// cached in a register across the loop body.
volatile bool g_bleConnected = false;
volatile bool g_advPending   = false; // re-advertise after a disconnect
volatile bool g_calRequested = false; // raised by BLE or serial, serviced in loop()

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    g_bleConnected = true;
    Serial.println("BLE client connected");
  }
  void onDisconnect(BLEServer *s) override {
    g_bleConnected = false;
    g_advPending = true;
    Serial.println("BLE client disconnected");
  }
};

// Short text commands from the app land here. This runs on the BLE stack's
// task, so it only raises a flag — calibrateCurrentZero() blocks for 200ms of
// delay() plus serial writes, which has no business inside a GATT callback.
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.rfind("CAL", 0) == 0) { // starts with "CAL"
      g_calRequested = true;
    }
  }
};

void initBLE() {
  BLEDevice::init("SolarDiag"); // name shown when the app scans

  // Without this the peer MTU stays at the 23-byte default, leaving 20 usable
  // bytes per notification for a ~90-byte packet: notify() logs the truncation
  // and sends anyway, so the app receives malformed JSON and drops it silently.
  // The negotiated value is min(client, server), so the app requesting 247 on
  // its own is not enough.
  BLEDevice::setMTU(517);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *svc = server->createService(NUS_SERVICE_UUID);

  g_txChar = svc->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  g_txChar->addDescriptor(new BLE2902());

  BLECharacteristic *rx =
      svc->createCharacteristic(NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rx->setCallbacks(new RxCallbacks());

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising as 'SolarDiag'");
}

// Build the reading packet as JSON and notify the app (missing probe -> null).
void sendBlePacket(float panelV, float batteryV, float current, float power,
                   float tPanel, float tAmbient) {
  if (!g_bleConnected || g_txChar == nullptr) return;

  char tp[12], ta[12];
  if (tPanel == DEVICE_DISCONNECTED_C)   strcpy(tp, "null");
  else                                   snprintf(tp, sizeof(tp), "%.1f", tPanel);
  if (tAmbient == DEVICE_DISCONNECTED_C) strcpy(ta, "null");
  else                                   snprintf(ta, sizeof(ta), "%.1f", tAmbient);

  unsigned long ts = millis() / 1000UL; // uptime seconds — app staleness heartbeat

  char json[176];
  snprintf(json, sizeof(json),
           "{\"v_panel\":%.2f,\"v_batt\":%.2f,\"current\":%.3f,\"power\":%.1f,"
           "\"t_panel\":%s,\"t_amb\":%s,\"ts\":%lu}",
           panelV, batteryV, current, power, tp, ta, ts);

  g_txChar->setValue((uint8_t *)json, strlen(json));
  g_txChar->notify();
}

void setup() {
  Serial.begin(115200);

  // 11dB attenuation gives a measurement range up to ~3.3V (full scale).
  analogSetAttenuation(ADC_11db);

  // Restore the stored ACS712 zero rather than measuring one now — see the note
  // by PREF_KEY_IZERO for why a boot-time measurement cannot be trusted here.
  g_prefs.begin(PREF_NAMESPACE, false);
  float stored = g_prefs.getFloat(PREF_KEY_IZERO, 0.0f);
  if (stored > 0.1f) {
    g_iZeroVoltage = stored;
    g_iZeroStored  = true;
    Serial.print("ACS712 zero restored from flash: ");
    Serial.print(g_iZeroVoltage, 4);
    Serial.println(" V (sensor domain)");
  } else {
    Serial.println("ACS712 has never been zeroed — current readings are PROVISIONAL.");
    Serial.println("  Stop all current (cover the panel), then send CAL over serial or BLE.");
  }

  // Start the DS18B20 bus and queue the first conversion asynchronously so the
  // loop never blocks on the ~750ms conversion time.
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  uint8_t nProbes = sensors.getDeviceCount();
  Serial.print("DS18B20 probes found: ");
  Serial.println(nProbes);

  // Print each probe's ROM address beside the index it gets read at, so the
  // panel/ambient pair can be hard-coded once a pinch test says which is which.
  for (uint8_t i = 0; i < nProbes; i++) {
    DeviceAddress addr;
    if (!sensors.getAddress(addr, i)) continue;
    Serial.print("  index ");
    Serial.print(i);
    Serial.print(" -> ");
    for (uint8_t b = 0; b < 8; b++) {
      if (addr[b] < 16) Serial.print('0');
      Serial.print(addr[b], HEX);
    }
    if (i == 0)      Serial.print("   (currently read as PANEL)");
    else if (i == 1) Serial.print("   (currently read as AMBIENT)");
    Serial.println();
  }

  initBLE();

  Serial.println("Solar diagnostic sensors ready.  Commands: CAL");
}

// Print a temperature or "n/a" if that probe is missing.
void printTemp(float c) {
  if (c == DEVICE_DISCONNECTED_C) {
    Serial.print("n/a");
  } else {
    Serial.print(c, 1);
  }
}

// Non-blocking serial command reader; recognises "CAL".
void pollSerialCommand() {
  static char buf[16];
  static uint8_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      for (uint8_t i = 0; i < len; i++) buf[i] = toupper(buf[i]);
      buf[len] = '\0';
      if (len && strcmp(buf, "CAL") == 0) g_calRequested = true;
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

void loop() {
  pollSerialCommand();

  // Service a CAL raised by either the serial reader or the BLE task.
  if (g_calRequested) {
    g_calRequested = false;
    Serial.println("Recalibrating ACS712 zero — no current must be flowing...");
    calibrateCurrentZero();
  }

  // ----- Voltages -----
  float panelV   = readPinVoltage(PIN_V_PANEL)   * PANEL_DIVIDER_RATIO;
  float batteryV = readPinVoltage(PIN_V_BATTERY) * BATTERY_DIVIDER_RATIO;

  // ----- Current (averaged over the PWM cycle, through the ÷2 divider) -----
  float iPinV   = readCurrentPinVoltage();
  float current = (iPinV * ACS712_OUT_DIVIDER - g_iZeroVoltage) / ACS712_SENSITIVITY;
  // The sign is kept. A negative reading is real information — reverse flow,
  // night-time drain, or a sensor wired backwards — and the old clamp to zero
  // hid all three behind a plausible-looking 0.000.
  if (fabsf(current) < CURRENT_DEADBAND_A) current = 0.0f;

  // ----- Power -----
  float power = panelV * current;

  // ----- Temperatures (from the conversion queued last tick) -----
  float tPanel   = sensors.getTempCByIndex(0);
  float tAmbient = sensors.getTempCByIndex(1);

  // ----- Report -----
  Serial.print("Panel: ");   Serial.print(panelV, 2);   Serial.print(" V");
  Serial.print("\tBatt: ");  Serial.print(batteryV, 2); Serial.print(" V");
  Serial.print("\tI: ");     Serial.print(current, 3);  Serial.print(" A");
  Serial.print("\tP: ");     Serial.print(power, 1);    Serial.print(" W");
  Serial.print("\tTp: ");    printTemp(tPanel);         Serial.print(" C");
  Serial.print("\tTa: ");    printTemp(tAmbient);       Serial.print(" C");
  Serial.println();

  // ----- Stream the same readings to the app over BLE -----
  sendBlePacket(panelV, batteryV, current, power, tPanel, tAmbient);

  // Restart advertising once, after a client disconnects.
  if (g_advPending) {
    g_advPending = false;
    delay(300);
    BLEDevice::startAdvertising();
    Serial.println("BLE re-advertising");
  }

  // Queue the next temperature conversion; it will be ready by the next tick.
  sensors.requestTemperatures();

  delay(1000); // 1s reporting interval
}
