// Host test: DWD MOSMIX/POI parsing (src/dwd_mosmix.*) with a small synthetic KML + CSV.
#include "dwd_mosmix.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  [PASS]  %s\n", msg); else { printf("  [FAIL]  %s\n", msg); ++fails; } } while (0)

int main() {
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    // 2026-11-13 00:00Z .. 72 hourly steps
    std::string k = "<kml><dwd:ForecastTimeSteps>";
    for (int h = 0; h < 72; ++h) {
        char b[80];
        snprintf(b, sizeof(b), "<dwd:TimeStep>2026-11-%02dT%02d:00:00.000Z</dwd:TimeStep>", 13 + h / 24, h % 24);
        k += b;
    }
    k += "</dwd:ForecastTimeSteps>";
    auto series = [&](const char *name, auto f) {
        k += std::string("<dwd:Forecast dwd:elementName=\"") + name + "\"><dwd:value>";
        for (int h = 0; h < 72; ++h) { k += " "; k += f(h); }
        k += "</dwd:value></dwd:Forecast>";
    };
    auto num = [](double v) { char b[24]; snprintf(b, sizeof(b), "%.2f", v); return std::string(b); };
    series("TTT", [&](int h) { return num(273.15 + 5 + 5 * sin((h % 24 - 9) * M_PI / 12)); });
    series("Td",  [&](int h) { (void)h; return num(273.15 + 1); });
    series("FF",  [&](int h) { (void)h; return num(5.0); });            // 18 km/h
    series("DD",  [&](int h) { (void)h; return num(270); });
    series("ww",  [&](int h) { return num(h >= 34 && h <= 36 ? 61 : 2); });   // rain on the 14th, ~11-13 local
    series("TX",  [&](int h) { return h % 24 == 18 ? num(273.15 + 12) : std::string("-"); });
    series("TN",  [&](int h) { return h % 24 == 6 ? num(273.15 - 2) : std::string("-"); });
    series("Rh00",[&](int h) { return h % 12 == 6 ? num(h < 24 ? 10 : 70) : std::string("-"); });
    k += "</kml>";

    WeatherSnapshot w;
    const time_t now = 1794571200;   // 2026-11-13 12:00Z (13:00 CET)
    CHECK(dwd_mosmix_parse(k.data(), k.size(), now, w), "MOSMIX parses");
    CHECK(strcmp(w.updated, "13:00") == 0, "current hour is 13:00 local");
    CHECK(fabsf(w.windKmh - 18.0f) < 0.1f && w.windDeg == 270, "wind 18 km/h from 270");
    CHECK(w.humidity > 40 && w.humidity < 80, "humidity from dew point");
    CHECK(w.dayCount == 3, "three days in 72 hours");
    CHECK(strcmp(w.days[0].date, "2026-11-13") == 0 && strcmp(w.days[1].date, "2026-11-14") == 0, "local dates");
    CHECK(fabsf(w.days[1].tempMaxC - 12.0f) < 0.01f, "day max from TX (06-18 UTC)");
    CHECK(fabsf(w.days[1].tempMinC + 2.0f) < 0.01f, "day min from TN (night)");
    CHECK(w.days[1].code == 61, "rain shows as the day's weather");
    CHECK(w.days[1].rainChance == 70, "12-hour precipitation probability");
    CHECK(!dwd_mosmix_parse(k.data(), 200, now, w), "truncated KML is rejected");
    CHECK(!dwd_mosmix_parse(k.data(), k.size(), now + 10 * 86400, w), "stale file is rejected");

    const char *csv =
        "surface observations;Parameter description;dry_bulb_temperature_at_2_meter_above_ground;relative_humidity;"
        "mean_wind_speed_during last_10_min_at_10_meters_above_ground;mean_wind_direction_during_last_10 min_at_10_meters_above_ground\r\n"
        "10865;Unit;Grad C;%;km/h;Grad\r\n"
        "Datum;Uhrzeit (UTC);Temperatur (2m);Relative Feuchte;Windgeschwindigkeit;Windrichtung\r\n"
        "13.11.26;11:00;5,2;68;12;270\r\n"
        "13.11.26;10:00;3,7;78;10;290\r\n";
    DwdObs o;
    CHECK(dwd_poi_parse(csv, strlen(csv), o), "POI parses");
    CHECK(fabsf(o.tempC - 5.2f) < 0.01f && fabsf(o.humidity - 68) < 0.01f, "newest row, decimal comma");
    CHECK(o.time == 1794567600, "observation time (UTC)");
    CHECK(dwd_ww_to_code(83) == 85 && dwd_ww_to_code(49) == 48 && dwd_ww_to_code(3) == 3, "ww mapping");
    double km = 0;
    const int st = dwd_nearest_station(48.137, 11.575, 60, false, &km);
    CHECK(st >= 0 && strcmp(dwd_station_id(st), "10865") == 0, "nearest station to Munich centre");
    CHECK(dwd_nearest_station(0.0, -30.0, 60, false) < 0, "no station in mid-Atlantic");
    printf("RESULT: %s (%d failing)\n", fails ? "FAILURES" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
