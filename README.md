# Wallie

Wallie is an ESP32-S3 medication reminder prototype. Scheduled doses currently
trigger the onboard RGB LED and a serial-log simulation; no pill or water is
physically dispensed.

The alarm LED stays on for 10 seconds and encodes the active medicine slots:
slot 1 is red, slot 2 is green, and slot 3 is blue. Simultaneous alarms combine
the colors (for example, slots 1 and 2 appear yellow, and all three appear white).

## Firmware layout

- `src/main.cpp` contains the HTTP API and application startup.
- `src/alarm_manager.cpp` handles NTP scheduling, dose simulation, and the LED alarm.
- `src/storage.cpp` loads and saves the profile and medicine schedule in LittleFS.
- `data/` contains the English web interface served by the ESP32.

## Wi-Fi configuration

Copy `include/wifi_config.example.h` to `include/wifi_config.h` and set your
Wi-Fi SSID and password in the local copy. The actual configuration file is
ignored by Git.

## Build and upload

Build and upload with PlatformIO for the `esp32-s3-devkitc-1` environment. Upload
the LittleFS filesystem image as well as the firmware so the web pages are
available on the device.
