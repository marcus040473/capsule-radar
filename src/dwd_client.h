#pragma once
// Deutscher Wetterdienst (DWD) Open Data über WLAN (Kern 0, adsb_task). Quelle: "DWD".
//  - Vorhersage: MOSMIX_L der nächsten Station (opendata.dwd.de), aktuelle Messwerte der
//    nächsten Station mit stündlichen Beobachtungen (POI) -- weather_fetch() nimmt das zuerst
//    und fällt sonst auf Open-Meteo zurück.
//  - Amtliche Warnungen für den Standort: WFS-Abfrage auf maps.dwd.de (Gemeindeebene).
// Das Regenradar (WMS dwd:Niederschlagsradar) steckt in wx_radar_client.cpp.
#include "weather.h"

bool dwd_weather_fetch(double lat, double lon, WeatherSnapshot &out);
bool dwd_warnings_fetch(double lat, double lon, WeatherWarnings &out);

// Liegt der Ort im Bereich des deutschen Radarverbunds / der DWD-Warnungen?
bool dwd_in_germany(double lat, double lon);
