#include "wx_radar_client.h"
#include "wx_radar.h"
#include "net_fetch.h"
#include "config.h"
#include "dwd_client.h"
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>

static PNG *s_png = nullptr;
static uint32_t s_decodedPixels = 0;
static uint32_t s_sourcePixels = 0;
static int s_minX = WX_RADAR_SOURCE_SIZE, s_minY = WX_RADAR_SOURCE_SIZE;
static int s_maxX = -1, s_maxY = -1;
// DWD radar PNGs (palette + transparency): outside the radar network the image is a
// translucent grey veil with a magenta border -- both are dropped, and if the centre (the
// user's spot) is under the veil the DWD has no coverage there and RainViewer is used instead.
static bool s_dwdMode = false;
static bool s_centerNoData = false;

static bool ensure_decoder(void) {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) { Serial.println("[wxradar] PSRAM decoder allocation failed"); return false; }
    s_png = new (mem) PNG();
    Serial.printf("[wxradar] PNG decoder in PSRAM (%u bytes)\n", (unsigned)sizeof(PNG));
    return true;
}

static int radar_png_line(PNGDRAW *draw) {
    uint16_t *dst = wx_radar_back_buffer();
    const int crop = (WX_RADAR_SOURCE_SIZE - WX_RADAR_SIZE) / 2;
    if (!dst) return 1;
    uint16_t line[WX_RADAR_SOURCE_SIZE];
    if (s_dwdMode && draw->iPixelType == PNG_PIXEL_INDEXED && draw->iBpp == 8 && draw->pPalette) {
        const uint8_t *pal = draw->pPalette;
        for (int x = 0; x < draw->iWidth && x < WX_RADAR_SOURCE_SIZE; ++x) {
            const uint8_t i = draw->pPixels[x];
            const uint8_t r = pal[i * 3], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
            const uint8_t a = draw->iHasAlpha ? pal[768 + i] : 255;
            const bool veil = a < 160 || (r > 200 && g < 60 && b > 200);   // no-data grey / border
            if (draw->y == WX_RADAR_SOURCE_SIZE / 2 && x == WX_RADAR_SOURCE_SIZE / 2)
                s_centerNoData = a > 20 && a < 160 && abs(r - g) < 12 && abs(g - b) < 12;
            line[x] = veil ? 0 : (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    } else if (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8) {
        const uint8_t *src = draw->pPixels;
        for (int x = 0; x < draw->iWidth; ++x, src += 4) {
            line[x] = (uint16_t)((src[2] >> 3) | ((src[1] >> 2) << 5) |
                                 ((src[0] >> 3) << 11));
        }
    } else {
        s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    }
    for (int x = 0; x < draw->iWidth; ++x) if (line[x]) {
        ++s_sourcePixels;
        if (x < s_minX) s_minX = x;
        if (x > s_maxX) s_maxX = x;
        if (draw->y < s_minY) s_minY = draw->y;
        if (draw->y > s_maxY) s_maxY = draw->y;
    }
    if (draw->y < crop || draw->y >= crop + WX_RADAR_SIZE) return 1;
    const int outY = draw->y - crop;
    const int c = WX_RADAR_SIZE / 2;
    const int dy = outY - c;
    for (int outX = 0; outX < WX_RADAR_SIZE; ++outX) {
        const int dx = outX - c;
        const uint16_t pixel =
            (dx * dx + dy * dy <= (c - 2) * (c - 2)) ? line[outX + crop] : 0;
        dst[outY * WX_RADAR_SIZE + outX] = pixel;
        if (pixel) ++s_decodedPixels;
    }
    return 1;
}

static bool https_get_string(const char *url, String &body, int timeoutMs) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(TLS_HANDSHAKE_S);  // core default is 120 s; see TLS_HANDSHAKE_S
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3500);
    http.setTimeout(timeoutMs);
    if (!http.begin(client, url)) return false;
    // MUST be setUserAgent(): addHeader() silently drops User-Agent (it is on
    // HTTPClient's "handled by code" list), leaving the default "ESP32HTTPClient".
    http.setUserAgent(ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        char tls[128] = "";
        const int tlsCode = client.lastError(tls, sizeof(tls));
        Serial.printf("[wxradar] HTTP %d tls=%d '%s' heap=%u largest=%u psram=%u\n",
                      status, tlsCode, tls, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        http.end(); return false;
    }
    body = http.getString();
    http.end();
    return body.length() > 0;
}

static bool decode_and_commit(uint8_t *image, size_t imageLen, uint32_t frameTime, double lat, double lon);

// Deutscher Wetterdienst: dwd:Niederschlagsradar (RV-Produkt, 1 km, alle 5 Minuten) als WMS-Bild
// in genau der Geometrie der RainViewer-Kachel (Web-Mercator, Zoom 7 bei 512 px), damit Ringe
// und km-Angaben gleich bleiben.
static bool dwd_radar_fetch(double lat, double lon) {
    const time_t now = time(nullptr);
    if (now < 1700000000L) return false;
    const double R = 6378137.0, res = 2.0 * M_PI * R / 65536.0, half = 256.0 * res;
    const double x = R * lon * M_PI / 180.0;
    const double y = R * log(tan(M_PI / 4.0 + lat * M_PI / 360.0));
    for (int back = 1; back <= 3; ++back) {           // the newest 5-minute frame that exists
        const time_t ft = (now / 300) * 300 - back * 300;
        struct tm g;
        gmtime_r(&ft, &g);
        char url[420];
        snprintf(url, sizeof(url),
                 "https://maps.dwd.de/geoserver/dwd/wms?service=WMS&version=1.1.1&request=GetMap"
                 "&layers=dwd:Niederschlagsradar&styles=&srs=EPSG:3857&bbox=%.1f,%.1f,%.1f,%.1f"
                 "&width=%d&height=%d&format=image/png&transparent=true"
                 "&time=%04d-%02d-%02dT%02d:%02d:00.000Z",
                 x - half, y - half, x + half, y + half, WX_RADAR_SOURCE_SIZE, WX_RADAR_SOURCE_SIZE,
                 g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min);
        uint8_t *image = nullptr; size_t imageLen = 0;
        if (!net_fetch_psram(url, ADSB_USER_AGENT, &image, &imageLen, 260000, 3500, 8500)) return false;
        if (imageLen < 8 || image[0] != 0x89 || image[1] != 'P') {   // service exception (XML): frame not there yet
            heap_caps_free(image);
            continue;
        }
        s_dwdMode = true;
        s_centerNoData = false;
        const bool ok = decode_and_commit(image, imageLen, (uint32_t)ft, lat, lon);
        s_dwdMode = false;
        return ok;
    }
    return false;
}

bool wx_radar_fetch(double lat, double lon) {
    if (WiFi.status() != WL_CONNECTED || !wx_radar_back_buffer() || !ensure_decoder()) return false;
    if (dwd_in_germany(lat, lon)) {
        if (dwd_radar_fetch(lat, lon)) return true;
        Serial.println("[wxradar] DWD radar unavailable here -> RainViewer");
    }

    String meta;
    if (!https_get_string("https://api.rainviewer.com/public/weather-maps.json", meta, 6500)) {
        Serial.println("[wxradar] metadata fetch failed"); return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, meta)) { Serial.println("[wxradar] metadata JSON failed"); return false; }
    const char *host = doc["host"] | "";
    JsonArrayConst past = doc["radar"]["past"].as<JsonArrayConst>();
    if (!host[0] || past.size() == 0) { Serial.println("[wxradar] no radar frames"); return false; }
    JsonObjectConst latest = past[past.size() - 1].as<JsonObjectConst>();
    const char *path = latest["path"] | "";
    const uint32_t frameTime = latest["time"] | 0;

    char url[320];
    snprintf(url, sizeof(url), "%s%s/512/7/%.5f/%.5f/2/1_1.png",
             host, path, lat, lon);
    // Stream the tile straight into PSRAM (net_fetch): a 512px PNG can be 100+ KB, and an
    // internal-heap String that big starves the live feed's TLS handshake.
    uint8_t *image = nullptr; size_t imageLen = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &image, &imageLen, 260000, 3500, 8500)) {
        Serial.println("[wxradar] tile fetch failed"); return false;
    }
    return decode_and_commit(image, imageLen, frameTime, lat, lon);
}

// Decode a 512 px PNG (freed here) into the back buffer and publish it.
static bool decode_and_commit(uint8_t *image, size_t imageLen, uint32_t frameTime, double lat, double lon) {
    memset(wx_radar_back_buffer(), 0, WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
    s_decodedPixels = 0;
    s_sourcePixels = 0;
    s_minX = s_minY = WX_RADAR_SOURCE_SIZE;
    s_maxX = s_maxY = -1;
    const int opened = s_png->openRAM(image, imageLen, radar_png_line);
    if (opened != PNG_SUCCESS) {
        Serial.printf("[wxradar] PNG open error %d\n", opened);
        heap_caps_free(image); return false;
    }
    Serial.printf("[wxradar] PNG %dx%d bpp=%d type=%d alpha=%d\n",
                  s_png->getWidth(), s_png->getHeight(), s_png->getBpp(),
                  s_png->getPixelType(), s_png->hasAlpha());
    if (s_png->getWidth() != WX_RADAR_SOURCE_SIZE || s_png->getHeight() != WX_RADAR_SOURCE_SIZE) {
        Serial.println("[wxradar] unexpected tile dimensions");
        s_png->close(); heap_caps_free(image); return false;
    }
    const int decoded = s_png->decode(nullptr, 0);
    s_png->close();
    heap_caps_free(image);           // PNG fully decoded (or failed) — buffer no longer needed
    if (decoded != PNG_SUCCESS) { Serial.printf("[wxradar] PNG decode error %d\n", decoded); return false; }
    if (s_decodedPixels == 0) {
        Serial.println("[wxradar] decoded tile is empty (0 coloured pixels)");
    }
    if (s_dwdMode && s_centerNoData) {                // no DWD coverage at this spot
        Serial.println("[wxradar] DWD: no radar coverage at the centre");
        return false;
    }
    wx_radar_set_source(s_dwdMode ? "DWD" : "RAINVIEWER");
    wx_radar_commit(frameTime, lat, lon);
    Serial.printf("[wxradar] frame %lu updated (%u bytes, source=%lu bbox=%d,%d-%d,%d crop=%lu)\n",
                  (unsigned long)frameTime, (unsigned)imageLen,
                  (unsigned long)s_sourcePixels, s_minX, s_minY, s_maxX, s_maxY,
                  (unsigned long)s_decodedPixels);
    return true;
}
