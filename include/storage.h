#pragma once

#include "wallie_types.h"

bool loadConfig(UserProfile& profile, std::vector<Medicine>& medicines);
bool saveConfig(const UserProfile& profile, const std::vector<Medicine>& medicines);
