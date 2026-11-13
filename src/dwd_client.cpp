#include "dwd_client.h"
#include "dwd_mosmix.h"
#include "net_fetch.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include "miniz.h"          // ESP32 ROM inflate (tinfl): the MOSMIX .kmz is a ZIP

// Rough box of the German radar composite / DWD warning areas. Inside it the radar is
// requested from the DWD; a pixel check then falls back to RainViewer if the spot has no
// DWD coverage after all (e.g. Strasbourg, Prague).
bool dwd_in_germany(double lat, double lon) {
    return lat > 47.0 && lat < 55.3 && lon > 5.6 && lon < 15.3;
}

bool dwd_point_in_germany(double lat, double lon) {
    static double cLat = 999, cLon = 999;
    static bool cIn = false;
    if (!dwd_in_germany(lat, lon)) return false;
    if (fabs(lat - cLat) < 0.005 && fabs(lon - cLon) < 0.005) return cIn;
    char url[360];
    snprintf(url, sizeof(url),
             "https://maps.dwd.de/geoserver/dwd/ows?service=WFS&version=2.0.0&request=GetFeature"
             "&typeName=dwd:Warngebiete_Gemeinden&outputFormat=application/json&propertyName=NAME&count=1"
             "&CQL_FILTER=INTERSECTS(SHAPE,POINT(%.5f%%20%.5f))", lat, lon);
    uint8_t *buf = nullptr; size_t len = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &buf, &len, 8000, 3500, 7000)) return true;   // ask again next time
    const char *k = "\"numberMatched\":";
    const char *p = nullptr;
    for (size_t i = 0; i + strlen(k) < len; ++i)
        if (!memcmp(buf + i, k, strlen(k))) { p = (const char *)buf + i + strlen(k); break; }
    const bool known = p != nullptr;
    const bool in = known && atoi(p) > 0;
    heap_caps_free(buf);
    if (!known) return true;
    cLat = lat; cLon = lon; cIn = in;
    Serial.printf("[dwd] %.4f, %.4f is %s Germany\n", lat, lon, in ? "in" : "outside");
    return in;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// First file of a ZIP -> freshly allocated, NUL-terminated PSRAM buffer (caller frees).
static char *unzip_first(const uint8_t *z, size_t zlen, size_t *outLen) {
    if (zlen < 30 || rd32(z) != 0x04034b50) return nullptr;
    const uint16_t flags = rd16(z + 6), method = rd16(z + 8);
    uint32_t csize = rd32(z + 18), usize = rd32(z + 22);
    const size_t data = 30 + rd16(z + 26) + rd16(z + 28);
    if ((flags & 8) || csize == 0 || usize == 0) {          // sizes only in the central directory
        for (size_t i = zlen >= 22 ? zlen - 22 : 0; i > 0 && i + 22 <= zlen; --i) {
            if (rd32(z + i) != 0x06054b50) continue;
            const uint32_t cd = rd32(z + i + 16);
            if (cd + 46 > zlen || rd32(z + cd) != 0x02014b50) return nullptr;
            csize = rd32(z + cd + 20);
            usize = rd32(z + cd + 24);
            break;
        }
    }
    if (data + csize > zlen || usize == 0 || usize > 2u * 1024 * 1024) return nullptr;
    char *out = (char *)heap_caps_malloc(usize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return nullptr;
    if (method == 0) {
        memcpy(out, z + data, usize);
    } else if (method == 8) {
        tinfl_decompressor *d = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor),
                                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!d) { heap_caps_free(out); return nullptr; }
        tinfl_init(d);
        size_t inLen = csize, oLen = usize;
        const tinfl_status st = tinfl_decompress(d, z + data, &inLen, (mz_uint8 *)out, (mz_uint8 *)out, &oLen,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        heap_caps_free(d);
        if (st != TINFL_STATUS_DONE || oLen != usize) {
            Serial.printf("[dwd] inflate failed (%d, %u of %u)\n", (int)st, (unsigned)oLen, (unsigned)usize);
            heap_caps_free(out);
            return nullptr;
        }
    } else {
        heap_caps_free(out);
        return nullptr;
    }
    out[usize] = 0;
    *outLen = usize;
    return out;
}

bool dwd_weather_fetch(double lat, double lon, WeatherSnapshot &out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    const time_t now = time(nullptr);
    if (now < 1700000000L) return false;                     // the forecast needs the clock
    double km = 0;
    const int st = dwd_nearest_station(lat, lon, DWD_STATION_MAX_KM, false, &km);
    if (st < 0) return false;
    char url[200];
    snprintf(url, sizeof(url),
             "https://opendata.dwd.de/weather/local_forecasts/mos/MOSMIX_L/single_stations/%s/kml/MOSMIX_L_LATEST_%s.kmz",
             dwd_station_id(st), dwd_station_id(st));
    uint8_t *zip = nullptr; size_t zlen = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &zip, &zlen, 120000, 3500, 9000)) {
        Serial.printf("[dwd] MOSMIX %s fetch failed\n", dwd_station_id(st));
        return false;
    }
    size_t klen = 0;
    char *kml = unzip_first(zip, zlen, &klen);
    heap_caps_free(zip);
    if (!kml) { Serial.println("[dwd] MOSMIX unzip failed"); return false; }
    WeatherSnapshot w;
    const bool ok = dwd_mosmix_parse(kml, klen, now, w);
    heap_caps_free(kml);
    if (!ok) { Serial.println("[dwd] MOSMIX parse failed"); return false; }
    Serial.printf("[dwd] forecast %s %s (%.0f km)\n", dwd_station_id(st), dwd_station_name(st), km);

    // measured "now" from the nearest station with hourly observations, if it is close and fresh
    double okm = 0;
    const int os = dwd_nearest_station(lat, lon, DWD_OBS_MAX_KM, true, &okm);
    if (os >= 0) {
        snprintf(url, sizeof(url), "https://opendata.dwd.de/weather/weather_reports/poi/%s-BEOB.csv", dwd_station_id(os));
        uint8_t *csv = nullptr; size_t clen = 0;
        if (net_fetch_psram(url, ADSB_USER_AGENT, &csv, &clen, 64000, 3500, 7000)) {
            DwdObs o;
            if (dwd_poi_parse((const char *)csv, clen, o) && now - o.time < 3 * 3600) {
                w.tempC = o.tempC;
                if (!isnan(o.humidity)) w.humidity = (int)lroundf(o.humidity);
                if (!isnan(o.windKmh)) w.windKmh = o.windKmh;
                if (!isnan(o.windDeg)) w.windDeg = (int)lroundf(o.windDeg) % 360;
                w.feelsC = dwd_apparent_temp(w.tempC, (float)w.humidity, w.windKmh);
                struct tm lt;
                localtime_r(&o.time, &lt);
                snprintf(w.updated, sizeof(w.updated), "%02d:%02d", lt.tm_hour, lt.tm_min);
                Serial.printf("[dwd] measured %s %s (%.0f km): %.1f C\n", dwd_station_id(os),
                              dwd_station_name(os), okm, (double)o.tempC);
            }
            heap_caps_free(csv);
        }
    }
    snprintf(w.source, sizeof(w.source), "DWD");
    out = w;
    return true;
}

static long iso_to_epoch(const char *s) {
    int y, mo, d, h, mi;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    // days-from-civil without touching the TZ (mktime would treat it as local time)
    y -= mo <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + (long)doe - 719468) * 86400L + h * 3600L + mi * 60L;
}

bool dwd_warnings_fetch(double lat, double lon, WeatherWarnings &out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    char url[400];
    // WFS 2.0 with EPSG:4326 takes the point as "lat lon"
    snprintf(url, sizeof(url),
             "https://maps.dwd.de/geoserver/dwd/ows?service=WFS&version=2.0.0&request=GetFeature"
             "&typeName=dwd:Warnungen_Gemeinden&outputFormat=application/json"
             "&propertyName=EVENT,SEVERITY,URGENCY,HEADLINE,ONSET,EXPIRES"
             "&CQL_FILTER=INTERSECTS(THE_GEOM,POINT(%.5f%%20%.5f))", lat, lon);
    uint8_t *buf = nullptr; size_t len = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &buf, &len, 48000, 3500, 8000)) return false;
    JsonDocument filter;
    filter["features"][0]["properties"] = true;
    JsonDocument doc;
    const bool ok = !deserializeJson(doc, (const char *)buf, len, DeserializationOption::Filter(filter));
    heap_caps_free(buf);
    if (!ok) return false;
    WeatherWarnings w = {};
    w.valid = true;
    const long now = (long)time(nullptr);
    for (JsonObject f : doc["features"].as<JsonArray>()) {
        JsonObject p = f["properties"];
        const char *sev = p["SEVERITY"] | "";
        int level = !strcmp(sev, "Extreme") ? 4 : !strcmp(sev, "Severe") ? 3 : !strcmp(sev, "Moderate") ? 2 : 1;
        WeatherWarning it = {};
        snprintf(it.event, sizeof(it.event), "%s", (const char *)(p["EVENT"] | ""));
        snprintf(it.headline, sizeof(it.headline), "%s", (const char *)(p["HEADLINE"] | ""));
        it.level = level;
        it.preliminary = !strcmp(p["URGENCY"] | "", "Future");
        it.onset = iso_to_epoch(p["ONSET"] | "");
        it.expires = iso_to_epoch(p["EXPIRES"] | "");
        if (it.expires && now > 1700000000L && it.expires < now) continue;   // already over
        if (!it.event[0]) continue;
        bool dup = false;                                     // one row per event and level
        for (int i = 0; i < w.count; ++i)
            if (!strcmp(w.items[i].event, it.event) && w.items[i].level == it.level) {
                if (it.expires > w.items[i].expires) w.items[i].expires = it.expires;
                dup = true;
            }
        if (dup) continue;
        // keep the 3 most severe (real warnings before pre-information)
        auto rank = [](const WeatherWarning &x) { return x.level * 2 + (x.preliminary ? 0 : 1); };
        int pos = w.count < 3 ? w.count : 3;
        while (pos > 0 && rank(w.items[pos - 1]) < rank(it)) {
            if (pos < 3) w.items[pos] = w.items[pos - 1];
            --pos;
        }
        if (pos < 3) {
            w.items[pos] = it;
            if (w.count < 3) ++w.count;
        }
    }
    out = w;
    return true;
}
