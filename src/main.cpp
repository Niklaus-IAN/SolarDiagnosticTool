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
const float ADC_VREF       = 3.3;   // ESP32 reference voltage
const int   ADC_RESOLUTION = 4095;  // 12-bit ADC
const int   ADC_SAMPLES    = 16;    // oversampling count for noise reduction

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
float g_iZeroVoltage = 2.5;             // refined by calibrateCurrentZero()

// ---------- DS18B20 (Phase 3) ----------
// Two probes share the bus: index 0 = panel, index 1 = ambient.
// NOTE: bus index follows factory serial, not wiring order — confirm by warming
// one probe, and hard-code 64-bit addresses for the final build.
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);

// Average several ADC samples and return the pin voltage (0 - ADC_VREF).
float readPinVoltage(int pin) {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(pin);
  }
  float avgCounts = (float)sum / ADC_SAMPLES;
  return (avgCounts / ADC_RESOLUTION) * ADC_VREF;
}

// Read the ACS712 output referred back through the external ÷2 divider.
float readCurrentSensorVoltage() {
  return readPinVoltage(PIN_CURRENT) * ACS712_OUT_DIVIDER;
}

// Measure the ACS712 quiescent output with no current flowing.
void calibrateCurrentZero() {
  long sum = 0;
  for (int i = 0; i < 200; i++) {
    sum += analogRead(PIN_CURRENT);
    delay(1);
  }
  float avgPinV = ((float)sum / 200.0 / ADC_RESOLUTION) * ADC_VREF;
  g_iZeroVoltage = avgPinV * ACS712_OUT_DIVIDER;
  Serial.print("ACS712 zero offset calibrated: ");
  Serial.print(g_iZeroVoltage, 4);
  Serial.println(" V (sensor domain)");
}

// ---------- BLE · Nordic UART Service (Phase 5) ----------
// Streams one JSON reading packet per second to the mobile app over the TX
// characteristic. The app writes short commands (e.g. "CAL") to the RX one.
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // phone -> ESP32 (write)
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> phone (notify)

BLECharacteristic *g_txChar = nullptr;
bool g_bleConnected = false;
bool g_advPending   = false; // re-advertise after a disconnect

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

// Short text commands from the app land here.
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.rfind("CAL", 0) == 0) { // starts with "CAL"
      Serial.println("BLE command: recalibrate current zero");
      calibrateCurrentZero();
    }
  }
};

void initBLE() {
  BLEDevice::init("SolarDiag"); // name shown when the app scans

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

  Serial.println("Calibrating ACS712 — ensure no current is flowing...");
  calibrateCurrentZero();

  // Start the DS18B20 bus and queue the first conversion asynchronously so the
  // loop never blocks on the ~750ms conversion time.
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  Serial.print("DS18B20 probes found: ");
  Serial.println(sensors.getDeviceCount());

  initBLE();

  Serial.println("Solar diagnostic sensors ready.");
}

// Print a temperature or "n/a" if that probe is missing.
void printTemp(float c) {
  if (c == DEVICE_DISCONNECTED_C) {
    Serial.print("n/a");
  } else {
    Serial.print(c, 1);
  }
}

void loop() {
  // ----- Voltages -----
  float panelV   = readPinVoltage(PIN_V_PANEL)   * PANEL_DIVIDER_RATIO;
  float batteryV = readPinVoltage(PIN_V_BATTERY) * BATTERY_DIVIDER_RATIO;

  // ----- Current (through the ÷2 divider) -----
  float current = (readCurrentSensorVoltage() - g_iZeroVoltage) / ACS712_SENSITIVITY;
  if (current < 0) current = 0; // clamp: DC flows one direction here

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
