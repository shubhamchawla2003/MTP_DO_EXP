#include <Arduino.h>

#define DO_PIN 4

// Temperature is fixed for now
#define WATER_TEMPERATURE 25

// Your measured air calibration
#define CAL1_V 355
#define CAL1_T 25

// DO saturation table
const uint16_t DO_Table[41] = {
  14460, 14220, 13820, 13440, 13090,
  12740, 12420, 12110, 11810, 11530,
  11260, 11010, 10770, 10530, 10300,
  10080, 9860, 9660, 9460, 9270,
  9080, 8900, 8730, 8570, 8410,
  8250, 8110, 7960, 7820, 7690,
  7560, 7430, 7300, 7180, 7070,
  6950, 6840, 6730, 6630, 6530,
  6410
};

void setup() {

  Serial.begin(115200);

  analogReadResolution(12);
  analogSetPinAttenuation(DO_PIN, ADC_11db);

  delay(2000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 DISSOLVED OXYGEN SENSOR");
  Serial.println("================================");
  Serial.println("Temperature = 25 C");
  Serial.println("Calibration = 355 mV");
  Serial.println("--------------------------------");
}

void loop() {

  const int samples = 50;

  long adcSum = 0;
  long voltageSum = 0;

  // Take 50 readings
  for (int i = 0; i < samples; i++) {

    adcSum += analogRead(DO_PIN);
    voltageSum += analogReadMilliVolts(DO_PIN);

    delay(10);
  }

  // Average readings
  float adcAverage =
      (float)adcSum / samples;

  float voltageAverage =
      (float)voltageSum / samples;

  // Temperature compensation
  uint16_t V_Saturation =
      (uint32_t)CAL1_V +
      (uint32_t)35 * WATER_TEMPERATURE -
      (uint32_t)CAL1_T * 35;

  // Calculate DO
  float DO =
      voltageAverage *
      DO_Table[WATER_TEMPERATURE] /
      V_Saturation;

  DO = DO / 1000.0;

  // Print results
  Serial.print("ADC = ");
  Serial.print(adcAverage, 0);

  Serial.print(" | Voltage = ");
  Serial.print(voltageAverage, 1);
  Serial.print(" mV");

  Serial.print(" | Temperature = ");
  Serial.print(WATER_TEMPERATURE);
  Serial.print(" C");

  Serial.print(" | DO = ");
  Serial.print(DO, 2);
  Serial.println(" mg/L");

  delay(1000);
}
