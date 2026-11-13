#pragma once
// Deutscher Wetterdienst (DWD) Open Data -> WeatherSnapshot. Portable (no Arduino), so the
// host tests can feed it real files. The network side lives in dwd_client.cpp.
//
//  - MOSMIX_L: the DWD's point forecast for ~5,600 stations worldwide, hourly for 10 days
//    (opendata.dwd.de/weather/local_forecasts/mos/MOSMIX_L/single_stations/<id>/kml/).
//    The .kmz is a ZIP holding one KML; dwd_client unzips it, dwd_mosmix_parse reads it.
//  - POI "BEOB" CSV: the latest hourly measurements of a weather station
//    (opendata.dwd.de/weather/weather_reports/poi/<id>-BEOB.csv).
#include "weather.h"
#include <stddef.h>
#include <time.h>

// Nearest station to lat/lon (index into DWD_STATIONS), -1 if none within maxKm.
// needObs: only stations that publish hourly measurements.
int dwd_nearest_station(double lat, double lon, double maxKm, bool needObs, double *distKm = nullptr);
const char *dwd_station_id(int idx);
const char *dwd_station_name(int idx);

// MOSMIX KML text -> current conditions (the hour closest to `now`) and up to 4 days
// starting today, in the device's local time zone (TZ). False if the text is unusable.
bool dwd_mosmix_parse(const char *kml, size_t len, time_t now, WeatherSnapshot &out);

// Latest row of a POI observation CSV. Fields that the station did not report are NAN.
struct DwdObs {
    time_t time;          // UTC
    float tempC, humidity, windKmh, windDeg;
};
bool dwd_poi_parse(const char *csv, size_t len, DwdObs &out);

// "Gefühlt": apparent temperature (Steadman/BoM, as Open-Meteo uses) from °C, % and km/h.
float dwd_apparent_temp(float tempC, float humidity, float windKmh);

// MOSMIX "ww" (WMO 4677 present weather) -> the code subset weather_condition() knows.
int dwd_ww_to_code(int ww);
