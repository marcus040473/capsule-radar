#pragma once
// Karte unter dem Radar: OpenStreetMap als dunkler Hintergrund, in der Farbe des Designs
// eingefärbt. Quellen: OpenStreetMap-Standardkarte (ohne Schlüssel, hell -> wird auf dunkel
// umgerechnet) oder, mit einem kostenlosen CARTO-Schlüssel, CARTO "dark_all" (schon dunkel).
//
// Ablauf
//  - Kern 1 (loop): map_bg_set_view() mit Mittelpunkt, Reichweite und Drehung des Radars.
//  - Kern 0 (adsb_task, map_client.cpp): map_bg_plan() wählt Zoomstufe und Kacheln, holt
//    pro Aufruf EINE Kachel, dekodiert sie zu Helligkeit (256 x 256) und setzt sie mit
//    map_bg_compose_tile() an die richtige Stelle des Bildes (Web-Mercator -> Radar).
//  - Kern 1: map_bg_ui_tick() übernimmt jedes neue Zwischenbild, färbt es ein und gibt es
//    dem Radar (radar::setMap). Die Karte baut sich so Kachel für Kachel auf.
// Speicher: zwei Helligkeitsbilder + ein RGB565-Bild = ~0,9 MB PSRAM, kein LVGL-Heap.
#include <stdint.h>
#include <stdbool.h>

#define MAP_TILE_PX 256                 // Standard-Kachelgröße (OSM und CARTO)

enum MapSource { MAP_SRC_OSM = 0, MAP_SRC_CARTO = 1 };

struct MapTile { int z, x, y; };        // x schon auf 0..2^z-1 gefaltet

bool map_bg_begin(void);                                    // Puffer anlegen (PSRAM)
void map_bg_set_view(double lat, double lon, float rangeKm, float rotDeg);   // Kern 1
void map_bg_set_source(int src);                            // Kern 1: MapSource (neu laden, wenn anders)
int  map_bg_source(void);                                   // Quelle der aktuellen Planung
const char *map_bg_attribution(void);                       // Quellenvermerk der angezeigten Karte

// ---- Kern 0 ----
// Gewünschte Ansicht geändert (oder noch nicht geplant)? Dann neu planen: liefert die Anzahl
// der Kacheln (Liste über map_bg_tile()), 0 = nichts zu tun.
int  map_bg_replan(void);
bool map_bg_tile(int i, MapTile *t);
// Kachel i einsetzen. lum = 512 x 512 Helligkeiten. Gibt false zurück, wenn Kern 1 das
// letzte Zwischenbild noch nicht übernommen hat (dann später noch einmal versuchen).
bool map_bg_compose_tile(int i, const uint8_t *lum);
bool map_bg_view_changed(void);                             // neue Ansicht seit dem Planen?

// ---- Kern 1 ----
// mode: 0 = aus, 1 = dunkel, 2 = hell. Färbt neu ein bei neuem Bild, anderem Design oder Modus.
void map_bg_ui_tick(int mode);
