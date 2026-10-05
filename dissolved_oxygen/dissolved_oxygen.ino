#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define DO_PIN 4
#define TEMP_PIN 5

#define CAL1_V 355
#define CAL1_T 25

#define SATURATION_MV_PER_C 35.0f
#define SATURATION_REF_MV 1600.0f

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

OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);

bool tempPresent = false;
unsigned long startMs = 0;

const char CSV_HEADER[] = "Uptime_s,DO_mg_L,Temperature_C,Voltage_mV,ADC,Status";

void prepareTempPin() {
  pinMode(TEMP_PIN, INPUT_PULLUP);
}

float clampTemperature(float temperatureC) {
  if (temperatureC < 0.0f) return 0.0f;
  if (temperatureC > 40.0f) return 40.0f;
  return temperatureC;
}

float saturationDoUgL(float temperatureC) {
  int t0 = (int)temperatureC;
  if (t0 >= 40) return (float)DO_Table[40];
  float frac = temperatureC - (float)t0;
  return (float)DO_Table[t0] +
         frac * ((float)DO_Table[t0 + 1] - (float)DO_Table[t0]);
}

bool readTemperatureC(float &temperatureC) {
  prepareTempPin();
  if (!tempPresent) {
    tempSensor.begin();
    prepareTempPin();
    tempSensor.setResolution(11);
    tempPresent = tempSensor.getDeviceCount() > 0;
    if (!tempPresent) return false;
  }

  tempSensor.requestTemperatures();
  temperatureC = tempSensor.getTempCByIndex(0);

  if (isnan(temperatureC) ||
      temperatureC == DEVICE_DISCONNECTED_C ||
      temperatureC < -10.0f ||
      temperatureC > 85.0f) {
    tempPresent = false;
    return false;
  }

  return true;
}

void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetPinAttenuation(DO_PIN, ADC_11db);

  prepareTempPin();
  tempSensor.begin();
  prepareTempPin();
  tempSensor.setResolution(11);
  tempPresent = tempSensor.getDeviceCount() > 0;

  delay(2000);

  // Lines starting with '#' are comments; do_logger.py skips them.
  Serial.println();
  Serial.println("# ESP32 DISSOLVED OXYGEN SENSOR");
  Serial.println("# DO pin = GPIO 4, DS18B20 on GPIO 5");
  Serial.println("# Calibration = 355 mV at 25 C");
  if (!tempPresent) {
    Serial.println("# ERROR: DS18B20 not detected on GPIO 5");
  }
  Serial.println(CSV_HEADER);
  startMs = millis();
}

void loop() {
  const int samples = 50;

  float temperatureC = 0.0f;
  bool temperatureOk = readTemperatureC(temperatureC);
  bool temperatureInRange = temperatureOk && temperatureC >= 0.0f && temperatureC <= 40.0f;
  float temperatureForDo = temperatureInRange ? temperatureC : 0.0f;

  long adcSum = 0;
  long voltageSum = 0;

  for (int i = 0; i < samples; i++) {
    adcSum += analogRead(DO_PIN);
    voltageSum += analogReadMilliVolts(DO_PIN);
    delay(10);
  }

  float adcAverage = (float)adcSum / samples;
  float voltageAverage = (float)voltageSum / samples;

  float mvPerC = SATURATION_MV_PER_C * ((float)CAL1_V / SATURATION_REF_MV);
  float V_saturation =
      (float)CAL1_V + mvPerC * (temperatureForDo - (float)CAL1_T);

  float dissolvedOxygen = 0.0f;
  bool doValid = temperatureInRange && V_saturation > 0.0f;
  if (doValid) {
    dissolvedOxygen =
        voltageAverage * saturationDoUgL(temperatureForDo) / V_saturation / 1000.0f;
  }

  const char *status = "OK";
  if (!temperatureOk) {
    status = "TEMP_SENSOR_MISSING";
  } else if (!temperatureInRange) {
    status = "TEMP_OUT_OF_RANGE";
  } else if (!doValid) {
    status = "INVALID_SATURATION_V";
  }

  // Invalid values are left blank so Excel treats them as gaps, not zeros.
  Serial.print((millis() - startMs) / 1000.0f, 1);
  Serial.print(',');
  if (doValid) Serial.print(dissolvedOxygen, 3);
  Serial.print(',');
  if (temperatureOk) Serial.print(temperatureC, 2);
  Serial.print(',');
  Serial.print(voltageAverage, 1);
  Serial.print(',');
  Serial.print(adcAverage, 0);
  Serial.print(',');
  Serial.println(status);

  delay(1000);
}
