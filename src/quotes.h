#pragma once
// Spruch des Tages: liebevolle, motivierende Sprüche (eigene Texte) und Grüße zu
// Advent, Weihnachten, Silvester und Neujahr. Portabel (Gerät und Simulator).
#include <stddef.h>

// Spruch für ein Datum (Jahr, Monat 1-12, Tag 1-31). title bekommt die Überschrift
// (z. B. "Spruch des Tages" oder "Frohe Weihnachten, Lisa!").
const char *quote_for_date(int year, int month, int day, char *title, size_t titleLen);
