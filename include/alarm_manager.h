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
   void startAlarm(uint8_t slot);
   void updateLedColor() const;
  void stopAlarm();

  bool alarmActive_ = false;
  unsigned long alarmStartedAt_ = 0;
   uint8_t activeSlotMask_ = 0;
};
