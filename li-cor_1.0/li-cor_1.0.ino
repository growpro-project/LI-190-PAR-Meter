// ========================================================================================
// PROJECT      : LI-COR LI-190SA Firmware
// Version      : v1.0
// Copyright    : Copyright (c) 2026 GrowPro (https://github.com/growpro-project)
// LICENSE      : PolyForm Noncommercial License 1.0.0
//                (https://polyformproject.org/licenses/noncommercial/1.0.0)
//                Commercial use is strictly prohibited without prior written permission.
// ----------------------------------------------------------------------------------------
// HARDWARE     : ESP32 + ADS1115 (LI-COR Quantum Sensor Reader)
// FUNCTION     : Continuous PPFD measurement & web interface
// ----------------------------------------------------------------------------------------
// MQTT         : Payload Example:
//                {
//                  "device": "LI-COR",
//                  "ppfd": 1692.0
//                }
// ----------------------------------------------------------------------------------------
// NOTE         : LittleFS Upload via Arduino IDE:
//                [⌘] + [Shift] + [P] -> "Upload LittleFS to Pico/ESP8266/ESP32"
// ========================================================================================


#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

Adafruit_ADS1115 ads;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
AsyncWebServer webServer(80);

String wifiSsid;
String wifiPassword;
String mqttServer;
String mqttUser;
String mqttPassword;
String topicStatus;
String hostName;
bool adsAvailable = false;
uint8_t adsAddress = 0;
bool configValid = false;

float shuntResistanceOhms = 0.0f;
float sensorMultiplierUmolPerUa = 0.0f;
float voltageMultiplierUmolPerMv = 0.0f;
float darkOffsetMv = 0.0f;
float wattsPerM2PerUmol = 0.0f;
float ppfdCorrectionFactor = 1.0f;
int differentialPolarity = 0;
uint8_t sampleCount = 0;
uint32_t sampleIntervalMs = 0;

uint32_t lastMqttPublish = 0;
uint32_t lastMqttAttempt = 0;
uint32_t mqttPublishIntervalMs = 5000;
bool mqttRetainStatus = true;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 5000;
constexpr size_t CONFIG_DOC_CAPACITY = 24576;
constexpr size_t MAX_FIELD_ADJUSTMENTS = 20;

struct SensorReading {
  float voltageMv;
  float currentUa;
  float ppfd;
  float rawPpfd;
  float estimatedWPerM2;
};

SensorReading latestReading = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
uint32_t lastSensorSample = 0;
bool hasSensorReading = false;

bool readConfigDocument(JsonDocument &doc) {
  if (!LittleFS.exists("/config.json")) return false;
  File file = LittleFS.open("/config.json", FILE_READ);
  if (!file) return false;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  return !error;
}

bool hasFiniteNumber(JsonVariantConst value) {
  return !value.isNull() && isfinite(value.as<float>());
}

bool hasText(JsonVariantConst value) {
  return !value.isNull() && value.is<const char *>();
}

bool validateConfigDocument(const JsonDocument &doc) {
  const char *topLevelKeys[] = {
    "SSID", "PASS", "MQTTSERVER", "MQTTUser", "MQTTPass", "TopicSTATUS",
    "MQTTIntervalSeconds", "MQTTRetainStatus", "ShowIrradiance", "ShowDLI", "DLILightHours", "PpfdCorrectionFactor", "HostName", "Sensor",
    "FactoryCalibration", "FieldAdjustments"
  };
  for (const char *key : topLevelKeys) {
    if (doc[key].isNull()) return false;
  }

  const String configuredHost = doc["HostName"].as<String>();
  if (configuredHost.isEmpty() || !hasFiniteNumber(doc["MQTTIntervalSeconds"])) return false;
  if (doc["MQTTIntervalSeconds"].as<float>() < 1.0f) return false;
  if (!doc["ShowIrradiance"].is<bool>()) return false;
  if (!doc["ShowDLI"].is<bool>() || !hasFiniteNumber(doc["DLILightHours"]) ||
      doc["DLILightHours"].as<float>() < 1.0f || doc["DLILightHours"].as<float>() > 24.0f) return false;
  if (!hasFiniteNumber(doc["PpfdCorrectionFactor"]) || doc["PpfdCorrectionFactor"].as<float>() <= 0.0f) return false;

  JsonObjectConst sensor = doc["Sensor"].as<JsonObjectConst>();
  const char *sensorKeys[] = {
    "Model", "Serial", "ActiveCalibration", "ShuntResistanceOhms",
    "MultiplierUmolPerUa", "VoltageMultiplierUmolPerMv", "DifferentialPolarity",
    "DarkOffsetMv", "SampleCount", "SampleIntervalMs", "WPerM2PerUmol"
  };
  for (const char *key : sensorKeys) {
    if (sensor[key].isNull()) return false;
  }
  const String calibration = sensor["ActiveCalibration"].as<String>();
  if (calibration != "certificate" && calibration != "field") return false;
  if (!hasFiniteNumber(sensor["ShuntResistanceOhms"]) || sensor["ShuntResistanceOhms"].as<float>() <= 0.0f) return false;
  if (!hasFiniteNumber(sensor["MultiplierUmolPerUa"]) || sensor["MultiplierUmolPerUa"].as<float>() <= 0.0f) return false;
  if (!hasFiniteNumber(sensor["VoltageMultiplierUmolPerMv"]) || sensor["VoltageMultiplierUmolPerMv"].as<float>() <= 0.0f) return false;
  if (sensor["DifferentialPolarity"].as<int>() != 1 && sensor["DifferentialPolarity"].as<int>() != -1) return false;
  if (!hasFiniteNumber(sensor["DarkOffsetMv"]) || sensor["DarkOffsetMv"].as<float>() < 0.0f) return false;
  if (sensor["SampleCount"].as<int>() < 1 || sensor["SampleCount"].as<int>() > 128) return false;
  if (sensor["SampleIntervalMs"].as<uint32_t>() < 100) return false;
  if (!hasFiniteNumber(sensor["WPerM2PerUmol"]) || sensor["WPerM2PerUmol"].as<float>() <= 0.0f) return false;

  JsonObjectConst factory = doc["FactoryCalibration"].as<JsonObjectConst>();
  if (!hasFiniteNumber(factory["MultiplierUmolPerUa"]) || factory["MultiplierUmolPerUa"].as<float>() <= 0.0f) return false;
  if (!hasFiniteNumber(factory["DarkOffsetMv"]) || factory["DarkOffsetMv"].as<float>() < 0.0f) return false;
  if (!hasText(factory["CalibrationDate"])) return false;

  JsonArrayConst adjustments = doc["FieldAdjustments"].as<JsonArrayConst>();
  if (adjustments.isNull() || adjustments.size() > MAX_FIELD_ADJUSTMENTS) return false;
  for (JsonObjectConst entry : adjustments) {
    if (!hasText(entry["Id"]) || !hasText(entry["Date"]) || !hasFiniteNumber(entry["ReferencePpfd"]) ||
        !hasFiniteNumber(entry["VoltageMultiplierUmolPerMv"]) || !hasFiniteNumber(entry["DarkOffsetMv"]) ||
        entry["ReferencePpfd"].as<float>() <= 0.0f || entry["VoltageMultiplierUmolPerMv"].as<float>() <= 0.0f ||
        entry["DarkOffsetMv"].as<float>() < 0.0f || entry["Active"].isNull()) return false;
  }
  return true;
}

bool writeConfig(const JsonDocument &doc) {
  const char *temporaryPath = "/config.tmp";
  LittleFS.remove(temporaryPath);
  File file = LittleFS.open(temporaryPath, FILE_WRITE);
  if (!file) return false;
  const size_t written = serializeJsonPretty(doc, file);
  file.close();
  if (written == 0) {
    LittleFS.remove(temporaryPath);
    return false;
  }
  LittleFS.remove("/config.json");
  return LittleFS.rename(temporaryPath, "/config.json");
}

void updateLegacyFieldAdjustment(JsonObject legacy, JsonObject entry, JsonObject sensor) {
  legacy["Available"] = true;
  legacy["Id"] = entry["Id"].as<const char *>();
  legacy["ReferencePpfd"] = entry["ReferencePpfd"].as<float>();
  legacy["VoltageMultiplierUmolPerMv"] = entry["VoltageMultiplierUmolPerMv"].as<float>();
  legacy["DarkOffsetMv"] = entry["DarkOffsetMv"].as<float>();
  legacy["Date"] = entry["Date"].as<const char *>();
  legacy["ShuntResistanceOhms"] = sensor["ShuntResistanceOhms"].as<float>();
  legacy["MultiplierUmolPerUa"] = sensor["MultiplierUmolPerUa"].as<float>();
}

void applyFieldAdjustmentToDocument(JsonDocument &doc, JsonObject selected) {
  JsonObject sensor = doc["Sensor"].as<JsonObject>();
  sensor["ActiveCalibration"] = "field";
  sensor["VoltageMultiplierUmolPerMv"] = selected["VoltageMultiplierUmolPerMv"].as<float>();
  sensor["DarkOffsetMv"] = selected["DarkOffsetMv"].as<float>();

  JsonArray adjustments = doc["FieldAdjustments"].as<JsonArray>();
  const String selectedId = selected["Id"].as<String>();
  for (JsonObject entry : adjustments) {
    const String entryId = entry["Id"].as<String>();
    entry["Active"] = entryId == selectedId;
  }

  JsonObject legacy = doc["LastFieldAdjustment"].as<JsonObject>();
  updateLegacyFieldAdjustment(legacy, selected, sensor);
}

void applyCertificateCalibrationToDocument(JsonDocument &doc) {
  JsonObject sensor = doc["Sensor"].as<JsonObject>();
  JsonObject factory = doc["FactoryCalibration"].as<JsonObject>();
  const float factoryMultiplier = factory["MultiplierUmolPerUa"].as<float>();
  const float configuredShunt = sensor["ShuntResistanceOhms"].as<float>();
  const float factoryFactor = factoryMultiplier * 1000.0f / configuredShunt;
  const float factoryDarkOffset = factory["DarkOffsetMv"].as<float>();
  sensor["ActiveCalibration"] = "certificate";
  sensor["MultiplierUmolPerUa"] = factoryMultiplier;
  sensor["VoltageMultiplierUmolPerMv"] = factoryFactor;
  sensor["DarkOffsetMv"] = factoryDarkOffset;

  JsonArray adjustments = doc["FieldAdjustments"].as<JsonArray>();
  for (JsonObject entry : adjustments) entry["Active"] = false;
  if (adjustments.size() > 0) {
    JsonObject latest = adjustments[adjustments.size() - 1].as<JsonObject>();
    JsonObject legacy = doc["LastFieldAdjustment"].as<JsonObject>();
    updateLegacyFieldAdjustment(legacy, latest, sensor);
  }
}

bool loadConfig() {
  DynamicJsonDocument doc(CONFIG_DOC_CAPACITY);
  if (!readConfigDocument(doc) || !validateConfigDocument(doc)) {
    configValid = false;
    return false;
  }

  wifiSsid = doc["SSID"].as<String>();
  wifiPassword = doc["PASS"].as<String>();
  mqttServer = doc["MQTTSERVER"].as<String>();
  mqttUser = doc["MQTTUser"].as<String>();
  mqttPassword = doc["MQTTPass"].as<String>();
  topicStatus = doc["TopicSTATUS"].as<String>();
  const uint32_t mqttIntervalSeconds = doc["MQTTIntervalSeconds"].as<uint32_t>();
  mqttPublishIntervalMs = mqttIntervalSeconds * 1000UL;
  mqttRetainStatus = doc["MQTTRetainStatus"].as<bool>();
  hostName = doc["HostName"].as<String>();
  ppfdCorrectionFactor = doc["PpfdCorrectionFactor"].as<float>();

  JsonObject sensor = doc["Sensor"].as<JsonObject>();
  shuntResistanceOhms = sensor["ShuntResistanceOhms"].as<float>();
  sensorMultiplierUmolPerUa = sensor["MultiplierUmolPerUa"].as<float>();
  voltageMultiplierUmolPerMv = sensor["VoltageMultiplierUmolPerMv"].as<float>();
  differentialPolarity = sensor["DifferentialPolarity"].as<int>();
  darkOffsetMv = sensor["DarkOffsetMv"].as<float>();
  sampleCount = sensor["SampleCount"].as<uint8_t>();
  sampleIntervalMs = sensor["SampleIntervalMs"].as<uint32_t>();
  wattsPerM2PerUmol = sensor["WPerM2PerUmol"].as<float>();
  configValid = true;
  return true;
}

uint8_t findAdsAddress() {
  for (uint8_t address = 0x48; address <= 0x4B; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) return address;
  }
  return 0;
}

bool readAdsDifferential01(int16_t &raw) {
  if (!adsAvailable || adsAddress == 0) return false;

  // Check that the ADS1115 still acknowledges before starting a conversion.
  Wire.beginTransmission(adsAddress);
  if (Wire.endTransmission() != 0) return false;

  // The Adafruit helper waits forever if an I2C read fails. Start the
  // conversion explicitly and use the known 128 SPS conversion time instead.
  ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);
  delay(10);
  raw = ads.getLastConversionResults();
  return true;
}

SensorReading readSensor() {
  if (!adsAvailable) {
    return { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
  }

  int64_t rawTotal = 0;
  uint8_t successfulSamples = 0;
  for (uint8_t i = 0; i < sampleCount; ++i) {
    int16_t raw = 0;
    if (readAdsDifferential01(raw)) {
      rawTotal += raw;
      ++successfulSamples;
    }
    delay(3);
  }

  if (successfulSamples == 0) {
    return { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
  }

  const int16_t averageRaw = static_cast<int16_t>(rawTotal / successfulSamples);
  const float rawVoltageMv = ads.computeVolts(averageRaw) * 1000.0f;
  const float voltageMv = rawVoltageMv * differentialPolarity;
  const float correctedVoltageMv = voltageMv - darkOffsetMv;
  const float currentUa = max(0.0f, correctedVoltageMv * 1000.0f / shuntResistanceOhms);
  const float rawPpfd = max(0.0f, correctedVoltageMv * voltageMultiplierUmolPerMv);
  const float ppfd = rawPpfd * ppfdCorrectionFactor;

  // Irradiance remains a solar-spectrum estimate and deliberately uses the
  // uncorrected sensor PPFD. The optional correction factor is for PPFD/DLI
  // and MQTT PPFD only.
  return { voltageMv, currentUa, ppfd, rawPpfd, rawPpfd * wattsPerM2PerUmol };
}

void updateSensorReading(bool force = false) {
  const uint32_t now = millis();
  if (force || !hasSensorReading || now - lastSensorSample >= sampleIntervalMs) {
    latestReading = readSensor();
    lastSensorSample = now;
    hasSensorReading = true;
  }
}

String statusJson(bool forceFresh = false) {
  updateSensorReading(forceFresh);
  DynamicJsonDocument doc(1024);
  doc["device"] = hostName;
  doc["type"] = "li-cor-status";
  doc["VoltageMv"] = latestReading.voltageMv;
  doc["CurrentUa"] = latestReading.currentUa;
  doc["Ppfd"] = latestReading.ppfd;
  doc["RawPpfd"] = latestReading.rawPpfd;
  doc["DarkOffsetMv"] = darkOffsetMv;
  doc["ShuntResistanceOhms"] = shuntResistanceOhms;
  doc["VoltageMultiplierUmolPerMv"] = voltageMultiplierUmolPerMv;
  doc["UpdatedAt"] = millis();
  String output;
  serializeJson(doc, output);
  return output;
}

String mqttStatusJson() {
  updateSensorReading();
  DynamicJsonDocument doc(256);
  doc["device"] = hostName;
  doc["Ppfd"] = latestReading.ppfd;
  String output;
  serializeJson(doc, output);
  return output;
}

void publishStatus() {
  if (!mqttClient.connected() || topicStatus.isEmpty()) return;
  const String payload = mqttStatusJson();
  mqttClient.publish(topicStatus.c_str(), payload.c_str(), mqttRetainStatus);
}

bool updateCalibration(float newDarkOffsetMv, float newVoltageFactor, float referencePpfd, const char *id, const char *date) {
  DynamicJsonDocument doc(CONFIG_DOC_CAPACITY);
  if (!readConfigDocument(doc) || !validateConfigDocument(doc)) return false;

  JsonArray adjustments = doc["FieldAdjustments"].as<JsonArray>();
  while (adjustments.size() >= MAX_FIELD_ADJUSTMENTS) adjustments.remove(0);
  JsonObject entry = adjustments.add<JsonObject>();
  const String adjustmentId = (id && id[0] != '\0') ? String(id) : String(millis());
  entry["Id"] = adjustmentId;
  entry["Date"] = date ? date : "";
  entry["ReferencePpfd"] = referencePpfd;
  entry["VoltageMultiplierUmolPerMv"] = newVoltageFactor;
  entry["DarkOffsetMv"] = newDarkOffsetMv;
  entry["Active"] = true;

  JsonObject sensor = doc["Sensor"].as<JsonObject>();
  sensor["DarkOffsetMv"] = newDarkOffsetMv;
  sensor["VoltageMultiplierUmolPerMv"] = newVoltageFactor;
  sensor["ActiveCalibration"] = "field";
  for (JsonObject adjustment : adjustments) {
    const String adjustmentIdValue = adjustment["Id"].as<String>();
    adjustment["Active"] = adjustmentIdValue == adjustmentId;
  }
  JsonObject field = doc["LastFieldAdjustment"].as<JsonObject>();
  updateLegacyFieldAdjustment(field, entry, sensor);
  if (!writeConfig(doc)) return false;
  loadConfig();
  return true;
}

bool restoreCertificateCalibration() {
  DynamicJsonDocument doc(CONFIG_DOC_CAPACITY);
  if (!readConfigDocument(doc) || !validateConfigDocument(doc)) return false;

  applyCertificateCalibrationToDocument(doc);
  if (!writeConfig(doc)) return false;
  loadConfig();
  return true;
}

bool useFieldAdjustment(const char *id) {
  if (!id || id[0] == '\0') return false;
  DynamicJsonDocument doc(CONFIG_DOC_CAPACITY);
  if (!readConfigDocument(doc) || !validateConfigDocument(doc)) return false;

  JsonArray adjustments = doc["FieldAdjustments"].as<JsonArray>();
  for (JsonObject entry : adjustments) {
    const String entryId = entry["Id"].as<String>();
    if (entryId == id) {
      applyFieldAdjustmentToDocument(doc, entry);
      if (!writeConfig(doc)) return false;
      loadConfig();
      return true;
    }
  }
  return false;
}

bool deleteFieldAdjustment(const char *id) {
  if (!id || id[0] == '\0') return false;
  DynamicJsonDocument doc(CONFIG_DOC_CAPACITY);
  if (!readConfigDocument(doc) || !validateConfigDocument(doc)) return false;

  JsonArray adjustments = doc["FieldAdjustments"].as<JsonArray>();
  int index = -1;
  bool wasActive = false;
  for (size_t i = 0; i < adjustments.size(); ++i) {
    JsonObject entry = adjustments[i].as<JsonObject>();
    const String entryId = entry["Id"] | "";
    if (entryId == id) {
      index = static_cast<int>(i);
      wasActive = entry["Active"] | false;
      break;
    }
  }
  if (index < 0) return false;
  adjustments.remove(index);

  if (adjustments.isNull() || adjustments.size() == 0) {
    applyCertificateCalibrationToDocument(doc);
  } else if (wasActive) {
    JsonObject replacement = adjustments[adjustments.size() - 1].as<JsonObject>();
    applyFieldAdjustmentToDocument(doc, replacement);
  }

  if (!writeConfig(doc)) return false;
  loadConfig();
  return true;
}

bool deleteAllFieldAdjustments() {
  DynamicJsonDocument doc(CONFIG_DOC_CAPACITY);
  if (!readConfigDocument(doc) || !validateConfigDocument(doc)) return false;

  JsonArray adjustments = doc["FieldAdjustments"].as<JsonArray>();
  adjustments.clear();
  applyCertificateCalibrationToDocument(doc);
  if (!writeConfig(doc)) return false;
  loadConfig();
  return true;
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  const String apSsid = (hostName.isEmpty() ? String("LI-COR") : hostName) + "-AP-Mode";
  if (WiFi.softAP(apSsid.c_str())) {
    Serial.print("Wi-Fi connection failed. Access point started: ");
    Serial.println(apSsid);
    Serial.print("Open http://");
    Serial.print(WiFi.softAPIP());
    Serial.println("/ to configure the device.");
  } else {
    Serial.println("ERROR: access point could not be started.");
  }
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.setHostname(hostName.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setHostname(hostName.c_str());
    Serial.print("Wi-Fi hostname: ");
    Serial.println(WiFi.getHostname());
    Serial.print("Wi-Fi connected: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection failed.");
  }
}

bool reconnectMqtt() {
  Serial.print("Attempting MQTT connection...");
  mqttClient.setServer(mqttServer.c_str(), 1883);
  const String clientId = hostName + "-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  bool connected = false;
  if (mqttUser.isEmpty()) {
    connected = mqttClient.connect(clientId.c_str());
  } else {
    connected = mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPassword.c_str());
  }

  if (connected) {
    Serial.println(" connected");
    return true;
  }

  Serial.print(" failed, rc=");
  Serial.println(mqttClient.state());
  return false;
}

void maintainMqttConnection() {
  if (WiFi.status() != WL_CONNECTED || mqttServer.isEmpty()) return;
  if (mqttClient.connected()) return;
  const uint32_t now = millis();
  if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttAttempt = now;
  if (reconnectMqtt()) {
    lastMqttAttempt = 0;
  }
}

void sendConfigResponse(AsyncWebServerRequest *request) {
  if (!LittleFS.exists("/config.json")) {
    request->send(500, "application/json", "{\"error\":\"config unavailable\"}");
    return;
  }
  request->send(LittleFS, "/config.json", "application/json");
}

void setupWebServer() {
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    const bool forceFresh = request->hasParam("fresh");
    request->send(200, "application/json", statusJson(forceFresh));
  });

  webServer.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendConfigResponse(request);
  });

  webServer.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > CONFIG_DOC_CAPACITY) {
        request->send(413, "application/json", "{\"error\":\"config too large\"}");
        return;
      }

      if (index == 0 && request->_tempObject == nullptr) {
        request->_tempObject = calloc(total + 1, sizeof(uint8_t));
        if (request->_tempObject == nullptr) {
          request->send(500, "application/json", "{\"error\":\"config buffer allocation failed\"}");
          return;
        }
      }
      if (request->_tempObject == nullptr) return;

      memcpy(static_cast<uint8_t *>(request->_tempObject) + index, data, len);
      if (index + len < total) return;

      DynamicJsonDocument doc(CONFIG_DOC_CAPACITY);
      if (deserializeJson(doc, static_cast<uint8_t *>(request->_tempObject), total)) {
        request->send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
      }
      if (!validateConfigDocument(doc) || !writeConfig(doc) || !loadConfig()) {
        request->send(500, "application/json", "{\"error\":\"config save failed\"}");
        return;
      }
      request->send(200, "application/json", "{\"saved\":true}");
    });

  webServer.on("/api/calibration/restore", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!restoreCertificateCalibration()) {
      request->send(500, "application/json", "{\"error\":\"certificate calibration restore failed\"}");
      return;
    }
    request->send(200, "application/json", "{\"restored\":\"certificate\"}");
  });

  webServer.on("/api/calibration/use", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index != 0 || total != len) {
        request->send(413, "application/json", "{\"error\":\"calibration payload too large\"}");
        return;
      }
      DynamicJsonDocument doc(512);
      if (deserializeJson(doc, data, len)) {
        request->send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
      }
      const String id = doc["id"].as<String>();
      if (!useFieldAdjustment(id.c_str())) {
        request->send(404, "application/json", "{\"error\":\"field adjustment not found\"}");
        return;
      }
      request->send(200, "application/json", "{\"used\":true}");
    });

  webServer.on("/api/calibration/delete", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index != 0 || total != len) {
        request->send(413, "application/json", "{\"error\":\"calibration payload too large\"}");
        return;
      }
      DynamicJsonDocument doc(512);
      if (deserializeJson(doc, data, len)) {
        request->send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
      }
      const String id = doc["id"].as<String>();
      if (!deleteFieldAdjustment(id.c_str())) {
        request->send(404, "application/json", "{\"error\":\"field adjustment not found\"}");
        return;
      }
      request->send(200, "application/json", "{\"deleted\":true}");
    });

  webServer.on("/api/calibration/delete-all", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!deleteAllFieldAdjustments()) {
      request->send(500, "application/json", "{\"error\":\"field adjustments could not be deleted\"}");
      return;
    }
    request->send(200, "application/json", "{\"deleted\":true,\"restored\":\"certificate\"}");
  });

  // Register the exact calibration subroutes before the generic route above them.
  // ESPAsyncWebServer also matches a route against child paths, so the generic
  // "/api/calibration" handler must be registered last.
  webServer.on("/api/calibration", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > 1024) {
        request->send(413, "application/json", "{\"error\":\"calibration payload too large\"}");
        return;
      }
      if (index == 0 && request->_tempObject == nullptr) {
        request->_tempObject = calloc(total + 1, sizeof(uint8_t));
        if (request->_tempObject == nullptr) {
          request->send(500, "application/json", "{\"error\":\"calibration buffer allocation failed\"}");
          return;
        }
      }
      if (request->_tempObject == nullptr) return;

      memcpy(static_cast<uint8_t *>(request->_tempObject) + index, data, len);
      if (index + len < total) return;

      DynamicJsonDocument doc(1024);
      if (deserializeJson(doc, static_cast<uint8_t *>(request->_tempObject), total)) {
        request->send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
      }
      if (!hasFiniteNumber(doc["darkOffsetMv"]) || !hasFiniteNumber(doc["voltageMultiplierUmolPerMv"]) ||
          !hasFiniteNumber(doc["referencePpfd"]) || !hasText(doc["id"]) || !hasText(doc["date"])) {
        request->send(400, "application/json", "{\"error\":\"incomplete calibration payload\"}");
        return;
      }
      const float offset = doc["darkOffsetMv"].as<float>();
      const float factor = doc["voltageMultiplierUmolPerMv"].as<float>();
      const float referencePpfd = doc["referencePpfd"].as<float>();
      const String id = doc["id"].as<String>();
      const String date = doc["date"].as<String>();
      if (offset < 0.0f || !(factor > 0.0f) || !(referencePpfd > 0.0f) || !updateCalibration(offset, factor, referencePpfd, id.c_str(), date.c_str())) {
        request->send(400, "application/json", "{\"error\":\"calibration save failed\"}");
        return;
      }
      request->send(200, "application/json", "{\"saved\":true}");
    });

  webServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"rebooting\":true}");
    delay(100);
    ESP.restart();
  });

  webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  webServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(250);

  if (!LittleFS.begin(false)) {
    Serial.println("ERROR: LittleFS mount failed.");
    startAccessPoint();
    setupWebServer();
    return;
  }
  if (!loadConfig()) {
    Serial.println("ERROR: config.json could not be loaded.");
    startAccessPoint();
  }

  Wire.begin(21, 22);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  delay(500);
  Serial.println("I2C initialized at 100 kHz.");

  adsAddress = findAdsAddress();
  if (adsAddress == 0) {
    Serial.println("ERROR: ADS1115 not found at 0x48-0x4B.");
  } else if (!ads.begin(adsAddress, &Wire)) {
    Serial.print("ERROR: ADS1115 initialization failed at 0x");
    Serial.println(adsAddress, HEX);
  } else {
    adsAvailable = true;
    ads.setGain(GAIN_SIXTEEN);
    ads.setDataRate(RATE_ADS1115_128SPS);
    Serial.print("ADS1115 found at 0x");
    Serial.println(adsAddress, HEX);
  }

  if (configValid) {
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi connection failed, starting AP mode");
      startAccessPoint();
    }
  }
  mqttClient.setBufferSize(2048);
  setupWebServer();
  Serial.println("LI-COR web interface started.");
}

void loop() {
  if (!configValid) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected. Reconnecting...");
    connectToWiFi();
  } else {
    maintainMqttConnection();
    if (mqttClient.connected()) mqttClient.loop();
  }

  updateSensorReading();

  if (millis() - lastMqttPublish >= mqttPublishIntervalMs) {
    lastMqttPublish = millis();
    publishStatus();
  }
}
