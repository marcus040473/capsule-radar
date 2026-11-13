#include "coastline.h"
#include "coastline_data.h"
#include "geo.h"
#include <vector>
#include <math.h>
#include <stdlib.h>

// Cached screen-space polylines for the current scope. Rebuilt only when the
// home position or range changes (a handful of times per session), so the per-
// frame render cost is zero — the chrome layer just repaints what's here.
static CoastLines s_lines;

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx) {
    coastline_project_into(s_lines, homeLat, homeLon, rangeKm, cx, cy, rOuterPx, 0);
}

void coastline_project_into(CoastLines &out, double homeLat, double homeLon, double rangeKm,
                            float cx, float cy, float rOuterPx, int minStepPx) {
    out.clear();
    if (rangeKm <= 0) return;
    const int step2 = minStepPx * minStepPx;
    // Thinning also happens before any trig, on the raw int16 coordinates: a point less than
    // ~0.7 px (in degrees) from the last kept one is skipped outright. At a few thousand km
    // per screen that skips >95% of the 1:10m coast, which keeps the projection fast.
    const int rawStep = minStepPx > 0
        ? (int)(0.7 * minStepPx * (rangeKm / rOuterPx) / 111.0 * COAST_SCALE) : 0;

    const double EDGE = 1.08;                       // include a touch past the rim, then clip
    const double rangeDeg  = rangeKm / 111.0;
    const double latMargin = rangeDeg * 1.20;
    const double cosLat    = cos(homeLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    const bool  fast    = minStepPx > 0;
    const float kRad    = (float)(M_PI / 180.0 / COAST_SCALE);
    const float lon0    = (float)(homeLon * M_PI / 180.0);
    const float sinP0   = (float)sin(homeLat * M_PI / 180.0), cosP0 = (float)cos(homeLat * M_PI / 180.0);
    const float pxPerRad = (float)(6371.0 / rangeKm * rOuterPx);

    const int16_t *p = COAST_PTS;
    size_t runStart = 0;
    auto flush = [&]() {                            // close the current in-range run
        const size_t n = out.pts.size() - runStart;
        if (n >= 2) out.len.push_back((uint16_t)n);
        else out.pts.resize(runStart);
        runStart = out.pts.size();
    };
    for (int poly = 0; poly < COAST_NUM_POLYS; ++poly) {
        const int n = COAST_POLY_LEN[poly];
        int lastLatI = 0, lastLonI = 0;
        bool haveLast = false;
        for (int i = 0; i < n; ++i) {
            const int latI = p[i * 2], lonI = p[i * 2 + 1];
            if (rawStep > 0 && haveLast &&
                abs(latI - lastLatI) < rawStep && abs(lonI - lastLonI) < rawStep) continue;
            const double lat = latI / (double)COAST_SCALE;
            const double lon = lonI / (double)COAST_SCALE;
            const double dlon = lon - homeLon;
            // cheap bounding-box reject (no trig) discards ~99% of the planet instantly;
            // the second dlon test wraps the antimeridian so e.g. home near 179E still works.
            const bool outside = (fabs(lat - homeLat) > latMargin) ||
                                 (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin);
            bool inRange = false;
            lv_point_t sp;
            if (!outside && fast) {
                // zoomed-out view: the same azimuthal projection in single precision, which the
                // S3's FPU does in hardware (double trig is software and ~30x slower)
                const float phi = latI * kRad, dl = lonI * kRad - lon0;
                const float sp1 = sinf(phi), cp1 = cosf(phi), cdl = cosf(dl);
                float cc = sinP0 * sp1 + cosP0 * cp1 * cdl;
                cc = cc > 1.0f ? 1.0f : (cc < -1.0f ? -1.0f : cc);
                const float c = acosf(cc);                         // angular distance
                if (c * 6371.0f <= (float)(rangeKm * EDGE)) {
                    const float sc = sinf(c), k = sc > 1e-6f ? c / sc : 1.0f;
                    const float ex = k * cp1 * sinf(dl), ny = k * (cosP0 * sp1 - sinP0 * cp1 * cdl);
                    sp.x = (lv_coord_t)lroundf(cx + ex * pxPerRad);
                    sp.y = (lv_coord_t)lroundf(cy - ny * pxPerRad);
                    inRange = true;
                }
            } else if (!outside) {
                const double dist = geo::haversineKm(homeLat, homeLon, lat, lon);
                if (dist <= rangeKm * EDGE) {
                    const double brg = geo::bearingDeg(homeLat, homeLon, lat, lon);
                    const double rPx = (dist / rangeKm) * rOuterPx;
                    const double a   = brg * M_PI / 180.0;
                    sp.x = (lv_coord_t)lroundf((float)(cx + rPx * sin(a)));
                    sp.y = (lv_coord_t)lroundf((float)(cy - rPx * cos(a)));
                    inRange = true;
                }
            }
            if (inRange) {
                lastLatI = latI; lastLonI = lonI; haveLast = true;
                if (step2 > 0 && out.pts.size() > runStart) {   // thin out: skip points too close to the last
                    const int dx = sp.x - out.pts.back().x, dy = sp.y - out.pts.back().y;
                    if (dx * dx + dy * dy < step2) continue;
                }
                if (out.pts.size() - runStart >= 65535) flush();
                out.pts.push_back(sp);
                continue;
            }
            haveLast = false;
            flush();
        }
        flush();
        p += n * 2;
    }
}

void coastline_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width) {
    coastline_draw_lines(ctx, s_lines, color, opa, width);
}

void coastline_draw_lines(lv_draw_ctx_t *ctx, const CoastLines &s_lines, lv_color_t color, lv_opa_t opa, lv_coord_t width) {
    if (s_lines.empty()) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa   = opa;
    d.round_start = d.round_end = 1;   // smooth the joints on the thicker line
    size_t at = 0;
    for (uint16_t n : s_lines.len) {
        for (size_t i = 1; i < n; ++i) {
            lv_point_t a = s_lines.pts[at + i - 1];
            lv_point_t b = s_lines.pts[at + i];
            lv_draw_line(ctx, &d, &a, &b);
        }
        at += n;
    }
}
