#pragma once
#include <Arduino.h>

void startWiFiTask();

// current timezone offset in minutes (e.g. JST = +540). Defined in wifi_server.cpp
extern int16_t tz_offset_minutes;
// auto on/off settings
extern bool auto_enabled;
extern uint16_t auto_on_minutes;
extern uint16_t auto_off_minutes;
