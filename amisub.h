/* AmiSubsonic - gemeinsamer Kern fuer CLI und MUI-Oberflaeche.
 *
 * Alles, was mit Navidrome/Subsonic spricht, steckt hier. Die Oberflaeche
 * benutzt nur diese Schnittstelle und weiss nichts von HTTP, XML,
 * Zeichensaetzen oder der Anmeldung.
 *
 * Aufteilung wie bei AmiHomeassist: erst dieser Kern plus ein CLI, das
 * gegen den echten Server verifiziert wird, danach erst MUI. Das erlaubt
 * Testen ohne ein einziges MUI-Objekt und laesst nebenbei ein brauchbares
 * Shell-Werkzeug zurueck.
 *
 * Was hier ABSICHTLICH fehlt: Wiedergabe. Die macht AmigaAMP extern, per
 * ARexx angesteuert (siehe amiamp.h). Der Kern baut nur die Stream-URL.
 */

#ifndef AMISUB_H
#define AMISUB_H

#include <exec/types.h>

/* Feldlaengen. Navidrome setzt keine harten Grenzen, das sind gemessene
 * Werte aus einer echten Bibliothek plus Reserve. Zu lange Angaben werden
 * abgeschnitten, nicht abgelehnt - ein Album mit sehr langem Titel soll
 * angezeigt werden, nicht das Holen zum Scheitern bringen. */
#define SUB_ID_LEN      40      /* Navidrome nimmt 22-stellige Base64-IDs */
#define SUB_NAME_LEN   128
#define SUB_ARTIST_LEN  96
#define SUB_ALBUM_LEN  128
#define SUB_SUFFIX_LEN   8      /* mp3, flac, ogg, opus ... */

/* Subsonic-API-Fassung, die wir ansagen. 1.16.1 ist die letzte von Subsonic
 * selbst veroeffentlichte; Navidrome nimmt sie an und schaltet damit auch
 * die OpenSubsonic-Erweiterungen frei, die wir fuer den LYRICS-Tab
 * brauchen. Hoeher zu gehen bringt nichts und riskiert eine Abweisung
 * durch aeltere Server. */
#define SUB_API_VERSION "1.16.1"
#define SUB_CLIENT      "AmiSubsonic"

struct Prefs {
    char host[128];             /* nur der Rechnername, ohne Schema */
    int  port;
    char user[64];
    char pass[128];
    BOOL https;                 /* AmiSSL-Pfad statt nacktem bsdsocket */

    /* Verzeichnis fuer die einmal geholten Cover. Vorgabe
     * "PROGDIR:Cache" - liegt das Programm auf einer Platte, ueberlebt
     * der Cache damit den Neustart. Ein Pfad auf RAM: wird beim Laden
     * verworfen und durch die Vorgabe ersetzt (prefs_load). */
    char cache[128];

    /* Vollstaendiger Pfad zu AmigaAMP, z.B. "Work:Audio/AmigaAMP".
     * Leer heisst: nicht eingetragen - dann wird AmigaAMP nicht
     * gestartet, sondern nach dem Pfad gefragt. Geraten wird NICHT:
     * jede Maschine legt das Programm woanders hin, und ein falsch
     * geratener Pfad ist schlimmer als eine klare Ansage. */
    char amppath[256];

    /* Verzeichnis mit eigenen MP3-Dateien, rekursiv durchsucht. Leer
     * heisst: kein eigener Bestand, die Ansicht Folder bleibt leer.
     * Geraten wird NICHT - der Anwender sucht es aus. */
    char folder[192];

    /* AHI-Unit fuer die Wiedergabe, 0 bis 3. Welcher Modus dahinter
     * liegt, stellt der Anwender einmal in den AHI-Voreinstellungen
     * ein - so macht es jedes andere Amiga-Programm auch. */
    int  ahiunit;

    /* Eigener Bildschirm statt Workbench. screenid ist die
     * Modus-Kennung aus dem ASL-Requester; 0 heisst "wie die
     * Workbench". Beides wirkt erst beim naechsten Start - ein Fenster
     * laesst sich nicht im Betrieb auf einen anderen Screen umhaengen. */
    BOOL  ownscreen;
    ULONG screenid;
    int   screenw, screenh, screend;
};

struct Artist {
    char id[SUB_ID_LEN];
    char name[SUB_ARTIST_LEN];
    int  albumcount;
};

struct Album {
    char id[SUB_ID_LEN];
    char name[SUB_ALBUM_LEN];
    char artist[SUB_ARTIST_LEN];
    char artistid[SUB_ID_LEN];
    char coverart[SUB_ID_LEN];
    int  year;
    int  songcount;
    int  duration;              /* Sekunden */

    /* Nur fuer eigene Alben von der Platte: das Verzeichnis (fuer
     * folder.jpg) und der Ausschnitt aus der Titelliste, der dazu
     * gehoert. Beim Server bleibt beides leer bzw. 0. */
    char dir[192];
    int  first, count;
};

struct Song {
    char id[SUB_ID_LEN];
    char title[SUB_NAME_LEN];
    char artist[SUB_ARTIST_LEN];
    char album[SUB_ALBUM_LEN];
    char albumid[SUB_ID_LEN];   /* fuer das Cover, siehe sub_album_cover */
    char coverart[SUB_ID_LEN];
    char suffix[SUB_SUFFIX_LEN];
    int  track;
    int  year;
    int  duration;              /* Sekunden, 0 = unbekannt */
    int  bitrate;               /* kbit/s, 0 = unbekannt */

    /* Leer heisst: der Titel kommt vom Server und wird ueber id geholt.
     * Sonst steht hier der vollstaendige AmigaDOS-Pfad einer eigenen
     * Datei, und id bleibt leer. Ein Titel ist ein Titel - nur die
     * Quelle unterscheidet sich. */
    char path[192];

    /* Nur bei eigenen Dateien: zu welchem Album der Titel gehoert
     * (Index in die Albenliste). So bleibt die Zuordnung erhalten, auch
     * wenn die Liste danach anders sortiert wird - ueber einen Bereich
     * "ab Zeile x, y Stueck" ginge das nicht. */
    int  lalbum;
};

/* Eine Radiostation, wie getInternetRadioStations sie liefert. Sie ist
 * KEIN Titel: es gibt weder Album noch Dauer noch Cover, und die Adresse
 * zeigt nicht auf den eigenen Server, sondern irgendwohin ins Netz. */
struct Radio {
    char id[SUB_ID_LEN];
    char name[SUB_NAME_LEN];
    char url[512];              /* streamUrl */
    char home[256];             /* homePageUrl, kann leer sein */
    BOOL unplayable;            /* Format, das keiner der Dekoder kann -
                                 * erst beim Abspielen am Content-Type
                                 * festgestellt, vorher unbekannt */
};

/* Eine wachsende Liste.
 *
 * Heisst SubList und nicht List, weil exec/lists.h ein 'struct SubList'
 * mitbringt - Exec's doppelt verkettete Liste. Der Name kollidiert
 * unweigerlich, sobald proto/exec.h dazukommt, und zwar mit einer
 * Fehlermeldung, die auf den falschen Header zeigt.
 *
 * Frueher stand bei AmiHomeassist hier ein festes
 * Feld, das allein 128 KB belegte und fuer grosse Anlagen trotzdem zu
 * klein war. Eine Musikbibliothek hat leicht Tausende Titel, ein festes
 * Feld scheidet damit von vornherein aus.
 *
 * Der Elementtyp steckt in itemsize, damit dieselbe Mechanik fuer
 * Artist, Album und Song taugt. */
struct SubList {
    void *items;
    int   count;
    int   capacity;
    int   itemsize;
};

void list_init(struct SubList *l, int itemsize);
void list_free(struct SubList *l);
void *list_get(struct SubList *l, int index);
void *list_add(struct SubList *l);     /* haengt ein genulltes Element an */

#define SUB_OK        0
#define SUB_ENOPREFS  1         /* Einstellungen fehlen oder unvollstaendig */
#define SUB_ENET      2         /* Netzwerk, DNS oder Verbindung */
#define SUB_EHTTP     3         /* Server antwortet, aber nicht mit 2xx */
#define SUB_EMEM      4
#define SUB_EAPI      5         /* Subsonic meldet status="failed" */
#define SUB_EBUSY     6         /* Dienst ueberlastet (HTTP 503), auch nach
                                 * einem zweiten Versuch - kein Fehler des
                                 * Programms, spaeter geht es wieder */

const char *sub_last_error(void);

/* Ein offener Strom vom Server, aus dem haeppchenweise gelesen wird -
 * die Grundlage des eigenen Abspielers. Die Felder sind privat, der
 * Aufrufer haelt nur die Struktur. */
struct SubStream {
    int   sock;
    void *ssl;
    long  total;        /* Gesamtlaenge der Datei, 0 = unbekannt */
    long  pos;          /* schon gelesene Bytes, ab Dateianfang */
    char  id[SUB_ID_LEN];
    /* Nur beim Radio gefuellt (sub_radio_open). */
    char  ctype[40];    /* Content-Type des Senders */
    long  metaint;      /* ICY: Musikbytes zwischen zwei Titelbloecken */
    long  bitrate;      /* icy-br in kbps, 0 = unbekannt */
    int   hops;         /* wie oft weitergeleitet wurde */
};

/* Oeffnet stream.view ab Byte offset. format=raw, also OHNE Umkodieren -
 * dieser Navidrome kann es nicht, und bei MP3-Quellen waere es ohnehin
 * nur Qualitaetsverlust (gemessen 20.9.2026, siehe Projekt.md). */
int  sub_stream_open(struct Prefs *p, const char *songid, long offset,
                     struct SubStream *st);
/* > 0 = Bytes, 0 = Strom zu Ende, < 0 = Fehler (sub_last_error). */
long sub_stream_read(struct SubStream *st, void *buf, long len);
void sub_stream_close(struct SubStream *st);

/* Oeffnet den Strom eines Radiosenders: beliebige http(s)-Adresse,
 * Weiterleitungen werden verfolgt. Gelesen und geschlossen wird mit
 * sub_stream_read/_close wie beim Titel. icy = TRUE bittet um den
 * laufenden Titel - dann steckt alle st->metaint Bytes ein Textblock
 * im Strom, den der Leser herausschneiden MUSS. */
int  sub_radio_open(const char *url, BOOL icy, struct SubStream *st);
/* TRUE, wenn der Content-Type nach MP3 aussieht (oder fehlt). */
BOOL sub_radio_is_mp3(const char *ctype);
/* TRUE, wenn der Content-Type nach AAC im ADTS-Rahmen aussieht - das
 * kann der Helix-Dekoder. MP4/M4A-Container kann er NICHT. */
BOOL sub_radio_is_aac(const char *ctype);
/* TRUE, wenn die Adresse nach AAC aussieht - nur eine Vermutung. */
BOOL sub_radio_url_aac(const char *url);

/* Misst, ob der Server einen Einstieg mitten im Titel erlaubt (HTTP 206).
 * fmt = NULL oder "" fragt ohne Umkodierung, sonst z.B. "mp3". */
int sub_range_probe(struct Prefs *p, const char *songid, long offset,
                    const char *fmt, int maxbitrate, char *out, int outsize);

/* Gibt bsdsocket und AmiSSL wieder frei.
 *
 * Beide bleiben ab dem ersten Zugriff offen, statt je Anfrage auf- und
 * zuzugehen: AmiSSLs Initialisierung ist teuer, und der Zertifikats-
 * speicher (290 Stueck aus AmiSSL:Certs) wuerde jedes Mal neu geladen.
 * Dafuer MUSS der Aufrufer das hier vor dem Beenden rufen - CLI wie
 * Oberflaeche. */
void sub_cleanup(void);

int  prefs_load(struct Prefs *p);
int  prefs_save(struct Prefs *p);

/* Zerlegt "https://navidrome:4533" in host, port und das https-Flag.
 * Oeffentlich, damit Einrichtung (CLI wie GUI) genau dieselbe Zerlegung
 * benutzt wie das Laden - zwei Fassungen davon waeren eine sichere
 * Fehlerquelle. Ohne Schema wird http angenommen, ohne Port 4533. */
int  prefs_set_host(struct Prefs *p, const char *value);

/* Loest den Rechnernamen auf und merkt sich das Ergebnis.
 *
 * Ausdruecklich zum Aufruf BEIM PROGRAMMSTART gedacht. Grund: die
 * Namensaufloesung ist die einzige Stelle im ganzen Netzweg, die sich
 * nicht begrenzen laesst - gethostbyname() blockiert, so lange der
 * Resolver will, und bei totem Netz heisst das unbegrenzt. Passiert das
 * mitten im Betrieb, steht die Oberflaeche und nimmt keine Eingabe mehr
 * an; genau so beobachtet, als das WLAN wegbrach. Beim Start ist derselbe
 * Haenger halb so schlimm, weil noch nichts offen ist und ein Ctrl-C aus
 * der Shell hilft.
 *
 * Danach laeuft jede weitere Anfrage ueber den gemerkten Wert. Nur im
 * Speicher, nicht auf Platte: hinter einer MyFritz-Adresse steckt eine
 * wechselnde IP. */
int  sub_resolve(struct Prefs *p);

/* Anmeldung pruefen, ohne Daten zu holen. Erster Aufruf nach dem
 * Einrichten - trennt "Server nicht erreichbar" sauber von "Passwort
 * falsch", was sonst beides nur als leere Liste ankaeme. */
int  sub_ping(struct Prefs *p);

int  sub_get_artists(struct Prefs *p, struct SubList *out);

/* type ist "alphabeticalByName", "newest", "frequent", "recent", "random"
 * oder "byYear". Bei artistid != NULL wird stattdessen getArtist
 * aufgerufen, also die Alben genau dieses Interpreten geholt. */
int  sub_get_albums(struct Prefs *p, const char *type, int size, int offset,
                    struct SubList *out);
int  sub_get_artist_albums(struct Prefs *p, const char *artistid,
                           struct SubList *out);

int  sub_get_album_songs(struct Prefs *p, const char *albumid,
                         struct Album *info, struct SubList *out);
/* Titel suchen. offset blaettert in der Trefferliste - mit LEERER
 * Suchanfrage liefert Navidrome damit die ganze Bibliothek in Seiten. */
/* Die in Navidrome als Favorit markierten Titel, in der Reihenfolge, in
 * der der Server sie liefert. */
int  sub_get_starred_songs(struct Prefs *p, struct SubList *out);

/* Die Cover-Kennung eines Albums - "al-<albumid>", OHNE Hash.
 *
 * Navidrome haengt an jede Cover-Kennung einen Hash ("_5eb2e1d6"), der
 * sich aendert, wenn sich die Datei aendert. Er gehoert aber zur EINZELNEN
 * DATEI, nicht zum Album: derselbe Album-Bezeichner taucht mit dutzenden
 * verschiedenen Hashes auf, einem je Titel. Wer ihn mitnimmt, holt
 * dasselbe Cover ein Dutzend Mal - am 6.9.2026 nachgemessen, 169 Dateien
 * fuer 51 Alben.
 *
 * Ohne Hash nimmt der Server die Kennung ebenfalls an und liefert
 * dasselbe Bild (gemessen, Byte fuer Byte gleich gross). Damit gibt es
 * GENAU EINE Datei je Album, und Bildwand wie Titelliste teilen sie
 * sich. Der Preis: aendert sich ein Cover auf dem Server, faellt das
 * hier nicht auf - dafuer ist der Scan-Knopf da. */
void sub_album_cover_id(const char *albumid, char *out, int outsize);

/* Dasselbe fuer einen Titel: die Kennung des Albums, zu dem er gehoert. */
void sub_album_cover(const struct Song *s, char *out, int outsize);

/* Eine beliebige Server-Methode aufrufen und die ROHE Antwort liefern.
 * Nur zum Nachsehen gedacht: welche Angaben schickt der Server mit?
 * Der Puffer kommt aus malloc() und gehoert dem Aufrufer. */
int  sub_api_raw(struct Prefs *p, const char *method, const char *extra,
                 char **body, long *len);

/* Die im Server hinterlegten Radiostationen. */
int  sub_get_radios(struct Prefs *p, struct SubList *out);

int  sub_search_songs(struct Prefs *p, const char *query, int size,
                      int offset,
                      struct SubList *out);

/* Fuer den RELATED-Tab. */
int  sub_get_similar(struct Prefs *p, const char *songid, int count,
                     struct SubList *out);

/* Angaben zu genau einem Titel. Die Oberflaeche braucht sie, sobald
 * etwas laeuft: AmigaAMP kann bei einem Netzstrom weder Titel noch Dauer
 * melden (siehe amiamp.h), also muessen beide von hier kommen. */
int  sub_get_song(struct Prefs *p, const char *songid, struct Song *out);

/* Fuer den LYRICS-Tab, erster Versuch: der eigene Server. Liefert SUB_OK
 * mit leerem Text, wenn er keinen hat - das ist kein Fehler, sondern bei
 * einer gewachsenen Sammlung der Normalfall, weil Navidrome nur
 * weitergibt, was als USLT-Tag oder .lrc-Datei vorliegt. */
int  sub_get_lyrics(struct Prefs *p, const char *songid,
                    char *out, int outsize);

/* Zweiter Versuch: lrclib.net. Kostenlos, ohne Schluessel, ohne
 * Anmeldung - dieselbe Quelle, aus der auch Feishin schoepft.
 *
 * synced enthaelt bei Erfolg die Fassung mit Zeitmarken
 * ("[00:12.34] Zeile"), aus der sich zusammen mit AmigaAMPs GETTIME die
 * mitlaufende Hervorhebung bauen laesst. Beide Ausgaben duerfen NULL
 * sein. */
int  sub_get_lyrics_net(const char *artist, const char *title,
                        const char *album, int duration,
                        char *plain, int plainsize,
                        char *synced, int syncedsize);

/* Baut die URL, die AmigaAMP per ARexx OPEN bekommt. Kein Netzverkehr,
 * nur Zeichenkettenarbeit - die Anmeldung steckt in den Query-Parametern,
 * AmigaAMP holt den Strom dann selbst. */
int  sub_stream_url(struct Prefs *p, const char *songid,
                    char *out, int outsize);

/* Holt das Cover und legt es als Datei ab (Vorgabe unter T:), damit
 * Dtpic.mui einen echten Pfad bekommt. maxsize ist die Kantenlaenge, die
 * der Server liefern soll - kleiner geholt heisst weniger Bytes ueber die
 * Leitung UND weniger Arbeit fuer die datatypes beim Skalieren. */
int  sub_get_cover(struct Prefs *p, const char *coverartid, int maxsize,
                   const char *path);

/* ---- Cover-Cache -------------------------------------------------- */
/* Jedes Cover wird EINMAL geholt und bleibt danach als Datei liegen.
 * Der Dateiname ist die Cover-ID des Servers; damit ist der Abgleich
 * spaeter trivial (fehlende ID = nachladen, unbekannte Datei = weg).
 * Die Platten hier laufen auf PFS3, lange Namen sind also kein Problem;
 * gefiltert wird trotzdem, weil ':' und '/' in AmigaDOS-Namen nicht
 * vorkommen duerfen. */

/* Baut <cache>/<id>.jpg. Kein Zugriff auf die Platte. */
void cache_path(struct Prefs *p, const char *coverartid,
                char *out, int outsize);

/* Legt das Cache-Verzeichnis an, falls noetig. SUB_OK oder ein Fehler.
 * Darf beliebig oft gerufen werden. */
int  cache_ensure(struct Prefs *p);

/* Liegt das Cover schon da? Nur ein Lock(), kein malloc - die
 * Oberflaeche darf das also auch waehrend eines Netzauftrags fragen. */
BOOL cache_have(struct Prefs *p, const char *coverartid);

/* Pfad zum Cover liefern und es holen, falls es fehlt. Der teure Teil
 * gehoert damit in den Netzprozess, die Oberflaeche bekommt am Ende nur
 * einen Dateinamen. */
int  sub_get_cover_cached(struct Prefs *p, const char *coverartid,
                          int maxsize, char *out, int outsize);

/* Navidrome liefert UTF-8, der Amiga will Latin-1. Ohne das steht in der
 * Liste "BÃ¼rgermeister" statt "Buergermeister". Arbeitet an Ort und
 * Stelle; die Latin-1-Fassung ist nie laenger als die UTF-8-Fassung. */
void utf8_to_latin1(char *s);

/* "4:07" aus 247 Sekunden. Ganzzahlig, wie alles hier. */
void duration_text(int secs, char *out, int outsize);

#endif
