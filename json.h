/* AmiSubsonic - kleiner JSON-Leser.
 *
 * Audiobookshelf spricht nur JSON, und seine Antworten sind gross: die
 * Beschreibung EINES Hoerbuchs hat 194 KB, weil zu jeder Datei alle
 * Etiketten mitkommen (gemessen 12.10.2026). Gebraucht werden davon je
 * Spur fuenf Felder.
 *
 * Deshalb kein Baum, sondern ein Durchlauf im SAX-Stil: json_walk() geht
 * den Text einmal durch und meldet jeden Wert zusammen mit seinem PFAD,
 * etwa "media.tracks.#.duration" ('#' steht fuer ein Feldelement). Der
 * Aufrufer vergleicht den Pfad und nimmt, was er braucht. Kein malloc,
 * kein Token-Feld - ein Token-Feld fuer 194 KB waeren rund 20000
 * Eintraege gewesen.
 *
 * Zahlen bleiben Text, bis jemand sie haben will: json_long() und
 * json_ms() rechnen ganzzahlig. Audiobookshelf schreibt Zeiten als
 * Sekunden mit Nachkommastellen ("14468.833823999998"), und hier gibt es
 * kein Fliesskomma (68020 ohne FPU). */

#ifndef JSON_H
#define JSON_H

#define JSON_STR        1   /* val zeigt hinter das oeffnende '"', roh */
#define JSON_NUM        2
#define JSON_LIT        3   /* true, false, null */
#define JSON_OBJ_BEGIN  4
#define JSON_OBJ_END    5
#define JSON_ARR_BEGIN  6
#define JSON_ARR_END    7

struct JsonEv {
    int         type;
    const char *path;   /* "media.tracks.#.duration", "" fuer die Wurzel */
    const char *val;    /* roher Text des Wertes (bei Zeichenketten noch
                         * mit Maskierungen, siehe json_text) */
    int         len;
    int         index;  /* Stelle im innersten Feld, -1 ausserhalb */
};

/* Rueckgabe ungleich 0 bricht den Durchlauf ab - wer gefunden hat, was
 * er sucht, muss den Rest von 194 KB nicht mehr lesen. */
typedef int (*JsonCb)(const struct JsonEv *ev, void *ud);

/* 0 = durchgelaufen oder vom Aufrufer abgebrochen, -1 = kein gueltiges
 * JSON (oder tiefer verschachtelt, als der Pfad fasst). */
int  json_walk(const char *text, long len, JsonCb cb, void *ud);

/* Zeichenkette entmaskiert und nach Latin-1 gewandelt. Bei JSON_LIT
 * "null" bleibt out leer. */
void json_text(const struct JsonEv *ev, char *out, int outsize);

long json_long(const struct JsonEv *ev);

/* "14468.833823999998" -> 14468833. Mehr als drei Nachkommastellen
 * werden abgeschnitten, nicht gerundet - eine Millisekunde ist beim
 * Hoerbuch keine Frage. */
long json_ms(const struct JsonEv *ev);

/* Pfadvergleich als Kurzform fuer die Rueckruffunktionen. */
#define JSON_IS(ev, p) (strcmp((ev)->path, (p)) == 0)

#endif
