// M3 UI: tileview (radar / list / stats) + tap-to-inspect detail card.
// Pure LVGL, portable. Taps hit-test via radar::hitTest; selection lives in radar.
#include "ui.h"
#include "radar_view.h"
#include "route.h"
#include "photo.h"
#include "weather.h"
#include "wx_radar.h"
#include "cloud_image.h"
#include "airports.h"
#include "config.h"
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <vector>

#define UI_GREEN lv_color_hex(0x1DFF86)
#define UI_INK   lv_color_hex(0xEAFFF3)
#define UI_SOFT  lv_color_hex(0x9AFFC8)
#define UI_DIM   lv_color_hex(0x5F7A6C)
#define UI_PANEL lv_color_hex(0x0C160F)
#define UI_EMERG lv_color_hex(0xFF5A3C)

static lv_obj_t *s_tv = nullptr;
static lv_obj_t *s_tileRadar = nullptr, *s_tileList = nullptr, *s_tileStats = nullptr, *s_tileWeather = nullptr;
static lv_obj_t *s_tileSettings = nullptr;   // links vom Radar (Spalte 1); das Radar sitzt in Spalte 2
static lv_obj_t *s_tilePhoto = nullptr;      // Spalte 0, ganz links: die Foto-Seite (siehe "photo")
static lv_obj_t *s_card = nullptr, *s_cardTitle = nullptr, *s_cardL = nullptr, *s_cardR = nullptr;
static lv_obj_t *s_cardRoute = nullptr;
static lv_obj_t *s_photo = nullptr, *s_photoCredit = nullptr;   // aircraft photo above the card
static char s_lastRouteReq[12] = "";
static lv_obj_t *s_hudWifi = nullptr, *s_hudCount = nullptr, *s_hudClock = nullptr, *s_hudBatt = nullptr, *s_hudDate = nullptr;
static lv_obj_t *s_hudBars[4] = { nullptr, nullptr, nullptr, nullptr };   // WiFi signal-strength bars
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_statsLbl = nullptr;
static lv_obj_t *s_statsNet = nullptr;
static lv_obj_t *s_hudGps   = nullptr;   // HUD satellite icon (hidden unless GPS auto-location is on)
static lv_obj_t *s_statsGps = nullptr;   // Stats view GPS status line
static lv_obj_t *s_weatherNow = nullptr, *s_weatherMeta = nullptr, *s_weatherDays = nullptr;
static lv_obj_t *s_wxCanvas = nullptr, *s_wxStatus = nullptr, *s_wxAirport = nullptr;
static lv_obj_t *s_wxFooter = nullptr, *s_wxMeta = nullptr, *s_wxAttrib = nullptr;
static lv_obj_t *s_wxRings[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_wxNorth = nullptr, *s_wxCenter = nullptr, *s_wxRange = nullptr;
static lv_obj_t *s_weatherModeBtn = nullptr, *s_weatherModeLbl = nullptr;
static lv_obj_t *s_weatherTitle = nullptr;
enum WeatherViewMode { WEATHER_RADAR, WEATHER_CLOUDS, WEATHER_FORECAST };
static WeatherViewMode s_weatherMode = WEATHER_RADAR;
static lv_obj_t *s_fcCurrent = nullptr, *s_fcCondition = nullptr, *s_fcUpdated = nullptr;
static lv_obj_t *s_fcMetricName[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcMetricValue[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDay[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayCondition[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayTemp[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayRain[3] = { nullptr, nullptr, nullptr };

// --------------------------------------------------------------------- units
// 0 = Aviation (ft, kt, km) · 1 = Metric (m, km/h, km) · 2 = Imperial (ft, mph, mi).
// The feed gives altitude in ft, speed in kt, vertical speed in fpm, distance in km.
static int s_units = 0;
void ui_set_units(int u) {
    s_units = (u < 0 || u > 2) ? 0 : u;
    radar::setUnits(s_units);   // scope altitude labels (ft/m) follow the same preset
}

// Accessibility: "large text" swaps every font one-or-two steps up. The flag must be
// set BEFORE ui_create() — fonts are baked into the widgets at creation time (the web
// toggle saves to NVS and reboots, so it always takes effect through this path).
static bool s_bigText = false;
void ui_set_large_text(bool on) { s_bigText = on; }
static const lv_font_t *F12() { return s_bigText ? &lv_font_montserrat_16 : &lv_font_montserrat_12; }
static const lv_font_t *F14() { return s_bigText ? &lv_font_montserrat_18 : &lv_font_montserrat_14; }
static const lv_font_t *F16() { return s_bigText ? &lv_font_montserrat_20 : &lv_font_montserrat_16; }

static void fmt_alt(char *b, size_t n, float ft, bool gnd) {
    if (gnd)            snprintf(b, n, "Boden");
    else if (s_units == 1) snprintf(b, n, "%.0f m",  ft * 0.3048f);
    else                snprintf(b, n, "%.0f ft", ft);
}
static void fmt_spd(char *b, size_t n, float kt) {
    if (kt != kt)          snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%.0f km/h", kt * 1.852f);
    else if (s_units == 2) snprintf(b, n, "%.0f mph",  kt * 1.15078f);
    else                   snprintf(b, n, "%.0f kt",   kt);
}
static void fmt_vs(char *b, size_t n, float fpm) {
    if (fpm != fpm)        snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%+.1f m/s", fpm * 0.00508f);
    else                   snprintf(b, n, "%+.0f fpm", fpm);
}
static float dist_val(float km) {
    if (s_units == 0) return km * 0.539957f;   // Aviation -> nautical miles
    if (s_units == 2) return km * 0.621371f;   // Imperial -> miles
    return km;                                   // Metric   -> km
}
static const char *dist_unit(void) { return s_units == 0 ? "nm" : (s_units == 2 ? "mi" : "km"); }

// Weather temperature: 0 = follow the Units preset (°F with Imperial, °C otherwise),
// 1 = always °C, 2 = always °F. Wind stays with the Units preset.
static int s_tempUnit = 0;
void ui_set_temp_unit(int t) { s_tempUnit = (t < 0 || t > 2) ? 0 : t; }
static bool temp_in_f(void) { return s_tempUnit == 2 || (s_tempUnit == 0 && s_units == 2); }
static float weather_temp(float c) { return temp_in_f() ? c * 1.8f + 32.0f : c; }
// "\xC2\xB0" is the UTF-8 degree sign; LVGL's built-in Montserrat fonts carry it (U+00B0).
static const char *weather_temp_unit(void) { return temp_in_f() ? "\xC2\xB0" "F" : "\xC2\xB0" "C"; }
static float weather_wind(float kmh) {
    if (s_units == 0) return kmh * 0.539957f;
    if (s_units == 2) return kmh * 0.621371f;
    return kmh;
}
static const char *weather_wind_unit(void) { return s_units == 0 ? "kt" : (s_units == 2 ? "mph" : "km/h"); }
static const char *cardinal(float deg) {
    static const char *p[] = {"N", "NO", "O", "SO", "S", "SW", "W", "NW"};
    int i = ((int)(deg + 22.5f) / 45) & 7;
    return p[i];
}

// Fold Latin-1 accents / drop any other non-ASCII so the Montserrat font never hits a
// missing glyph (which renders as an empty box). Belt-and-suspenders for card text.
static void fold_ascii(char *s) {
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        if (*p < 0x80) { *o++ = (char)*p++; continue; }
        if (*p == 0xC3 && p[1]) {                       // Latin-1 Supplement (U+00C0..U+00FF)
            const unsigned char d = p[1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else                             r = '?';
            *o++ = r; p += 2; continue;
        }
        ++p;                                            // skip other multibyte lead + continuation
        while (*p >= 0x80 && *p < 0xC0) ++p;
    }
    *o = 0;
}

// ----------------------------------------------------------------- detail card
// Idle clock up (radar_view scene step >= 1): the HUD, zoom button and card make way for it.
static bool s_clockUp = false;

static void refresh_card(void) {
    AcInfo in;
    const bool sel = radar::selected(in);
    if (s_clockUp || !sel) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (s_photo)       lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        if (!sel) s_lastRouteReq[0] = 0;
        return;
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    char title[40];
    if (in.type[0]) snprintf(title, sizeof(title), "%s  %s", in.call[0] ? in.call : "-", in.type);
    else            snprintf(title, sizeof(title), "%s", in.call[0] ? in.call : "-");
    fold_ascii(title);
    lv_label_set_text(s_cardTitle, title);
    lv_obj_set_style_text_color(s_cardTitle, in.emergency ? UI_EMERG : UI_INK, 0);

    char altS[16], vsS[24], spdS[16], sqS[16], hdgS[8];
    fmt_alt(altS, sizeof(altS), in.altFt, in.onGround);
    fmt_vs (vsS,  sizeof(vsS),  in.vsFpm);
    fmt_spd(spdS, sizeof(spdS), in.gsKt);
    if (in.squawk < 0)          snprintf(sqS, sizeof(sqS), "-");
    else                        snprintf(sqS, sizeof(sqS), "%04d", in.squawk);
    // HDG is the aircraft's own ground track (what rotates the glyph), not the bearing
    // from home — NaN (unknown) prints as "-".
    if (in.track != in.track)   snprintf(hdgS, sizeof(hdgS), "-");
    else                        snprintf(hdgS, sizeof(hdgS), "%03.0f", in.track);

    char left[96], right[96];
    snprintf(left,  sizeof(left),  "HÖHE  %s\nTEMPO %s\nENTF  %.1f %s", altS, spdS, dist_val(in.distKm), dist_unit());
    snprintf(right, sizeof(right), "STEIG %s\nKURS  %s\nSQK   %s", vsS, hdgS, sqS);
    lv_label_set_text(s_cardL, left);
    lv_label_set_text(s_cardR, right);

    // route (origin -> destination), looked up asynchronously by callsign
    if (in.call[0] && strcmp(in.call, s_lastRouteReq) != 0) {
        snprintf(s_lastRouteReq, sizeof(s_lastRouteReq), "%s", in.call);
        route_request(in.call);
    }
    char rfrom[40], rto[40];
    if (!in.call[0]) {
        lv_label_set_text(s_cardRoute, "Route -");                 // no callsign -> nothing to look up
    } else if (route_get(in.call, rfrom, sizeof(rfrom), rto, sizeof(rto))) {
        char rt[96];
        if (rfrom[0] || rto[0]) snprintf(rt, sizeof(rt), "%s -> %s", rfrom[0] ? rfrom : "?", rto[0] ? rto : "?");
        else                    snprintf(rt, sizeof(rt), "Route unbekannt");
        fold_ascii(rt);
        lv_label_set_text(s_cardRoute, rt);
    } else {
        lv_label_set_text(s_cardRoute, "Route wird gesucht...");     // pending: lookup in flight
    }

    // aircraft photo (planespotters), shown above the card when one is available
    if (in.hex[0]) photo_request(in.hex);
    int pw = 0, ph = 0; char pcred[40];
    if (s_photo && in.hex[0] && photo_get(in.hex, &pw, &ph, pcred, sizeof(pcred)) && pw > 0 && ph > 0) {
        int mw, mh;
        lv_color_t *pbuf = photo_buffer(&mw, &mh);
        lv_canvas_set_buffer(s_photo, pbuf, pw, ph, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(s_photo, pw, ph);
        lv_obj_align(s_photo, LV_ALIGN_CENTER, 0, -28 - ph / 2);   // sit lower: fill the band down to the card
        lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_photo);
        if (s_photoCredit) {
            char c[52];
            snprintf(c, sizeof(c), "Foto: %s", pcred[0] ? pcred : "planespotters.net");
            lv_label_set_text(s_photoCredit, c);
            lv_obj_align_to(s_photoCredit, s_photo, LV_ALIGN_OUT_BOTTOM_MID, 0, 1);
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_photo) {
        // No image to show yet: hide the canvas, but use the caption line to tell the
        // user what's happening — "Loading..." while the fetch is in flight, or a quiet
        // "No photo" once it finished without one. Unobtrusive (small, dim) but informative.
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) {
            const bool done = in.hex[0] && photo_done(in.hex);
            lv_label_set_text(s_photoCredit, done ? "Kein Foto verfügbar" : "Foto wird geladen...");
            lv_obj_align(s_photoCredit, LV_ALIGN_CENTER, 0, -104);   // where the photo would sit
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// --------------------------------------------------------------------- input
static bool s_themeHeld = false;   // this press reached the theme hold: its lift is not a tap
static int s_rangeIdx = -1;
static float s_rangeKm = RANGE_KM_DEFAULT;   // current display range (km), for the stats view
static void (*s_rangeCb)(float) = nullptr;
static lv_obj_t *s_zoomBtn = nullptr, *s_zoomLbl = nullptr;
static lv_obj_t *s_offBtn = nullptr, *s_offLbl = nullptr, *s_offRing = nullptr;
static void (*s_poweroffCb)(void) = nullptr;
static uint32_t s_offT0 = 0;
static bool s_offFired = false;
static lv_timer_t *s_offRestoreTimer = nullptr;

// The button exists only on boards that can actually power off: it stays hidden until a
// non-null callback is registered (null hides it again).
void ui_set_poweroff_cb(void (*cb)(void)) {
    s_poweroffCb = cb;
    if (!s_offBtn) return;
    if (cb) lv_obj_clear_flag(s_offBtn, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(s_offBtn, LV_OBJ_FLAG_HIDDEN);
}

// Power off needs a 1.5 s HOLD so a swipe or stray tap can't turn the radar off.
#define POWEROFF_HOLD_MS 1500
// If the radar is still running a few seconds after "Powering off", the shutdown didn't
// happen (e.g. the PMIC refused): go back to the hold prompt so the user isn't stuck.
#define POWEROFF_RESTORE_MS 5000
static void poweroff_restore_cb(lv_timer_t *t) {
    s_offRestoreTimer = nullptr;
    lv_timer_del(t);
    s_offFired = false;
    if (s_offLbl) lv_label_set_text(s_offLbl, LV_SYMBOL_POWER " Halten zum Ausschalten");
    if (s_offRing) lv_arc_set_end_angle(s_offRing, 0);
}

// Hold progress: the finger covers the button label, so a ring round the screen edge fills
// clockwise from 12 o'clock over the hold and unwinds if the finger lifts early.
#define POWEROFF_UNWIND_MS 400   // a full ring unwinds in this long (a partial one, pro rata)
// The ring is never hidden, just set to zero length: showing or hiding a full-screen object
// repaints the whole screen (~200 ms), while an angle change repaints only the changed part.
static void off_ring_set(int32_t deg) {
    if (!s_offRing) return;
    lv_arc_set_end_angle(s_offRing, (uint16_t)(deg <= 0 ? 0 : deg > 360 ? 360 : deg));
}
static void off_ring_anim_cb(void *var, int32_t v) { (void)var; off_ring_set(v); }
static int32_t off_ring_deg() {
    if (!s_offRing) return 0;
    return lv_arc_get_angle_end(s_offRing);
}
static void off_ring_unwind() {
    const int32_t deg = off_ring_deg();
    if (deg <= 0) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_offRing);
    lv_anim_set_exec_cb(&a, off_ring_anim_cb);
    lv_anim_set_values(&a, deg, 0);
    lv_anim_set_time(&a, (uint32_t)(POWEROFF_UNWIND_MS * deg / 360));
    lv_anim_start(&a);
}

static void poweroff_cb(lv_event_t *e) {
    const lv_event_code_t c = lv_event_get_code(e);
    if (!s_offLbl) return;
    if (c == LV_EVENT_PRESSED) {
        if (s_offRestoreTimer) { lv_timer_del(s_offRestoreTimer); s_offRestoreTimer = nullptr; }
        // Pressed again mid-unwind: carry on from where the ring is instead of from empty.
        const int32_t deg = off_ring_deg();
        lv_anim_del(s_offRing, off_ring_anim_cb);
        s_offT0 = lv_tick_get() - (uint32_t)(POWEROFF_HOLD_MS * deg / 360); s_offFired = false;
        lv_label_set_text(s_offLbl, "Weiter halten...");
    } else if (c == LV_EVENT_PRESSING && !s_offFired) {
        const uint32_t held = lv_tick_elaps(s_offT0);
        off_ring_set((int32_t)(360 * held / POWEROFF_HOLD_MS));
        if (held >= POWEROFF_HOLD_MS) {
            s_offFired = true;
            lv_label_set_text(s_offLbl, "Wird ausgeschaltet");
            if (s_poweroffCb) s_poweroffCb();
            s_offRestoreTimer = lv_timer_create(poweroff_restore_cb, POWEROFF_RESTORE_MS, nullptr);
            lv_timer_set_repeat_count(s_offRestoreTimer, 1);
        }
    } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && !s_offFired) {
        lv_label_set_text(s_offLbl, LV_SYMBOL_POWER " Halten zum Ausschalten");
        off_ring_unwind();
    }
}

void ui_set_range_cb(void (*cb)(float)) { s_rangeCb = cb; }

static void zoom_cb(lv_event_t *e) {   // fires on PRESS (robust vs scroll-cancel on the tileview)
    (void)e;
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 250) return;      // debounce repeated/held presses
    last = now;
    if (!s_rangeCb) return;
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    s_rangeIdx = (s_rangeIdx + 1) % n;
    s_rangeCb(RANGE_STEPS_KM[s_rangeIdx]);
}

void ui_set_range_km(float km) {
    s_rangeKm = km;
    if (s_zoomLbl) {
        char b[20];
        snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %.0f %s", dist_val(km), dist_unit());
        lv_label_set_text(s_zoomLbl, b);
    }
    int best = 0; float bd = 1e9f;                 // sync the cycle index to the shown range
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    for (int i = 0; i < n; ++i) { float d = km - RANGE_STEPS_KM[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
    s_rangeIdx = best;
}

// A still hold on the radar cycles the visual theme, on lift. When UI_THEME_HOLD_MS have
// passed, a hint says "Lift to change theme"; moving the finger then cancels. Cycling on lift
// (not when the time passes) keeps a press that turns into a swipe from changing the theme.
// LVGL's own long press (400 ms) is not used: firm taps reached it, so tapping off a plane
// card with a half-second press changed the theme and left the card open.
static bool s_lpArmed = false;      // hold time passed, finger still down and still
static bool s_lpMoved = false;      // finger travelled past the drag threshold since the press
static lv_point_t s_lpOrigin;
static uint32_t s_lpT0 = 0;         // when the press began (LVGL tick)
static lv_obj_t *s_holdHint = nullptr;   // "Lift to change theme", shown while a hold is armed
// Finger travel that counts as a drag. 10 px (LVGL's scroll limit) is under 1 mm on these
// panels, less than a still fingertip drifts: on a 1.43, 3 of 15 holds were cancelled at
// 12-14 px (@h0verin, #36). A real swipe across tiles still cancels through PRESS_LOST.
#define LP_MOVE_PX 25

static void hold_hint_show(bool on) {
    if (!s_holdHint) return;
    if (on) lv_obj_clear_flag(s_holdHint, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_holdHint, LV_OBJ_FLAG_HIDDEN);
}

static void radar_press_cb(lv_event_t *e) {
    s_themeHeld = false;
    s_lpArmed = false;
    s_lpMoved = false;
    s_lpT0 = lv_tick_get();
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) lv_indev_get_point(indev, &s_lpOrigin);
}

static void radar_pressing_cb(lv_event_t *e) {
    if (s_lpMoved) return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const long dx = (long)p.x - s_lpOrigin.x, dy = (long)p.y - s_lpOrigin.y;
    if (dx * dx + dy * dy > (long)LP_MOVE_PX * LP_MOVE_PX) {   // a drag: no theme change
        s_lpMoved = true;
        if (s_lpArmed) { s_lpArmed = false; hold_hint_show(false); }
        return;
    }
    if (!s_lpArmed && lv_tick_elaps(s_lpT0) >= UI_THEME_HOLD_MS) {
        s_lpArmed = true;
        s_themeHeld = true;
        hold_hint_show(true);
    }
}

static void radar_release_cb(lv_event_t *e) {   // still finger lifted after the hold -> next theme
    (void)e;
    if (s_lpArmed && !s_lpMoved) {
        hold_hint_show(false);
        radar::cycleTheme();
    }
    s_lpArmed = false;
}

static void radar_presslost_cb(lv_event_t *e) {   // scroll stole the press: a swipe, not a hold
    (void)e;
    if (s_lpArmed) hold_hint_show(false);
    s_lpArmed = false;
}

// A tap just outside the open card closes it, even on a plane: a finger aimed at the X or the
// card's edge often lands a few px past the card, and that tap picked a plane instead. A second
// tap picks the plane. Only taps that reach the radar tile come here: the zoom button keeps its own.
static bool near_open_card(const lv_point_t &p) {
    if (!s_card || lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(s_card, &a);
    const lv_coord_t m = UI_CARD_CLOSE_MARGIN;
    return p.x >= a.x1 - m && p.x <= a.x2 + m && p.y >= a.y1 - m && p.y <= a.y2 + m;
}

static void radar_clicked_cb(lv_event_t *e) {
    (void)e;
    if (s_themeHeld) { s_themeHeld = false; return; }   // the lift after a theme hold is not a tap
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    radar::select(near_open_card(p) ? -1 : radar::hitTest(p.x, p.y));   // hit -> select; miss or next to the card -> clear
    refresh_card();
}

// ----------------------------------------------------------------- list/stats
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock) {
    // bar count from RSSI (dBm): the weaker the signal, the fewer lit bars
    int level;
    if      (!wifiUp)     level = 0;
    else if (rssi >= -55) level = 4;   // excellent
    else if (rssi >= -67) level = 3;   // good
    else if (rssi >= -75) level = 2;   // ok
    else                  level = 1;   // weak (connected but marginal)
    // colour: red = no WiFi, amber = connected but feed stale (no fresh data), white = healthy
    const lv_color_t col = !wifiUp ? UI_EMERG : (feedOk ? UI_INK : lv_color_hex(0xFFB23C));
    for (int i = 0; i < 4; ++i) {
        if (!s_hudBars[i]) continue;
        lv_obj_set_style_bg_color(s_hudBars[i], col, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], (i < level) ? LV_OPA_COVER : 45, 0);
    }
    if (s_hudClock && clock) lv_label_set_text(s_hudClock, clock);
}

void ui_set_battery(int pct, bool charging, bool present) {
    if (!s_hudBatt) return;
    if (!present || pct < 0) { lv_label_set_text(s_hudBatt, ""); return; }   // USB-only -> hide
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                      pct > 55 ? LV_SYMBOL_BATTERY_3 :
                      pct > 35 ? LV_SYMBOL_BATTERY_2 :
                      pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%s%d", charging ? LV_SYMBOL_CHARGE : "", sym, pct);
    lv_label_set_text(s_hudBatt, buf);
    lv_obj_set_style_text_color(s_hudBatt, (pct <= 15 && !charging) ? UI_EMERG : UI_INK, 0);
}

static lv_obj_t *s_mapAttr = nullptr;   // "Karte (c) OSM, CARTO" under the date while a map is drawn
void ui_set_date(const char *date) {
    if (s_hudDate && date) lv_label_set_text(s_hudDate, date);
    // called every second: also keep the map attribution in step with the map
    if (s_mapAttr && !s_clockUp) {
        if (radar::mapShown()) lv_obj_clear_flag(s_mapAttr, LV_OBJ_FLAG_HIDDEN);
        else                   lv_obj_add_flag(s_mapAttr, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_set_netinfo(const char *line) {
    if (s_statsNet && line) lv_label_set_text(s_statsNet, line);
}

// Which provider served the last aircraft data. Shown on the Stats view so users can tell
// whether they are on airplanes.live (feeder households) or a fallback — until now that
// was only visible on the serial log, and it kept coming up in support threads.
static char s_feedHost[40] = "-";
void ui_set_feed_source(const char *host) {
    if (!host || !host[0] || host[0] == '?') { snprintf(s_feedHost, sizeof(s_feedHost), "-"); return; }
    // strip the machine prefixes so the label reads like the project name users know
    if      (strncmp(host, "api.",      4) == 0) host += 4;
    else if (strncmp(host, "opendata.", 9) == 0) host += 9;
    snprintf(s_feedHost, sizeof(s_feedHost), "%s", host);
}

// GPS indicator. state: 0 = off / no module (hidden), 1 = acquiring (amber), 2 = fix (green).
static bool s_gpsPresent = false;
static int  s_gpsState = 0, s_gpsSats = 0;
static lv_obj_t *s_setGpsLbl = nullptr;   // settings page "GPS-Signal" value (only while it is open)
static void set_gps_label(void);
void ui_set_gps_present(bool present) { s_gpsPresent = present; }

void ui_set_gps(int state, int sats, float altM) {
    s_gpsState = state;
    s_gpsSats = sats;
    set_gps_label();
    if (state <= 0) {                                 // hidden when GPS auto-location is off
        if (s_hudGps)   lv_label_set_text(s_hudGps, "");
        if (s_statsGps) lv_label_set_text(s_statsGps, "");
        return;
    }
    const bool fix = (state >= 2);
    const lv_color_t col = fix ? UI_GREEN : lv_color_hex(0xFFB23C);   // amber while acquiring
    if (s_hudGps) {
        char b[16];
        snprintf(b, sizeof(b), LV_SYMBOL_GPS "%d", sats);
        lv_label_set_text(s_hudGps, b);
        lv_obj_set_style_text_color(s_hudGps, col, 0);
    }
    if (s_statsGps) {
        char s[56];
        if (fix && altM == altM) {                    // fix with a known GPS altitude
            char altS[16];
            if (s_units == 1) snprintf(altS, sizeof(altS), "%.0f m",  altM);
            else              snprintf(altS, sizeof(altS), "%.0f ft", altM * 3.28084f);
            snprintf(s, sizeof(s), LV_SYMBOL_GPS " Fix  " LV_SYMBOL_BULLET "  %d Sat.  " LV_SYMBOL_BULLET "  %s", sats, altS);
        }
        else if (fix) snprintf(s, sizeof(s), LV_SYMBOL_GPS " Fix  " LV_SYMBOL_BULLET "  %d Sat.", sats);
        else     snprintf(s, sizeof(s), LV_SYMBOL_GPS " suche  (%d Sat.)", sats);
        lv_label_set_text(s_statsGps, s);
        lv_obj_set_style_text_color(s_statsGps, col, 0);
    }
}

// Refresh the scrollable contact list. Rows are updated in place and only added/removed
// at the end: recreating every row (the old lv_obj_clean) reset the scroll to the top on
// every poll and deleted the row under a pressing finger, eating taps.
// A row is a plain full-width label and taps are hit-tested on the list itself: a
// button-style row (lv_list_add_btn + per-row events/styles/user_data) costs ~0.7 KB of
// LVGL's 64 KB pool and exhausts it when the list shows the full feed (60 aircraft).
static std::vector<lv_obj_t *> s_listRows;
// Whether each row currently carries s_rowEmgStyle. lv_obj_add_style() does not
// de-duplicate and the per-object style count is a 6-bit field: re-adding the style on
// every poll would overflow it after 64 refreshes and crash the radar. Touch the style
// only when a row's emergency state changes.
static std::vector<bool> s_listRowEmg;
// Shared styles (one copy each, not per-row local styles that cost pool memory per row).
// s_rowStyle reproduces the old lv_list button look: full width, a row every 42 px with
// F16, a 1 px separator underneath in the default theme's grey.
static lv_style_t s_rowStyle, s_rowEmgStyle;
static bool s_rowStylesInit = false;

// Taps land on the list container (rows are not clickable); map the point to a row.
// The aircraft is picked at touch-down, by hex: build_list rewrites rows in place on every
// poll, so mapping the row only at release could open a different plane if a poll landed
// between press and release.
static char s_listPressHex[sizeof(AcInfo::hex)];

static void list_press_cb(lv_event_t *e) {
    (void)e;
    s_listPressHex[0] = '\0';
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t la, a;
    lv_obj_get_coords(s_list, &la);
    if (p.x < la.x1 || p.x > la.x2) return;
    for (size_t i = 0; i < s_listRows.size(); ++i) {
        lv_obj_get_coords(s_listRows[i], &a);
        if (p.y >= a.y1 && p.y <= a.y2 + 2) {     // +2 swallows the 2 px row gap
            AcInfo in;                              // row i shows radar::info(i) (build_list)
            if (radar::info((int)i, in)) snprintf(s_listPressHex, sizeof(s_listPressHex), "%s", in.hex);
            return;
        }
    }
}

static void list_tap_cb(lv_event_t *e) {
    (void)e;
    if (!s_listPressHex[0] || !radar::selectHex(s_listPressHex)) return;   // left the feed mid-press
    refresh_card();
    lv_obj_set_tile_id(s_tv, 2, 0, LV_ANIM_ON);   // jump back to the radar (column 2)
}

static void build_list(void) {
    if (!s_list) return;
    if (!s_rowStylesInit) {
        lv_style_init(&s_rowStyle);
        lv_style_set_width(&s_rowStyle, lv_pct(100));
        lv_style_set_pad_hor(&s_rowStyle, 13);
        lv_style_set_pad_ver(&s_rowStyle, 10);
        lv_style_set_border_side(&s_rowStyle, LV_BORDER_SIDE_BOTTOM);
        lv_style_set_border_width(&s_rowStyle, 1);
        lv_style_set_border_color(&s_rowStyle, lv_color_hex(0xE0E0E0));
        lv_style_init(&s_rowEmgStyle);   // emergency rows only: red text (the rest inherits)
        lv_style_set_text_color(&s_rowEmgStyle, UI_EMERG);
        s_rowStylesInit = true;
    }
    const int n = radar::count();
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        char altS[16], txt[64];
        fmt_alt(altS, sizeof(altS), in.altFt, in.onGround);
        snprintf(txt, sizeof(txt), "%-8.8s  %-8s %4.1f %s",
                 in.call[0] ? in.call : in.hex, altS, dist_val(in.distKm), dist_unit());
        if (i < (int)s_listRows.size()) {          // existing row: just refresh its content
            lv_obj_t *l = s_listRows[i];
            lv_label_set_text(l, txt);
            if (s_listRowEmg[i] != in.emergency) {
                if (in.emergency) lv_obj_add_style(l, &s_rowEmgStyle, 0);
                else              lv_obj_remove_style(l, &s_rowEmgStyle, 0);
                s_listRowEmg[i] = in.emergency;
            }
        } else {                                   // new row: append at the end
            lv_obj_t *l = lv_label_create(s_list);
            lv_obj_add_style(l, &s_rowStyle, 0);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);   // fixed width: a long line ends in '...'
            lv_label_set_text(l, txt);
            if (in.emergency) lv_obj_add_style(l, &s_rowEmgStyle, 0);
            s_listRows.push_back(l);
            s_listRowEmg.push_back(in.emergency);
        }
    }
    // Surplus rows go from the end — safe mid-press: the press lives on the list container,
    // and list_press_cb already took the pressed plane's hex, not a row pointer.
    for (int i = (int)s_listRows.size() - 1; i >= n; --i) {
        lv_obj_del(s_listRows[i]);
        s_listRows.erase(s_listRows.begin() + i);
        s_listRowEmg.erase(s_listRowEmg.begin() + i);
    }
}

static void build_stats(void) {
    if (!s_statsLbl) return;
    const int n = radar::count();
    int emg = 0;
    float nearest = 1e9f, highest = -1e9f;
    char nearestCall[12] = "-";
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        if (in.emergency) emg++;
        if (in.distKm < nearest) { nearest = in.distKm; snprintf(nearestCall, sizeof(nearestCall), "%s", in.call[0] ? in.call : in.hex); }
        if (!in.onGround && in.altFt > highest) highest = in.altFt;
    }
    char altH[16];
    fmt_alt(altH, sizeof(altH), (highest > -1e8f) ? highest : 0.0f, false);
    char st[260];
    snprintf(st, sizeof(st),
             "Flugzeuge    %d\n"
             "Notfälle     %d\n"
             "Nächstes     %s\n"
             "             %.1f %s\n"
             "Höchstes     %s\n"
             "Reichweite   %.0f %s\n"
             "Datenquelle  %s",
             n, emg, n ? nearestCall : "-", dist_val(n ? nearest : 0.0f), dist_unit(),
             altH, dist_val(s_rangeKm), dist_unit(), s_feedHost);
    lv_label_set_text(s_statsLbl, st);
}

static void build_weather(void) {
    if (!s_weatherNow || !s_weatherMeta || !s_weatherDays || !s_wxFooter) return;
    WeatherSnapshot w;
    if (!weather_get(w)) {
        lv_label_set_text(s_weatherNow, "Keine Vorhersage");
        lv_label_set_text(s_weatherMeta, "Warte auf WLAN-Daten...");
        lv_label_set_text(s_wxFooter, "WETTERDATEN AUSSTEHEND");
        lv_label_set_text(s_weatherDays, "");
    } else {
        char now[96];
        snprintf(now, sizeof(now), "%.0f %s\n%s", weather_temp(w.tempC),
                 weather_temp_unit(), weather_condition(w.code));
        lv_label_set_text(s_weatherNow, now);
        char meta[128];
        snprintf(meta, sizeof(meta), "Gefühlt %.0f %s   Feuchte %d%%\nWind %.0f %s  %s   Stand %s",
                 weather_temp(w.feelsC), weather_temp_unit(), w.humidity,
                 weather_wind(w.windKmh), weather_wind_unit(), cardinal((float)w.windDeg), w.updated);
        lv_label_set_text(s_weatherMeta, meta);

        char footer[96];
        snprintf(footer, sizeof(footer), "%.0f %s   %s", weather_temp(w.tempC),
                 weather_temp_unit(), weather_condition(w.code));
        lv_label_set_text(s_wxFooter, footer);
        char wxmeta[96];
        snprintf(wxmeta, sizeof(wxmeta), "WIND %s %.0f %s   FEUCHTE %d%%",
                 cardinal((float)w.windDeg), weather_wind(w.windKmh), weather_wind_unit(), w.humidity);
        lv_label_set_text(s_wxMeta, wxmeta);

        char current[24];
        snprintf(current, sizeof(current), "%.0f %s", weather_temp(w.tempC), weather_temp_unit());
        lv_label_set_text(s_fcCurrent, current);
        lv_label_set_text(s_fcCondition, weather_condition(w.code));
        lv_label_set_text(s_fcMetricValue[0], current);
        char hum[16]; snprintf(hum, sizeof(hum), "%d%%", w.humidity);
        lv_label_set_text(s_fcMetricValue[1], hum);
        char wind[28]; snprintf(wind, sizeof(wind), "%s %.0f %s", cardinal((float)w.windDeg),
                                weather_wind(w.windKmh), weather_wind_unit());
        lv_label_set_text(s_fcMetricValue[2], wind);
        char updated[24]; snprintf(updated, sizeof(updated), "STAND %s", w.updated);
        lv_label_set_text(s_fcUpdated, updated);

        for (int col = 0; col < 3; ++col) {
            const int i = col + 1;
            if (i < w.dayCount) {
                lv_label_set_text(s_fcDay[col], weather_day_name(w.days[i].date));
                lv_label_set_text(s_fcDayCondition[col], weather_condition(w.days[i].code));
                char temps[28];
                snprintf(temps, sizeof(temps), "%.0f / %.0f %s",
                         weather_temp(w.days[i].tempMaxC), weather_temp(w.days[i].tempMinC), weather_temp_unit());
                lv_label_set_text(s_fcDayTemp[col], temps);
                char chance[20]; snprintf(chance, sizeof(chance), "REGEN %d%%", w.days[i].rainChance);
                lv_label_set_text(s_fcDayRain[col], chance);
            } else {
                lv_label_set_text(s_fcDay[col], "-");
                lv_label_set_text(s_fcDayCondition[col], "");
                lv_label_set_text(s_fcDayTemp[col], "");
                lv_label_set_text(s_fcDayRain[col], "");
            }
        }

        char days[320] = "";
        for (int i = 1; i < w.dayCount && i < 4; ++i) {
            char row[104];
            snprintf(row, sizeof(row), "%-3s  %-14s  %2.0f/%2.0f %s  %3d%%\n",
                     weather_day_name(w.days[i].date), weather_condition(w.days[i].code),
                     weather_temp(w.days[i].tempMaxC), weather_temp(w.days[i].tempMinC),
                     weather_temp_unit(), w.days[i].rainChance);
            strncat(days, row, sizeof(days) - strlen(days) - 1);
        }
        lv_label_set_text(s_weatherDays, days);
    }

    const uint16_t *radarPixels = nullptr, *cloudPixels = nullptr;
    uint32_t frameTime = 0, version = 0;
    double rlat = 0, rlon = 0;
    const bool cloudMode = s_weatherMode == WEATHER_CLOUDS;
    const bool forecastMode = s_weatherMode == WEATHER_FORECAST;
    bool haveImage = false;
    if (cloudMode)
        haveImage = cloud_image_front(&cloudPixels, &frameTime, &rlat, &rlon, &version);
    else
        haveImage = wx_radar_front(&radarPixels, &frameTime, &rlat, &rlon, &version);
    const uint16_t *pixels = cloudMode ? cloudPixels : radarPixels;
    if (haveImage && pixels && s_wxCanvas) {
        lv_canvas_set_buffer(s_wxCanvas, (void *)pixels, WX_RADAR_SIZE, WX_RADAR_SIZE, LV_IMG_CF_TRUE_COLOR);
        lv_obj_clear_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wxStatus, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_wxCanvas);
        char iata[4]; float d = 0, b = 0;
        if (airports_nearest_iata(rlat, rlon, 200.0f, iata, &d, &b)) {
            char apt[64];
            snprintf(apt, sizeof(apt), "O  %s   %.0f %s %s", iata, dist_val(d), dist_unit(), cardinal(b));
            lv_label_set_text(s_wxAirport, apt);
        } else lv_label_set_text(s_wxAirport, "RADARMITTE");
        char stamp[6] = "--:--";
        time_t ft = (time_t)frameTime; struct tm ti;
        if (frameTime && localtime_r(&ft, &ti)) snprintf(stamp, sizeof(stamp), "%02d:%02d", ti.tm_hour, ti.tm_min);
        char attr[64];
        snprintf(attr, sizeof(attr), cloudMode ? "SAT %s  |  EUMETSAT" : "RADAR %s  |  RAINVIEWER", stamp);
        lv_label_set_text(s_wxAttrib, attr);
    } else {
        if (s_wxCanvas) lv_obj_add_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wxStatus, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_wxAirport, "RADARMITTE");
        lv_label_set_text(s_wxAttrib, cloudMode ? "WARTE AUF SATELLITENDATEN" : "WARTE AUF RADARDATEN");
    }

    lv_obj_t *forecastObjs[] = {
        s_fcCurrent, s_fcCondition, s_fcUpdated,
        s_fcMetricName[0], s_fcMetricName[1], s_fcMetricName[2],
        s_fcMetricValue[0], s_fcMetricValue[1], s_fcMetricValue[2],
        s_fcDay[0], s_fcDay[1], s_fcDay[2],
        s_fcDayCondition[0], s_fcDayCondition[1], s_fcDayCondition[2],
        s_fcDayTemp[0], s_fcDayTemp[1], s_fcDayTemp[2],
        s_fcDayRain[0], s_fcDayRain[1], s_fcDayRain[2]
    };
    lv_obj_t *radarObjs[] = { s_wxCanvas, s_wxStatus, s_wxAirport, s_wxFooter, s_wxMeta,
                              s_wxAttrib, s_wxNorth, s_wxCenter, s_wxRange,
                              s_wxRings[0], s_wxRings[1], s_wxRings[2] };
    for (lv_obj_t *o : forecastObjs) if (o) {
        if (forecastMode) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t *o : radarObjs) if (o) {
        if (forecastMode) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!forecastMode && haveImage) lv_obj_add_flag(s_wxStatus, LV_OBJ_FLAG_HIDDEN);
    if (!forecastMode && !haveImage) lv_obj_add_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_wxRange, cloudMode ? "200 KM" : "75 KM");
    lv_label_set_text(s_weatherModeLbl,
        s_weatherMode == WEATHER_RADAR ? "WOLKEN" :
        s_weatherMode == WEATHER_CLOUDS ? "3-TAGE-VORHERSAGE" : "WETTERRADAR");
    if (s_weatherTitle) lv_label_set_text(s_weatherTitle,
        s_weatherMode == WEATHER_RADAR ? "WETTERRADAR" :
        s_weatherMode == WEATHER_CLOUDS ? "SAT-WOLKEN" : "WETTER");
}

// Registered once, on the whole weather panel (see ui_create) -- the mode pill and the
// image are deliberately non-clickable so every tap funnels here. The debounce swallows
// repeats from a held press, and a duplicate should another target ever be added back.
static void weather_mode_cb(lv_event_t *) {
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 250) return;
    last = now;
    s_weatherMode = (WeatherViewMode)(((int)s_weatherMode + 1) % 3);
    build_weather();
}

void ui_set_weather_forecast(bool forecast) {
    s_weatherMode = forecast ? WEATHER_FORECAST : WEATHER_RADAR;
    build_weather();
}

// Rebuild whichever of list/stats is currently on screen (called on poll and on swipe).
static void (*s_viewChangedCb)(int) = nullptr;
void ui_set_view_changed_cb(void (*cb)(int idx)) { s_viewChangedCb = cb; }

// ---------------------------------------------------------------- idle clock
// radar_view's scene steps. From the first one the HUD, the zoom button and the card make
// way for the dial, and a transparent layer on top takes the touch that wakes the radar, so
// that touch does not also select a plane, swipe a view or change the theme. LVGL counts the
// press as activity, which is what makes main.cpp send the clock away.
static lv_obj_t *s_clockCatcher = nullptr;

static void clock_stage_cb(int stage) {
    const bool up = stage != 0;
    if (up == s_clockUp) return;
    s_clockUp = up;
    lv_obj_t *chrome[] = { s_hudWifi, s_hudGps, s_hudCount, s_hudClock, s_hudBatt, s_hudDate, s_zoomBtn, s_mapAttr };
    for (lv_obj_t *o : chrome) {
        if (!o) continue;
        if (up) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!up && s_mapAttr && !radar::mapShown()) lv_obj_add_flag(s_mapAttr, LV_OBJ_FLAG_HIDDEN);   // no map: stays hidden
    refresh_card();                         // hides the card while the clock is up, restores it after
    if (!s_clockCatcher) return;
    if (up) lv_obj_clear_flag(s_clockCatcher, LV_OBJ_FLAG_HIDDEN);
    else if (!lv_obj_has_state(s_clockCatcher, LV_STATE_PRESSED))
        lv_obj_add_flag(s_clockCatcher, LV_OBJ_FLAG_HIDDEN);   // still pressed: hidden on release
}

static void clock_catcher_release_cb(lv_event_t *e) {
    if (!s_clockUp) lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
}

// Sweep compositor hooks (polled by display.cpp every loop pass).
bool ui_sweep_visible(void) {
    return s_tv && lv_tileview_get_tile_act(s_tv) == s_tileRadar && !lv_obj_is_scrolling(s_tv);
}

int ui_sweep_masks(int16_t out[][5], int max) {
    lv_obj_t *objs[5] = { s_card, s_photo, s_photoCredit, s_zoomBtn, s_holdHint };   // detail card, its photo, range button, hold hint
    int n = 0;
    for (lv_obj_t *o : objs) {
        if (n >= max || !o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        out[n][0] = a.x1; out[n][1] = a.y1; out[n][2] = a.x2; out[n][3] = a.y2;
        out[n][4] = lv_obj_get_style_radius(o, LV_PART_MAIN);
        n++;
    }
    return n;
}

static int active_view_index(void) {
    if (!s_tv) return 0;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    if (act == s_tileList)    return 1;
    if (act == s_tileStats)   return 2;
    if (act == s_tileWeather) return 3;
    if (act == s_tileSettings) return 4;
    if (act == s_tilePhoto)    return 5;
    return 0;
}

static void refresh_active_tile(void) {
    if (!s_tv) return;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    if (act == s_tileList)  build_list();
    else if (act == s_tileStats) build_stats();
    else if (act == s_tileWeather) build_weather();
    else if (act == s_tileSettings) ui_settings_refresh();
}

void ui_on_data_updated(void) {
    refresh_card();
    if (s_hudCount) {
        char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%d", radar::countInRange());
        lv_label_set_text(s_hudCount, cbuf);
    }
    refresh_active_tile();   // only the visible tile pays the rebuild cost
}

// ------------------------------------------------------------------- building
static lv_obj_t *make_tile_title(lv_obj_t *tile, const char *txt) {
    lv_obj_t *l = lv_label_create(tile);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, F16(), 0);
    lv_obj_set_style_text_color(l, UI_GREEN, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 22);
    return l;
}

// A full-screen round panel that clips its content to the circle (for list/stats views).
static lv_obj_t *make_round_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 462, 462);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x05100A), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, UI_GREEN, 0);
    lv_obj_set_style_border_opa(p, 50, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_clip_corner(p, true, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

// The X in the card's top-right corner: closes the card, as a tap on empty scope does. With
// planes round the card, that tap often selected one of them instead.
static void card_close_cb(lv_event_t *e) {
    (void)e;
    radar::select(-1);
    refresh_card();
}

static void build_card(void) {
    s_card = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_card);
    // large text needs a taller card (three 18px data lines + the route line below them)
    lv_obj_set_size(s_card, s_bigText ? 316 : 300, s_bigText ? 148 : 118);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, s_bigText ? 56 : 66);
    lv_obj_set_style_bg_color(s_card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, 235, 0);
    lv_obj_set_style_radius(s_card, 14, 0);
    lv_obj_set_style_border_color(s_card, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_card, 90, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_pad_all(s_card, 12, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   // consume taps (don't deselect)
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_cardTitle = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardTitle, F16(), 0);
    lv_obj_set_style_text_color(s_cardTitle, UI_INK, 0);
    lv_obj_align(s_cardTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cardL = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardL, F14(), 0);
    lv_obj_set_style_text_color(s_cardL, UI_SOFT, 0);
    lv_obj_align(s_cardL, LV_ALIGN_TOP_LEFT, 0, s_bigText ? 30 : 26);

    s_cardR = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardR, F14(), 0);
    lv_obj_set_style_text_color(s_cardR, UI_SOFT, 0);
    lv_obj_align(s_cardR, LV_ALIGN_TOP_LEFT, s_bigText ? 160 : 150, s_bigText ? 30 : 26);

    s_cardRoute = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardRoute, F14(), 0);
    lv_obj_set_style_text_color(s_cardRoute, UI_GREEN, 0);
    // fill the card width and dot out long "Origin -> Destination" instead of clipping
    lv_obj_set_width(s_cardRoute, s_bigText ? 292 : 276);   // card 316/300 minus pad 12 x2
    lv_label_set_long_mode(s_cardRoute, LV_LABEL_LONG_DOT);
    lv_obj_align(s_cardRoute, LV_ALIGN_TOP_LEFT, 0, s_bigText ? 100 : 76);

    // The X sits in the card's top-right corner, 4 px in from its edges (the content area
    // starts 13 px in: 12 px padding plus the 1 px frame). A child of the card: it hides with
    // it, and the card's sweep mask covers it.
    // Its tap area grows by UI_CARD_CLOSE_HIT on every side; LVGL clips that to the card.
    const lv_coord_t cs = s_bigText ? 32 : 28;
    lv_obj_t *x = lv_obj_create(s_card);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, cs, cs);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 9, -9);
    lv_obj_set_style_radius(x, 10, 0);                        // the card's 14 minus the 4 px inset
    lv_obj_set_style_border_color(x, UI_GREEN, 0);            // the card's own frame
    lv_obj_set_style_border_opa(x, 90, 0);
    lv_obj_set_style_border_width(x, 1, 0);
    lv_obj_set_style_bg_color(x, UI_GREEN, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(x, 60, LV_STATE_PRESSED);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(x, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);   // a small drift must not swipe the view
    lv_obj_set_ext_click_area(x, UI_CARD_CLOSE_HIT);
    lv_obj_add_event_cb(x, card_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, F14(), 0);
    lv_obj_set_style_text_color(xl, UI_SOFT, 0);
    lv_obj_center(xl);

    // aircraft photo + credit, floating above the card (hidden until one loads)
    s_photo = lv_canvas_create(s_tileRadar);
    lv_obj_set_style_radius(s_photo, 6, 0);
    lv_obj_set_style_clip_corner(s_photo, true, 0);
    lv_obj_set_style_border_color(s_photo, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_photo, 170, 0);
    lv_obj_set_style_border_width(s_photo, 1, 0);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    s_photoCredit = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_photoCredit, F12(), 0);
    lv_obj_set_style_text_color(s_photoCredit, UI_DIM, 0);
    lv_label_set_text(s_photoCredit, "");
    lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------------------ settings
// Einstellungen-Seite links vom Radar: eine senkrecht scrollbare Liste aus Schiebereglern,
// Schaltern und Tipp-Knöpfen (jeder Tipp schaltet zur nächsten Option weiter). Werte kommen
// aus main.cpp (Getter) und gehen dorthin zurück (Setter), damit Gerät und Webseite dieselbe
// Logik und denselben NVS-Speicher benutzen.
static int  (*s_setGet)(int) = nullptr;
static void (*s_setSet)(int, int, bool) = nullptr;
static int  (*s_netCount)(void) = nullptr;
static const char *(*s_netName)(int) = nullptr;
static void (*s_netAdd)(void) = nullptr;
static int  s_setSound = 2;                           // 0 none, 1 on/off (buzzer), 2 levels
static char s_setUrl[64] = "";
static lv_obj_t *s_setList = nullptr;
static lv_obj_t *s_setWidget[UI_SET_COUNT] = {};      // slider / switch / cycle-button label
static lv_obj_t *s_setThemeLbl = nullptr;
static lv_obj_t *s_setQr = nullptr, *s_setUrlLbl = nullptr;
static lv_obj_t *s_confirmLbl = nullptr;              // a two-tap button armed by its first tap
static lv_timer_t *s_confirmTimer = nullptr;

static const char *const kSetSound[]  = { "Aus", "Leise", "Laut" };
static const char *const kSetOnOff[]  = { "Aus", "An" };
static const char *const kSetUnits[]  = { "Luftfahrt", "Metrisch", "Imperial" };
static const char *const kSetTrail[]  = { "Aus", "Kurz", "Mittel", "Lang" };
static const char *const kSetAlerts[] = { "Aus", "Nur Notfälle", "Neue + Notfälle" };
static const char *const kSetMap[]    = { "Aus", "Dunkel", "Hell" };
static const char *const kSetRotate[] = { "0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0" };
static const char *const kSetThemes[] = { "Phosphor", "Orb", "Amber CRT", "Militär" };

static int set_option_count(int id) {
    switch (id) {
        case UI_SET_SOUND:   return s_setSound == 1 ? 2 : 3;
        case UI_SET_ALERTS:  return 3;
        case UI_SET_UNITS:   return 3;
        case UI_SET_TRAIL:   return 4;
        case UI_SET_ROTATE:  return 4;
        case UI_SET_MAP:     return 3;
        case UI_SET_WIFINET: return s_netCount ? s_netCount() : 0;
        default:             return 0;
    }
}

static const char *set_option_text(int id, int v) {
    const int n = set_option_count(id);
    if (n <= 0) return id == UI_SET_WIFINET ? "keins" : "-";
    v = (v % n + n) % n;
    switch (id) {
        case UI_SET_SOUND:   return s_setSound == 1 ? kSetOnOff[v] : kSetSound[v];
        case UI_SET_ALERTS:  return kSetAlerts[v];
        case UI_SET_UNITS:   return kSetUnits[v];
        case UI_SET_TRAIL:   return kSetTrail[v];
        case UI_SET_ROTATE:  return kSetRotate[v];
        case UI_SET_MAP:     return kSetMap[v];
        case UI_SET_WIFINET: { const char *s = s_netName ? s_netName(v) : nullptr; return (s && *s) ? s : "?"; }
        default:             return "-";
    }
}

static int set_get(int id) { return s_setGet ? s_setGet(id) : 0; }

static void set_cycle_label(int id, int v) {
    if (s_setWidget[id]) lv_label_set_text(s_setWidget[id], set_option_text(id, v));
}

static void set_theme_label(void) {
    const int th = radar::theme();
    if (s_setThemeLbl) lv_label_set_text(s_setThemeLbl, kSetThemes[(th >= 0 && th < 4) ? th : 0]);
}

// QR code + address of the web page (or a note when there is no WiFi)
static void set_url_refresh(void) {
    if (!s_setQr || !s_setUrlLbl) return;
    if (s_setUrl[0]) {
        lv_qrcode_update(s_setQr, s_setUrl, strlen(s_setUrl));
        lv_obj_clear_flag(s_setQr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_setUrlLbl, s_setUrl);
    } else {
        lv_obj_add_flag(s_setQr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_setUrlLbl, "Kein WLAN verbunden");
    }
}

void ui_set_settings_cb(int (*get)(int), void (*set)(int, int, bool)) {
    s_setGet = get;
    s_setSet = set;
    ui_settings_refresh();
}

void ui_set_settings_sound(int mode) { s_setSound = mode < 0 ? 0 : (mode > 2 ? 2 : mode); }

void ui_set_settings_wifi_cb(int (*count)(void), const char *(*name)(int), void (*add)(void)) {
    s_netCount = count;
    s_netName = name;
    s_netAdd = add;
}

void ui_set_settings_url(const char *url) {
    if (!url) url = "";
    if (strcmp(url, s_setUrl) == 0) return;           // called every second: only act on a change
    snprintf(s_setUrl, sizeof(s_setUrl), "%s", url);
    set_url_refresh();
}

void ui_settings_refresh(void) {
    if (!s_setList) return;
    for (int id = 0; id < UI_SET_COUNT; ++id) {
        lv_obj_t *w = s_setWidget[id];
        if (!w) continue;
        const int v = set_get(id);
        if (lv_obj_check_type(w, &lv_slider_class)) {
            if (!lv_obj_has_state(w, LV_STATE_PRESSED)) lv_slider_set_value(w, v, LV_ANIM_OFF);   // don't fight a drag
        } else if (lv_obj_check_type(w, &lv_switch_class)) {
            if (v) lv_obj_add_state(w, LV_STATE_CHECKED); else lv_obj_clear_state(w, LV_STATE_CHECKED);
        } else {
            set_cycle_label(id, v);
        }
    }
    set_theme_label();
}

static void set_slider_cb(lv_event_t *e) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target(e);
    const bool final = lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED;   // RELEASED / PRESS_LOST
    if (s_setSet) s_setSet(id, (int)lv_slider_get_value(s), final);
}

static void set_switch_cb(lv_event_t *e) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    if (s_setSet) s_setSet(id, lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0, true);
}

static void set_cycle_cb(lv_event_t *e) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    const int n = set_option_count(id);
    if (n <= 0) return;
    const int v = ((set_get(id) + 1) % n + n) % n;
    if (s_setSet) s_setSet(id, v, true);
    set_cycle_label(id, set_get(id));   // show what main actually applied
}

// "GPS-Signal": the same states as the HUD icon (amber while searching, green with a fix)
static void set_gps_label(void) {
    if (!s_setGpsLbl) return;
    char b[40];
    lv_color_t c = UI_DIM;
    if (!s_gpsPresent)        snprintf(b, sizeof(b), "Kein GPS-Modul");
    else if (s_gpsState <= 0) snprintf(b, sizeof(b), "Aus");
    else if (s_gpsState == 1) { snprintf(b, sizeof(b), LV_SYMBOL_GPS " Sucht (%d Sat.)", s_gpsSats); c = lv_color_hex(0xFFB23C); }
    else                      { snprintf(b, sizeof(b), LV_SYMBOL_GPS " Empfang %d Sat.", s_gpsSats); c = UI_GREEN; }
    lv_label_set_text(s_setGpsLbl, b);
    lv_obj_set_style_text_color(s_setGpsLbl, c, 0);
}

static void set_theme_cb(lv_event_t *) {
    radar::setTheme((radar::theme() + 1) % 4);   // saved by main's theme-changed callback
    set_theme_label();
}

// Two-tap buttons (power off, add WiFi): the first tap arms the button and asks for a
// second one; it disarms itself after 3 s.
struct ConfirmAction { const char *text; void (*run)(void); };
static void confirm_disarm(void) {
    if (s_confirmTimer) { lv_timer_del(s_confirmTimer); s_confirmTimer = nullptr; }
    if (s_confirmLbl) {
        const ConfirmAction *a = (const ConfirmAction *)lv_obj_get_user_data(s_confirmLbl);
        if (a) lv_label_set_text(s_confirmLbl, a->text);
    }
    s_confirmLbl = nullptr;
}
static void confirm_timer_cb(lv_timer_t *) { s_confirmTimer = nullptr; confirm_disarm(); }
static void confirm_cb(lv_event_t *e) {
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    const ConfirmAction *a = (const ConfirmAction *)lv_obj_get_user_data(lbl);
    if (s_confirmLbl == lbl) {                 // second tap: do it
        confirm_disarm();
        if (a && a->run) a->run();
        return;
    }
    confirm_disarm();                          // another button was armed: drop that one
    s_confirmLbl = lbl;
    lv_label_set_text(lbl, "Wirklich?");
    s_confirmTimer = lv_timer_create(confirm_timer_cb, 3000, nullptr);
    lv_timer_set_repeat_count(s_confirmTimer, 1);
}
static void run_poweroff(void) { if (s_poweroffCb) s_poweroffCb(); }
static void run_wifi_add(void) { if (s_netAdd) s_netAdd(); }
static const ConfirmAction kActPowerOff = { "Ausschalten", run_poweroff };
static const ConfirmAction kActWifiAdd  = { "Hinzufügen", run_wifi_add };

// Speicher: LVGLs Heap ist knapp (64 KB, davon braucht der Rest der Oberfläche ~45 KB). Darum
// teilen sich alle Zeilen und Bedienelemente eine Handvoll statischer Styles, und Lage/Größe
// macht das Flex-Layout -- lokale Styles pro Objekt würden je Objekt Heap kosten.
static lv_style_t s_stRow, s_stRowTall, s_stLbl, s_stBtn, s_stBtnPr, s_stBtnLbl;
static lv_style_t s_stTrack, s_stInd, s_stKnob, s_stSwKnob, s_stHint;

static void set_styles_init(void) {
    const lv_coord_t h = s_bigText ? 52 : 46;
    lv_style_init(&s_stRow);
    lv_style_set_width(&s_stRow, lv_pct(100));
    lv_style_set_height(&s_stRow, h);
    lv_style_set_pad_hor(&s_stRow, 4);
    lv_style_set_border_side(&s_stRow, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_stRow, UI_GREEN);
    lv_style_set_border_opa(&s_stRow, 40);
    lv_style_set_border_width(&s_stRow, 1);
    lv_style_set_layout(&s_stRow, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_stRow, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(&s_stRow, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_style_set_flex_cross_place(&s_stRow, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_stRow, LV_FLEX_ALIGN_CENTER);

    lv_style_init(&s_stRowTall);                       // slider + QR rows: content stacked
    lv_style_set_height(&s_stRowTall, LV_SIZE_CONTENT);
    lv_style_set_pad_ver(&s_stRowTall, 10);
    lv_style_set_pad_row(&s_stRowTall, 10);
    lv_style_set_pad_hor(&s_stRowTall, 14);
    lv_style_set_flex_flow(&s_stRowTall, LV_FLEX_FLOW_COLUMN);
    lv_style_set_flex_main_place(&s_stRowTall, LV_FLEX_ALIGN_START);
    lv_style_set_flex_cross_place(&s_stRowTall, LV_FLEX_ALIGN_START);

    lv_style_init(&s_stLbl);
    lv_style_set_text_font(&s_stLbl, F14());
    lv_style_set_text_color(&s_stLbl, UI_SOFT);

    lv_style_init(&s_stBtn);
    lv_style_set_width(&s_stBtn, s_bigText ? 164 : 136);
    lv_style_set_height(&s_stBtn, s_bigText ? 36 : 32);
    lv_style_set_radius(&s_stBtn, 16);
    lv_style_set_pad_hor(&s_stBtn, 8);
    lv_style_set_bg_color(&s_stBtn, UI_PANEL);
    lv_style_set_bg_opa(&s_stBtn, LV_OPA_COVER);
    lv_style_set_border_color(&s_stBtn, UI_GREEN);
    lv_style_set_border_width(&s_stBtn, 1);
    lv_style_set_shadow_width(&s_stBtn, 0);
    lv_style_set_layout(&s_stBtn, LV_LAYOUT_FLEX);    // centres its one label
    lv_style_set_flex_main_place(&s_stBtn, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_cross_place(&s_stBtn, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_stBtn, LV_FLEX_ALIGN_CENTER);
    lv_style_init(&s_stBtnPr);
    lv_style_set_bg_color(&s_stBtnPr, lv_color_hex(0x1F3A2B));
    lv_style_init(&s_stBtnLbl);
    lv_style_set_text_font(&s_stBtnLbl, F14());
    lv_style_set_text_color(&s_stBtnLbl, UI_GREEN);
    lv_style_set_width(&s_stBtnLbl, lv_pct(100));      // fixed one-line box: long WiFi names end in "..."
    lv_style_set_height(&s_stBtnLbl, lv_font_get_line_height(F14()));
    lv_style_set_text_align(&s_stBtnLbl, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&s_stTrack);                          // slider + switch background
    lv_style_set_bg_color(&s_stTrack, lv_color_hex(0x1F3A2B));
    lv_style_set_bg_opa(&s_stTrack, LV_OPA_COVER);
    lv_style_set_radius(&s_stTrack, LV_RADIUS_CIRCLE);
    lv_style_init(&s_stInd);
    lv_style_set_bg_color(&s_stInd, UI_GREEN);
    lv_style_set_bg_opa(&s_stInd, LV_OPA_COVER);
    lv_style_set_radius(&s_stInd, LV_RADIUS_CIRCLE);
    lv_style_init(&s_stKnob);
    lv_style_set_bg_color(&s_stKnob, UI_GREEN);
    lv_style_set_bg_opa(&s_stKnob, LV_OPA_COVER);
    lv_style_set_radius(&s_stKnob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s_stKnob, 7);
    lv_style_init(&s_stSwKnob);
    lv_style_set_bg_color(&s_stSwKnob, UI_INK);
    lv_style_set_bg_opa(&s_stSwKnob, LV_OPA_COVER);
    lv_style_set_radius(&s_stSwKnob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s_stSwKnob, -3);              // knob a little inside the track

    lv_style_init(&s_stHint);
    lv_style_set_width(&s_stHint, lv_pct(100));
    lv_style_set_text_font(&s_stHint, F12());
    lv_style_set_text_color(&s_stHint, UI_DIM);
    lv_style_set_text_align(&s_stHint, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_stHint, 10);
}

// One row: label on the left, control on the right (or underneath, for sliders).
static lv_obj_t *set_row(const char *label, bool tall) {
    lv_obj_t *r = lv_obj_create(s_setList);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &s_stRow, 0);
    if (tall) lv_obj_add_style(r, &s_stRowTall, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(r);
    lv_obj_add_style(l, &s_stLbl, 0);
    lv_label_set_text(l, label);
    return r;
}

static void set_add_slider(int id, const char *label, int lo, int hi) {
    lv_obj_t *r = set_row(label, true);
    lv_obj_t *s = lv_slider_create(r);
    lv_obj_remove_style_all(s);                         // drop the theme's styles: ours are shared
    lv_obj_add_style(s, &s_stTrack, LV_PART_MAIN);
    lv_obj_add_style(s, &s_stInd, LV_PART_INDICATOR);
    lv_obj_add_style(s, &s_stKnob, LV_PART_KNOB);
    lv_obj_set_size(s, lv_pct(100), 8);
    lv_slider_set_range(s, lo, hi);
    lv_obj_set_ext_click_area(s, 14);                   // a fat target for a finger on an 8 px bar
    lv_obj_add_event_cb(s, set_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
    lv_obj_add_event_cb(s, set_slider_cb, LV_EVENT_RELEASED, (void *)(intptr_t)id);
    lv_obj_add_event_cb(s, set_slider_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)id);
    s_setWidget[id] = s;
}

static void set_add_switch(int id, const char *label) {
    lv_obj_t *r = set_row(label, false);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_remove_style_all(sw);   // no theme styles: their colour fade would replay every time the page opens
    lv_obj_add_style(sw, &s_stTrack, LV_PART_MAIN);
    lv_obj_add_style(sw, &s_stInd, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(sw, &s_stSwKnob, LV_PART_KNOB);
    lv_obj_set_size(sw, s_bigText ? 56 : 50, s_bigText ? 30 : 26);
    lv_obj_set_ext_click_area(sw, 10);
    lv_obj_add_event_cb(sw, set_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
    s_setWidget[id] = sw;
}

// Tap button showing the current option; returns its label.
// user == SET_BTN_SELF: the click callback gets the button's own label as user data.
#define SET_BTN_SELF ((void *)-1)
static lv_obj_t *set_add_button(const char *label, lv_event_cb_t cb, void *user) {
    lv_obj_t *r = set_row(label, false);
    lv_obj_t *b = lv_obj_create(r);                     // plain obj: lighter than lv_btn + theme
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_stBtn, 0);
    lv_obj_add_style(b, &s_stBtnPr, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_add_style(l, &s_stBtnLbl, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, "-");
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user == SET_BTN_SELF ? (void *)l : user);
    return l;
}

static void set_add_cycle(int id, const char *label) {
    s_setWidget[id] = set_add_button(label, set_cycle_cb, (void *)(intptr_t)id);
}

static void set_add_confirm(const char *label, const ConfirmAction *act) {
    lv_obj_t *l = set_add_button(label, confirm_cb, SET_BTN_SELF);
    lv_obj_set_user_data(l, (void *)act);
    lv_label_set_text(l, act->text);
}

// Der Inhalt (die Liste) entsteht erst, wenn die Seite ins Bild geschoben wird, und wird beim
// Verlassen wieder gelöscht: ständig vorhanden würde er ~10-15 KB des LVGL-Heaps belegen, den
// das Radar zum Zeichnen braucht. Panel und Titel bleiben (billig), damit beim Wischen sofort
// etwas zu sehen ist.
static lv_obj_t *s_setPanel = nullptr;

static void settings_open(void) {
    if (s_setList || !s_setPanel) return;
    s_setList = lv_obj_create(s_setPanel);
    lv_obj_remove_style_all(s_setList);
    lv_obj_set_size(s_setList, s_bigText ? 340 : 310, 372);
    lv_obj_align(s_setList, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_flex_flow(s_setList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_setList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_setList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(s_setList, 90, 0);   // the last rows can scroll up out of the round edge

    set_add_slider(UI_SET_BRIGHT, "Helligkeit", 5, 255);
    if (s_setSound) {
        set_add_cycle(UI_SET_SOUND, "Ton");
        set_add_cycle(UI_SET_ALERTS, "Hinweiston bei");
    }
    s_setThemeLbl = set_add_button("Design", set_theme_cb, nullptr);
    set_add_cycle(UI_SET_MAP, "Karte");
    set_add_cycle(UI_SET_UNITS, "Einheiten");
    set_add_cycle(UI_SET_TRAIL, "Flugspuren");
    set_add_cycle(UI_SET_ROTATE, "Bild drehen");
    set_add_switch(UI_SET_SWEEP, "Radar-Sweep");
    set_add_switch(UI_SET_AIRPORTS, "Flughäfen");
    set_add_switch(UI_SET_HIDEGROUND, "Am Boden ausblenden");
    set_add_switch(UI_SET_CLOCK, "Uhr im Ruhezustand");
    if (s_gpsPresent) set_add_switch(UI_SET_GPS, "GPS-Standort");
    {
        lv_obj_t *r = set_row("GPS-Signal", false);
        s_setGpsLbl = lv_label_create(r);
        lv_obj_add_style(s_setGpsLbl, &s_stLbl, 0);
        set_gps_label();
    }
    set_add_switch(UI_SET_WIFI, "WLAN");
    if (s_netCount) set_add_cycle(UI_SET_WIFINET, "WLAN-Netz");
    if (s_netAdd)   set_add_confirm("WLAN hinzufügen", &kActWifiAdd);

    // QR code of the web page: point a phone camera at it to open the settings there
    lv_obj_t *qr = set_row("Webseite (mit dem Handy scannen)", true);
    lv_obj_set_style_flex_cross_place(qr, LV_FLEX_ALIGN_CENTER, 0);
    s_setQr = lv_qrcode_create(qr, s_bigText ? 150 : 132, lv_color_black(), UI_INK);
    lv_obj_set_style_border_color(s_setQr, UI_INK, 0);   // a quiet zone: scanners need a light margin
    lv_obj_set_style_border_width(s_setQr, 6, 0);
    s_setUrlLbl = lv_label_create(qr);
    lv_obj_add_style(s_setUrlLbl, &s_stBtnLbl, 0);
    lv_obj_set_height(s_setUrlLbl, LV_SIZE_CONTENT);
    set_url_refresh();

    if (s_poweroffCb) set_add_confirm("Gerät", &kActPowerOff);

    lv_obj_t *hint = lv_label_create(s_setList);
    lv_obj_add_style(hint, &s_stHint, 0);
    lv_label_set_text(hint, "Standort, Reichweite, Foto und mehr\nauf der Webseite");

    ui_settings_refresh();
}

static void settings_close(void) {
    if (!s_setList) return;
    confirm_disarm();
    lv_obj_del(s_setList);   // takes every row with it
    s_setList = nullptr;
    s_setThemeLbl = nullptr;
    s_setQr = nullptr;
    s_setUrlLbl = nullptr;
    s_setGpsLbl = nullptr;
    for (lv_obj_t *&w : s_setWidget) w = nullptr;
}

static void build_settings(void) {
    set_styles_init();
    lv_obj_clear_flag(s_tileSettings, LV_OBJ_FLAG_SCROLLABLE);
    s_setPanel = make_round_panel(s_tileSettings);
    make_tile_title(s_setPanel, UI_SETTINGS_TITLE);
}

// --------------------------------------------------------------------- photo
// Foto-Seite ganz links: das über die Webseite hochgeladene Bild, bildschirmfüllend. Die
// Pixel liegen im PSRAM (user_photo.cpp); LVGL zeichnet direkt daraus, ohne eigenen Heap.
static lv_obj_t *s_userImg = nullptr, *s_userHint = nullptr;
static lv_img_dsc_t s_userDsc;

void ui_set_user_photo(const uint16_t *px) {
    if (!s_userImg) return;
    if (px) {
        lv_img_cache_invalidate_src(&s_userDsc);
        memset(&s_userDsc, 0, sizeof(s_userDsc));
        s_userDsc.header.always_zero = 0;
        s_userDsc.header.w = SCREEN_W;
        s_userDsc.header.h = SCREEN_H;
        s_userDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_userDsc.data_size = (uint32_t)SCREEN_W * SCREEN_H * 2;
        s_userDsc.data = (const uint8_t *)px;
        lv_img_set_src(s_userImg, &s_userDsc);
        lv_obj_clear_flag(s_userImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_userHint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_userImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_userHint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_photo(void) {
    lv_obj_clear_flag(s_tilePhoto, LV_OBJ_FLAG_SCROLLABLE);
    s_userImg = lv_img_create(s_tilePhoto);
    lv_obj_center(s_userImg);
    lv_obj_add_flag(s_userImg, LV_OBJ_FLAG_HIDDEN);
    s_userHint = lv_label_create(s_tilePhoto);
    lv_obj_set_style_text_font(s_userHint, F16(), 0);
    lv_obj_set_style_text_color(s_userHint, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_userHint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_userHint, "Noch kein Foto.\n\nHochladen auf der Webseite\ncapsuleradar.local");
    lv_obj_center(s_userHint);
}

// View index (0 radar, 1 list, 2 stats, 3 weather, 4 settings, 5 photo) -> tile column.
// Photo | Settings | Radar | List | Stats | Weather: views 0..3 are columns 2..5.
void ui_show_view(int idx) {
    if (!s_tv || idx < 0 || idx > 5) return;
    if (idx == 4) settings_open();
    const uint32_t col = idx == 5 ? 0 : (idx == 4 ? 1 : (uint32_t)(idx + 2));
    lv_obj_set_tile_id(s_tv, col, 0, LV_ANIM_OFF);
}

// ------------------------------------------------------------------- splash
static void splash_fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void splash_del_cb(lv_anim_t *a) { lv_obj_del((lv_obj_t *)a->var); }

static void splash_dismiss_cb(lv_timer_t *t) {
    lv_obj_t *cont = (lv_obj_t *)t->user_data;
    lv_timer_del(t);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cont);
    lv_anim_set_exec_cb(&a, splash_fade_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 600);
    lv_anim_set_ready_cb(&a, splash_del_cb);
    lv_anim_start(&a);
}

void ui_splash_show(void) {
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // concentric rings
    const lv_coord_t dia[3] = { 210, 142, 78 };
    const lv_opa_t   op[3]  = { 90, 120, 160 };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *r = lv_obj_create(cont);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, dia[i], dia[i]);
        lv_obj_align(r, LV_ALIGN_CENTER, 0, -8);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(r, UI_GREEN, 0);
        lv_obj_set_style_border_opa(r, op[i], 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }
    // rotating sweep
    lv_obj_t *sweep = lv_spinner_create(cont, 1400, 55);
    lv_obj_set_size(sweep, 210, 210);
    lv_obj_align(sweep, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_arc_opa(sweep, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sweep, UI_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sweep, 4, LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "CAPSULE RADAR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 118);

    lv_obj_t *sub = lv_label_create(cont);
    lv_label_set_text(sub, "Live-ADS-B-Radar");
    lv_obj_set_style_text_font(sub, F14(), 0);
    lv_obj_set_style_text_color(sub, UI_SOFT, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 150);

    lv_timer_t *t = lv_timer_create(splash_dismiss_cb, 2200, cont);   // hold, then fade out
    lv_timer_set_repeat_count(t, 1);
}

void ui_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    // Photo | Settings | Radar | List | Stats | Weather -- swipe right on the radar for the
    // settings, once more for the photo.
    s_tilePhoto    = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_tileSettings = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR);
    s_tileRadar = lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_HOR);
    s_tileList  = lv_tileview_add_tile(s_tv, 3, 0, LV_DIR_HOR);
    s_tileStats = lv_tileview_add_tile(s_tv, 4, 0, LV_DIR_HOR);
    s_tileWeather = lv_tileview_add_tile(s_tv, 5, 0, LV_DIR_LEFT);
    // Rebuild the list/stats with the latest data the moment they slide into view
    // (between polls they'd otherwise show whatever was there when last visible).
    // Settings content lives only while its tile is (partly) on screen: build it as soon as a
    // swipe starts to reveal column 0, drop it once the swipe settles on another tile.
    lv_obj_add_event_cb(s_tv, [](lv_event_t *) {
        const lv_coord_t x = lv_obj_get_scroll_x(s_tv);
        if (x > 0 && x < 2 * SCREEN_W) settings_open();   // column 1 (settings) partly on screen
    }, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(s_tv, [](lv_event_t *) {
        if (lv_tileview_get_tile_act(s_tv) != s_tileSettings) settings_close();
        refresh_active_tile();
        if (s_viewChangedCb) s_viewChangedCb(active_view_index());   // let main remember the last view
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- radar tile ---
    lv_obj_clear_flag(s_tileRadar, LV_OBJ_FLAG_SCROLLABLE);
    radar::init(s_tileRadar);
    radar::setRangeLabelVisible(false);                     // the zoom button shows the range instead
    lv_obj_add_flag(s_tileRadar, LV_OBJ_FLAG_CLICKABLE);     // receive taps (planes/empty)
    lv_obj_add_event_cb(s_tileRadar, radar_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_presslost_cb, LV_EVENT_PRESS_LOST, NULL);
    build_card();

    // on-screen range/zoom button (reliable single tap; bottom, above the 'S' marker)
    s_zoomBtn = lv_btn_create(s_tileRadar);
    lv_obj_set_size(s_zoomBtn, 120, 44);
    lv_obj_set_ext_click_area(s_zoomBtn, 18);   // invisibly enlarge the tap target (easier to hit)
    lv_obj_align(s_zoomBtn, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_style_radius(s_zoomBtn, 18, 0);
    lv_obj_set_style_bg_color(s_zoomBtn, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_zoomBtn, 225, 0);
    lv_obj_set_style_border_color(s_zoomBtn, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_zoomBtn, 1, 0);
    lv_obj_set_style_border_opa(s_zoomBtn, 170, 0);
    lv_obj_clear_flag(s_zoomBtn, LV_OBJ_FLAG_SCROLL_CHAIN);  // tapping it must not swipe the tileview
    lv_obj_add_event_cb(s_zoomBtn, zoom_cb, LV_EVENT_PRESSED, NULL);  // fire on touch-down, not release
    s_zoomLbl = lv_label_create(s_zoomBtn);
    lv_label_set_text(s_zoomLbl, LV_SYMBOL_LOOP " 30 km");
    lv_obj_set_style_text_font(s_zoomLbl, F14(), 0);
    lv_obj_set_style_text_color(s_zoomLbl, UI_GREEN, 0);
    lv_obj_center(s_zoomLbl);

    // top status HUD (wifi / aircraft count / clock); white reads on both themes.
    // WiFi is a 4-bar signal meter: bar count = RSSI strength, colour = feed health.
    s_hudWifi = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_hudWifi);
    lv_obj_set_size(s_hudWifi, 21, 14);
    lv_obj_align(s_hudWifi, LV_ALIGN_TOP_MID, -94, 50);
    lv_obj_clear_flag(s_hudWifi, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; ++i) {
        s_hudBars[i] = lv_obj_create(s_hudWifi);
        lv_obj_remove_style_all(s_hudBars[i]);
        lv_obj_set_size(s_hudBars[i], 3, (lv_coord_t)(4 + i * 3));   // 4, 7, 10, 13 px tall
        lv_obj_align(s_hudBars[i], LV_ALIGN_BOTTOM_LEFT, (lv_coord_t)(i * 5), 0);
        lv_obj_set_style_radius(s_hudBars[i], 1, 0);
        lv_obj_set_style_bg_color(s_hudBars[i], UI_INK, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_hudBars[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_hudGps = lv_label_create(s_tileRadar);     // GPS satellite icon (between WiFi bars and count)
    lv_obj_set_style_text_font(s_hudGps, F14(), 0);
    lv_obj_set_style_text_color(s_hudGps, UI_GREEN, 0);
    lv_label_set_text(s_hudGps, "");             // hidden until ui_set_gps() says GPS is on
    lv_obj_align(s_hudGps, LV_ALIGN_TOP_MID, -62, 50);

    s_hudCount = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudCount, F14(), 0);
    lv_obj_set_style_text_color(s_hudCount, UI_INK, 0);
    lv_label_set_text(s_hudCount, "0");
    lv_obj_align(s_hudCount, LV_ALIGN_TOP_MID, -34, 50);

    s_hudClock = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudClock, F14(), 0);
    lv_obj_set_style_text_color(s_hudClock, UI_INK, 0);
    lv_label_set_text(s_hudClock, "--:--");
    lv_obj_align(s_hudClock, LV_ALIGN_TOP_MID, 30, 50);

    s_hudBatt = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudBatt, F14(), 0);
    lv_obj_set_style_text_color(s_hudBatt, UI_INK, 0);
    lv_label_set_text(s_hudBatt, "");
    lv_obj_align(s_hudBatt, LV_ALIGN_TOP_MID, 92, 50);

    s_hudDate = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudDate, F12(), 0);
    lv_obj_set_style_text_color(s_hudDate, UI_INK, 0);
    lv_obj_set_style_text_opa(s_hudDate, 140, 0);
    lv_label_set_text(s_hudDate, "");
    lv_obj_align(s_hudDate, LV_ALIGN_TOP_MID, 0, 70);

    // map attribution (OpenStreetMap + CARTO ask for it wherever their map is shown)
    s_mapAttr = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_mapAttr, F12(), 0);
    lv_obj_set_style_text_color(s_mapAttr, UI_INK, 0);
    lv_obj_set_style_text_opa(s_mapAttr, 110, 0);
    lv_label_set_text(s_mapAttr, "Karte \xC2\xA9 OpenStreetMap, CARTO");
    lv_obj_align(s_mapAttr, LV_ALIGN_TOP_MID, 0, s_bigText ? 92 : 86);
    lv_obj_add_flag(s_mapAttr, LV_OBJ_FLAG_HIDDEN);

    // The theme hold's hint (radar_pressing_cb), styled like the zoom button and centred like
    // the detail card. Opaque: while it shows, it sits over the middle of an open card.
    s_holdHint = lv_label_create(s_tileRadar);
    lv_label_set_text(s_holdHint, "Loslassen: Design wechseln");
    lv_obj_set_style_text_font(s_holdHint, F14(), 0);
    lv_obj_set_style_text_color(s_holdHint, UI_GREEN, 0);
    lv_obj_set_style_bg_color(s_holdHint, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_holdHint, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_holdHint, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_holdHint, 1, 0);
    lv_obj_set_style_border_opa(s_holdHint, 170, 0);
    lv_obj_set_style_radius(s_holdHint, 18, 0);
    lv_obj_set_style_pad_hor(s_holdHint, 18, 0);
    lv_obj_set_style_pad_ver(s_holdHint, 13, 0);
    lv_obj_align(s_holdHint, LV_ALIGN_CENTER, 0, s_bigText ? 56 : 66);
    lv_obj_add_flag(s_holdHint, LV_OBJ_FLAG_HIDDEN);

    // idle clock: a transparent layer above everything takes the touch that wakes the radar
    s_clockCatcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_clockCatcher);
    lv_obj_set_size(s_clockCatcher, SCREEN_W, SCREEN_H);
    lv_obj_clear_flag(s_clockCatcher, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(s_clockCatcher, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_clockCatcher, clock_catcher_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_clockCatcher, clock_catcher_release_cb, LV_EVENT_PRESS_LOST, NULL);
    radar::setClockStageCb(clock_stage_cb);

    // --- list tile (circular panel, clipped to the round screen) ---
    lv_obj_t *lp = make_round_panel(s_tileList);
    make_tile_title(lp, "FLUGZEUGE");
    s_list = lv_list_create(lp);
    lv_obj_set_size(s_list, s_bigText ? 340 : 300, 372);   // wider rows so big-font distances don't clip
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);
    // rows are plain labels: they inherit font/colour from here (see build_list)
    lv_obj_set_style_text_font(s_list, F16(), 0);
    lv_obj_set_style_text_color(s_list, UI_SOFT, 0);
    lv_obj_add_event_cb(s_list, list_press_cb, LV_EVENT_PRESSED, NULL);  // pick the plane at touch-down
    lv_obj_add_event_cb(s_list, list_tap_cb, LV_EVENT_CLICKED, NULL);   // rows aren't clickable

    // --- stats tile (circular panel) ---
    lv_obj_t *sp = make_round_panel(s_tileStats);
    make_tile_title(sp, "STATISTIK");
    s_statsLbl = lv_label_create(sp);
    lv_obj_set_style_text_font(s_statsLbl, F16(), 0);
    lv_obj_set_style_text_color(s_statsLbl, UI_SOFT, 0);
    lv_label_set_text(s_statsLbl, "Flugzeuge    0");
    lv_obj_align(s_statsLbl, LV_ALIGN_CENTER, 0, -16);

    s_statsGps = lv_label_create(sp);               // GPS status line (hidden unless GPS is on)
    lv_obj_set_style_text_font(s_statsGps, F14(), 0);
    lv_obj_set_style_text_color(s_statsGps, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_statsGps, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsGps, "");
    lv_obj_align(s_statsGps, LV_ALIGN_CENTER, 0, 90);

    // footer: where to reach the configuration page (IP / hostname / setup AP)
    s_statsNet = lv_label_create(sp);
    lv_obj_set_width(s_statsNet, 320);
    lv_obj_set_style_text_font(s_statsNet, F14(), 0);
    lv_obj_set_style_text_color(s_statsNet, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_statsNet, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsNet, "");
    lv_obj_align(s_statsNet, LV_ALIGN_CENTER, 0, 132);

    // power off (hold) -- top of the Stats view, under the title
    s_offBtn = lv_btn_create(sp);
    lv_obj_set_size(s_offBtn, s_bigText ? 270 : 230, 40);   // DE: longer label than "Hold to power off"
    lv_obj_set_ext_click_area(s_offBtn, 12);
    lv_obj_align(s_offBtn, LV_ALIGN_CENTER, 0, -150);
    lv_obj_set_style_radius(s_offBtn, 18, 0);
    lv_obj_set_style_bg_color(s_offBtn, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_offBtn, 225, 0);
    lv_obj_set_style_border_color(s_offBtn, UI_EMERG, 0);
    lv_obj_set_style_border_width(s_offBtn, 1, 0);
    lv_obj_clear_flag(s_offBtn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(s_offBtn, poweroff_cb, LV_EVENT_ALL, NULL);
    s_offLbl = lv_label_create(s_offBtn);
    lv_label_set_text(s_offLbl, LV_SYMBOL_POWER " Halten zum Ausschalten");
    lv_obj_set_style_text_font(s_offLbl, F14(), 0);
    lv_obj_set_style_text_color(s_offLbl, UI_EMERG, 0);
    lv_obj_center(s_offLbl);
    lv_obj_add_flag(s_offBtn, LV_OBJ_FLAG_HIDDEN);   // shown only when ui_set_poweroff_cb(non-null)

    // hold-progress ring round the screen edge (top layer, so it shows over the Stats tile)
    s_offRing = lv_arc_create(lv_layer_top());
    lv_obj_set_size(s_offRing, SCREEN_W, SCREEN_H);
    lv_obj_center(s_offRing);
    lv_obj_remove_style(s_offRing, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_offRing, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(s_offRing, 270);              // 0 degrees = 12 o'clock
    lv_arc_set_bg_angles(s_offRing, 0, 360);
    lv_arc_set_angles(s_offRing, 0, 0);
    lv_obj_set_style_arc_opa(s_offRing, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_offRing, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_offRing, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_offRing, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_offRing, UI_EMERG, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_offRing, false, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_offRing, 0, 0);

    lv_obj_t *ver = lv_label_create(sp);            // firmware version (so users can tell what's flashed)
    lv_obj_set_style_text_font(ver, F12(), 0);
    lv_obj_set_style_text_color(ver, UI_DIM, 0);
    lv_label_set_text(ver, "Capsule Radar v" FW_VERSION);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 170);

    // --- weather tile (current conditions + next three days) ---
    lv_obj_t *wp = make_round_panel(s_tileWeather);
    // The entire view cycles the weather mode: there is nothing else to touch here, and a
    // 34px pill 18px from the bottom of a ROUND panel is a genuinely hard target -- measured
    // presses aimed at it landed a few px low, outside it, and read as the button ignoring
    // you. Everything below is left non-clickable so all taps funnel here.
    //
    // CLICKED, not PRESSED: this panel scroll-chains to the tileview, so dragging across it
    // is how you swipe off the view. On PRESSED every such swipe would also cycle the mode.
    // Cancel-on-scroll is exactly what a full-screen target wants -- and the finger drift
    // that made CLICKED unreliable on a small pill no longer matters now position does not.
    lv_obj_add_flag(wp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wp, weather_mode_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_bg_color(wp, lv_color_black(), 0); // hide square radar-tile bounds on AMOLED
    s_weatherTitle = make_tile_title(wp, "WETTERRADAR");
    lv_obj_set_style_bg_color(s_weatherTitle, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_weatherTitle, 170, 0);
    lv_obj_set_style_pad_left(s_weatherTitle, 8, 0);
    lv_obj_set_style_pad_right(s_weatherTitle, 8, 0);
    lv_obj_set_style_pad_top(s_weatherTitle, 2, 0);
    lv_obj_set_style_pad_bottom(s_weatherTitle, 2, 0);
    lv_obj_set_style_radius(s_weatherTitle, 8, 0);
    s_weatherNow = lv_label_create(wp);
    lv_obj_set_width(s_weatherNow, 330);
    lv_obj_set_style_text_font(s_weatherNow, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_weatherNow, UI_INK, 0);
    lv_obj_set_style_text_align(s_weatherNow, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weatherNow, "Keine Vorhersage");
    lv_obj_align(s_weatherNow, LV_ALIGN_TOP_MID, 0, 64);

    s_weatherMeta = lv_label_create(wp);
    lv_obj_set_width(s_weatherMeta, 380);
    lv_obj_set_style_text_font(s_weatherMeta, F14(), 0);
    lv_obj_set_style_text_color(s_weatherMeta, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_weatherMeta, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weatherMeta, "Warte auf WLAN-Daten...");
    lv_obj_align(s_weatherMeta, LV_ALIGN_TOP_MID, 0, 150);

    s_weatherDays = lv_label_create(wp);
    lv_obj_set_width(s_weatherDays, 390);
    lv_obj_set_style_text_font(s_weatherDays, F16(), 0);
    lv_obj_set_style_text_color(s_weatherDays, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_weatherDays, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_weatherDays, "");
    lv_obj_align(s_weatherDays, LV_ALIGN_TOP_LEFT, 42, 234);
    // Legacy formatted labels are retained only to avoid touching older data-update
    // plumbing; the redesigned forecast uses independent aligned objects below.
    lv_obj_add_flag(s_weatherNow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weatherMeta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weatherDays, LV_OBJ_FLAG_HIDDEN);

    // Default mode: genuine precipitation radar with aviation-style overlays.
    s_wxAirport = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxAirport, F14(), 0);
    lv_obj_set_style_text_color(s_wxAirport, UI_SOFT, 0);
    lv_obj_set_style_bg_color(s_wxAirport, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxAirport, 160, 0);
    lv_obj_set_style_pad_left(s_wxAirport, 6, 0);
    lv_obj_set_style_pad_right(s_wxAirport, 6, 0);
    lv_obj_set_style_radius(s_wxAirport, 6, 0);
    lv_label_set_text(s_wxAirport, "RADARMITTE");
    lv_obj_align(s_wxAirport, LV_ALIGN_TOP_MID, 0, 46);

    s_wxCanvas = lv_canvas_create(wp);
    lv_obj_set_size(s_wxCanvas, WX_RADAR_SIZE, WX_RADAR_SIZE);
    lv_obj_align(s_wxCanvas, LV_ALIGN_TOP_MID, 0, 52);
    // Not clickable: taps pass through to the panel, which owns the whole view.
    lv_obj_clear_flag(s_wxCanvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(s_wxCanvas);

    s_wxStatus = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxStatus, F14(), 0);
    lv_obj_set_style_text_color(s_wxStatus, UI_DIM, 0);
    lv_label_set_text(s_wxStatus, "LADE WETTERRADAR...");
    lv_obj_align(s_wxStatus, LV_ALIGN_TOP_MID, 0, 222);

    const int ringSize[3] = { 360, 240, 120 };
    for (int i = 0; i < 3; ++i) {
        s_wxRings[i] = lv_obj_create(wp);
        lv_obj_remove_style_all(s_wxRings[i]);
        lv_obj_set_size(s_wxRings[i], ringSize[i], ringSize[i]);
        lv_obj_align(s_wxRings[i], LV_ALIGN_TOP_MID, 0, 52 + (360 - ringSize[i]) / 2);
        lv_obj_set_style_radius(s_wxRings[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(s_wxRings[i], UI_GREEN, 0);
        lv_obj_set_style_border_opa(s_wxRings[i], i == 0 ? 180 : 90, 0);
        lv_obj_set_style_border_width(s_wxRings[i], 1, 0);
        lv_obj_clear_flag(s_wxRings[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    s_wxNorth = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxNorth, F12(), 0);
    lv_obj_set_style_text_color(s_wxNorth, UI_GREEN, 0);
    lv_label_set_text(s_wxNorth, "N");
    lv_obj_align(s_wxNorth, LV_ALIGN_TOP_MID, 0, 58);
    s_wxCenter = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxCenter, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_wxCenter, UI_INK, 0);
    lv_label_set_text(s_wxCenter, "+");
    lv_obj_align(s_wxCenter, LV_ALIGN_TOP_MID, 0, 219);
    s_wxRange = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxRange, F12(), 0);
    lv_obj_set_style_text_color(s_wxRange, UI_GREEN, 0);
    lv_label_set_text(s_wxRange, "75 KM");
    lv_obj_align(s_wxRange, LV_ALIGN_TOP_MID, 128, 225);

    s_wxFooter = lv_label_create(wp);
    lv_obj_set_width(s_wxFooter, 360);
    lv_obj_set_style_text_font(s_wxFooter, F16(), 0);
    lv_obj_set_style_text_color(s_wxFooter, UI_INK, 0);
    lv_obj_set_style_text_align(s_wxFooter, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wxFooter, "WETTERDATEN AUSSTEHEND");
    lv_obj_align(s_wxFooter, LV_ALIGN_TOP_MID, 0, 326);
    lv_obj_set_style_bg_color(s_wxFooter, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxFooter, 185, 0);
    lv_obj_set_style_pad_hor(s_wxFooter, 8, 0);
    lv_obj_set_style_radius(s_wxFooter, 7, 0);
    s_wxMeta = lv_label_create(wp);
    lv_obj_set_width(s_wxMeta, 360);
    lv_obj_set_style_text_font(s_wxMeta, F14(), 0);
    lv_obj_set_style_text_color(s_wxMeta, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_wxMeta, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wxMeta, "");
    lv_obj_align(s_wxMeta, LV_ALIGN_TOP_MID, 0, 351);
    lv_obj_set_style_bg_color(s_wxMeta, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxMeta, 185, 0);
    lv_obj_set_style_pad_hor(s_wxMeta, 8, 0);
    lv_obj_set_style_radius(s_wxMeta, 7, 0);
    s_wxAttrib = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxAttrib, F12(), 0);
    lv_obj_set_style_text_color(s_wxAttrib, UI_DIM, 0);
    lv_label_set_text(s_wxAttrib, "WARTE AUF RADARDATEN");
    lv_obj_align(s_wxAttrib, LV_ALIGN_TOP_MID, 0, 376);
    lv_obj_set_style_bg_color(s_wxAttrib, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxAttrib, 170, 0);
    lv_obj_set_style_pad_hor(s_wxAttrib, 6, 0);
    lv_obj_set_style_radius(s_wxAttrib, 6, 0);

    // Forecast mode: independent, aligned objects instead of a tiny text table.
    s_fcCurrent = lv_label_create(wp);
    lv_obj_set_style_text_font(s_fcCurrent, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_fcCurrent, UI_INK, 0);
    lv_label_set_text(s_fcCurrent, "-- \xC2\xB0" "C");
    lv_obj_align(s_fcCurrent, LV_ALIGN_TOP_MID, 0, 68);
    s_fcCondition = lv_label_create(wp);
    lv_obj_set_style_text_font(s_fcCondition, F16(), 0);
    lv_obj_set_style_text_color(s_fcCondition, UI_SOFT, 0);
    lv_label_set_text(s_fcCondition, "Warte auf Daten");
    lv_obj_align(s_fcCondition, LV_ALIGN_TOP_MID, 0, 105);

    const char *metricNames[3] = { "GEFÜHLT", "FEUCHTE", "WIND" };
    const int colX[3] = { -122, 0, 122 };
    for (int i = 0; i < 3; ++i) {
        s_fcMetricName[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcMetricName[i], F12(), 0);
        lv_obj_set_style_text_color(s_fcMetricName[i], UI_DIM, 0);
        lv_label_set_text(s_fcMetricName[i], metricNames[i]);
        lv_obj_align(s_fcMetricName[i], LV_ALIGN_TOP_MID, colX[i], 150);
        s_fcMetricValue[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcMetricValue[i], F16(), 0);
        lv_obj_set_style_text_color(s_fcMetricValue[i], UI_INK, 0);
        lv_label_set_text(s_fcMetricValue[i], "-");
        lv_obj_align(s_fcMetricValue[i], LV_ALIGN_TOP_MID, colX[i], 170);

        s_fcDay[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcDay[i], F16(), 0);
        lv_obj_set_style_text_color(s_fcDay[i], UI_GREEN, 0);
        lv_label_set_text(s_fcDay[i], "---");
        lv_obj_align(s_fcDay[i], LV_ALIGN_TOP_MID, colX[i], 226);
        s_fcDayCondition[i] = lv_label_create(wp);
        lv_obj_set_width(s_fcDayCondition[i], 116);
        lv_obj_set_style_text_font(s_fcDayCondition[i], F12(), 0);
        lv_obj_set_style_text_color(s_fcDayCondition[i], UI_SOFT, 0);
        lv_obj_set_style_text_align(s_fcDayCondition[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_fcDayCondition[i], LV_LABEL_LONG_WRAP);
        lv_label_set_text(s_fcDayCondition[i], "");
        lv_obj_align(s_fcDayCondition[i], LV_ALIGN_TOP_MID, colX[i], 254);
        s_fcDayTemp[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcDayTemp[i], F14(), 0);
        lv_obj_set_style_text_color(s_fcDayTemp[i], UI_INK, 0);
        lv_label_set_text(s_fcDayTemp[i], "");
        lv_obj_align(s_fcDayTemp[i], LV_ALIGN_TOP_MID, colX[i], 292);
        s_fcDayRain[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcDayRain[i], F12(), 0);
        lv_obj_set_style_text_color(s_fcDayRain[i], lv_color_hex(0x4DDCFF), 0);
        lv_label_set_text(s_fcDayRain[i], "");
        lv_obj_align(s_fcDayRain[i], LV_ALIGN_TOP_MID, colX[i], 320);
    }
    s_fcUpdated = lv_label_create(wp);
    lv_obj_set_style_text_font(s_fcUpdated, F12(), 0);
    lv_obj_set_style_text_color(s_fcUpdated, UI_DIM, 0);
    lv_label_set_text(s_fcUpdated, "");
    lv_obj_align(s_fcUpdated, LV_ALIGN_TOP_MID, 0, 365);

    s_weatherModeBtn = lv_btn_create(wp);
    lv_obj_set_size(s_weatherModeBtn, 164, 34);
    lv_obj_align(s_weatherModeBtn, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(s_weatherModeBtn, 17, 0);
    lv_obj_set_style_bg_color(s_weatherModeBtn, UI_PANEL, 0);
    lv_obj_set_style_border_color(s_weatherModeBtn, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_weatherModeBtn, 1, 0);
    lv_obj_clear_flag(s_weatherModeBtn, LV_OBJ_FLAG_SCROLL_CHAIN);
    // Purely a label showing which mode comes next -- the panel handles the tap, so this
    // must NOT be clickable or it would swallow presses that land on it.
    lv_obj_clear_flag(s_weatherModeBtn, LV_OBJ_FLAG_CLICKABLE);
    s_weatherModeLbl = lv_label_create(s_weatherModeBtn);
    lv_obj_set_style_text_font(s_weatherModeLbl, F12(), 0);
    lv_obj_set_style_text_color(s_weatherModeLbl, UI_GREEN, 0);
    lv_label_set_text(s_weatherModeLbl, "3-TAGE-VORHERSAGE");
    lv_obj_center(s_weatherModeLbl);

    build_settings();

    build_photo();
    lv_obj_set_tile_id(s_tv, 2, 0, LV_ANIM_OFF);   // start on the radar (column 2)

    ui_splash_show();   // branded boot splash on top (auto-fades)
}
