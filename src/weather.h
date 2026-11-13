#pragma once

#include <stddef.h>

// Portable weather snapshot shared by the network task and LVGL UI.
// Temperatures are Celsius, wind is km/h and precipitation is millimetres;
// the UI converts these according to the selected unit preset.
struct WeatherDay {
    char date[11];
    int code;
    float tempMinC;
    float tempMaxC;
    int rainChance;
};

struct WeatherSnapshot {
    bool valid;
    char updated[6];
    int code;
    float tempC;
    float feelsC;
    int humidity;
    float windKmh;
    int windDeg;
    WeatherDay days[4];
    int dayCount;
    char source[8];        // "DWD" or "" (Open-Meteo)
};

// Amtliche Warnungen des DWD für den Standort (dwd_client.cpp holt sie, die UI zeigt sie).
// level: 1 gelb (Minor), 2 orange (Moderate), 3 rot (Severe), 4 violett (Extreme).
struct WeatherWarning {
    char event[40];        // "WINDBÖEN", "FROST" ...
    char headline[80];     // "Amtliche WARNUNG vor WINDBÖEN"
    int level;
    bool preliminary;      // Vorabinformation (URGENCY Future)
    long onset, expires;   // UTC epoch seconds (0 = unknown)
};
struct WeatherWarnings {
    int count;             // 0 = keine Warnung
    WeatherWarning items[3];   // the most severe first
    bool valid;            // false until the first successful fetch
};
void weather_warn_store(const WeatherWarnings &w);
bool weather_warn_get(WeatherWarnings &w);
// colour of a warning level (0xRRGGBB)
unsigned weather_warn_color(int level);

void weather_store(const WeatherSnapshot &snapshot);
bool weather_get(WeatherSnapshot &snapshot);
const char *weather_condition(int code);
const char *weather_day_name(const char *isoDate);
