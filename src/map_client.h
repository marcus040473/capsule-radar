#pragma once
// Lädt die Kartenkacheln für map_bg (Gerät, Kern 0 / adsb_task). Pro Aufruf höchstens
// eine Kachel, damit das Flugzeug-Feed zwischendurch weiterläuft.
void map_client_service(bool enabled);
// Kern 1, beim Start: CARTO-Schlüssel (leer = OpenStreetMap ohne Schlüssel).
void map_client_set_carto_key(const char *key);
