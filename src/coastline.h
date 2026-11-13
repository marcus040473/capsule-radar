#pragma once
// World coastline background for the radar scope.
// Project the embedded Natural Earth coastline (coastline_data.h) into screen
// polylines for the current scope, then draw them under the aircraft. Projection
// is done once per home/range change (cheap bbox cull + great-circle), never per
// frame; drawing happens inside the static chrome layer's DRAW_MAIN callback.
#include <lvgl.h>

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx);

void coastline_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width);

// Same projection into a caller-owned set of polylines (the ISS page's zoomed-out view).
// minStepPx > 0 drops points closer than that to the last kept one -- at thousands of km
// per screen the 1:10m coast has far more detail than pixels, and every point costs RAM.
// Stored flat (all points in one vector, a length per polyline): two allocations instead of
// thousands of small ones, so a big view lands in PSRAM and not in scarce internal RAM.
#include <vector>
#include <stdint.h>
struct CoastLines {
    std::vector<lv_point_t> pts;
    std::vector<uint16_t>   len;
    void clear() { pts.clear(); len.clear(); }
    bool empty() const { return len.empty(); }
    void release() { std::vector<lv_point_t>().swap(pts); std::vector<uint16_t>().swap(len); }
};
void coastline_project_into(CoastLines &out, double homeLat, double homeLon, double rangeKm,
                            float cx, float cy, float rOuterPx, int minStepPx);
void coastline_draw_lines(lv_draw_ctx_t *ctx, const CoastLines &lines, lv_color_t color, lv_opa_t opa, lv_coord_t width);
