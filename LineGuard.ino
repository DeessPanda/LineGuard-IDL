/*
 * LineGuard ESP32 Firmware
 * Isolated low-voltage DC prototype voltage monitor + BLE GATT server.
 *
 * SAFETY: ONLY for an isolated 12 V DC bench supply. NEVER connect to
 * utility mains, overhead distribution lines, or hazardous electrical sources.
 *
 * Libraries (Arduino IDE Library Manager):
 *   - ArduinoJson by Benoit Blanchon (v7.x)
 * Board: "ESP32 Dev Module" (Espressif ESP32 core v3.x)
 * Pin: GPIO34 (ADC1_CH6)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <math.h>
#include <ctype.h>

// ---------- defaults ----------
#define FW_VERSION          "1.0.0"
#define DEFAULT_DEVICE_ID   "LG-NODE-001"
#define DEFAULT_POLE        "LG-001"

#define ADC_PIN             34
#define R_TOP               100000.0f   // 100 kΩ
#define R_BOTTOM            27000.0f    // 27 kΩ
#define ADC_SAMPLES         15
#define SAMPLE_INTERVAL_MS  500
#define HEARTBEAT_MS        5000
#define DEFAULT_DEBOUNCE_MS 3000
#define DEFAULT_HYST_V      0.3f
#define MAX_ADC_MV          3200

// ---------- UUIDs (must match index.html) ----------
#define SERVICE_UUID         "6c1b0001-7a3c-4f9e-9b2a-1d4e5f6a7b80"
#define CHAR_DEVICE_ID_UUID  "6c1b0002-7a3c-4f9e-9b2a-1d4e5f6a7b80"
#define CHAR_POLE_UUID       "6c1b0003-7a3c-4f9e-9b2a-1d4e5f6a7b80"
#define CHAR_TELEMETRY_UUID  "6c1b0004-7a3c-4f9e-9b2a-1d4e5f6a7b80"
#define CHAR_CONFIG_UUID     "6c1b0005-7a3c-4f9e-9b2a-1d4e5f6a7b80"

// ---------- state ----------
enum VState : uint8_t { ST_NORMAL=0, ST_WARNING=1, ST_CRITICAL=2, ST_LOSS=3 };

Preferences prefs;
String deviceId = DEFAULT_DEVICE_ID;
String pole     = DEFAULT_POLE;
float calibration = 1.0f;
float vNormal     = 10.0f;
float vWarning    = 8.0f;
float vCritical   = 1.0f;
float hystV       = DEFAULT_HYST_V;
uint32_t debounceMs = DEFAULT_DEBOUNCE_MS;
float latitude = 0.0f, longitude = 0.0f;
bool locConfigured = false;

BLEServer* pServer = nullptr;
BLEService* pService = nullptr;
BLECharacteristic* pTelemetryChar = nullptr;
BLECharacteristic* pConfigChar = nullptr;
BLECharacteristic* pDeviceIdChar = nullptr;
BLECharacteristic* pPoleChar = nullptr;

volatile bool deviceConnected = false;
volatile bool telemetrySubscribed = false;
volatile uint16_t connId = 0;
volatile bool sendHello = false;
volatile bool sendConfigEvent = false;
volatile bool configWritePending = false;
String pendingConfigJson = "";

uint8_t nextMessageId = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastSampleMs = 0;

VState currentState = ST_NORMAL;
VState pendingState = ST_NORMAL;
bool pendingSet = false;
unsigned long pendingSince = 0;
unsigned long stateChangedAtMs = 0;
unsigned long alertSinceMs = 0;

float lastVoltage = 0.0f;
uint32_t lastAdcMv = 0;
bool sensorValid = false;

// ---------- helpers ----------
const char* stateToString(VState s) {
  switch (s) {
    case ST_NORMAL:   return "NORMAL";
    case ST_WARNING:  return "WARNING";
    case ST_CRITICAL: return "CRITICAL_UNDERVOLTAGE";
    case ST_LOSS:     return "SUPPLY_LOSS";
  }
  return "UNKNOWN";
}

const char* stateMessage(VState s) {
  switch (s) {
    case ST_NORMAL:   return "Voltage within expected range.";
    case ST_WARNING:  return "Low-voltage condition. Inspect the monitored section.";
    case ST_CRITICAL: return "Severe undervoltage. Possible supply loss or line interruption; inspection required.";
    case ST_LOSS:     return "Possible supply loss or line interruption; inspection required.";
  }
  return "State unknown.";
}

bool tokenOk(const String& s, size_t maxLen) {
  if (s.length() == 0 || s.length() > maxLen) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
  }
  return true;
}

// ---------- preferences ----------
void loadPrefs() {
  prefs.begin("lineguard", false);
  deviceId       = prefs.getString("deviceId", DEFAULT_DEVICE_ID);
  pole           = prefs.getString("pole", DEFAULT_POLE);
  calibration    = prefs.getFloat("cal", 1.0f);
  vNormal        = prefs.getFloat("vN", 10.0f);
  vWarning       = prefs.getFloat("vW", 8.0f);
  vCritical      = prefs.getFloat("vC", 1.0f);
  hystV          = prefs.getFloat("hys", DEFAULT_HYST_V);
  debounceMs     = prefs.getUInt("deb", DEFAULT_DEBOUNCE_MS);
  latitude       = prefs.getFloat("lat", 0.0f);
  longitude      = prefs.getFloat("lon", 0.0f);
  locConfigured  = prefs.getBool("locSet", false);
}

void savePrefs() {
  prefs.putString("deviceId", deviceId);
  prefs.putString("pole", pole);
  prefs.putFloat("cal", calibration);
  prefs.putFloat("vN", vNormal);
  prefs.putFloat("vW", vWarning);
  prefs.putFloat("vC", vCritical);
  prefs.putFloat("hys", hystV);
  prefs.putUInt("deb", debounceMs);
  prefs.putFloat("lat", latitude);
  prefs.putFloat("lon", longitude);
  prefs.putBool("locSet", locConfigured);
}

// ---------- measurement ----------
bool readVoltage(float& vin) {
  uint32_t samples[ADC_SAMPLES];
  int n = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    uint32_t mv = analogReadMilliVolts(ADC_PIN);
    if (mv < MAX_ADC_MV) samples[n++] = mv;
  }
  if (n == 0) { sensorValid = false; return false; }

  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (samples[j] < samples[i]) { uint32_t t=samples[i]; samples[i]=samples[j]; samples[j]=t; }

  uint32_t median = (n % 2) ? samples[n/2] : (samples[n/2 - 1] + samples[n/2]) / 2;
  lastAdcMv = median;
  vin = (median / 1000.0f) * ((R_TOP + R_BOTTOM) / R_BOTTOM) * calibration;
  sensorValid = true;
  return true;
}

VState classifyHyst(float v, VState cur) {
  float h = hystV;
  switch (cur) {
    case ST_NORMAL:
      if (v >= vNormal   - h) return ST_NORMAL;
      if (v >= vWarning  - h) return ST_WARNING;
      if (v >= vCritical - h) return ST_CRITICAL;
      return ST_LOSS;
    case ST_WARNING:
      if (v >= vNormal   + h) return ST_NORMAL;
      if (v >= vWarning  - h) return ST_WARNING;
      if (v >= vCritical - h) return ST_CRITICAL;
      return ST_LOSS;
    case ST_CRITICAL:
      if (v >= vNormal   + h) return ST_NORMAL;
      if (v >= vWarning  + h) return ST_WARNING;
      if (v >= vCritical - h) return ST_CRITICAL;
      return ST_LOSS;
    case ST_LOSS:
      if (v >= vNormal   + h) return ST_NORMAL;
      if (v >= vWarning  + h) return ST_WARNING;
      if (v >= vCritical + h) return ST_CRITICAL;
      return ST_LOSS;
  }
  return ST_LOSS;
}

void updateState(float v, unsigned long now) {
  VState cand = classifyHyst(v, currentState);
  if (cand == currentState) { pendingSet = false; return; }
  if (!pendingSet || cand != pendingState) {
    pendingState = cand; pendingSince = now; pendingSet = true; return;
  }
  if (now - pendingSince >= debounceMs) {
    currentState = cand;
    stateChangedAtMs = now;
    alertSinceMs = (currentState == ST_NORMAL) ? 0 : now;
    pendingSet = false;
    Serial.printf("State -> %s (%.2f V)\n", stateToString(currentState), lastVoltage);
    sendTelemetry("STATE_CHANGE", false);
  }
}

// ---------- telemetry JSON ----------
String buildTelemetryJson(const char* event, bool withConfig) {
  JsonDocument doc;
  doc["type"] = "telemetry";
  doc["event"] = event;
  doc["fw"] = FW_VERSION;
  doc["deviceId"] = deviceId;
  doc["pole"] = pole;
  doc["online"] = true;
  doc["uptimeMs"] = millis();
  doc["sensorValid"] = sensorValid;
  doc["adcMv"] = lastAdcMv;
  doc["voltage"] = round(lastVoltage * 100.0f) / 100.0f;
  doc["state"] = stateToString(currentState);
  doc["stateCode"] = (uint8_t)currentState;
  doc["stateChangedAtMs"] = stateChangedAtMs;

  doc["alert"]["active"]  = (currentState != ST_NORMAL);
  doc["alert"]["type"]    = stateToString(currentState);
  doc["alert"]["message"] = stateMessage(currentState);
  doc["alert"]["sinceMs"] = alertSinceMs;
  doc["alert"]["voltage"] = round(lastVoltage * 100.0f) / 100.0f;

  doc["location"]["configured"] = locConfigured;
  if (locConfigured) {
    doc["location"]["latitude"]  = latitude;
    doc["location"]["longitude"] = longitude;
  }

  if (withConfig) {
    doc["config"]["cal"]        = calibration;
    doc["config"]["vNormal"]    = vNormal;
    doc["config"]["vWarning"]   = vWarning;
    doc["config"]["vCritical"]  = vCritical;
    doc["config"]["hysteresis"] = hystV;
    doc["config"]["debounceMs"] = debounceMs;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// ---------- BLE chunking ----------
// Notification payload: [0x01][msgId][seq][total][chunk bytes...]
void sendChunkedJson(const String& json) {
  if (!deviceConnected || !telemetrySubscribed) return;
  uint16_t mtu = pServer->getPeerMTU(connId);
  if (mtu < 23) mtu = 23;

  size_t maxPayload = mtu - 3 - 4;
  if (maxPayload > 500) maxPayload = 500;
  if (maxPayload < 1)   maxPayload = 1;

  uint8_t total = (uint8_t)((json.length() + maxPayload - 1) / maxPayload);
  if (total == 0) total = 1;

  uint8_t msgId = nextMessageId++;
  uint8_t buffer[4 + 500];

  for (uint8_t seq = 0; seq < total; seq++) {
    size_t offset = seq * maxPayload;
    size_t len = json.length() - offset;
    if (len > maxPayload) len = maxPayload;
    buffer[0] = 0x01;
    buffer[1] = msgId;
    buffer[2] = seq;
    buffer[3] = total;
    memcpy(buffer + 4, json.c_str() + offset, len);
    pTelemetryChar->setValue(buffer, 4 + len);
    pTelemetryChar->notify();
    delay(10);
  }
}

void sendTelemetry(const char* event, bool withConfig) {
  if (!deviceConnected) return;
  String json = buildTelemetryJson(event, withConfig);
  pTelemetryChar->setValue(json.c_str());
  if (telemetrySubscribed) sendChunkedJson(json);
  lastHeartbeatMs = millis();
}

// ---------- configuration ----------
void sendConfigAck(bool ok, const String& err) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (!ok) doc["error"] = err;
  String out;
  serializeJson(doc, out);
  pConfigChar->setValue(out.c_str());
  Serial.printf("Config ack: %s\n", out.c_str());
}

void handleConfigWrite(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) { sendConfigAck(false, "invalid_json"); return; }

  bool commit = doc["commit"].isNull() ? false : doc["commit"].as<bool>();
  if (!commit) { sendConfigAck(false, "commit_required"); return; }

  String newDeviceId = doc["deviceId"].isNull() ? deviceId : doc["deviceId"].as<String>();
  String newPole     = doc["pole"].isNull()     ? pole     : doc["pole"].as<String>();
  float newCal  = doc["calibrationFactor"].isNull() ? calibration : doc["calibrationFactor"].as<float>();
  float newVn   = doc["vNormal"].isNull()   ? vNormal   : doc["vNormal"].as<float>();
  float newVw   = doc["vWarning"].isNull()  ? vWarning  : doc["vWarning"].as<float>();
  float newVc   = doc["vCritical"].isNull() ? vCritical : doc["vCritical"].as<float>();
  float newHys  = doc["hysteresisV"].isNull()  ? hystV      : doc["hysteresisV"].as<float>();
  uint32_t newDeb = doc["debounceMs"].isNull() ? debounceMs : doc["debounceMs"].as<uint32_t>();

  bool clearLoc = doc["clearLocation"].isNull() ? false : doc["clearLocation"].as<bool>();
  bool hasLat = !doc["latitude"].isNull();
  bool hasLon = !doc["longitude"].isNull();
  float newLat = hasLat ? doc["latitude"].as<float>()  : latitude;
  float newLon = hasLon ? doc["longitude"].as<float>() : longitude;

  if (!tokenOk(newDeviceId, 24)) { sendConfigAck(false, "invalid_device_id"); return; }
  if (!tokenOk(newPole, 16))     { sendConfigAck(false, "invalid_pole"); return; }
  if (newCal < 0.5f || newCal > 1.5f) { sendConfigAck(false, "calibration_out_of_range"); return; }
  if (newVc < 0.2f || newVw <= newVc || newVn <= newVw) { sendConfigAck(false, "threshold_order_invalid"); return; }
  if (newHys < 0.0f || newHys > 2.0f) { sendConfigAck(false, "hysteresis_out_of_range"); return; }
  if (newDeb < 500 || newDeb > 60000) { sendConfigAck(false, "debounce_out_of_range"); return; }
  if ((hasLat != hasLon) && !clearLoc) { sendConfigAck(false, "lat_lon_pair_required"); return; }
  if (hasLat && (newLat < -90.0f || newLat > 90.0f))   { sendConfigAck(false, "lat_out_of_range"); return; }
  if (hasLon && (newLon < -180.0f || newLon > 180.0f)) { sendConfigAck(false, "lon_out_of_range"); return; }

  deviceId = newDeviceId;
  pole = newPole;
  calibration = newCal;
  vNormal = newVn; vWarning = newVw; vCritical = newVc;
  hystV = newHys; debounceMs = newDeb;

  if (clearLoc) { locConfigured = false; }
  else if (hasLat && hasLon) { latitude = newLat; longitude = newLon; locConfigured = true; }

  savePrefs();
  pDeviceIdChar->setValue(deviceId.c_str());
  pPoleChar->setValue(pole.c_str());

  pendingSet = false;
  sendConfigAck(true, "");
  sendConfigEvent = true;
}

// ---------- BLE callbacks ----------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    deviceConnected = true;
    connId = s->getConnId();
    telemetrySubscribed = false;
    sendHello = false;
    sendConfigEvent = false;
    lastHeartbeatMs = 0;
    Serial.println("BLE connected");
  }
  void onDisconnect(BLEServer* s) override {
    (void)s;
    deviceConnected = false;
    telemetrySubscribed = false;
    sendHello = false;
    sendConfigEvent = false;
    Serial.println("BLE disconnected");
    BLEDevice::startAdvertising();
  }
};

class TelemetryCallbacks : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic* c, uint16_t cid, uint16_t sub) override {
    (void)c; (void)cid; (void)sub;
    telemetrySubscribed = true;
    sendHello = true;
  }
};

class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v(c->getValue().c_str());
    if (v.length() > 0) { pendingConfigJson = v; configWritePending = true; }
  }
};

// ---------- BLE init ----------
void initBle() {
  String name = "LineGuard-" + pole;
  BLEDevice::init(name.c_str());
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  pService = pServer->createService(SERVICE_UUID);

  pDeviceIdChar = pService->createCharacteristic(CHAR_DEVICE_ID_UUID, BLECharacteristic::PROPERTY_READ);
  pDeviceIdChar->setValue(deviceId.c_str());

  pPoleChar = pService->createCharacteristic(CHAR_POLE_UUID, BLECharacteristic::PROPERTY_READ);
  pPoleChar->setValue(pole.c_str());

  pTelemetryChar = pService->createCharacteristic(
      CHAR_TELEMETRY_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pTelemetryChar->addDescriptor(new BLE2902());
  pTelemetryChar->setCallbacks(new TelemetryCallbacks());
  pTelemetryChar->setValue("{\"type\":\"telemetry\",\"event\":\"BOOT\"}");

  pConfigChar = pService->createCharacteristic(
      CHAR_CONFIG_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pConfigChar->setCallbacks(new ConfigCallbacks());
  pConfigChar->setValue("{\"ok\":true,\"hint\":\"write JSON with commit=true\"}");

  pService->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising started");
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nLineGuard booting...");
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);
  loadPrefs();

  float v0;
  if (readVoltage(v0)) {
    lastVoltage = v0;
    currentState = classifyHyst(v0, ST_NORMAL);
    stateChangedAtMs = millis();
    alertSinceMs = (currentState == ST_NORMAL) ? 0 : millis();
    Serial.printf("Initial: %.2f V -> %s\n", lastVoltage, stateToString(currentState));
  }
  initBle();
}

void loop() {
  unsigned long now = millis();

  if (configWritePending) {
    String j = pendingConfigJson;
    configWritePending = false;
    handleConfigWrite(j);
  }

  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    float v;
    if (readVoltage(v)) { lastVoltage = v; updateState(v, now); }
  }

  if (deviceConnected && telemetrySubscribed) {
    if (sendHello) { sendHello = false; sendTelemetry("HELLO", true); }
    else if (sendConfigEvent) { sendConfigEvent = false; sendTelemetry("CONFIG", true); }
    else if (lastHeartbeatMs == 0 || (now - lastHeartbeatMs) >= HEARTBEAT_MS) {
      sendTelemetry("HEARTBEAT", false);
    }
  }
  delay(10);
}
