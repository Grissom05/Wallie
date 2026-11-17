#include "storage.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>

namespace {
constexpr char CONFIG_PATH[] = "/config.json";

bool isValidTime(const String& time) {
  if (time.length() != 5 || time[2] != ':') {
    return false;
  }
  for (size_t index = 0; index < time.length(); ++index) {
    if (index != 2 && !isDigit(time[index])) {
      return false;
    }
  }
  const int hour = time.substring(0, 2).toInt();
  const int minute = time.substring(3, 5).toInt();
  return hour <= 23 && minute <= 59;
}

bool readMedicine(JsonObjectConst object, Medicine& medicine) {
  if (!object["id"].is<uint32_t>() || !object["name"].is<const char*>() ||
      !object["slot"].is<uint8_t>() || !object["stock"].is<uint16_t>() ||
      !object["capacity"].is<uint16_t>() ||
      !object["low"].is<uint8_t>() || !object["dose"].is<uint8_t>() ||
      !object["water"].is<bool>() || !object["times"].is<JsonArrayConst>() ||
      !object["days"].is<JsonArrayConst>()) {
    return false;
  }

  medicine.id = object["id"].as<uint32_t>();
  medicine.name = object["name"].as<String>();
  medicine.slot = object["slot"].as<uint8_t>();
  medicine.stock = object["stock"].as<uint16_t>();
  medicine.capacity = object["capacity"].as<uint16_t>();
  medicine.lowStockThreshold = object["low"].as<uint8_t>();
  medicine.dose = object["dose"].as<uint8_t>();
  medicine.requiresWater = object["water"].as<bool>();

  for (JsonVariantConst value : object["times"].as<JsonArrayConst>()) {
    if (!value.is<const char*>()) {
      return false;
    }
    medicine.times.push_back(value.as<String>());
  }

  for (JsonVariantConst value : object["days"].as<JsonArrayConst>()) {
    if (!value.is<uint8_t>()) {
      return false;
    }
    medicine.days.push_back(value.as<uint8_t>());
  }

  if (medicine.id == 0 || medicine.name.length() < 2 ||
      medicine.name.length() > 30 || medicine.slot < 1 || medicine.slot > 3 ||
      medicine.stock > 200 || medicine.capacity < medicine.stock ||
      medicine.capacity > 200 || medicine.lowStockThreshold > 50 ||
      medicine.dose < 1 || medicine.dose > 4 || medicine.times.empty() ||
      medicine.times.size() > 4 || medicine.days.size() != 7) {
    return false;
  }

  for (size_t index = 0; index < medicine.times.size(); ++index) {
    if (!isValidTime(medicine.times[index]) ||
        std::find(medicine.times.begin(), medicine.times.begin() + index,
                  medicine.times[index]) != medicine.times.begin() + index) {
      return false;
    }
  }

  bool hasScheduledDay = false;
  for (uint8_t day : medicine.days) {
    if (day > 1) {
      return false;
    }
    hasScheduledDay = hasScheduledDay || day == 1;
  }
  return hasScheduledDay;
}
}  // namespace

bool loadConfig(UserProfile& profile, std::vector<Medicine>& medicines) {
  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("[FS] No configuration file found; using defaults.");
    return true;
  }

  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.printf("[FS] Failed to open %s for reading.\n", CONFIG_PATH);
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error) {
    Serial.printf("[FS] Failed to parse %s: %s\n", CONFIG_PATH, error.c_str());
    return false;
  }

  if (!document["prof"].is<JsonObject>() || !document["meds"].is<JsonArray>()) {
    Serial.printf("[FS] Invalid configuration structure in %s.\n", CONFIG_PATH);
    return false;
  }

  UserProfile loadedProfile;
  const JsonObjectConst profileObject = document["prof"].as<JsonObjectConst>();
  loadedProfile.name = profileObject["name"] | "";
  loadedProfile.notify = profileObject["notify"] | "";
  loadedProfile.pin = profileObject["pin"] | "1234";
  loadedProfile.registered = profileObject["reg"] | false;

  std::vector<Medicine> loadedMedicines;
  for (JsonObjectConst object : document["meds"].as<JsonArrayConst>()) {
    Medicine medicine;
    if (!readMedicine(object, medicine)) {
      Serial.printf("[FS] Invalid medicine entry in %s; configuration was not loaded.\n",
                    CONFIG_PATH);
      return false;
    }
    const auto duplicate = std::find_if(
        loadedMedicines.begin(), loadedMedicines.end(),
        [&medicine](const Medicine& loaded) {
          return loaded.id == medicine.id || loaded.slot == medicine.slot;
        });
    if (duplicate != loadedMedicines.end()) {
      Serial.printf("[FS] Duplicate medicine ID or slot in %s; configuration was not loaded.\n",
                    CONFIG_PATH);
      return false;
    }
    loadedMedicines.push_back(medicine);
  }

  profile = loadedProfile;
  medicines = loadedMedicines;
  Serial.printf("[FS] Loaded user '%s' and %u medicine(s).\n",
                profile.name.c_str(), static_cast<unsigned>(medicines.size()));
  return true;
}

bool saveConfig(const UserProfile& profile, const std::vector<Medicine>& medicines) {
  JsonDocument document;

  document["prof"]["name"] = profile.name;
  document["prof"]["notify"] = profile.notify;
  document["prof"]["pin"] = profile.pin;
  document["prof"]["reg"] = profile.registered;

  JsonArray medicineArray = document["meds"].to<JsonArray>();
  for (const Medicine& medicine : medicines) {
    JsonObject object = medicineArray.add<JsonObject>();
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

  File file = LittleFS.open(CONFIG_PATH, "w");
  if (!file) {
    Serial.printf("[FS] Failed to open %s for writing.\n", CONFIG_PATH);
    return false;
  }

  const size_t bytesWritten = serializeJson(document, file);
  file.close();
  if (bytesWritten == 0) {
    Serial.printf("[FS] Failed to write %s.\n", CONFIG_PATH);
    return false;
  }

  Serial.printf("[FS] Saved configuration to %s.\n", CONFIG_PATH);
  return true;
}
