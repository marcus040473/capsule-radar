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

// Grobe Vorprüfung (Rechteck um Deutschland), ohne Netz.
bool dwd_in_germany(double lat, double lon);
// Liegt der Ort wirklich in Deutschland (DWD-Warngebiet einer Gemeinde)? Einmal beim DWD
// nachgefragt und für den Ort gemerkt; ohne Antwort zählt das Rechteck. Kern 0.
bool dwd_point_in_germany(double lat, double lon);
