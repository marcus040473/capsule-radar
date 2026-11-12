#pragma once
#include <stdint.h>
// M3 UI: swipeable views (radar / list / stats) + tap-to-inspect detail card.
// Pure LVGL, portable (device + SDL simulator). Builds on top of radar_view.
void ui_create(void);            // build the whole UI on the active screen
bool ui_sweep_visible(void);     // radar tile shown and not mid-swipe (sweep compositor)
int  ui_sweep_masks(int16_t out[][5], int max);   // logical x1,y1,x2,y2,radius: rounded boxes the sweep must not paint over
void ui_on_data_updated(void);   // refresh card/list/stats after radar::update()
void ui_show_view(int idx);      // 0 = radar, 1 = list, 2 = stats, 3 = weather, 4 = settings (left of the radar)
void ui_set_view_changed_cb(void (*cb)(int idx));  // fired on swipe — lets main remember the last view
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock);  // HUD: signal bars (count=RSSI, colour: red=down, amber=stale feed, white=ok) + clock
void ui_set_battery(int pct, bool charging, bool present);  // top HUD battery indicator
void ui_set_date(const char *date);  // top HUD date line (e.g. "08 Jun 2026")
void ui_set_netinfo(const char *line);  // stats view footer: how to reach the config page
void ui_set_feed_source(const char *host);  // Stats "Feed" line: which ADS-B provider served the last data
void ui_set_gps(int state, int sats, float altM);   // GPS indicator: state 0=off/hidden 1=acquiring 2=fix; HUD icon + Stats line (altM = GPS altitude, NAN if unknown)
void ui_splash_show(void);  // branded boot splash (auto-fades, covers init time)
void ui_set_range_cb(void (*cb)(float km));  // on-screen zoom button -> notify main
void ui_set_range_km(float km);              // update the zoom button label / sync the cycle
void ui_set_units(int preset);               // 0 = Aviation (ft,kt,km) · 1 = Metric (m,km/h,km) · 2 = Imperial (ft,mph,mi)
void ui_set_temp_unit(int t);                // weather temperature: 0 = follow Units · 1 = °C · 2 = °F
void ui_set_large_text(bool on);             // accessibility: bigger fonts everywhere. Call BEFORE ui_create()
void ui_set_weather_forecast(bool forecast); // false = WX radar, true = 3-day forecast
void ui_set_poweroff_cb(void (*cb)(void));   // Stats view: hold the Power off button

// ---- Einstellungen-Seite (links vom Radar) ----
// Die Seite liest die aktuellen Werte über den Getter (beim Aufbau und jedes Mal, wenn sie
// eingeblendet wird -- so zeigt sie auch Änderungen von der Webseite) und meldet Änderungen
// über den Setter. save=false: Schieberegler wird noch gezogen (nur live anwenden),
// save=true: endgültiger Wert (anwenden + speichern). Das Design stellt die Seite selbst
// über radar::setTheme() um (das speichert main.cpp schon über setThemeChangedCb).
enum UiSetting {
    UI_SET_BRIGHT,      // 5..255
    UI_SET_VOLUME,      // 0..100
    UI_SET_MUTE,        // 0/1
    UI_SET_UNITS,       // 0..2  (Luftfahrt / Metrisch / Imperial)
    UI_SET_TRAIL,       // 0..3  (Aus / Kurz / Mittel / Lang)
    UI_SET_SWEEP,       // 0/1
    UI_SET_AIRPORTS,    // 0/1
    UI_SET_HIDEGROUND,  // 0/1
    UI_SET_CLOCK,       // 0/1
    UI_SET_ALERTS,      // 0..2  (Aus / Nur Notfälle / Neue Flugzeuge + Notfälle)
    UI_SET_COUNT
};
void ui_set_settings_cb(int (*get)(int id), void (*set)(int id, int value, bool save));
void ui_set_settings_volume(bool show);      // false: Lautstärke-Regler ausblenden (Buzzer ohne Pegel)
void ui_settings_refresh(void);              // Werte neu einlesen (z. B. nach einer Änderung über die Webseite)
