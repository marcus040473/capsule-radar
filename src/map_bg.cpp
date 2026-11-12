// Karte unter dem Radar -- siehe map_bg.h. Portabel (Gerät und Desktop-Simulator).
#include "map_bg.h"
#include "radar_view.h"
#include "config.h"
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
static void *big_alloc(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM); }
#else
static void *big_alloc(size_t n) { return calloc(1, n); }
#endif

static const double EARTH_CIRC_M = 40075016.686;           // Äquatorumfang (Web Mercator)
static const int    NPIX = SCREEN_W * SCREEN_H;

static uint8_t  *s_back = nullptr;      // Kern 0 setzt hier zusammen
static uint8_t  *s_front = nullptr;     // Kern 1: letzte übernommene Fassung
static uint16_t *s_rgb = nullptr;       // Kern 1: eingefärbt, das zeigt das Radar
static lv_img_dsc_t s_dsc;

// Wunsch (Kern 1 schreibt, Kern 0 liest; ein Zähler macht Änderungen sichtbar)
struct View { double lat, lon; float rangeKm, rotDeg; };
static volatile uint32_t s_wantGen = 0;
static View s_want = {0, 0, 0, 0};
static uint32_t s_planGen = 0xFFFFFFFF; // Kern 0: für welche Ansicht geplant wurde

// Plan (nur Kern 0)
static const int MAX_TILES = 25;
static MapTile s_tiles[MAX_TILES];
static int     s_tileN = 0;
static int     s_z = 0;
static double  s_cwx = 0, s_cwy = 0;    // Radarmitte in Welt-Pixeln (Zoom s_z, 512er Kacheln)
static float   s_ex[2], s_ny[2];        // Bildschirm-Offset -> Welt-Pixel: wx = ex0*dx + ex1*dy ...
static float   s_wxx, s_wxy, s_wyx, s_wyy;

// Übergabe Kern 0 -> Kern 1
static volatile bool s_ready = false;   // s_back enthält ein neues Zwischenbild

bool map_bg_begin(void) {
    if (s_back) return true;
    s_back  = (uint8_t *)big_alloc(NPIX);
    s_front = (uint8_t *)big_alloc(NPIX);
    s_rgb   = (uint16_t *)big_alloc((size_t)NPIX * 2);
    if (!s_back || !s_front || !s_rgb) return false;
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.w = SCREEN_W;
    s_dsc.header.h = SCREEN_H;
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.data_size = (uint32_t)NPIX * 2;
    s_dsc.data = (const uint8_t *)s_rgb;
    return true;
}

void map_bg_set_view(double lat, double lon, float rangeKm, float rotDeg) {
    if (lat == s_want.lat && lon == s_want.lon && rangeKm == s_want.rangeKm && rotDeg == s_want.rotDeg) return;
    s_want.lat = lat; s_want.lon = lon; s_want.rangeKm = rangeKm; s_want.rotDeg = rotDeg;
    s_wantGen = s_wantGen + 1;
}

bool map_bg_view_changed(void) { return s_planGen != s_wantGen; }

int map_bg_replan(void) {
    if (!s_back || !map_bg_view_changed()) return 0;
    if (s_ready) return 0;     // core 1 is about to copy s_back: plan on the next call
    const uint32_t gen = s_wantGen;
    const View v = s_want;
    s_planGen = gen;
    s_tileN = 0;
    if (v.rangeKm <= 0.0f) return 0;

    const double latR = v.lat * M_PI / 180.0;
    const double mpp  = v.rangeKm * 1000.0 / RADAR_R_OUTER_PX;     // Meter je Bildschirmpixel
    // Zoom so, dass ein Kachelpixel etwa ein Bildschirmpixel ist (Faktor 0,7 .. 1,4)
    const double worldPxWanted = EARTH_CIRC_M * cos(latR) / mpp;
    int z = (int)lround(log2(worldPxWanted / MAP_TILE_PX));
    if (z < 2) z = 2;
    if (z > 16) z = 16;
    s_z = z;
    const double worldPx = (double)MAP_TILE_PX * (double)(1 << z);
    const double mPerWorldPx = EARTH_CIRC_M * cos(latR) / worldPx;
    const double k = mpp / mPerWorldPx;                            // Welt-Pixel je Bildschirmpixel

    s_cwx = (v.lon + 180.0) / 360.0 * worldPx;
    s_cwy = (1.0 - log(tan(latR) + 1.0 / cos(latR)) / M_PI) / 2.0 * worldPx;

    // Bildschirm (dx nach rechts, dy nach unten, ab Mitte) -> Welt-Pixel. Wie
    // geo::projectToScreen: oben = Peilung rotDeg. Ost = ux*cos + uy*sin, Nord = -ux*sin + uy*cos
    // mit ux = dx, uy = -dy; Welt-y wächst nach Süden.
    const double th = v.rotDeg * M_PI / 180.0, c = cos(th), s = sin(th);
    s_wxx = (float)( k * c);  s_wxy = (float)(-k * s);   // wx += wxx*dx + wxy*dy
    s_wyx = (float)( k * s);  s_wyy = (float)( k * c);   // wy += wyx*dx + wyy*dy

    // Welt-Bereich der vier Bildschirmecken -> Kacheln
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    const double hx = SCREEN_W / 2.0, hy = SCREEN_H / 2.0;
    const double cx[4] = { -hx, hx, -hx, hx }, cy[4] = { -hy, -hy, hy, hy };
    for (int i = 0; i < 4; ++i) {
        const double wx = s_cwx + s_wxx * cx[i] + s_wxy * cy[i];
        const double wy = s_cwy + s_wyx * cx[i] + s_wyy * cy[i];
        if (wx < minX) minX = wx;
        if (wx > maxX) maxX = wx;
        if (wy < minY) minY = wy;
        if (wy > maxY) maxY = wy;
    }
    const int n = 1 << z;
    const int tx0 = (int)floor(minX / MAP_TILE_PX), tx1 = (int)floor(maxX / MAP_TILE_PX);
    int ty0 = (int)floor(minY / MAP_TILE_PX), ty1 = (int)floor(maxY / MAP_TILE_PX);
    if (ty0 < 0) ty0 = 0;
    if (ty1 > n - 1) ty1 = n - 1;
    // Mitte zuerst: die Karte erscheint von innen nach außen
    const int ctx = (int)floor(s_cwx / MAP_TILE_PX), cty = (int)floor(s_cwy / MAP_TILE_PX);
    for (int ring = 0; ring <= 4 && s_tileN < MAX_TILES; ++ring)
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                const int d = abs(tx - ctx) > abs(ty - cty) ? abs(tx - ctx) : abs(ty - cty);
                if (d != ring || s_tileN >= MAX_TILES) continue;
                s_tiles[s_tileN].z = z;
                s_tiles[s_tileN].x = ((tx % n) + n) % n;
                s_tiles[s_tileN].y = ty;
                s_tileN++;                     // (compose unwraps x next to the centre again)
            }
    memset(s_back, 0, NPIX);   // the next hand-over starts a fresh picture
    return s_tileN;
}

bool map_bg_tile(int i, MapTile *t) {
    if (i < 0 || i >= s_tileN) return false;
    *t = s_tiles[i];
    return true;
}

bool map_bg_compose_tile(int i, const uint8_t *lum) {
    if (s_ready) return false;                   // core 1 has not taken the last picture yet
    if (i < 0 || i >= s_tileN || !lum) return true;
    const MapTile &t = s_tiles[i];
    const int n = 1 << t.z;
    // Tile origin in world pixels, unwrapped next to the radar centre (date line)
    double ox = (double)t.x * MAP_TILE_PX;
    const double world = (double)n * MAP_TILE_PX;
    if (ox - s_cwx >  world / 2) ox -= world;
    if (ox - s_cwx < -world / 2) ox += world;
    const double oy = (double)t.y * MAP_TILE_PX;
    // Offset of the radar centre inside this tile (float is fine: small numbers)
    const float bx = (float)(s_cwx - ox), by = (float)(s_cwy - oy);
    const int cx = SCREEN_W / 2, cy = SCREEN_H / 2;
    for (int y = 0; y < SCREEN_H; ++y) {
        const float dy = (float)(y - cy);
        float wx = bx + s_wxy * dy + s_wxx * (float)(0 - cx);
        float wy = by + s_wyy * dy + s_wyx * (float)(0 - cx);
        uint8_t *row = s_back + (size_t)y * SCREEN_W;
        for (int x = 0; x < SCREEN_W; ++x, wx += s_wxx, wy += s_wyx) {
            if (wx < 0.0f || wy < 0.0f || wx >= (float)MAP_TILE_PX || wy >= (float)MAP_TILE_PX) continue;
            row[x] = lum[(int)wy * MAP_TILE_PX + (int)wx];
        }
    }
    s_ready = true;
    return true;
}

// ------------------------------------------------------------------ Kern 1
static int  s_lastMode = -1, s_lastTheme = -1;
static bool s_haveFront = false;

static uint32_t theme_tint(int theme) {           // 0 = keine Karte (Orb hat eigenen Hintergrund)
    switch (theme) {
        case THEME_AMBER:    return 0xFFB23C;
        case THEME_MILITARY: return 0x49C46B;
        case THEME_ORB:      return 0;
        default:             return 0x1DFF86;      // Phosphor
    }
}

void map_bg_ui_tick(int mode) {
    if (!s_back) return;
    bool fresh = false;
    if (s_ready) {
        memcpy(s_front, s_back, NPIX);
        s_ready = false;
        s_haveFront = true;
        fresh = true;
    }
    const int theme = radar::theme();
    if (!fresh && mode == s_lastMode && theme == s_lastTheme) return;
    s_lastMode = mode;
    s_lastTheme = theme;
    const uint32_t tint = theme_tint(theme);
    if (mode <= 0 || !tint || !s_haveFront) { radar::setMap(nullptr); return; }
    // CARTO dark_all: Hintergrund ~#0e0e0e, Straßen grau, Beschriftung hell. Den Grund schwarz
    // lassen, den Rest strecken und in der Designfarbe einfärben ("dunkel" / "hell").
    const int gain = (mode >= 2) ? 230 : 130;      // /256 nach der Streckung
    const int tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
    uint16_t lut[256];
    for (int l = 0; l < 256; ++l) {
        int v = (l - 16) * 2;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        v = v * gain / 256;
        const int r = tr * v / 255, g = tg * v / 255, b = tb * v / 255;
        lut[l] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    for (int i = 0; i < NPIX; ++i) s_rgb[i] = lut[s_front[i]];
    lv_img_cache_invalidate_src(&s_dsc);
    radar::setMap(&s_dsc);
}
