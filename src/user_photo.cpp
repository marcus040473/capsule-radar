// Eigenes Foto: siehe user_photo.h.
#include "user_photo.h"
#include "config.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>

static const char *PHOTO_PATH = "/photo.jpg";
static const char *PHOTO_TMP  = "/photo.tmp";
static const size_t PHOTO_MAX_BYTES = 600 * 1024;   // the browser sends ~50-150 KB; this is a sanity cap

static bool s_fsOk = false;
static volatile bool s_reload = false;               // set on core 1, served on core 0
// Handoff core 0 -> core 1. s_ready is written by core 0 only while s_readyFlag is false and
// read by core 1 only while it is true.
static uint16_t *volatile s_ready = nullptr;
static volatile bool s_readyFlag = false;
static uint16_t *s_shown = nullptr;                  // core 1: the buffer the UI currently shows

// --------------------------------------------------------------- decoding (core 0)
static uint16_t *s_dst = nullptr;
static bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= SCREEN_H) continue;
        for (int i = 0; i < w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= SCREEN_W) continue;
            s_dst[yy * SCREEN_W + xx] = bmp[j * w + i];
        }
    }
    return true;
}

static uint16_t *decode_file(void) {
    if (!s_fsOk || !LittleFS.exists(PHOTO_PATH)) return nullptr;
    File f = LittleFS.open(PHOTO_PATH, "r");
    if (!f) return nullptr;
    const size_t len = f.size();
    if (len == 0 || len > PHOTO_MAX_BYTES) { f.close(); return nullptr; }
    uint8_t *jpg = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!jpg) { f.close(); return nullptr; }
    const size_t got = f.read(jpg, len);
    f.close();
    uint16_t *px = nullptr;
    uint16_t jw = 0, jh = 0;
    if (got == len && TJpgDec.getJpgSize(&jw, &jh, jpg, len) == JDR_OK && jw && jh) {
        px = (uint16_t *)heap_caps_calloc((size_t)SCREEN_W * SCREEN_H, 2, MALLOC_CAP_SPIRAM);
        if (px) {
            uint8_t scale = 1;   // the browser already sized it; shrink only an oversized file
            while ((jw / scale) > SCREEN_W * 2 || (jh / scale) > SCREEN_H * 2) { scale <<= 1; if (scale >= 8) break; }
            // centre the image on the screen (a smaller one gets a black border)
            const int ox = ((int)SCREEN_W - (int)(jw / scale)) / 2;
            const int oy = ((int)SCREEN_H - (int)(jh / scale)) / 2;
            s_dst = px;
            TJpgDec.setJpgScale(scale);
            TJpgDec.setSwapBytes(false);
            TJpgDec.setCallback(jpg_out);
            if (TJpgDec.drawJpg(ox, oy, jpg, len) != JDR_OK) { heap_caps_free(px); px = nullptr; }
            s_dst = nullptr;
        }
    }
    heap_caps_free(jpg);
    Serial.printf("[userphoto] %s (%ux%u)\n", px ? "decoded" : "decode failed", (unsigned)jw, (unsigned)jh);
    return px;
}

void user_photo_service(void) {
    if (!s_reload || s_readyFlag) return;    // nothing asked, or core 1 hasn't taken the last one
    s_reload = false;
    s_ready = decode_file();                  // nullptr = no photo (deleted or unreadable)
    s_readyFlag = true;
}

// ------------------------------------------------------------------ core 1
bool user_photo_take(const uint16_t **px) {
    if (!s_readyFlag) return false;
    uint16_t *old = s_shown;
    s_shown = s_ready;
    s_ready = nullptr;
    s_readyFlag = false;
    *px = s_shown;
    // The caller points the UI at the new buffer right after this returns; the old one is
    // no longer drawn after that, but free it only after LVGL had a chance to switch.
    if (old) {
        static uint16_t *s_free = nullptr;
        if (s_free) heap_caps_free(s_free);
        s_free = old;   // freed on the next change (one image of slack, ~0.4 MB of PSRAM)
    }
    return true;
}

bool user_photo_begin(void) {
    s_fsOk = LittleFS.begin(true);            // formats the "spiffs" partition on first use
    Serial.printf("[userphoto] filesystem %s\n", s_fsOk ? "ok" : "FAILED");
    s_reload = true;
    return s_fsOk;
}

static File s_up;
static size_t s_upLen = 0;
static bool s_upOk = false;

void user_photo_upload_chunk(int status, const uint8_t *buf, size_t len) {
    // status: 0 = start, 1 = write, 2 = end, 3 = aborted (Arduino's HTTPUploadStatus order)
    if (!s_fsOk) { s_upOk = false; return; }
    if (status == 0) {
        if (s_up) s_up.close();
        s_up = LittleFS.open(PHOTO_TMP, "w");
        s_upLen = 0;
        s_upOk = (bool)s_up;
    } else if (status == 1) {
        if (!s_upOk || !s_up) return;
        s_upLen += len;
        if (s_upLen > PHOTO_MAX_BYTES || s_up.write(buf, len) != len) s_upOk = false;
    } else if (status == 2) {
        if (s_up) s_up.close();
        if (s_upOk && s_upLen > 0) {
            LittleFS.remove(PHOTO_PATH);
            s_upOk = LittleFS.rename(PHOTO_TMP, PHOTO_PATH);
            if (s_upOk) s_reload = true;
        } else {
            s_upOk = false;
            LittleFS.remove(PHOTO_TMP);
        }
    } else {
        if (s_up) s_up.close();
        LittleFS.remove(PHOTO_TMP);
        s_upOk = false;
    }
}

bool user_photo_upload_ok(void) { return s_upOk; }

void user_photo_delete(void) {
    if (!s_fsOk) return;
    LittleFS.remove(PHOTO_PATH);
    s_reload = true;
}

bool user_photo_exists(void) { return s_fsOk && LittleFS.exists(PHOTO_PATH); }

const char *user_photo_path(void) { return PHOTO_PATH; }
