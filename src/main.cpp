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


#include <Arduino.h>

const int analogPin = 34; // ESP32 pin connected to the sensor's S pin

// The resistor divider scale factor. 
// Standard 0-25V modules step voltage down by 5.
const float VCC_DividerRatio = 5.0; 

// The ESP32 ADC reference voltage is 3.3V, but 11dB attenuation 
// allows a measurement range up to about 3.6V.
const float ADC_VREF = 3.3; 
const int ADC_Resolution = 4095; // 12-bit ADC

void setup() {
  Serial.begin(115200);
  
  // Set the ADC attenuation to 11dB (allows reading up to ~3.3V - 3.6V)
  analogSetAttenuation(ADC_11db);
}

void loop() {
  // Read the analog value (0 - 4095)
  int analogValue = analogRead(analogPin);
  
  // Convert the analog reading to actual voltage at the ESP32 pin
  float pinVoltage = (analogValue / (float)ADC_Resolution) * ADC_VREF;
  
  // Calculate original input voltage using the divider ratio
  float inputVoltage = pinVoltage * VCC_DividerRatio;
  
  // Print results to the Serial Monitor
  Serial.print("Analog Value: ");
  Serial.print(analogValue);
  Serial.print("\t Pin Voltage: ");
  Serial.print(pinVoltage, 2);
  Serial.print("V \t Calculated Input Voltage: ");
  Serial.print(inputVoltage, 2);
  Serial.println("V");

  delay(1000); // Wait 1 second before next reading
}
