#include "dwd_mosmix.h"
#include "dwd_stations.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <vector>

// ------------------------------------------------------------------ stations
int dwd_nearest_station(double lat, double lon, double maxKm, bool needObs, double *distKm) {
    const double kx = cos(lat * M_PI / 180.0);
    double best = 1e18;
    int bi = -1;
    for (int i = 0; i < DWD_STATION_COUNT; ++i) {
        const DwdStation &s = DWD_STATIONS[i];
        if (needObs && !s.obs) continue;
        double dlon = s.lon / 100.0 - lon;
        if (dlon > 180) dlon -= 360;
        if (dlon < -180) dlon += 360;
        const double dx = dlon * kx, dy = s.lat / 100.0 - lat;
        const double d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; bi = i; }
    }
    const double km = sqrt(best) * 111.195;
    if (bi < 0 || km > maxKm) return -1;
    if (distKm) *distKm = km;
    return bi;
}

const char *dwd_station_id(int idx) {
    return (idx >= 0 && idx < DWD_STATION_COUNT) ? DWD_STATIONS[idx].id : "";
}

const char *dwd_station_name(int idx) {
    return (idx >= 0 && idx < DWD_STATION_COUNT) ? DWD_STATION_NAMES + DWD_STATIONS[idx].name : "";
}

// ------------------------------------------------------------------ helpers
static long days_from_civil(int y, unsigned m, unsigned d) {   // H. Hinnant
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static time_t utc_time(int y, int mo, int d, int h, int mi, int s) {
    return (time_t)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400L + h * 3600L + mi * 60L + s);
}

float dwd_apparent_temp(float t, float rh, float windKmh) {
    if (isnan(t)) return t;
    if (isnan(rh)) rh = 70.0f;
    if (isnan(windKmh)) windKmh = 0.0f;
    const float e = rh / 100.0f * 6.105f * expf(17.27f * t / (237.7f + t));
    return t + 0.33f * e - 0.70f * (windKmh / 3.6f) - 4.0f;
}

int dwd_ww_to_code(int ww) {
    if (ww < 0) return -1;
    if (ww <= 3) return ww;
    if (ww < 40) return 3;                       // haze, dust, mist ... -> overcast-ish
    if (ww < 50) return ww == 48 || ww == 49 ? 48 : 45;
    if (ww == 56 || ww == 57) return ww;
    if (ww < 56) return ww <= 51 ? 51 : (ww <= 53 ? 53 : 55);
    if (ww < 60) return 61;                      // drizzle and rain
    if (ww == 66 || ww == 67) return ww;
    if (ww < 66) return ww <= 61 ? 61 : (ww <= 63 ? 63 : 65);
    if (ww < 70) return ww == 68 ? 71 : 73;      // rain and snow ("Schneeregen")
    if (ww < 80) return ww <= 71 ? 71 : (ww <= 73 ? 73 : (ww <= 75 ? 75 : 77));
    if (ww <= 82) return ww;
    if (ww == 83) return 85;                     // rain-and-snow showers
    if (ww == 84) return 86;
    if (ww <= 86) return ww;
    if (ww < 95) return 80;                      // hail/graupel showers
    return ww >= 96 ? 96 : 95;
}

// ------------------------------------------------------------------ MOSMIX KML
namespace {
struct Kml {
    std::string_view s;
    // values of <dwd:Forecast dwd:elementName="NAME"> (NAN where the KML says "-")
    bool series(const char *name, size_t n, std::vector<float> &out) const {
        char key[48];
        snprintf(key, sizeof(key), "dwd:elementName=\"%s\"", name);
        const size_t at = s.find(key);
        if (at == std::string_view::npos) return false;
        const size_t v0 = s.find("<dwd:value>", at);
        const size_t v1 = v0 == std::string_view::npos ? v0 : s.find("</dwd:value>", v0);
        if (v1 == std::string_view::npos) return false;
        out.assign(n, NAN);
        size_t i = v0 + 11, k = 0;
        while (i < v1 && k < n) {
            while (i < v1 && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
            if (i >= v1) break;
            size_t j = i;
            while (j < v1 && s[j] != ' ' && s[j] != '\n' && s[j] != '\r' && s[j] != '\t') ++j;
            if (!(j - i == 1 && s[i] == '-')) {
                char tok[24];
                const size_t l = j - i < sizeof(tok) - 1 ? j - i : sizeof(tok) - 1;
                memcpy(tok, s.data() + i, l);
                tok[l] = 0;
                out[k] = strtof(tok, nullptr);
            }
            ++k;
            i = j;
        }
        return true;
    }
};

float kelvin(float k) { return isnan(k) ? NAN : k - 273.15f; }

float humidity_from(float tC, float tdC) {   // Magnus
    if (isnan(tC) || isnan(tdC)) return NAN;
    const float a = 17.625f, b = 243.04f;
    const float rh = 100.0f * expf(a * tdC / (b + tdC)) / expf(a * tC / (b + tC));
    return rh > 100.0f ? 100.0f : rh;
}

time_t local_midnight(time_t ref, int addDays) {
    struct tm tm;
    localtime_r(&ref, &tm);
    tm.tm_mday += addDays;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}
}  // namespace

bool dwd_mosmix_parse(const char *text, size_t len, time_t now, WeatherSnapshot &out) {
    Kml k{std::string_view(text, len)};
    // forecast hours
    std::vector<time_t> t;
    const size_t ts0 = k.s.find("<dwd:ForecastTimeSteps>");
    const size_t ts1 = ts0 == std::string_view::npos ? ts0 : k.s.find("</dwd:ForecastTimeSteps>", ts0);
    if (ts1 == std::string_view::npos) return false;
    for (size_t p = k.s.find("<dwd:TimeStep>", ts0); p != std::string_view::npos && p < ts1;
         p = k.s.find("<dwd:TimeStep>", p + 1)) {
        int y, mo, d, h, mi;
        char buf[24];
        const size_t l = len - (p + 14) < sizeof(buf) - 1 ? len - (p + 14) : sizeof(buf) - 1;
        memcpy(buf, text + p + 14, l);
        buf[l] = 0;
        if (sscanf(buf, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) == 5) t.push_back(utc_time(y, mo, d, h, mi, 0));
    }
    const size_t n = t.size();
    if (n < 6) return false;
    std::vector<float> TTT, Td, TX, TN, ww, FF, DD, Rh00, R101;
    if (!k.series("TTT", n, TTT) || !k.series("ww", n, ww)) return false;
    k.series("Td", n, Td);
    k.series("TX", n, TX);
    k.series("TN", n, TN);
    k.series("FF", n, FF);
    k.series("DD", n, DD);
    k.series("Rh00", n, Rh00);
    k.series("R101", n, R101);
    auto at = [](const std::vector<float> &v, size_t i) { return i < v.size() ? v[i] : NAN; };

    WeatherSnapshot w = {};
    // now: the forecast hour closest to `now`
    size_t i0 = 0;
    for (size_t i = 1; i < n; ++i)
        if (llabs((long long)(t[i] - now)) < llabs((long long)(t[i0] - now))) i0 = i;
    if (llabs((long long)(t[i0] - now)) > 3 * 3600) return false;   // stale file or wrong clock
    w.tempC = kelvin(at(TTT, i0));
    if (isnan(w.tempC)) return false;
    const float hum = humidity_from(w.tempC, kelvin(at(Td, i0)));
    w.humidity = isnan(hum) ? 0 : (int)lroundf(hum);
    const float ff = at(FF, i0), dd = at(DD, i0);
    w.windKmh = isnan(ff) ? 0.0f : ff * 3.6f;
    w.windDeg = isnan(dd) ? 0 : (int)lroundf(dd) % 360;
    w.code = isnan(at(ww, i0)) ? -1 : dwd_ww_to_code((int)lroundf(at(ww, i0)));
    w.feelsC = dwd_apparent_temp(w.tempC, isnan(hum) ? NAN : hum, w.windKmh);
    {
        struct tm lt;
        localtime_r(&t[i0], &lt);
        snprintf(w.updated, sizeof(w.updated), "%02d:%02d", lt.tm_hour, lt.tm_min);
    }

    // days: today + 3, in local time
    for (int d = 0; d < 4; ++d) {
        const time_t d0 = local_midnight(now, d), d1 = local_midnight(now, d + 1);
        float tmax = -1e9f, tmin = 1e9f, txv = NAN, tnv = NAN, rain = NAN, r101 = NAN;
        int codeDay = -1, codeAll = -1, hours = 0;
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            const time_t ti = t[i];
            if (ti >= d0 && ti < d1) {
                any = true;
                ++hours;
                const float c = kelvin(at(TTT, i));
                if (!isnan(c)) { if (c > tmax) tmax = c; if (c < tmin) tmin = c; }
                const float x = kelvin(at(TX, i)), m = kelvin(at(TN, i));
                if (!isnan(x) && ti >= d0 + 12 * 3600) txv = isnan(txv) ? x : fmaxf(txv, x);   // TX of 06-18 UTC
                if (!isnan(m) && ti < d0 + 12 * 3600) tnv = isnan(tnv) ? m : fminf(tnv, m);    // TN of the night
                const float wv = at(ww, i);
                if (!isnan(wv)) {
                    const int code = dwd_ww_to_code((int)lroundf(wv));
                    struct tm lt;
                    localtime_r(&ti, &lt);
                    if (lt.tm_hour >= 6 && lt.tm_hour <= 21 && code > codeDay) codeDay = code;
                    if (code > codeAll) codeAll = code;
                }
                const float p1 = at(R101, i);
                if (!isnan(p1)) r101 = isnan(r101) ? p1 : fmaxf(r101, p1);
            }
            // 12-hour precipitation probability (Rh00, given at 06 and 18 UTC): the windows
            // ending in [08:00 local, 08:00 local next day) cover this day
            const float rh = at(Rh00, i);
            if (!isnan(rh) && ti >= d0 + 8 * 3600 && ti < d1 + 8 * 3600) rain = isnan(rain) ? rh : fmaxf(rain, rh);
        }
        if (!any || (d > 0 && hours < 6)) break;      // the forecast's ragged last day: leave it out
        WeatherDay &day = w.days[w.dayCount];
        struct tm lt;
        localtime_r(&d0, &lt);
        strftime(day.date, sizeof(day.date), "%Y-%m-%d", &lt);
        day.code = codeDay >= 0 ? codeDay : codeAll;
        day.tempMaxC = isnan(txv) ? tmax : fmaxf(txv, tmax);
        day.tempMinC = isnan(tnv) ? tmin : fminf(tnv, tmin);
        const float p = isnan(rain) ? r101 : rain;
        day.rainChance = isnan(p) ? 0 : (int)lroundf(fminf(100.0f, fmaxf(0.0f, p)));
        ++w.dayCount;
    }
    if (w.dayCount == 0) return false;
    w.valid = true;
    out = w;
    return true;
}

// ------------------------------------------------------------------ POI observations
bool dwd_poi_parse(const char *csv, size_t len, DwdObs &out) {
    std::string_view s(csv, len);
    std::vector<std::string_view> lines;
    for (size_t p = 0; p < s.size() && lines.size() < 4;) {
        size_t e = s.find('\n', p);
        if (e == std::string_view::npos) e = s.size();
        std::string_view l = s.substr(p, e - p);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        lines.push_back(l);
        p = e + 1;
    }
    if (lines.size() < 4) return false;
    auto fields = [](std::string_view l) {
        std::vector<std::string_view> f;
        size_t p = 0;
        while (p <= l.size()) {
            size_t e = l.find(';', p);
            if (e == std::string_view::npos) e = l.size();
            f.push_back(l.substr(p, e - p));
            p = e + 1;
        }
        return f;
    };
    const std::vector<std::string_view> head = fields(lines[0]), row = fields(lines[3]);
    auto col = [&](const char *name) -> float {
        for (size_t i = 0; i < head.size() && i < row.size(); ++i) {
            if (head[i] != name) continue;
            std::string_view v = row[i];
            if (v.empty() || v[0] == '-') return NAN;
            char b[24];
            const size_t l = v.size() < sizeof(b) - 1 ? v.size() : sizeof(b) - 1;
            for (size_t j = 0; j < l; ++j) b[j] = v[j] == ',' ? '.' : v[j];
            b[l] = 0;
            return strtof(b, nullptr);
        }
        return NAN;
    };
    if (row.size() < 3) return false;
    int d, mo, y, h, mi;
    char b[40];
    snprintf(b, sizeof(b), "%.*s %.*s", (int)row[0].size(), row[0].data(), (int)row[1].size(), row[1].data());
    if (sscanf(b, "%d.%d.%d %d:%d", &d, &mo, &y, &h, &mi) != 5) return false;
    out.time = utc_time(y < 100 ? 2000 + y : y, mo, d, h, mi, 0);
    out.tempC = col("dry_bulb_temperature_at_2_meter_above_ground");
    out.humidity = col("relative_humidity");
    out.windKmh = col("mean_wind_speed_during last_10_min_at_10_meters_above_ground");
    out.windDeg = col("mean_wind_direction_during_last_10 min_at_10_meters_above_ground");
    return !isnan(out.tempC);
}
