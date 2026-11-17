#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "alarm_manager.h"
#include "storage.h"
#include "wallie_types.h"
#include "wifi_config.h"

#include <algorithm>
#include <vector>

namespace {
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr char API_CONTENT_TYPE[] = "application/json";
constexpr char SESSION_TOKEN[] = "esp32-secure-token";

WebServer server(80);
AlarmManager alarmManager;
UserProfile userProfile;
DispenserState dispenserState;
std::vector<Medicine> medicines;
unsigned long lastWiFiRetryAt = 0;

void sendError(int statusCode, const char* message) {
  JsonDocument document;
  document["error"] = message;
  String response;
  serializeJson(document, response);
  server.send(statusCode, API_CONTENT_TYPE, response);
}

bool parseRequestBody(JsonDocument& document) {
  const DeserializationError error = deserializeJson(document, server.arg("plain"));
  if (!error) {
    return true;
  }

  Serial.printf("[API] Invalid JSON request body: %s.\n", error.c_str());
  sendError(400, "Invalid JSON");
  return false;
}

bool saveCurrentConfig() {
  if (saveConfig(userProfile, medicines)) {
    return true;
  }
  sendError(500, "Could not save configuration");
  return false;
}

void sendJsonDocument(JsonDocument& document, int statusCode = 200) {
  String response;
  serializeJson(document, response);
  server.send(statusCode, API_CONTENT_TYPE, response);
}

bool isValidPin(const String& pin) {
  if (pin.length() != 4) {
    return false;
  }
  for (size_t index = 0; index < pin.length(); ++index) {
    if (!isDigit(pin[index])) {
      return false;
    }
  }
  return true;
}

bool isValidTime(const String& value) {
  if (value.length() != 5 || value[2] != ':') {
    return false;
  }
  for (size_t index = 0; index < value.length(); ++index) {
    if (index != 2 && !isDigit(value[index])) {
      return false;
    }
  }
  const int hour = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

bool readMedicine(const JsonDocument& document, Medicine& medicine) {
  if (!document["name"].is<const char*>() ||
      !document["slot"].is<int>() ||
      !document["stock"].is<int>() ||
      !document["capacity"].is<int>() ||
      !document["low"].is<int>() ||
      !document["dose"].is<int>() ||
      !document["water"].is<bool>() ||
      !document["times"].is<JsonArrayConst>() ||
      !document["days"].is<JsonArrayConst>()) {
    sendError(400, "Missing or invalid medicine fields");
    return false;
  }

  const String name = document["name"].as<String>();
  const int slot = document["slot"].as<int>();
  const int stock = document["stock"].as<int>();
  const int capacity = document["capacity"].as<int>();
  const int lowStockThreshold = document["low"].as<int>();
  const int dose = document["dose"].as<int>();
  const JsonArrayConst times = document["times"].as<JsonArrayConst>();
  const JsonArrayConst days = document["days"].as<JsonArrayConst>();

  if (name.length() < 2 || name.length() > 30 ||
      slot < 1 || slot > dispenserState.slots ||
      stock < 0 || stock > 200 || capacity < stock || capacity > 200 ||
      lowStockThreshold < 0 || lowStockThreshold > 50 || dose < 1 || dose > 4 ||
      times.size() < 1 || times.size() > 4 || days.size() != 7) {
    sendError(400, "Medicine fields are outside the allowed range");
    return false;
  }

  Medicine parsed;
  parsed.name = name;
  parsed.slot = static_cast<uint8_t>(slot);
  parsed.stock = static_cast<uint16_t>(stock);
  parsed.capacity = static_cast<uint16_t>(capacity);
  parsed.lowStockThreshold = static_cast<uint8_t>(lowStockThreshold);
  parsed.dose = static_cast<uint8_t>(dose);
  parsed.requiresWater = document["water"].as<bool>();

  for (JsonVariantConst value : times) {
    if (!value.is<const char*>()) {
      sendError(400, "Invalid medicine schedule");
      return false;
    }
    const String time = value.as<String>();
    if (!isValidTime(time) ||
        std::find(parsed.times.begin(), parsed.times.end(), time) != parsed.times.end()) {
      sendError(400, "Invalid or duplicate medicine schedule");
      return false;
    }
    parsed.times.push_back(time);
  }

  bool hasScheduledDay = false;
  for (JsonVariantConst value : days) {
    if (!value.is<int>() || (value.as<int>() != 0 && value.as<int>() != 1)) {
      sendError(400, "Invalid medicine schedule");
      return false;
    }
    const uint8_t enabled = static_cast<uint8_t>(value.as<int>());
    parsed.days.push_back(enabled);
    hasScheduledDay = hasScheduledDay || enabled == 1;
  }
  if (!hasScheduledDay) {
    sendError(400, "Select at least one scheduled day");
    return false;
  }

  medicine = parsed;
  return true;
}

bool isSlotAvailable(uint8_t slot, uint32_t excludedMedicineId = 0) {
  return std::none_of(medicines.begin(), medicines.end(),
                      [slot, excludedMedicineId](const Medicine& medicine) {
                        return medicine.slot == slot && medicine.id != excludedMedicineId;
                      });
}

void appendMedicine(JsonObject object, const Medicine& medicine) {
  object["id"] = medicine.id;
  object["name"] = medicine.name;
  object["slot"] = medicine.slot;
  object["stock"] = medicine.stock;
  object["capacity"] = medicine.capacity;
  object["low"] = medicine.lowStockThreshold;
  object["dose"] = medicine.dose;
  object["water"] = medicine.requiresWater;

  JsonArray times = object["times"].to<JsonArray>();
  for (const String& time : medicine.times) {
    times.add(time);
  }
  JsonArray days = object["days"].to<JsonArray>();
  for (uint8_t day : medicine.days) {
    days.add(day);
  }
}

void handleGetProfile() {
  Serial.println("[API] GET /api/profile");
  if (!userProfile.registered) {
    sendError(404, "Not registered");
    return;
  }

  JsonDocument document;
  document["name"] = userProfile.name;
  document["notify"] = userProfile.notify;
  sendJsonDocument(document);
}

void handlePutProfile() {
  Serial.println("[API] PUT /api/profile");
  JsonDocument document;
  if (!parseRequestBody(document)) {
    return;
  }
  if (!document["name"].is<const char*>() || !document["notify"].is<const char*>()) {
    sendError(400, "Invalid profile fields");
    return;
  }

  const String name = document["name"].as<String>();
  if (name.length() < 2 || name.length() > 24) {
    sendError(400, "Name must contain 2 to 24 characters");
    return;
  }

  userProfile.name = name;
  userProfile.notify = document["notify"].as<String>();
  Serial.printf("[API] Profile updated for '%s'.\n", userProfile.name.c_str());
  if (saveCurrentConfig()) {
    server.send(200, API_CONTENT_TYPE, "{\"status\":\"ok\"}");
  }
}

void handleLogin() {
  Serial.println("[API] POST /api/login");
  JsonDocument document;
  if (!parseRequestBody(document)) {
    return;
  }
  if (!document["pin"].is<const char*>()) {
    sendError(400, "PIN is required");
    return;
  }

  if (userProfile.registered && document["pin"].as<String>() == userProfile.pin) {
    Serial.println("[API] Login accepted.");
    JsonDocument response;
    response["token"] = SESSION_TOKEN;
    sendJsonDocument(response);
  } else {
    Serial.println("[API] Login rejected.");
    sendError(403, "Wrong PIN");
  }
}

void handleRegister() {
  Serial.println("[API] POST /api/register");
  if (userProfile.registered) {
    sendError(409, "Already registered");
    return;
  }

  JsonDocument document;
  if (!parseRequestBody(document)) {
    return;
  }
  if (!document["name"].is<const char*>() || !document["pin"].is<const char*>()) {
    sendError(400, "Name and PIN are required");
    return;
  }

  const String name = document["name"].as<String>();
  const String pin = document["pin"].as<String>();
  if (name.length() < 2 || name.length() > 24 || !isValidPin(pin)) {
    sendError(400, "Invalid name or PIN");
    return;
  }

  userProfile.name = name;
  userProfile.pin = pin;
  userProfile.registered = true;
  Serial.printf("[API] Registered user '%s'.\n", userProfile.name.c_str());
  if (saveCurrentConfig()) {
    JsonDocument response;
    response["token"] = SESSION_TOKEN;
    sendJsonDocument(response);
  }
}

void handlePutPin() {
  Serial.println("[API] PUT /api/pin");
  JsonDocument document;
  if (!parseRequestBody(document)) {
    return;
  }
  if (!document["oldPin"].is<const char*>() || !document["newPin"].is<const char*>()) {
    sendError(400, "Current and new PIN are required");
    return;
  }

  if (document["oldPin"].as<String>() != userProfile.pin) {
    sendError(403, "Wrong PIN");
    return;
  }

  const String newPin = document["newPin"].as<String>();
  if (!isValidPin(newPin)) {
    sendError(400, "PIN must contain four digits");
    return;
  }

  userProfile.pin = newPin;
  Serial.println("[API] PIN updated.");
  if (saveCurrentConfig()) {
    server.send(200, API_CONTENT_TYPE, "{\"status\":\"ok\"}");
  }
}

void handleGetState() {
  Serial.println("[API] GET /api/state");
  JsonDocument document;
  document["slots"] = dispenserState.slots;
  document["water"] = dispenserState.water;
  sendJsonDocument(document);
}

void handleGetMedicines() {
  Serial.println("[API] GET /api/medicines");
  JsonDocument document;
  JsonArray array = document.to<JsonArray>();
  for (const Medicine& medicine : medicines) {
    appendMedicine(array.add<JsonObject>(), medicine);
  }
  sendJsonDocument(document);
}

void handleAddMedicine() {
  Serial.println("[API] POST /api/medicines");
  JsonDocument document;
  if (!parseRequestBody(document)) {
    return;
  }

  Medicine medicine;
  if (!readMedicine(document, medicine)) {
    return;
  }
  if (!isSlotAvailable(medicine.slot)) {
    sendError(409, "Slot is already occupied");
    return;
  }

  uint32_t nextId = 1;
  for (const Medicine& existing : medicines) {
    nextId = std::max(nextId, existing.id + 1);
  }
  medicine.id = nextId;
  medicines.push_back(medicine);
  Serial.printf("[API] Added '%s' in slot %u (ID: %lu).\n",
                medicine.name.c_str(), medicine.slot,
                static_cast<unsigned long>(medicine.id));

  if (!saveCurrentConfig()) {
    medicines.pop_back();
    return;
  }

  JsonDocument response;
  appendMedicine(response.to<JsonObject>(), medicines.back());
  sendJsonDocument(response);
}

void handleDispenseTest() {
  Serial.println("[API] POST /api/dispense");
  JsonDocument document;
  if (!parseRequestBody(document)) {
    return;
  }
  if (!document["medicineId"].is<uint32_t>()) {
    sendError(400, "Medicine ID is required");
    return;
  }

  const uint32_t medicineId = document["medicineId"].as<uint32_t>();
  const auto medicine = std::find_if(
      medicines.begin(), medicines.end(),
      [medicineId](const Medicine& item) { return item.id == medicineId; });
  if (medicine == medicines.end()) {
    sendError(404, "Medicine not found");
    return;
  }

  alarmManager.simulateDose(*medicine, "manual test");
  server.send(200, API_CONTENT_TYPE, "{\"status\":\"simulated\"}");
}

bool parsePathId(const String& path, uint32_t& id) {
  const size_t prefixLength = String("/api/medicines/").length();
  if (path.length() <= prefixLength) {
    return false;
  }

  const String idText = path.substring(prefixLength);
  for (size_t index = 0; index < idText.length(); ++index) {
    if (!isDigit(idText[index])) {
      return false;
    }
  }
  id = static_cast<uint32_t>(strtoul(idText.c_str(), nullptr, 10));
  return id != 0;
}

void handleDynamicRoute() {
  const String uri = server.uri();
  if (server.method() == HTTP_POST && uri.startsWith("/api/slots/") &&
      uri.endsWith("/load")) {
    const String slotText = uri.substring(11, uri.length() - 5);
    const int slot = slotText.toInt();
    if (slotText.length() == 0 || slot < 1 || slot > dispenserState.slots) {
      sendError(400, "Invalid slot");
      return;
    }
    Serial.printf("[API] Slot %d moved to the loading position.\n", slot);
    server.send(204, "text/plain", "");
    return;
  }

  if (!uri.startsWith("/api/medicines/")) {
    sendError(404, "Not found");
    return;
  }

  uint32_t targetId = 0;
  if (!parsePathId(uri, targetId)) {
    sendError(400, "Invalid medicine ID");
    return;
  }
  const auto medicine = std::find_if(
      medicines.begin(), medicines.end(),
      [targetId](const Medicine& item) { return item.id == targetId; });
  if (medicine == medicines.end()) {
    sendError(404, "Medicine not found");
    return;
  }

  if (server.method() == HTTP_PUT) {
    JsonDocument document;
    if (!parseRequestBody(document)) {
      return;
    }

    Medicine updated;
    if (!readMedicine(document, updated)) {
      return;
    }
    if (!isSlotAvailable(updated.slot, targetId)) {
      sendError(409, "Slot is already occupied");
      return;
    }

    updated.id = targetId;
    updated.lastTriggeredOccurrence = medicine->lastTriggeredOccurrence;
    const Medicine previous = *medicine;
    *medicine = updated;
    if (!saveCurrentConfig()) {
      *medicine = previous;
      return;
    }

    JsonDocument response;
    appendMedicine(response.to<JsonObject>(), *medicine);
    sendJsonDocument(response);
    Serial.printf("[API] Updated medicine ID %lu.\n",
                  static_cast<unsigned long>(targetId));
    return;
  }

  if (server.method() == HTTP_DELETE) {
    const size_t index = static_cast<size_t>(medicine - medicines.begin());
    const Medicine removed = *medicine;
    medicines.erase(medicine);
    if (!saveCurrentConfig()) {
      medicines.insert(medicines.begin() + index, removed);
      return;
    }
    Serial.printf("[API] Deleted '%s' (ID: %lu).\n",
                  removed.name.c_str(), static_cast<unsigned long>(targetId));
    server.send(204, "text/plain", "");
    return;
  }

  sendError(405, "Method not allowed");
}

void handleNotFound() {
  Serial.printf("[API] Route not found: %s.\n", server.uri().c_str());
  handleDynamicRoute();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(WALLIE_WIFI_SSID, WALLIE_WIFI_PASSWORD);
  Serial.println("[WIFI] Connecting to the configured Wi-Fi network.");

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Dispenser address: http://%s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI] Connection timed out; retrying in the background.");
    lastWiFiRetryAt = millis();
  }
}

void retryWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED ||
      millis() - lastWiFiRetryAt < WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  lastWiFiRetryAt = millis();
  Serial.println("[WIFI] Retrying the Wi-Fi connection.");
  WiFi.reconnect();
}

void registerRoutes() {
  server.serveStatic("/", LittleFS, "/index.html");
  server.serveStatic("/index.html", LittleFS, "/index.html");
  server.serveStatic("/register.html", LittleFS, "/register.html");
  server.serveStatic("/dashboard.html", LittleFS, "/dashboard.html");
  server.serveStatic("/favicon.png", LittleFS, "/favicon.png");
  server.serveStatic("/logo.png", LittleFS, "/logo.png");

  server.on("/api/profile", HTTP_GET, handleGetProfile);
  server.on("/api/profile", HTTP_PUT, handlePutProfile);
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/register", HTTP_POST, handleRegister);
  server.on("/api/state", HTTP_GET, handleGetState);
  server.on("/api/medicines", HTTP_GET, handleGetMedicines);
  server.on("/api/medicines", HTTP_POST, handleAddMedicine);
  server.on("/api/pin", HTTP_PUT, handlePutPin);
  server.on("/api/dispense", HTTP_POST, handleDispenseTest);
  server.onNotFound(handleNotFound);
}

void factoryResetIfRequested() {
  if (digitalRead(0) != LOW) {
    return;
  }

  Serial.println("[SYSTEM] Factory reset requested; removing saved configuration.");
  if (!LittleFS.remove("/config.json")) {
    Serial.println("[SYSTEM] No saved configuration was removed.");
  }
  for (uint8_t blink = 0; blink < 5; ++blink) {
    rgbLedWrite(RGB_BUILTIN, 255, 0, 0);
    delay(200);
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    delay(200);
  }
  Serial.println("[SYSTEM] Factory reset completed. Restart the board.");
  while (true) {
    delay(100);
  }
}
}  // namespace

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n--- WALLIE DISPENSER STARTING ---");

  pinMode(0, INPUT_PULLUP);
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
  if (!LittleFS.begin(true)) {
    Serial.println("[SYSTEM] Failed to mount LittleFS; setup cannot continue.");
    return;
  }

  factoryResetIfRequested();
  if (!loadConfig(userProfile, medicines)) {
    Serial.println("[SYSTEM] Configuration could not be loaded; defaults remain active.");
  }

  connectToWiFi();
  alarmManager.begin();
  registerRoutes();
  server.begin();
  Serial.println("[SYSTEM] Web server started.");
}

void loop() {
  server.handleClient();
  retryWiFiIfNeeded();
  alarmManager.checkSchedules(medicines);
  alarmManager.update();
  delay(2);
}
