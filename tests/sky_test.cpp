// Host test for sky.h: sun elevation against known values, ISS elevation geometry.
#include "../src/sky.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0;
static void near(const char *what, double got, double want, double tol) {
    const bool ok = fabs(got - want) <= tol;
    printf("  [%s]  %-48s got %8.3f  want %8.3f (+-%.2f)\n", ok ? "PASS" : "FAIL", what, got, want, tol);
    if (!ok) ++fails;
}
int main() {
    // Berlin 52.52N 13.405E. 2026-06-21 11:10 UTC ~ solar noon: elevation ~ 90-52.52+23.44 = 60.9
    near("sun Berlin, summer solstice noon", sky::sunElevationDeg(52.52, 13.405, 1782040200), 60.9, 0.4);
    // Berlin 2026-12-21 11:12 UTC ~ solar noon: 90-52.52-23.44 = 14.04
    near("sun Berlin, winter solstice noon", sky::sunElevationDeg(52.52, 13.405, 1797851520), 14.0, 0.4);
    // Berlin 2026-12-21 23:00 UTC: deep night, below -50
    const double night = sky::sunElevationDeg(52.52, 13.405, 1797894000);
    printf("  [%s]  sun Berlin, winter midnight below -40         got %8.3f\n", night < -40 ? "PASS" : "FAIL", night);
    if (night >= -40) ++fails;
    // ISS straight overhead -> 90 deg; 2000 km away at 420 km -> below the horizon-ish (< 5 deg)
    near("ISS overhead", sky::elevationDeg(50, 10, 50, 10, 420), 90.0, 0.01);
    const double far = sky::elevationDeg(50, 10, 50 + 2000.0 / 111.2, 10, 420);
    printf("  [%s]  ISS 2000 km away low (< 5 deg)                got %8.3f\n", far < 5 ? "PASS" : "FAIL", far);
    if (far >= 5) ++fails;
    near("bearing due east at the equator", sky::bearingDeg(0, 0, 0, 1), 90.0, 0.01);
    printf("RESULT: %s (%d failing)\n", fails ? "FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
