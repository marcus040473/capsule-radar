#pragma once
// Eigenes Foto für die Foto-Seite (links von den Einstellungen).
// Das Foto wird über die Webseite hochgeladen (der Browser schneidet es schon auf
// SCREEN_W x SCREEN_H zu und speichert es als JPEG), liegt im Flash (LittleFS,
// /photo.jpg) und wird in einen PSRAM-Puffer als RGB565 dekodiert. Es steht damit
// nur auf dem Gerät -- nicht in der Firmware und nicht im (öffentlichen) Repository.
//
// Kerne: Hochladen/Löschen laufen im Webserver (loop(), Kern 1), das Dekodieren in
// adsb_task (Kern 0), wo TJpgDec auch für die Flugzeugfotos benutzt wird -- so greift
// nie mehr als ein Kern gleichzeitig auf den Decoder zu.
#include <stdint.h>
#include <stddef.h>

bool user_photo_begin(void);            // setup(): Dateisystem einbinden, erstes Dekodieren anfordern
void user_photo_service(void);          // adsb_task (Kern 0): dekodiert, wenn angefordert
// loop() (Kern 1): liefert true, wenn sich das Bild geändert hat; *px = neues Bild oder nullptr (gelöscht).
bool user_photo_take(const uint16_t **px);

// Webserver (Kern 1)
void user_photo_upload_chunk(int status, const uint8_t *buf, size_t len);   // HTTPUpload-Status
bool user_photo_upload_ok(void);        // nach UPLOAD_FILE_END: war die Datei brauchbar?
void user_photo_delete(void);
bool user_photo_exists(void);
const char *user_photo_path(void);      // "/photo.jpg" auf LittleFS (zum Ausliefern an die Webseite)
