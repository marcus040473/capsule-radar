// Spruch des Tages -- siehe quotes.h. Alle Texte sind für dieses Geschenk geschrieben.
#include "quotes.h"
#include "config.h"
#include <stdio.h>

static const char *const kDaily[] = {
    "Du bist genau richtig, so wie du bist.",
    "Heute ist ein guter Tag, um stolz auf dich zu sein.",
    "Schritt für Schritt kommst du überall hin.",
    "Dein Lächeln macht die Welt ein bisschen heller.",
    "Du schaffst mehr, als du dir zutraust.",
    "Gönn dir heute etwas Schönes - du hast es verdient.",
    "Auch kleine Fortschritte sind Fortschritte.",
    "Du wirst geliebt. Heute, morgen und jeden Tag.",
    "Mutig ist, wer es trotzdem versucht.",
    "Atme tief durch - du hast alles, was du brauchst.",
    "Jeder Tag ist eine neue Chance für etwas Wunderbares.",
    "Du bist stärker, als du denkst.",
    "Schön, dass es dich gibt.",
    "Was du heute mit Liebe tust, kommt zu dir zurück.",
    "Glaub an dich - ich tu es auch.",
    "Ein freundliches Wort von dir kann einen ganzen Tag retten.",
    "Fehler sind Beweise dafür, dass du es versuchst.",
    "Heute darfst du langsam sein. Ankommen zählt.",
    "Du bringst Wärme in jeden Raum.",
    "Dein Herz weiß den Weg. Hör ihm zu.",
    "Nach jedem Regen kommt ein Regenbogen - schau nach oben.",
    "Du musst nicht perfekt sein, um großartig zu sein.",
    "Mach heute etwas, worauf du dich morgen freust.",
    "Du bist ein Geschenk für die Menschen um dich herum.",
    "Träume groß, fang klein an, bleib dran.",
    "Lachen ist die schönste Abkürzung zum Glück.",
    "Heute ist dein Tag. Nimm ihn dir.",
    "Du hast schon so viel geschafft - sei stolz darauf.",
    "Sei sanft zu dir. Du gibst jeden Tag dein Bestes.",
    "Die Welt ist schöner, weil du darin bist.",
    "Ein kleiner Schritt heute ist ein großer Schritt morgen.",
    "Du darfst Pausen machen. Auch Sterne leuchten nachts.",
    "Dein Mut ist größer als jede Sorge.",
    "Freu dich auf die kleinen Dinge - sie sind die großen.",
    "Du bist nie allein. Jemand denkt gerade an dich.",
    "Vertrau dem Weg - auch Umwege zeigen schöne Aussichten.",
    "Heute wird gut. Und wenn nicht, wird morgen besser.",
    "Deine Freundlichkeit ist deine Superkraft.",
    "Du bist mein Lieblingsmensch.",
    "Wo du bist, ist ein schöner Ort.",
    "Jeder Flieger da draußen hat ein Ziel - und du hast so viele schöne.",
    "Blick nach oben: Der Himmel steht dir offen.",
    "Hab heute Rückenwind - in allem, was du tust.",
    "Du hebst ab, wenn du an dich glaubst.",
    "Mit dir an Bord wird jede Reise schöner.",
    "Gute Laune ist ansteckend - und du hast reichlich davon.",
    "Du machst das wunderbar. Wirklich.",
    "Sei heute dein eigener größter Fan.",
    "Es ist okay, nicht alles zu wissen. Du findest es heraus.",
    "Das Beste kommt noch.",
    "Du bist wertvoll - einfach so, ohne etwas leisten zu müssen.",
    "Ein neuer Morgen, neue Möglichkeiten.",
    "Lass dich heute von etwas Schönem überraschen.",
    "Deine Ideen sind es wert, ausprobiert zu werden.",
    "Wer so herzlich ist wie du, macht die Welt wärmer.",
    "Halte inne und genieß den Moment - er gehört dir.",
    "Du bist der Grund, warum jemand heute lächelt.",
    "Was auch kommt: Du meisterst das.",
    "Klar zum Start? Der Tag wartet auf dich!",
    "Danke, dass du du bist.",
};
static const int kDailyN = sizeof(kDaily) / sizeof(kDaily[0]);

static const char *const kAdvent[] = {
    "Zünde ein Licht an und mach es dir gemütlich - du hast es verdient.",
    "Die schönste Zeit des Jahres ist da - genieß jeden Moment.",
    "Plätzchenduft, Kerzenschein und du: Was will man mehr?",
    "Draußen ist es kalt, aber in deinem Herzen ist es warm.",
    "Jeder Tag im Advent ist ein kleines Türchen voller Freude.",
    "Lass es heute funkeln - so wie deine Augen.",
    "Gönn dir eine Tasse Tee und ein bisschen Ruhe.",
    "Vorfreude ist die schönste Freude.",
};
static const int kAdventN = sizeof(kAdvent) / sizeof(kAdvent[0]);

// Days since 1970-01-01 (proleptic Gregorian), for an even rotation through the list.
static long day_number(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

const char *quote_for_date(int year, int month, int day, char *title, size_t titleLen) {
    const long n = day_number(year, month, day);
    if (month == 12 && day == 24) {
        snprintf(title, titleLen, "Frohe Weihnachten, %s!", UI_RECIPIENT_NAME);
        return "Heute ist Heiligabend. Ich wünsche dir Liebe, Wärme und ganz viel Glück - "
               "und dass dieses kleine Radar dir jeden Tag ein Lächeln schenkt.";
    }
    if (month == 12 && (day == 25 || day == 26)) {
        snprintf(title, titleLen, "Frohe Weihnachten, %s!", UI_RECIPIENT_NAME);
        return day == 25 ? "Genieß die Feiertage, lass dich verwöhnen und iss ruhig noch ein Plätzchen mehr."
                         : "Noch ein Feiertag zum Kuscheln, Lachen und Ausruhen. Schön, dass es dich gibt.";
    }
    if (month == 12 && day == 31) {
        snprintf(title, titleLen, "Guten Rutsch, %s!", UI_RECIPIENT_NAME);
        return "Danke für dieses Jahr mit dir. Auf ein neues voller Abenteuer, Glück und Gesundheit!";
    }
    if (month == 1 && day == 1) {
        snprintf(title, titleLen, "Frohes neues Jahr, %s!", UI_RECIPIENT_NAME);
        return "Ein ganz neues Jahr - 365 Chancen für schöne Momente. Ich freu mich auf jeden davon mit dir.";
    }
    if (month == 12 && day < 24) {
        snprintf(title, titleLen, "Schöne Adventszeit, %s!", UI_RECIPIENT_NAME);
        return kAdvent[(int)(((n % kAdventN) + kAdventN) % kAdventN)];
    }
    snprintf(title, titleLen, "Spruch des Tages");
    return kDaily[(int)(((n % kDailyN) + kDailyN) % kDailyN)];
}
