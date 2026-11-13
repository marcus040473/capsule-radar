#pragma once
// Himmelsrechnung für den ISS-Hinweis (header-only, für die Host-Tests):
//  - Sonnenhöhe an einem Ort (NOAA-Näherung, ~0,1°): ist es dunkel genug, die ISS zu sehen?
//  - Höhe und Richtung eines Satelliten über dem Horizont aus seinem Punkt auf der Erde.
#include <math.h>
#include <time.h>

namespace sky {

static const double DEG = M_PI / 180.0;
static const double EARTH_R_KM = 6371.0;

// Sun elevation in degrees at (lat, lon) for a UTC unix time.
inline double sunElevationDeg(double lat, double lon, time_t utc) {
    const double jd = (double)utc / 86400.0 + 2440587.5;          // Julian day
    const double T = (jd - 2451545.0) / 36525.0;                  // Julian centuries since J2000
    const double L0 = fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
    const double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
    const double C = sin(M * DEG) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                     sin(2 * M * DEG) * (0.019993 - 0.000101 * T) + sin(3 * M * DEG) * 0.000289;
    const double trueLong = L0 + C;
    const double omega = 125.04 - 1934.136 * T;
    const double lambda = trueLong - 0.00569 - 0.00478 * sin(omega * DEG);
    const double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    const double eps = eps0 + 0.00256 * cos(omega * DEG);
    const double decl = asin(sin(eps * DEG) * sin(lambda * DEG));
    const double y = tan(eps * DEG / 2) * tan(eps * DEG / 2);
    const double eqTime = 4.0 / DEG * (y * sin(2 * L0 * DEG) - 2 * e * sin(M * DEG) +
                          4 * e * y * sin(M * DEG) * cos(2 * L0 * DEG) -
                          0.5 * y * y * sin(4 * L0 * DEG) - 1.25 * e * e * sin(2 * M * DEG));   // minutes
    const double minutesUtc = fmod((double)utc / 60.0, 1440.0);
    double trueSolar = fmod(minutesUtc + eqTime + 4.0 * lon, 1440.0);
    if (trueSolar < 0) trueSolar += 1440.0;
    const double ha = trueSolar / 4.0 - 180.0;                    // hour angle, degrees
    const double cosZen = sin(lat * DEG) * sin(decl) + cos(lat * DEG) * cos(decl) * cos(ha * DEG);
    return 90.0 - acos(fmax(-1.0, fmin(1.0, cosZen))) / DEG;
}

// Elevation (deg) of an object at altKm above the point (sLat, sLon), seen from (lat, lon).
inline double elevationDeg(double lat, double lon, double sLat, double sLon, double altKm) {
    const double p1 = lat * DEG, p2 = sLat * DEG, dl = (sLon - lon) * DEG;
    const double c = sin(p1) * sin(p2) + cos(p1) * cos(p2) * cos(dl);   // cos of the central angle
    const double g = acos(fmax(-1.0, fmin(1.0, c)));
    const double r = EARTH_R_KM, R = EARTH_R_KM + altKm;
    // angle above the local horizon: atan2(R cos g - r, R sin g)
    return atan2(R * cos(g) - r, R * sin(g)) / DEG;
}

// Initial great-circle bearing (deg, 0 = north, clockwise) from (lat, lon) to (lat2, lon2).
inline double bearingDeg(double lat, double lon, double lat2, double lon2) {
    const double p1 = lat * DEG, p2 = lat2 * DEG, dl = (lon2 - lon) * DEG;
    const double b = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / DEG;
    return fmod(b + 360.0, 360.0);
}

// German 8-point compass word for a bearing.
inline const char *compassWord(double deg) {
    static const char *const w[8] = { "Nord", "Nordost", "Ost", "S\xC3\xBC" "dost",
                                      "S\xC3\xBC" "d", "S\xC3\xBC" "dwest", "West", "Nordwest" };
    return w[(int)floor(fmod(deg + 22.5 + 360.0, 360.0) / 45.0) % 8];
}

}  // namespace sky
