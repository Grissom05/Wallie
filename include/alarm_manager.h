#pragma once

#include "wallie_types.h"

#include <vector>

class AlarmManager {
 public:
  void begin();
  void checkSchedules(std::vector<Medicine>& medicines);
  void update();
  void simulateDose(const Medicine& medicine, const char* reason);

 private:
  void startAlarm();
  void stopAlarm();

  bool alarmActive_ = false;
  bool ledOn_ = false;
  unsigned long alarmStartedAt_ = 0;
  unsigned long lastBlinkAt_ = 0;
};
