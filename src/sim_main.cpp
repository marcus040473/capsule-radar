// Native (Mac/Linux) LVGL simulator for Capsule Radar.
// Runs the SAME LVGL UI (include/lv_conf.h + ui_boot) in an SDL2 window — no
// hardware, no Arduino_GFX. Only compiled for the `native` PlatformIO env.
//
//   pio run -e native            # build
//   pio run -e native -t exec    # build + run (or run .pio/build/native/program)
//
// Note: the real panel is a 466x466 *round* AMOLED; this square window shows the
// full buffer, so the corners (hidden on the device) are visible here.
#include <SDL.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "radar_view.h"
#include "ui.h"
#include "touch_queue.h"
#include "route.h"
#include "weather.h"
#include "wx_radar.h"
#include "cloud_image.h"
#include "aircraft.h"
#include "map_bg.h"
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SIM_W SCREEN_W   // 466
#define SIM_H SCREEN_H   // 466

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex = NULL;

// Mouse acts as the touch input device (left button = press), through the same TouchQueue
// path as the device (display.cpp): sampled between flush strips as well as when LVGL
// reads, and replayed to LVGL in order.
static TouchQueue s_touchQ;
static Uint32     s_touchLastMs = 0;
static bool       s_touchReplaying = false;

static void sim_touch_sample(bool force) {
    const Uint32 now = SDL_GetTicks();
    if (!force && now - s_touchLastMs < TOUCH_SAMPLE_MS) return;
    s_touchLastMs = now;
    int x, y;
    const Uint32 btn = SDL_GetMouseState(&x, &y);
    if (btn & SDL_BUTTON(SDL_BUTTON_LEFT)) s_touchQ.add(TouchQueue::DOWN, (int16_t)x, (int16_t)y, now);
    else                                   s_touchQ.add(TouchQueue::UP, 0, 0, now);
}

// LVGL -> SDL texture. Accumulate dirty areas, present once per refresh.
static void sdl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    sim_touch_sample(false);
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    SDL_Rect r = { area->x1, area->y1, w, h };
    SDL_UpdateTexture(s_tex, &r, px, w * (int)sizeof(lv_color_t));
    if (lv_disp_flush_is_last(drv)) {
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }
    lv_disp_flush_ready(drv);
}

static void sdl_mouse_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    if (!s_touchReplaying) sim_touch_sample(true);
    TouchQueue::Sample s;
    if (!s_touchQ.next(s)) s = s_touchQ.delivered();
    data->point.x = s.x;
    data->point.y = s.y;
    data->state = s.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !s_touchQ.empty();
    s_touchReplaying = data->continue_reading;
}

// ---- mock ADS-B data (sim only): 6 aircraft near Dénia that drift along track --
static std::vector<Aircraft> g_mockAcs;
static std::vector<Aircraft> g_mockInit;
static RadarSettings g_set;

static Aircraft mk(const char *call, const char *hex, double distKm, double brgDeg,
                   float altFt, float track, float gsKt, int sq) {
    Aircraft a;
    a.flight = call;
    a.hex = hex;
    const double br = brgDeg * M_PI / 180.0;
    const double latR = HOME_LAT_DEFAULT * M_PI / 180.0;
    a.lat = HOME_LAT_DEFAULT + (distKm * cos(br)) / 111.0;
    a.lon = HOME_LON_DEFAULT + (distKm * sin(br)) / (111.0 * cos(latR));
    a.altBaro = altFt;
    a.onGround = false;
    a.track = track;
    a.gs = gsKt;
    a.squawk = sq;
    return a;
}

// Map under the radar: the device downloads CARTO tiles; the simulator paints test tiles
// instead -- a grid every 0.25 degrees and a circle exactly on the radar's third ring
// (160/218 of the range), which shows that the map projection lines up with the scope.
static uint8_t s_simTile[MAP_TILE_PX * MAP_TILE_PX];
static void sim_map_tile(const MapTile &t) {
    const double W = (double)MAP_TILE_PX * (1 << t.z);
    const double ringKm = g_set.rangeKm * 160.0 / 218.0;
    const double latH = g_set.homeLat * M_PI / 180.0, lonH = g_set.homeLon * M_PI / 180.0;
    const double kmPerPx = 40075.016686 * cos(latH) / W;
    const double gridTol = 0.0015 * (1 << (12 - (t.z > 12 ? 12 : t.z)));
    for (int py = 0; py < MAP_TILE_PX; ++py)
        for (int px = 0; px < MAP_TILE_PX; ++px) {
            const double wx = (double)t.x * MAP_TILE_PX + px, wy = (double)t.y * MAP_TILE_PX + py;
            const double lon = wx / W * 360.0 - 180.0;
            const double lat = atan(sinh(M_PI * (1.0 - 2.0 * wy / W))) * 180.0 / M_PI;
            const double la = lat * M_PI / 180.0, lo = lon * M_PI / 180.0;
            const double a = sin((la - latH) / 2) * sin((la - latH) / 2) +
                             cos(la) * cos(latH) * sin((lo - lonH) / 2) * sin((lo - lonH) / 2);
            const double dKm = 2 * 6371.0 * asin(sqrt(a));
            uint8_t v = 240;                                                  // OSM-like light ground
            if (fabs(fmod(lat + 360.0, 0.25)) < gridTol || fabs(fmod(lon + 360.0, 0.25)) < gridTol) v = 185;   // road casing
            if (fabs(dKm - ringKm) < kmPerPx * 1.5) v = 40;                   // dark like map text
            s_simTile[py * MAP_TILE_PX + px] = v;
        }
}
static void sim_map_refresh(void) {
    map_bg_begin();
    map_bg_set_view(g_set.homeLat, g_set.homeLon, g_set.rangeKm, (float)g_set.rotationDeg);
    map_bg_ui_tick(1);                          // hand over anything pending first
    const int n = map_bg_replan();
    for (int i = 0; i < n; ++i) {
        MapTile t;
        if (!map_bg_tile(i, &t)) break;
        sim_map_tile(t);
        map_bg_compose_tile(i, s_simTile);
        map_bg_ui_tick(1);                      // tint + hand to the radar (like loop() on core 1)
    }
    printf("[sim] map: %d test tiles\n", n);
}
static void sim_range_cb(float km) { g_set.rangeKm = km; radar::update(g_mockAcs, g_set); ui_set_range_km(km); sim_map_refresh(); }

static void mock_init() {
    g_set.homeLat = HOME_LAT_DEFAULT;
    g_set.homeLon = HOME_LON_DEFAULT;
    g_set.rangeKm = RANGE_KM_DEFAULT;
    g_set.rotationDeg = 0.0;
    g_mockAcs.clear();
    // in-range (< 50 km)
    g_mockAcs.push_back(mk("RESCUE51", "306006",  3.0, 170.0,  1200,  20,  42, 7700));
    g_mockAcs.push_back(mk("IBE3174",  "301001",  5.0,  60.0,  6200, 250, 286, 4655));
    g_mockAcs.push_back(mk("EC-ABC",   "305005",  7.0, 200.0,  2800,  70,  96, 7000));
    g_mockAcs.push_back(mk("VLG28PK",  "303003",  8.0,   0.0, 34000, 180, 448, 1000));
    g_mockAcs.push_back(mk("RYR4521",  "302002", 11.0, 135.0, 11025, 225, 412, 3421));
    // out of range (> 50 km) -> shown as rim arrows pointing their way
    g_mockAcs.push_back(mk("AFR1234",  "401001", 55.0,  30.0, 37000, 210, 470, 1000));
    g_mockAcs.push_back(mk("DLH88X",   "402002", 62.0, 300.0, 39000, 120, 455, 2000));
    g_mockAcs.push_back(mk("BAW777",   "403003", 75.0, 200.0, 41000,  20, 480, 3000));
    g_mockAcs.push_back(mk("UAE9",     "404004", 90.0, 110.0, 38000, 290, 490, 4000));
    g_mockInit = g_mockAcs;
}

static void mock_step(double dt) {
    const double latR = HOME_LAT_DEFAULT * M_PI / 180.0;
    for (size_t i = 0; i < g_mockAcs.size(); ++i) {
        Aircraft &a = g_mockAcs[i];
        const double stepKm = (double)a.gs * 1.852 * (dt / 3600.0);   // kt -> km in dt s
        const double br = a.track * M_PI / 180.0;
        a.lat += (stepKm * cos(br)) / 111.0;
        a.lon += (stepKm * sin(br)) / (111.0 * cos(a.lat * M_PI / 180.0));
        const double dLat = (a.lat - HOME_LAT_DEFAULT) * 111.0;
        const double dLon = (a.lon - HOME_LON_DEFAULT) * 111.0 * cos(latR);
        if (sqrt(dLat * dLat + dLon * dLon) > RANGE_KM_DEFAULT * 1.04) {
            a.lat = g_mockInit[i].lat;     // respawn so the scene stays populated
            a.lon = g_mockInit[i].lon;
        }
    }
}

// Idle clock. Screenshots use the classic watch pose (10:09:32.5) so every run matches;
// the CLOCK_DEMO GIF runs from 10:09:30 at the real pace.
static bool sim_clock_fixed(ClockHands &out) { out = clock_hands(10, 9, 32, 500); return true; }
static bool sim_clock_running(ClockHands &out) {
    const Uint32 ms = 30000 + SDL_GetTicks();
    out = clock_hands(10, 9 + (int)(ms / 60000), (int)(ms / 1000 % 60), (int)(ms % 1000));
    return true;
}

// Save what LVGL last rendered as <prefix>-<name>.bmp.
static void save_shot(const char *prefix, const char *name) {
    int ow, oh;
    SDL_GetRendererOutputSize(s_ren, &ow, &oh);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return;
    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    char path[300];
    snprintf(path, sizeof(path), "%s-%s.bmp", prefix, name);
    SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    printf("[sim] saved %s\n", path);
}

// Settings page mock (the device keeps these in main.cpp + NVS).
static int s_simSet[UI_SET_COUNT] = { 180, 2, 2, 0, 2, 0, 1, 1, 0, 1, 1, 0, 1 };
static const char *const s_simNets[] = { "FRITZ!Box 7590 XY", "Handy-Hotspot" };
static int         sim_net_count(void) { return 2; }
static const char *sim_net_name(int i) { return (i >= 0 && i < 2) ? s_simNets[i] : ""; }
static void        sim_net_add(void) { printf("[sim] add WiFi network (device: restarts into the portal)\n"); }
// a test pattern for the photo page (the device decodes the uploaded JPEG instead)
static uint16_t s_simPhoto[SIM_W * SIM_H];
static void sim_make_photo(void) {
    for (int y = 0; y < SIM_H; ++y)
        for (int x = 0; x < SIM_W; ++x) {
            const int r = x * 31 / SIM_W, g = y * 63 / SIM_H, b = 31 - r;
            const int dx = x - SIM_W / 2, dy = y - SIM_H / 2;
            const bool ring = abs(dx * dx + dy * dy - 120 * 120) < 900;
            s_simPhoto[y * SIM_W + x] = ring ? 0xFFFF : (uint16_t)((r << 11) | (g << 5) | b);
        }
}
static int  sim_setting_get(int id) { return (id >= 0 && id < UI_SET_COUNT) ? s_simSet[id] : 0; }
static void sim_setting_set(int id, int v, bool save) {
    if (id < 0 || id >= UI_SET_COUNT) return;
    s_simSet[id] = v;
    if (id == UI_SET_UNITS) { ui_set_units(v); ui_on_data_updated(); }
    if (id == UI_SET_ROTATE) printf("[sim] rotation %d deg (device: display::setRotation)\n", v * 90);
    if (id == UI_SET_TRAIL) radar::setTrailLength(v);
    if (id == UI_SET_SWEEP) radar::setSweepEnabled(v != 0);
    if (id == UI_SET_AIRPORTS) radar::setAirportsEnabled(v != 0);
    printf("[sim] setting %d = %d%s\n", id, v, save ? " (saved)" : "");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IOLBF, 0);  // line-buffered: logs appear even when piped to a file
    if (getenv("BIGTEXT")) {           // BIGTEXT=1 ./program — preview the large-text mode
        ui_set_large_text(true);
        radar::setLargeText(true);
    }
    const char *shotPath = (argc >= 3 && strcmp(argv[1], "--shot") == 0) ? argv[2] : NULL;
    const char *gifPath  = (argc >= 3 && strcmp(argv[1], "--gif")  == 0) ? argv[2] : NULL;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[sim] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    s_win = SDL_CreateWindow("Capsule Radar (sim)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             SIM_W, SIM_H, SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_win || !s_ren) {
        printf("[sim] window/renderer creation failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(s_ren, SIM_W, SIM_H);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
    printf("[sim] SDL video driver: %s\n", SDL_GetCurrentVideoDriver());

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[SIM_W * 100];
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SIM_W * 100);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = sdl_flush;
    disp_drv.hor_res  = SIM_W;
    disp_drv.ver_res  = SIM_H;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    ui_create();
    ui_set_range_cb(sim_range_cb);   // on-screen zoom button
    ui_set_range_km(RANGE_KM_DEFAULT);
    ui_set_poweroff_cb([]() {});   // mock: represent a PMIC-fitted board (shows the Stats power-off button)
    ui_set_settings_cb(sim_setting_get, sim_setting_set);   // mock settings page values
    ui_set_settings_wifi_cb(sim_net_count, sim_net_name, sim_net_add);
    ui_set_settings_url("http://192.168.1.42/");
    ui_set_gps_present(true);          // mock a -G board: GPS switch + signal line
    ui_set_gps(2, 9, 120.0f);
    if (!getenv("NOMAP")) { mock_init(); sim_map_refresh(); }   // NOMAP=1: the plain scope
    sim_make_photo();
    ui_set_user_photo(s_simPhoto);
    mock_init();
    radar::update(g_mockAcs, g_set);
    ui_on_data_updated();
    WeatherSnapshot forecast = {};
    forecast.valid = true;
    snprintf(forecast.updated, sizeof(forecast.updated), "14:00");
    forecast.code = 1; forecast.tempC = 27; forecast.feelsC = 29;
    forecast.humidity = 61; forecast.windKmh = 18; forecast.windDeg = 85;
    const char *dates[] = {"2026-06-08", "2026-06-09", "2026-06-10", "2026-06-11"};
    const int codes[] = {1, 2, 61, 0};
    const float lows[] = {20, 19, 18, 21}, highs[] = {28, 27, 24, 29};
    const int rain[] = {10, 20, 75, 5};
    forecast.dayCount = 4;
    for (int i = 0; i < 4; ++i) {
        snprintf(forecast.days[i].date, sizeof(forecast.days[i].date), "%s", dates[i]);
        forecast.days[i].code = codes[i]; forecast.days[i].tempMinC = lows[i];
        forecast.days[i].tempMaxC = highs[i]; forecast.days[i].rainChance = rain[i];
    }
    weather_store(forecast);
    wx_radar_begin();
    if (uint16_t *wx = wx_radar_back_buffer()) {
        for (int y = 0; y < WX_RADAR_SIZE; ++y) for (int x = 0; x < WX_RADAR_SIZE; ++x) {
            const int centre = WX_RADAR_SIZE / 2;
            const int dx = x - centre, dy = y - centre;
            uint16_t c = 0;
            if (dx * dx + dy * dy < (centre - 2) * (centre - 2)) {
                const int a = (x - 250) * (x - 250) + (y - 115) * (y - 115);
                const int b = (x - 105) * (x - 105) + (y - 245) * (y - 245);
                if (a < 1500) c = a < 350 ? 0xFBE0 : (a < 800 ? 0xFFE0 : 0x07E0);
                if (b < 900) c = b < 250 ? 0xF800 : 0x07E0;
            }
            wx[y * WX_RADAR_SIZE + x] = c;
        }
        wx_radar_commit(1784397600UL, HOME_LAT_DEFAULT, HOME_LON_DEFAULT);
    }
    // Representative Meteosat-style mock. The native simulator has no network
    // clients, so populate the shared satellite buffer with a dark Earth field
    // and layered cloud bands that exercise the exact same UI path as hardware.
    cloud_image_begin();
    if (uint16_t *sat = cloud_image_back_buffer()) {
        const int centre = WX_RADAR_SIZE / 2;
        for (int y = 0; y < WX_RADAR_SIZE; ++y) for (int x = 0; x < WX_RADAR_SIZE; ++x) {
            const int dx = x - centre, dy = y - centre;
            uint16_t c = 0;
            if (dx * dx + dy * dy < (centre - 2) * (centre - 2)) {
                const float wave1 = y - (116.0f + 0.42f * x + 20.0f * sinf(x * 0.035f));
                const float wave2 = y - (320.0f - 0.52f * x + 14.0f * sinf(x * 0.055f));
                const float cloud = expf(-(wave1 * wave1) / 950.0f) +
                                    0.75f * expf(-(wave2 * wave2) / 620.0f);
                const float texture = 0.72f + 0.28f * sinf(x * 0.19f + y * 0.11f) *
                                                       sinf(x * 0.047f - y * 0.16f);
                const float v = fminf(1.0f, cloud * texture);
                const int r = (int)(8 + 235 * v);
                const int g = (int)(23 + 225 * v);
                const int b = (int)(42 + 205 * v);
                c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            }
            sat[y * WX_RADAR_SIZE + x] = c;
        }
        cloud_image_commit(1784397900UL, HOME_LAT_DEFAULT, HOME_LON_DEFAULT);
    }
    ui_on_data_updated();
    printf("[sim] Capsule Radar simulator running (%dx%d) with 6 mock aircraft.\n", SIM_W, SIM_H);

    Uint32 last = SDL_GetTicks();
    Uint32 lastData = last;
    const Uint32 start = last;
    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_t) radar::cycleTheme();  // T = switch theme
        }
        Uint32 now = SDL_GetTicks();
        lv_tick_inc(now - last);
        last = now;
        if (now - lastData >= 1000) {       // simulate a 1 Hz ADS-B poll
            lastData = now;
            mock_step(1.0);
            radar::update(g_mockAcs, g_set);
            ui_on_data_updated();
            char clk[8];
            snprintf(clk, sizeof(clk), "14:%02d", (int)((now / 1000) % 60));  // mock clock
            ui_set_status(true, true, -58, clk);   // mock: connected, fresh, strong signal
            ui_set_battery(78, false, true);   // mock battery
            ui_set_date("08.06.2026");        // mock date
            ui_set_netinfo("Einstellungen unter\ncapsuleradar.local\n192.168.1.42");  // mock net info
            ui_set_feed_source("api.airplanes.live");                          // mock feed source
        }
        // fulfil route lookups with a mock (the sim has no network)
        char wc[12];
        if (route_pending(wc, sizeof(wc))) {
            static const char *cities[] = { "Madrid", "London", "Paris", "Berlin",
                                            "Rome", "Lisbon", "Amsterdam", "Dublin" };
            int h = 0;
            for (const char *p = wc; *p; ++p) h += (unsigned char)*p;
            route_store(wc, cities[h % 8], cities[(h / 2 + 3) % 8]);
        }
        lv_timer_handler();

        // headless screenshot mode: grab the boot splash early (before it fades)
        static bool splashSaved = false;
        if (shotPath && !splashSaved && now - start > 900) {
            splashSaved = true;
            lv_refr_now(NULL);
            SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
            int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                char path[300]; snprintf(path, sizeof(path), "%s-splash.bmp", shotPath);
                SDL_SaveBMP(surf, path); SDL_FreeSurface(surf);
                printf("[sim] saved %s\n", path);
            }
        }

        // animated GIF capture (--gif <prefix>): grab frames after the splash fades.
        // CLOCK_DEMO=1: the radar turns into the idle clock at the first frame and back at
        // frame 55 (~3 s), so the GIF shows both animations.
        static Uint32 lastGif = 0;
        static int gifFrame = 0;
        static const bool clockDemo = getenv("CLOCK_DEMO") != NULL;
        if (gifPath && now - start > 3000 && now - lastGif >= 55) {
            lastGif = now;
            if (clockDemo && gifFrame == 0) { radar::setClockTimeSource(sim_clock_running); radar::setClock(true); }
            if (clockDemo && gifFrame == 55) radar::setClock(false);
            if (gifFrame >= (clockDemo ? 90 : 60)) { run = false; }
            else {
                SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                    char path[300]; snprintf(path, sizeof(path), "%s-%03d.bmp", gifPath, gifFrame);
                    SDL_SaveBMP(surf, path); SDL_FreeSurface(surf);
                }
                gifFrame++;
            }
        }

        // headless screenshot mode (--shot <prefix>): settle, then grab all views/themes
        if (shotPath && now - start > 4000) {
            for (int k = 0; k < 150; ++k) {              // fast-forward to build up the flow map
                mock_step(1.0);
                radar::update(g_mockAcs, g_set);
            }
            radar::select(0);                            // select an aircraft so the card shows
            ui_on_data_updated();
            { char wc[12]; if (route_pending(wc, sizeof(wc))) route_store(wc, "Puerto del Rosario", "Frankfurt am Main"); }   // long: exercises the card's dot-out
            ui_on_data_updated();                        // pick up the mock route for the card
            int ow, oh;
            SDL_GetRendererOutputSize(s_ren, &ow, &oh);
            struct Shot { const char *name; int view; int theme; bool forecast; int units; };
            const Shot shots[11] = {
                { "radar",  0, THEME_PHOSPHOR, false, 0 },
                { "orb", 0, THEME_ORB, false, 0 },
                { "amber",  0, THEME_AMBER, false, 0 },
                { "military",0, THEME_MILITARY, false, 0 },
                { "metric", 0, THEME_PHOSPHOR, false, 1 },   // scope labels + card in metres
                { "list",   1, THEME_PHOSPHOR, false, 0 },
                { "stats",  2, THEME_PHOSPHOR, false, 0 },
                { "weather",3, THEME_PHOSPHOR, false, 0 },
                { "forecast",3, THEME_PHOSPHOR, true, 0 },
                { "settings",4, THEME_PHOSPHOR, false, 0 },
                { "photo",  5, THEME_PHOSPHOR, false, 0 },
            };
            for (int v = 0; v < 11; ++v) {
                ui_set_units(shots[v].units);
                ui_set_range_km(g_set.rangeKm);   // like handleUnits() on the device: zoom label,
                ui_on_data_updated();             // then the card/list in the new units
                radar::setTheme(shots[v].theme);
                ui_set_weather_forecast(shots[v].forecast);
                ui_show_view(shots[v].view);
                lv_refr_now(NULL);                       // force the view into the buffer
                SDL_RenderClear(s_ren);
                SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                    char path[300];
                    snprintf(path, sizeof(path), "%s-%s.bmp", shotPath, shots[v].name);
                    SDL_SaveBMP(surf, path);
                    SDL_FreeSurface(surf);
                    printf("[sim] saved %s\n", path);
                }
            }
            // the idle clock, fully entered, on the radar view
            radar::setClockTimeSource(sim_clock_fixed);
            ui_show_view(0);
            const struct { const char *name; int theme; } clocks[3] = {
                { "clock", THEME_PHOSPHOR }, { "clock-orb", THEME_ORB }, { "clock-military", THEME_MILITARY },
            };
            for (const auto &c : clocks) {
                radar::setTheme(c.theme);
                radar::setClock(true);
                lv_tick_inc(CLOCK_ENTER_MS + 100);       // jump to the end of the entry
                lv_timer_handler();
                lv_refr_now(NULL);
                save_shot(shotPath, c.name);
                radar::setClock(false);
                lv_tick_inc(CLOCK_EXIT_MS + 100);
                lv_timer_handler();
            }
            ui_set_units(0);
            radar::setTheme(THEME_PHOSPHOR);
            run = false;
        }
        SDL_Delay(5);
    }

    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}
