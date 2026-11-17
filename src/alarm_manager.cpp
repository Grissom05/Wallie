#include "alarm_manager.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <time.h>

namespace {
constexpr uint8_t BOOT_BUTTON_PIN = 0;
constexpr uint32_t ALARM_DURATION_MS = 10000;
constexpr uint32_t LED_BLINK_INTERVAL_MS = 250;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 200;
constexpr uint32_t NTP_SYNC_INTERVAL_MS = 3600000;
constexpr char NTP_SERVER_PRIMARY[] = "ntp1.inrim.it";
constexpr char NTP_SERVER_SECONDARY[] = "pool.ntp.org";
constexpr char ITALY_TIME_ZONE[] = "CET-1CEST,M3.5.0,M10.5.0/3";

volatile bool stopAlarmRequested = false;
volatile unsigned long lastButtonInterruptAt = 0;
volatile bool clockSynchronized = false;
char lastSyncTime[16] = "Waiting";

void IRAM_ATTR onBootButtonPressed() {
  const unsigned long now = millis();
  if (now - lastButtonInterruptAt > BUTTON_DEBOUNCE_MS) {
    stopAlarmRequested = true;
    lastButtonInterruptAt = now;
  }
}

void onTimeSynchronized(struct timeval*) {
  clockSynchronized = true;
  const time_t now = time(nullptr);
  const struct tm* localTime = localtime(&now);
  if (localTime == nullptr) {
    Serial.println("[NTP] Time synchronized, but local time conversion failed.");
    return;
  }

  snprintf(lastSyncTime, sizeof(lastSyncTime), "%02d:%02d:%02d",
           localTime->tm_hour, localTime->tm_min, localTime->tm_sec);
  Serial.printf("[NTP] Time synchronized: %s.\n", lastSyncTime);
}
}  // namespace

void AlarmManager::begin() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOOT_BUTTON_PIN), onBootButtonPressed, FALLING);
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);

  sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
  sntp_set_time_sync_notification_cb(onTimeSynchronized);
  configTzTime(ITALY_TIME_ZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
}

void AlarmManager::checkSchedules(std::vector<Medicine>& medicines) {
  if (!clockSynchronized) {
    return;
  }

  const time_t now = time(nullptr);
  const struct tm* localTime = localtime(&now);
  if (localTime == nullptr) {
    Serial.println("[SCHEDULER] Failed to read local time.");
    return;
  }

  char currentTime[6];
  char occurrence[24];
  snprintf(currentTime, sizeof(currentTime), "%02d:%02d",
           localTime->tm_hour, localTime->tm_min);
  snprintf(occurrence, sizeof(occurrence), "%04d-%03d %s",
           localTime->tm_year + 1900, localTime->tm_yday, currentTime);

  const uint8_t dayIndex = localTime->tm_wday == 0
                               ? 6
                               : static_cast<uint8_t>(localTime->tm_wday - 1);
  for (Medicine& medicine : medicines) {
    if (medicine.days.size() != 7 || medicine.days[dayIndex] != 1 ||
        medicine.lastTriggeredOccurrence == occurrence) {
      continue;
    }

    for (const String& scheduledTime : medicine.times) {
      if (scheduledTime == currentTime) {
        medicine.lastTriggeredOccurrence = occurrence;
        Serial.printf("[SCHEDULER] Scheduled dose is due for '%s' at %s.\n",
                      medicine.name.c_str(), currentTime);
        simulateDose(medicine, "scheduled alarm");
        break;
      }
    }
  }
}

void AlarmManager::update() {
  if (stopAlarmRequested) {
    stopAlarmRequested = false;
    if (alarmActive_) {
      Serial.println("[ALARM] BOOT button pressed; alarm acknowledged.");
      stopAlarm();
    }
  }

  if (!alarmActive_) {
    return;
  }

  const unsigned long now = millis();
  if (now - alarmStartedAt_ >= ALARM_DURATION_MS) {
    stopAlarm();
  } else if (now - lastBlinkAt_ >= LED_BLINK_INTERVAL_MS) {
    lastBlinkAt_ = now;
    ledOn_ = !ledOn_;
    rgbLedWrite(RGB_BUILTIN, ledOn_ ? 255 : 0, 0, ledOn_ ? 100 : 0);
  }
}

void AlarmManager::simulateDose(const Medicine& medicine, const char* reason) {
  Serial.printf(
      "[DISPENSE] Simulation only: dispense %u pill(s) of '%s' from slot %u, %s water "
      "(%s). No motor is connected; stock was not changed.\n",
      medicine.dose, medicine.name.c_str(), medicine.slot,
      medicine.requiresWater ? "with" : "without", reason);
  startAlarm();
}

void AlarmManager::startAlarm() {
  if (alarmActive_) {
    Serial.println("[ALARM] Alarm is already active.");
    return;
  }

  alarmActive_ = true;
  ledOn_ = true;
  alarmStartedAt_ = millis();
  lastBlinkAt_ = alarmStartedAt_;
  rgbLedWrite(RGB_BUILTIN, 255, 0, 100);
  Serial.println("[ALARM] Alarm started; the RGB LED will blink for 10 seconds.");
}

void AlarmManager::stopAlarm() {
  alarmActive_ = false;
  ledOn_ = false;
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
  Serial.println("[ALARM] Alarm stopped; the LED is off.");
}
