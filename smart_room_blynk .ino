/*
  SMART ROOM IoT - ESP32-S3 + BLYNK (Wokwi / thiet bi that)
  Cam bien : HC-SR04, DHT22, LDR, MQ-2, IR (nut nhan trong Wokwi)
  LED      : vang = den phong, do = bao gas, xanh duong = co nguoi

  BLYNK DATASTREAMS (tao trong Blynk Console):
    V0  Distance      (Double, cm)
    V1  Temperature   (Double, C)
    V2  Humidity      (Double, %)
    V3  Light ADC     (Integer, 0-4095)
    V4  Gas ADC       (Integer, 0-4095)
    V5  Presence      (Integer 0/1)
    V6  Room LED      (Integer 0/1)
    V7  Gas alarm     (Integer 0/1)
    V8  Manual LED    (Integer 0/1)  <- Switch tu dashboard
    V9  Auto mode     (Integer 0/1)  <- Switch tu dashboard
  EVENT (tuy chon): gas_alert  (Blynk Console > Events)

  LUU Y PHAN CUNG THAT: ESP32-S3 KHONG chiu 5V. Dung chia ap cho
  chan ECHO (HC-SR04) va AO (MQ-2). Gia tri MQ-2 chi la tuong doi, khong phai ppm.
*/

// ====== THONG TIN BLYNK (thay bang cua ban) ======
#define BLYNK_TEMPLATE_ID "TMPL6CPppQDPZ"
#define BLYNK_TEMPLATE_NAME "Smart Room IoT"
#define BLYNK_AUTH_TOKEN "KkzfHA0_FcY3b6NvawqQjme_pUJOCh_8"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>

// Wokwi dung WiFi ao: SSID "Wokwi-GUEST", khong mat khau
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ====== CHAN KET NOI (ESP32-S3 DevKitC-1) ======
const int PIN_LED_ROOM     = 8;
const int PIN_LED_GAS      = 9;
const int PIN_LED_PRESENCE = 10;
const int PIN_DHT          = 4;
const int PIN_TRIG         = 5;
const int PIN_ECHO         = 6;
const int PIN_IR           = 7;   // active-low
const int PIN_LIGHT_ADC    = 1;
const int PIN_GAS_ADC      = 2;

// ====== NGUONG (hieu chuan lai voi cam bien that) ======
const int   LIGHT_DARK_THRESHOLD = 2200;   // LDR > nguong = toi
const int   GAS_ALARM_THRESHOLD  = 2300;   // MQ-2 raw
const float PRESENCE_DISTANCE_CM = 120.0;
const bool  IR_ACTIVE_LOW        = true;
const unsigned long GAS_NOTIFY_INTERVAL_MS = 60000UL; // toi da 1 thong bao/phut

DHT dht(PIN_DHT, DHT22);
BlynkTimer timer;

float temperatureC = NAN, humidityPct = NAN, distanceCm = -1.0;
int   lightRaw = 0, gasRaw = 0;
bool  irDetected = false, personDetected = false, gasAlarm = false;
bool  roomLedOn = false, autoMode = true, manualLed = false, gasLedState = false;
unsigned long lastBlinkMs = 0, lastNotifyMs = 0;

// ---------- DOC CAM BIEN ----------
float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long duration = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (duration == 0) return -1.0;
  return duration / 58.0;
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    temperatureC = t;
    humidityPct  = h;
  }

  distanceCm = readDistanceCm();
  lightRaw   = analogRead(PIN_LIGHT_ADC);
  gasRaw     = analogRead(PIN_GAS_ADC);

  int ir = digitalRead(PIN_IR);
  irDetected = IR_ACTIVE_LOW ? (ir == LOW) : (ir == HIGH);
  bool ultrasonic = (distanceCm >= 2.0 && distanceCm <= PRESENCE_DISTANCE_CM);
  personDetected = irDetected || ultrasonic;
  gasAlarm = (gasRaw >= GAS_ALARM_THRESHOLD);

  Serial.println("\n--- SMART ROOM SENSOR DATA ---");
  Serial.printf("Temperature : %.1f C\n", temperatureC);
  Serial.printf("Humidity    : %.1f %%\n", humidityPct);
  if (distanceCm < 0) Serial.println("Distance    : No echo");
  else Serial.printf("Distance    : %.1f cm\n", distanceCm);
  Serial.printf("Light ADC   : %d\n", lightRaw);
  Serial.printf("Gas ADC     : %d (raw, not ppm)\n", gasRaw);
  Serial.printf("Presence    : %s\n", personDetected ? "YES" : "NO");
  Serial.printf("Gas alarm   : %s\n", gasAlarm ? "YES" : "NO");
  Serial.printf("Mode        : %s\n", autoMode ? "AUTO" : "MANUAL");
}

// ---------- DIEU KHIEN LED ----------
void setRoomLed(bool state) {
  if (roomLedOn != state) {
    roomLedOn = state;
    digitalWrite(PIN_LED_ROOM, roomLedOn ? HIGH : LOW);
  }
}

void updateLedLogic() {
  const unsigned long now = millis();

  digitalWrite(PIN_LED_PRESENCE, personDetected ? HIGH : LOW);

  // Bao gas co uu tien cao nhat
  if (gasAlarm) {
    setRoomLed(false);
    if (now - lastBlinkMs >= 300UL) {
      lastBlinkMs = now;
      gasLedState = !gasLedState;
    }
    digitalWrite(PIN_LED_GAS, gasLedState ? HIGH : LOW);
    return;
  }

  gasLedState = false;
  digitalWrite(PIN_LED_GAS, LOW);

  if (autoMode) {
    bool isDark = (lightRaw > LIGHT_DARK_THRESHOLD);
    setRoomLed(personDetected && isDark);
  } else {
    setRoomLed(manualLed);
  }
}

// ---------- BLYNK ----------
BLYNK_WRITE(V8) { manualLed = (param.asInt() == 1); }
BLYNK_WRITE(V9) { autoMode  = (param.asInt() == 1); }

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V8, V9);
  Serial.println("Blynk connected.");
}

void publishDashboard() {
  if (!Blynk.connected()) return;
  Blynk.virtualWrite(V0, distanceCm < 0 ? 0 : distanceCm);
  if (!isnan(temperatureC)) Blynk.virtualWrite(V1, temperatureC);
  if (!isnan(humidityPct))  Blynk.virtualWrite(V2, humidityPct);
  Blynk.virtualWrite(V3, lightRaw);
  Blynk.virtualWrite(V4, gasRaw);
  Blynk.virtualWrite(V5, personDetected ? 1 : 0);
  Blynk.virtualWrite(V6, roomLedOn ? 1 : 0);
  Blynk.virtualWrite(V7, gasAlarm ? 1 : 0);

  // Gui canh bao day ve dien thoai (gioi han tan suat)
  if (gasAlarm && (millis() - lastNotifyMs >= GAS_NOTIFY_INTERVAL_MS || lastNotifyMs == 0)) {
    lastNotifyMs = millis();
    Blynk.logEvent("gas_alert", "Phat hien khi gas/khoi trong phong!");
  }
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_ROOM, OUTPUT);
  pinMode(PIN_LED_GAS, OUTPUT);
  pinMode(PIN_LED_PRESENCE, OUTPUT);
  digitalWrite(PIN_LED_ROOM, LOW);
  digitalWrite(PIN_LED_GAS, LOW);
  digitalWrite(PIN_LED_PRESENCE, LOW);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_IR, INPUT_PULLUP);
  analogReadResolution(12);
  dht.begin();

  Serial.println("Smart Room IoT (Blynk) starting...");
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);

  timer.setInterval(2000L, readSensors);
  timer.setInterval(2200L, publishDashboard);
  readSensors();
}

void loop() {
  Blynk.run();
  timer.run();
  updateLedLogic();
  delay(10);
}
