#include "wx_radar.h"
#include <mutex>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

static std::mutex s_mutex;
static uint16_t *s_front = nullptr, *s_back = nullptr;
static uint32_t s_frameTime = 0, s_version = 0;
static double s_lat = 0, s_lon = 0;
static char s_pendingSrc[12] = "RAINVIEWER", s_src[12] = "RAINVIEWER";
static float s_pendingKm = 50.0f, s_ringKm = 50.0f;

void wx_radar_set_ring_km(float km) { s_pendingKm = km; }

float wx_radar_ring_km(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_ringKm;
}

void wx_radar_set_source(const char *source) {
    strncpy(s_pendingSrc, source ? source : "", sizeof(s_pendingSrc) - 1);
    s_pendingSrc[sizeof(s_pendingSrc) - 1] = 0;
}

void wx_radar_source(char *buf, unsigned len) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!len) return;
    strncpy(buf, s_src, len - 1);
    buf[len - 1] = 0;
}

static uint16_t *alloc_pixels(void) {
    const size_t bytes = WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
#ifdef ARDUINO
    return (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return (uint16_t *)malloc(bytes);
#endif
}

void wx_radar_begin(void) {
    if (s_front) return;
    s_front = alloc_pixels();
    s_back = alloc_pixels();
    const size_t bytes = WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
    if (s_front) memset(s_front, 0, bytes);
    if (s_back) memset(s_back, 0, bytes);
}

uint16_t *wx_radar_back_buffer(void) { return s_back; }

void wx_radar_commit(uint32_t frameTime, double lat, double lon) {
    if (!s_front || !s_back) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    uint16_t *old = s_front; s_front = s_back; s_back = old;
    s_frameTime = frameTime; s_lat = lat; s_lon = lon; ++s_version;
    memcpy(s_src, s_pendingSrc, sizeof(s_src));
    s_ringKm = s_pendingKm;
}

bool wx_radar_front(const uint16_t **pixels, uint32_t *frameTime,
                    double *lat, double *lon, uint32_t *version) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_front || s_version == 0) return false;
    if (pixels) *pixels = s_front;
    if (frameTime) *frameTime = s_frameTime;
    if (lat) *lat = s_lat;
    if (lon) *lon = s_lon;
    if (version) *version = s_version;
    return true;
}
