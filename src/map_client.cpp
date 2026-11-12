// Kartenkacheln: CARTO "dark_all" (OpenStreetMap-Daten, (c) OpenStreetMap-Mitwirkende,
// (c) CARTO; frei für nicht-kommerzielle Nutzung in diesem Umfang). Siehe map_bg.h.
#include "map_client.h"
#include "map_bg.h"
#include "net_fetch.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>

static PNG     *s_png = nullptr;
static uint8_t *s_lum = nullptr;        // eine dekodierte Kachel als Helligkeit (512 x 512, PSRAM)
static int      s_idx = 0, s_count = 0;
static bool     s_pending = false;      // s_lum hält Kachel s_idx, wartet aufs Einsetzen
static int      s_fails = 0;
static uint32_t s_nextMs = 0;
static const uint32_t SETTLE_MS = 1200; // nach einer Änderung kurz warten (Zoom-Knopf mehrfach)

static int png_line(PNGDRAW *d) {
    if (d->y < 0 || d->y >= MAP_TILE_PX) return 1;
    uint16_t line[MAP_TILE_PX];
    s_png->getLineAsRGB565(d, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    uint8_t *dst = s_lum + (size_t)d->y * MAP_TILE_PX;
    const int w = d->iWidth < MAP_TILE_PX ? d->iWidth : MAP_TILE_PX;
    for (int x = 0; x < w; ++x) {
        const uint16_t p = line[x];
        const int r = ((p >> 11) & 31) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
        dst[x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
    return 1;
}

static bool ensure(void) {
    if (!s_lum) s_lum = (uint8_t *)heap_caps_malloc((size_t)MAP_TILE_PX * MAP_TILE_PX, MALLOC_CAP_SPIRAM);
    if (!s_png) {
        void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (mem) s_png = new (mem) PNG();
    }
    return s_lum && s_png && map_bg_begin();
}

static bool fetch_tile(const MapTile &t) {
    char url[112];
    snprintf(url, sizeof(url), "https://%c.basemaps.cartocdn.com/dark_all/%d/%d/%d@2x.png",
             "abcd"[(t.x + t.y) & 3], t.z, t.x, t.y);
    uint8_t *img = nullptr; size_t len = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &img, &len, 400000, 3500, 9000)) {
        Serial.printf("[map] tile %d/%d/%d fetch failed\n", t.z, t.x, t.y);
        return false;
    }
    memset(s_lum, 0, (size_t)MAP_TILE_PX * MAP_TILE_PX);
    bool ok = s_png->openRAM(img, len, png_line) == PNG_SUCCESS;
    if (ok) ok = s_png->decode(nullptr, 0) == PNG_SUCCESS;
    s_png->close();
    heap_caps_free(img);
    if (!ok) Serial.printf("[map] tile %d/%d/%d decode failed\n", t.z, t.x, t.y);
    return ok;
}

void map_client_service(bool enabled) {
    if (!enabled || WiFi.status() != WL_CONNECTED || !ensure()) return;
    const uint32_t now = millis();
    if (map_bg_view_changed()) {
        const int n = map_bg_replan();
        if (map_bg_view_changed()) return;            // core 1 still copying: plan next time
        s_count = n; s_idx = 0; s_pending = false; s_fails = 0;
        s_nextMs = now + SETTLE_MS;
        Serial.printf("[map] new view: %d tiles\n", n);
        return;
    }
    if (s_idx >= s_count || (int32_t)(now - s_nextMs) < 0) return;
    if (!s_pending) {
        MapTile t;
        if (!map_bg_tile(s_idx, &t)) { s_idx = s_count; return; }
        if (!fetch_tile(t)) {
            ++s_fails;
            s_nextMs = millis() + (s_fails < 3 ? 5000UL : 60000UL);   // no network/server: back off
            return;
        }
        s_fails = 0;
        s_pending = true;
        if (map_bg_view_changed()) { s_pending = false; return; }   // zoomed meanwhile: drop it
    }
    if (map_bg_compose_tile(s_idx, s_lum)) {          // false: core 1 hasn't taken the last one
        s_pending = false;
        ++s_idx;
    }
}
