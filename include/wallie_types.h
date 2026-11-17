#pragma once

#include <Arduino.h>
#include <vector>

struct Medicine {
  uint32_t id = 0;
  String name;
  uint8_t slot = 0;
  uint16_t stock = 0;
  uint16_t capacity = 0;
  uint8_t lowStockThreshold = 0;
  uint8_t dose = 1;
  bool requiresWater = false;
  std::vector<String> times;
  std::vector<uint8_t> days;
  String lastTriggeredOccurrence;
};

struct UserProfile {
  String name;
  String notify;
  String pin = "1234";
  bool registered = false;
};

struct DispenserState {
  uint8_t slots = 3;
  String water = "ok";
};
