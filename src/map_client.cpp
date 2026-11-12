// Kartenkacheln für map_bg (siehe map_bg.h):
//  - OpenStreetMap-Standardkarte, tile.openstreetmap.org -- ohne Schlüssel. Nutzungsregeln
//    (operations.osmfoundation.org/policies/tiles): eindeutiger User-Agent (ADSB_USER_AGENT
//    nennt Gerät und Projekt), geringe Last. Darum: höchstens ~9 Kacheln je Ansicht, eine
//    pro Durchgang, und ein Kachel-Zwischenspeicher, damit Hin- und Herzoomen nichts neu lädt.
//  - CARTO "dark_all" mit einem kostenlosen Schlüssel (carto.com/basemaps/apikey); ohne
//    Schlüssel liefert CARTO seit August 2026 nur noch Kacheln mit "API KEY REQUIRED".
// Karten (c) OpenStreetMap-Mitwirkende; CARTO-Stil (c) CARTO.
#include "map_client.h"
#include "map_bg.h"
#include "net_fetch.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>

static const size_t TILE_BYTES = (size_t)MAP_TILE_PX * MAP_TILE_PX;
static PNG     *s_png = nullptr;
static uint8_t *s_lum = nullptr;        // eine dekodierte Kachel als Helligkeit (PSRAM)
static int      s_idx = 0, s_count = 0;
static bool     s_pending = false;      // s_lum hält Kachel s_idx, wartet aufs Einsetzen
static int      s_fails = 0;
static uint32_t s_nextMs = 0;
static const uint32_t SETTLE_MS = 1200; // nach einer Änderung kurz warten (Zoom-Knopf mehrfach)
static char     s_key[80] = "";

// Kachel-Zwischenspeicher (PSRAM): die letzten CACHE_N Kacheln als Helligkeit
static const int CACHE_N = 16;          // 16 x 64 KB = 1 MB PSRAM (the larger weather images need some too)
struct CacheEntry { int src, z, x, y; uint32_t used; uint8_t *lum; };
static CacheEntry s_cache[CACHE_N];
static uint32_t   s_useCtr = 0;

void map_client_set_carto_key(const char *key) {
    snprintf(s_key, sizeof(s_key), "%s", key ? key : "");
}

static CacheEntry *cache_find(int src, const MapTile &t) {
    for (CacheEntry &c : s_cache)
        if (c.lum && c.src == src && c.z == t.z && c.x == t.x && c.y == t.y) { c.used = ++s_useCtr; return &c; }
    return nullptr;
}

static void cache_put(int src, const MapTile &t, const uint8_t *lum) {
    CacheEntry *slot = nullptr;
    for (CacheEntry &c : s_cache) {              // an empty slot, else the least recently used
        if (!c.lum) { c.lum = (uint8_t *)heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM); if (c.lum) { slot = &c; break; } continue; }
        if (!slot || c.used < slot->used) slot = &c;
    }
    if (!slot || !slot->lum) return;
    memcpy(slot->lum, lum, TILE_BYTES);
    slot->src = src; slot->z = t.z; slot->x = t.x; slot->y = t.y; slot->used = ++s_useCtr;
}

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
    if (!s_lum) s_lum = (uint8_t *)heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_png) {
        void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (mem) s_png = new (mem) PNG();
    }
    return s_lum && s_png && map_bg_begin();
}

static bool fetch_tile(int src, const MapTile &t) {
    char url[200];
    if (src == MAP_SRC_CARTO)
        snprintf(url, sizeof(url), "https://basemaps.cartocdn.com/rastertiles/dark_all/%d/%d/%d.png?key=%s",
                 t.z, t.x, t.y, s_key);
    else
        snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", t.z, t.x, t.y);
    uint8_t *img = nullptr; size_t len = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &img, &len, 200000, 3500, 9000)) {
        Serial.printf("[map] tile %d/%d/%d fetch failed (%s)\n", t.z, t.x, t.y, src == MAP_SRC_CARTO ? "CARTO" : "OSM");
        return false;
    }
    memset(s_lum, src == MAP_SRC_OSM ? 240 : 0, TILE_BYTES);   // blank = the source's ground colour
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
        Serial.printf("[map] new view: %d tiles (%s)\n", n, map_bg_source() == MAP_SRC_CARTO ? "CARTO" : "OSM");
        return;
    }
    if (s_idx >= s_count || (int32_t)(now - s_nextMs) < 0) return;
    const int src = map_bg_source();
    if (!s_pending) {
        MapTile t;
        if (!map_bg_tile(s_idx, &t)) { s_idx = s_count; return; }
        if (const CacheEntry *c = cache_find(src, t)) {
            memcpy(s_lum, c->lum, TILE_BYTES);       // already here: no download
        } else {
            if (!fetch_tile(src, t)) {
                ++s_fails;
                s_nextMs = millis() + (s_fails < 3 ? 5000UL : 60000UL);   // no network/server: back off
                return;
            }
            cache_put(src, t, s_lum);
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
