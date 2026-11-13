# Lisas Radar – Android-App

Kleine App zum Flug- & Wetterradar:

- **findet das Radar im WLAN** von selbst (zuletzt benutzte Adresse → `lisasradar.local` →
  mDNS-Suche nach `_http._tcp`), Adresse sonst von Hand eingeben (Tipp auf den Titel);
- zeigt die **Einstellungsseite** des Radars (alle Schalter, Foto hochladen, Firmware-Update);
- Knopf **„Meinen Standort ans Radar senden“**: Handy-Standort bestimmen, bestätigen, per
  `POST /loc` (Header `X-Radar: 1`) ans Radar – das zentriert sofort neu und speichert den Ort.
  Braucht Firmware 1.8.0-de13 oder neuer (`/api/info`, `/loc` in `src/main.cpp`).

Ohne Bibliotheken, nur das Android-Framework (Java, Oberfläche im Code in
`app/src/main/java/de/lisasradar/app/MainActivity.java`). Ab Android 8.

**Bauen:** passiert im Release-Workflow (`.github/workflows/release.yml`, Job `apk`); die APK hängt als `LisasRadar.apk` am Firmware-Release.
Lokal mit Android-SDK: `gradle -p android assembleRelease` (Gradle 8.9+ und JDK 17).

**Signatur:** `app/lisasradar.jks` liegt bewusst im Repo, damit jede neue Version sich als
Update über die alte installieren lässt. Für eine private, selbst installierte App reicht das;
für den Play Store bräuchte es einen geheimen Schlüssel.
