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

// ---------- Pin assignments ----------
const int PIN_VOLTAGE = 34; // Voltage divider module S pin (ADC1)
const int PIN_CURRENT = 35; // ACS712 OUT pin (ADC1)
const int PIN_ONEWIRE = 4;  // DS18B20 data line (4.7k pull-up to 3.3V)

// ---------- ADC constants ----------
const float ADC_VREF       = 3.3;   // ESP32 reference voltage
const int   ADC_RESOLUTION = 4095;  // 12-bit ADC
const int   ADC_SAMPLES    = 16;    // oversampling count for noise reduction

// ---------- Voltage divider ----------
// Standard 0-25V module steps the input voltage down by 5.
const float VCC_DIVIDER_RATIO = 5.0;

// ---------- ACS712 current sensor ----------
// 30A module: 66 mV per amp. Zero-current output is calibrated at boot.
const float ACS712_SENSITIVITY = 0.066; // V per A (30A variant)
float g_iZeroVoltage = 2.5;              // refined by calibrateCurrentZero()

// ---------- DS18B20 ----------
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

// Measure the ACS712 quiescent output with no current flowing.
void calibrateCurrentZero() {
  long sum = 0;
  for (int i = 0; i < 200; i++) {
    sum += analogRead(PIN_CURRENT);
    delay(1);
  }
  g_iZeroVoltage = ((float)sum / 200.0 / ADC_RESOLUTION) * ADC_VREF;
  Serial.print("ACS712 zero offset calibrated: ");
  Serial.print(g_iZeroVoltage, 4);
  Serial.println(" V");
}

void setup() {
  Serial.begin(115200);

  // 11dB attenuation gives a measurement range up to ~3.3V (full scale).
  analogSetAttenuation(ADC_11db);

  Serial.println("Calibrating ACS712 — ensure no current is flowing...");
  calibrateCurrentZero();

  // Start the DS18B20 and queue the first conversion asynchronously so the
  // loop never blocks on the ~750ms conversion time.
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  Serial.println("Solar diagnostic sensors ready.");
}

void loop() {
  // ----- Voltage -----
  float vPin = readPinVoltage(PIN_VOLTAGE);
  float inputVoltage = vPin * VCC_DIVIDER_RATIO;

  // ----- Current -----
  float iPin = readPinVoltage(PIN_CURRENT);
  float current = (iPin - g_iZeroVoltage) / ACS712_SENSITIVITY;
  if (current < 0) current = 0; // clamp DC-only readings

  // ----- Temperature (result of the conversion queued last tick) -----
  float temperatureC = sensors.getTempCByIndex(0);

  // ----- Report -----
  Serial.print("Voltage: ");
  Serial.print(inputVoltage, 2);
  Serial.print(" V\tCurrent: ");
  Serial.print(current, 3);
  Serial.print(" A\tTemp: ");
  if (temperatureC == DEVICE_DISCONNECTED_C) {
    Serial.print("n/a");
  } else {
    Serial.print(temperatureC, 1);
    Serial.print(" C");
  }
  Serial.println();

  // Queue the next temperature conversion; it will be ready by the next tick.
  sensors.requestTemperatures();

  delay(1000); // 1s reporting interval
}
