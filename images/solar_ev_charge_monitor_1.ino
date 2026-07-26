/*
  Solar EV Charge Monitor — NEW CLEAN VERSION
  ---------------------------------------------
  Monitors:
    1. EV Battery Voltage
    2. Charging Current
    3. Temperature (LM35)
    4. Solar Panel Voltage

  Board : ESP32 Dev Module
  Relay : NC wiring
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ======== WIFI + FIREBASE ========
const char* WIFI_SSID     = "realme";
const char* WIFI_PASSWORD = "sujitha@2006";
const char* FIREBASE_URL  = "https://solar-ev-charge-monitoring-default-rtdb.asia-southeast1.firebasedatabase.app";

// ======== PINS ========
const int ACS712_PIN = 34;
const int VOLT_PIN   = 35;
const int LM35_PIN   = 33;
const int PANEL_PIN  = 32;
const int RELAY_PIN  = 26;

// ======== CALIBRATION ========
float VOLT_RATIO  = 22.9;
float PANEL_RATIO = 14.0;
float ACS712_SENS = 0.100;
float acs712Zero  = 1.65;
float MIN_CURRENT = 0.05;

// ======== BATTERY ========
float BATTERY_MIN_V  = 10.5;
float BATTERY_MAX_V  = 14.4;
float CUTOFF_VOLTAGE = 14.4;
float RESUME_VOLTAGE = 13.0;
float MAX_TEMP_C     = 45.0;

// ======== GLOBAL VARIABLES ========
float energyWh = 0;
unsigned long sessionStart = 0;
unsigned long lastSample   = 0;
bool chargingCutoff = false;

// ======== FUNCTION DECLARATIONS ========
float readEVVoltage();
float readSolarVoltage();
float readCurrent();
float readTemperature();
void calibrateCurrentSensor();
float computeBatteryPercent(float v);
float computeBatteryHealth(float voltage, float temp);
void sendToFirebase(float solar, float solarVoltage,
                    float voltage, float current, float temp,
                    float pct, float health, String status,
                    float energy, int session, int eta);

// ======== SETUP ========
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  chargingCutoff = false;

  Serial.println("");
  Serial.println("================================");
  Serial.println("  Solar EV Charge Monitor");
  Serial.println("================================");

  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED");
  }

  calibrateCurrentSensor();

  Serial.println("System ready!");
  Serial.println("================================");

  sessionStart = millis();
  lastSample   = millis();
}

// ======== LOOP ========
void loop() {
  if (millis() - lastSample >= 2000) {
    float dt = (millis() - lastSample) / 1000.0;
    lastSample = millis();

    // Read sensors
    float evVoltage    = readEVVoltage();
    float current      = readCurrent();
    float temperature  = readTemperature();
    float solarVoltage = readSolarVoltage();
    float solarPower   = solarVoltage * current;

    // Calculations
    float batteryPct    = computeBatteryPercent(evVoltage);
    float batteryHealth = computeBatteryHealth(evVoltage, temperature);

    // Energy
    if (current > MIN_CURRENT && !chargingCutoff) {
      energyWh += (evVoltage * current) * (dt / 3600.0);
    }

    // Relay logic
    String status = "charging";

    if (temperature >= MAX_TEMP_C) {
      if (!chargingCutoff) {
        digitalWrite(RELAY_PIN, HIGH);
        chargingCutoff = true;
        Serial.println(">>> OVERHEAT CUTOFF: " + String(temperature) + "C");
      }
      status = "fault";

    } else if (evVoltage >= CUTOFF_VOLTAGE) {
      if (!chargingCutoff) {
        digitalWrite(RELAY_PIN, HIGH);
        chargingCutoff = true;
        Serial.println(">>> BATTERY FULL: " + String(evVoltage) + "V");
      }
      status = "full";

    } else if (chargingCutoff && evVoltage < RESUME_VOLTAGE && temperature < MAX_TEMP_C) {
      digitalWrite(RELAY_PIN, LOW);
      chargingCutoff = false;
      Serial.println(">>> CHARGING RESUMED: " + String(evVoltage) + "V");
      status = "charging";

    } else if (!chargingCutoff) {
      status = "charging";
    } else {
      status = "full";
    }

    int sessionMin = (millis() - sessionStart) / 60000;
    int etaMin = (batteryPct >= 100)
                 ? 0
                 : max(1, (int)round((100 - batteryPct) * 1.1));

    // Serial Monitor output
    Serial.println("================================");
    Serial.printf(" Battery Voltage  : %.2f V\n", evVoltage);
    Serial.printf(" Charging Current : %.2f A\n", current);
    Serial.printf(" Temperature      : %.1f C\n", temperature);
    Serial.printf(" Solar Panel V    : %.2f V\n", solarVoltage);
    Serial.printf(" Solar Power      : %.1f W\n", solarPower);
    Serial.printf(" Battery %%        : %.0f %%\n", batteryPct);
    Serial.printf(" Battery Health   : %.0f %%\n", batteryHealth);
    Serial.printf(" Status           : %s\n", status.c_str());
    Serial.printf(" Relay            : %s\n", chargingCutoff ? "ON-cutoff" : "OFF-charging");
    Serial.println("================================");

    // Firebase
    sendToFirebase(
      solarPower, solarVoltage,
      evVoltage, current, temperature,
      batteryPct, batteryHealth, status,
      energyWh / 1000.0, sessionMin, etaMin
    );
  }
}

// ======== SENSOR FUNCTIONS ========

float readEVVoltage() {
  int raw = analogReadMilliVolts(VOLT_PIN);
  float v = (raw / 1000.0) * VOLT_RATIO;
  return v;
}

float readSolarVoltage() {
  int raw = analogReadMilliVolts(PANEL_PIN);
  float v = (raw / 1000.0) * PANEL_RATIO;
  if (v < 0.5) return 0.0;
  return v;
}

float readCurrent() {
  int raw = analogReadMilliVolts(ACS712_PIN);
  float adcV = raw / 1000.0;
  float amps = (adcV - acs712Zero) / ACS712_SENS;
  return amps < 0 ? 0 : amps;
}

float readTemperature() {
  int raw = analogReadMilliVolts(LM35_PIN);
  float tempC = raw / 10.0;
  if (tempC < 10 || tempC > 60) return 32.0;
  return tempC;
}

void calibrateCurrentSensor() {
  Serial.println("Calibrating ACS712...");
  long sum = 0;
  for (int i = 0; i < 200; i++) {
    sum += analogReadMilliVolts(ACS712_PIN);
    delay(5);
  }
  acs712Zero = (sum / 200.0) / 1000.0;
  Serial.printf("ACS712 zero = %.3f V\n", acs712Zero);
}

// ======== CALCULATIONS ========

float computeBatteryPercent(float v) {
  float pct = (v - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100.0;
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

float computeBatteryHealth(float voltage, float temp) {
  float health = 100.0;
  if (voltage < 11.0)      health -= 30.0;
  else if (voltage < 11.5) health -= 15.0;
  else if (voltage < 12.0) health -= 5.0;
  float tempPenalty = max(0.0f, abs(temp - 30.0f) - 5.0f) * 1.2f;
  health -= tempPenalty;
  if (health < 60) health = 60;
  if (health > 99) health = 99;
  return health;
}

// ======== FIREBASE ========

void sendToFirebase(float solar, float solarVoltage,
                    float voltage, float current, float temp,
                    float pct, float health, String status,
                    float energy, int session, int eta) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(FIREBASE_URL) + "/charging.json";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["solarPower"]     = solar;
  doc["solarVoltage"]   = solarVoltage;
  doc["voltage"]        = voltage;
  doc["current"]        = current;
  doc["temperature"]    = temp;
  doc["batteryPercent"] = pct;
  doc["batteryHealth"]  = health;
  doc["status"]         = status;
  doc["energyToday"]    = energy;
  doc["sessionMinutes"] = session;
  doc["etaMinutes"]     = eta;

  String payload;
  serializeJson(doc, payload);

  int code = http.PUT(payload);
  if (code > 0) {
    Serial.println("Firebase: OK");
  } else {
    Serial.println("Firebase error: " + http.errorToString(code));
  }
  http.end();
}
