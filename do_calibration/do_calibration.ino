#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Calibration helper for the analog DO probe used in dissolved_oxygen.ino.
// Open a serial monitor at 115200 baud and send single-letter commands
// (h = help). Results are printed as #define lines to paste into
// dissolved_oxygen.ino.

#define DO_PIN 4
#define TEMP_PIN 5

// Values currently in dissolved_oxygen.ino; used for the live DO check.
#define CURRENT_CAL1_V 355.0f
#define CURRENT_CAL1_T 25.0f
#define SATURATION_MV_PER_C 35.0f
#define SATURATION_REF_MV 1600.0f

#define SAMPLES_PER_READING 100
#define WINDOW 60                // readings (~1 per second)
#define STABLE_STDDEV_MV 5.0f
#define STABLE_DRIFT_MV 1.5f
#define STABLE_TEMP_DRIFT_C 0.15f
#define MIN_TWO_POINT_SPAN_C 5.0f

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

float voltWindow[WINDOW];
float tempWindow[WINDOW];
int windowCount = 0;
int windowHead = 0;

struct CalPoint {
  bool set;
  float mv;
  float tempC;
};

struct WindowStats {
  float meanMv;
  float stddevMv;
  float driftMv;
  float meanTempC;
  float tempDriftC;
  bool full;
  bool stable;
};

CalPoint point1 = {false, 0.0f, 0.0f};
CalPoint point2 = {false, 0.0f, 0.0f};
CalPoint zeroPoint = {false, 0.0f, 0.0f};

float saturationDoMgL(float temperatureC) {
  if (temperatureC < 0.0f) temperatureC = 0.0f;
  if (temperatureC > 40.0f) temperatureC = 40.0f;
  int t0 = (int)temperatureC;
  if (t0 >= 40) return DO_Table[40] / 1000.0f;
  float frac = temperatureC - (float)t0;
  return (DO_Table[t0] + frac * ((float)DO_Table[t0 + 1] - (float)DO_Table[t0])) / 1000.0f;
}

// Same formula as dissolved_oxygen.ino, so the live check matches the logger.
float doWithCurrentCal(float mv, float temperatureC) {
  float mvPerC = SATURATION_MV_PER_C * (CURRENT_CAL1_V / SATURATION_REF_MV);
  float vSat = CURRENT_CAL1_V + mvPerC * (temperatureC - CURRENT_CAL1_T);
  if (vSat <= 0.0f) return NAN;
  return mv * saturationDoMgL(temperatureC) / vSat;
}

bool readTemperatureC(float &temperatureC) {
  pinMode(TEMP_PIN, INPUT_PULLUP);
  if (tempSensor.getDeviceCount() == 0) {
    tempSensor.begin();
    pinMode(TEMP_PIN, INPUT_PULLUP);
    tempSensor.setResolution(12);
    if (tempSensor.getDeviceCount() == 0) return false;
  }
  tempSensor.requestTemperatures();
  temperatureC = tempSensor.getTempCByIndex(0);
  return !(isnan(temperatureC) || temperatureC == DEVICE_DISCONNECTED_C ||
           temperatureC < -10.0f || temperatureC > 85.0f);
}

float readMillivolts() {
  long sum = 0;
  for (int i = 0; i < SAMPLES_PER_READING; i++) {
    sum += analogReadMilliVolts(DO_PIN);
    delay(5);
  }
  return (float)sum / SAMPLES_PER_READING;
}

void resetWindow() {
  windowCount = 0;
  windowHead = 0;
}

void pushReading(float mv, float temperatureC) {
  voltWindow[windowHead] = mv;
  tempWindow[windowHead] = temperatureC;
  windowHead = (windowHead + 1) % WINDOW;
  if (windowCount < WINDOW) windowCount++;
}

// Index 0 is the oldest reading in the window.
int windowIndex(int i) {
  return (windowHead - windowCount + i + WINDOW) % WINDOW;
}

WindowStats computeStats() {
  WindowStats s = {0, 0, 0, 0, 0, false, false};
  if (windowCount == 0) return s;

  float sumV = 0, sumT = 0;
  for (int i = 0; i < windowCount; i++) {
    sumV += voltWindow[windowIndex(i)];
    sumT += tempWindow[windowIndex(i)];
  }
  s.meanMv = sumV / windowCount;
  s.meanTempC = sumT / windowCount;

  float var = 0;
  for (int i = 0; i < windowCount; i++) {
    float d = voltWindow[windowIndex(i)] - s.meanMv;
    var += d * d;
  }
  s.stddevMv = sqrtf(var / windowCount);

  int half = windowCount / 2;
  if (half > 0) {
    float firstV = 0, secondV = 0, firstT = 0, secondT = 0;
    for (int i = 0; i < half; i++) {
      firstV += voltWindow[windowIndex(i)];
      firstT += tempWindow[windowIndex(i)];
      secondV += voltWindow[windowIndex(windowCount - half + i)];
      secondT += tempWindow[windowIndex(windowCount - half + i)];
    }
    s.driftMv = (secondV - firstV) / half;
    s.tempDriftC = (secondT - firstT) / half;
  }

  s.full = windowCount == WINDOW;
  s.stable = s.full &&
             s.stddevMv <= STABLE_STDDEV_MV &&
             fabsf(s.driftMv) <= STABLE_DRIFT_MV &&
             fabsf(s.tempDriftC) <= STABLE_TEMP_DRIFT_C;
  return s;
}

void printHelp() {
  Serial.println();
  Serial.println("========== DO SENSOR CALIBRATION ==========");
  Serial.println("Commands (type letter + Enter):");
  Serial.println("  1  capture SATURATION point (single-point cal, or the HIGH temp point)");
  Serial.println("  2  capture second saturation point at a LOW temp (two-point cal)");
  Serial.println("  0  capture ZERO point (probe in sodium sulfite solution, optional check)");
  Serial.println("  s  show captured points and #define lines");
  Serial.println("  r  restart the stability window (after moving the probe)");
  Serial.println("  c  clear all captured points");
  Serial.println("  h  this help");
  Serial.println();
  Serial.println("Single-point (100% saturation) procedure:");
  Serial.println("  a) Aerate tap/DI water for ~10 min (air pump or vigorous stirring),");
  Serial.println("     OR hold the wet probe in air just above water (not dripping).");
  Serial.println("  b) Keep the probe still in that condition; send 'r'.");
  Serial.println("  c) Wait until the line shows [STABLE] (window of ~60 s).");
  Serial.println("  d) Send '1'. Paste the printed CAL1_V / CAL1_T into dissolved_oxygen.ino.");
  Serial.println("Two-point: do '1' in warm saturated water, then '2' in cold saturated");
  Serial.println("  water at least 5 C cooler. Prints a measured mV-per-C slope.");
  Serial.println("Probe tips: new membrane / fresh electrolyte needs ~30 min polarisation;");
  Serial.println("  keep water moving past the membrane while reading.");
  Serial.println("===========================================");
  Serial.println();
}

void printPoint(const char *name, const CalPoint &p) {
  Serial.print("  ");
  Serial.print(name);
  if (!p.set) {
    Serial.println(": not captured");
    return;
  }
  Serial.print(": ");
  Serial.print(p.mv, 1);
  Serial.print(" mV at ");
  Serial.print(p.tempC, 2);
  Serial.println(" C");
}

void printResults() {
  Serial.println();
  Serial.println("----------- CALIBRATION RESULT -----------");
  printPoint("Point 1 (saturation)", point1);
  printPoint("Point 2 (low temp)  ", point2);
  printPoint("Zero point          ", zeroPoint);

  if (point1.set) {
    Serial.println();
    Serial.println("Paste into dissolved_oxygen.ino (replace existing lines):");
    Serial.print("#define CAL1_V ");
    Serial.println(point1.mv, 1);
    Serial.print("#define CAL1_T ");
    Serial.println(point1.tempC, 1);
  }

  if (point1.set && point2.set) {
    float span = point1.tempC - point2.tempC;
    if (fabsf(span) < MIN_TWO_POINT_SPAN_C) {
      Serial.print("Two-point: temperatures only ");
      Serial.print(fabsf(span), 1);
      Serial.println(" C apart; need >= 5 C for a usable slope.");
    } else {
      float slope = (point1.mv - point2.mv) / span;
      float mvPerCSetting = slope * SATURATION_REF_MV / point1.mv;
      Serial.print("Measured slope = ");
      Serial.print(slope, 2);
      Serial.println(" mV/C");
      Serial.println("To use it, also set in dissolved_oxygen.ino:");
      Serial.print("#define SATURATION_MV_PER_C ");
      Serial.println(mvPerCSetting, 2);
      Serial.print("(currently 35.0 -> ");
      Serial.print(SATURATION_MV_PER_C * point1.mv / SATURATION_REF_MV, 2);
      Serial.println(" mV/C for this CAL1_V)");
    }
  }

  if (zeroPoint.set) {
    Serial.print("Zero offset = ");
    Serial.print(zeroPoint.mv, 1);
    Serial.println(" mV (should be close to 0; large values mean a worn membrane/electrolyte).");
  }
  Serial.println("------------------------------------------");
  Serial.println();
}

void capture(CalPoint &p, const char *name) {
  WindowStats s = computeStats();
  if (windowCount < 10) {
    Serial.println(">> Not enough readings yet, wait a few seconds and try again.");
    return;
  }
  if (!s.stable) {
    Serial.println(">> WARNING: reading is not [STABLE] yet; capturing anyway.");
    Serial.println(">>          Send 'r', wait for [STABLE], and capture again for best accuracy.");
  }
  p.set = true;
  p.mv = s.meanMv;
  p.tempC = s.meanTempC;
  Serial.print(">> Captured ");
  Serial.print(name);
  Serial.print(": ");
  Serial.print(p.mv, 1);
  Serial.print(" mV at ");
  Serial.print(p.tempC, 2);
  Serial.print(" C (avg of ");
  Serial.print(windowCount);
  Serial.println(" readings)");
  printResults();
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case '1': capture(point1, "point 1"); break;
      case '2': capture(point2, "point 2"); break;
      case '0': capture(zeroPoint, "zero point"); break;
      case 's': case 'S': printResults(); break;
      case 'r': case 'R':
        resetWindow();
        Serial.println(">> Stability window restarted.");
        break;
      case 'c': case 'C':
        point1.set = point2.set = zeroPoint.set = false;
        Serial.println(">> Cleared all captured points.");
        break;
      case 'h': case 'H': case '?': printHelp(); break;
      default: break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(DO_PIN, ADC_11db);

  pinMode(TEMP_PIN, INPUT_PULLUP);
  tempSensor.begin();
  pinMode(TEMP_PIN, INPUT_PULLUP);
  tempSensor.setResolution(12);

  delay(2000);
  printHelp();
  if (tempSensor.getDeviceCount() == 0) {
    Serial.println("ERROR: DS18B20 not detected on GPIO 5 (calibration needs temperature).");
  }
}

void loop() {
  handleSerial();

  float temperatureC = 0.0f;
  if (!readTemperatureC(temperatureC)) {
    Serial.println("ERROR: DS18B20 not detected on GPIO 5");
    delay(1000);
    return;
  }

  float mv = readMillivolts();
  pushReading(mv, temperatureC);
  WindowStats s = computeStats();

  float doNow = doWithCurrentCal(s.meanMv, s.meanTempC);
  float doSat = saturationDoMgL(s.meanTempC);

  Serial.print("V=");
  Serial.print(mv, 1);
  Serial.print(" mV  T=");
  Serial.print(temperatureC, 2);
  Serial.print(" C  | avg=");
  Serial.print(s.meanMv, 1);
  Serial.print(" sd=");
  Serial.print(s.stddevMv, 1);
  Serial.print(" drift=");
  if (s.driftMv >= 0) Serial.print('+');
  Serial.print(s.driftMv, 1);
  Serial.print(" mV  | DO(current cal)=");
  Serial.print(doNow, 2);
  Serial.print(" sat=");
  Serial.print(doSat, 2);
  Serial.print(" mg/L (");
  Serial.print(doNow / doSat * 100.0f, 0);
  Serial.print("%)  ");
  if (s.stable) {
    Serial.println("[STABLE]");
  } else if (!s.full) {
    Serial.print("[settling ");
    Serial.print(windowCount);
    Serial.print('/');
    Serial.print(WINDOW);
    Serial.println(']');
  } else {
    Serial.println("[NOT STABLE]");
  }
}
