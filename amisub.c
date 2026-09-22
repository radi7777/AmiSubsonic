/* AmiSubsonic - Kern: Einstellungen, HTTP, Subsonic/Navidrome-Zugriff. */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>

#include <libraries/amisslmaster.h>
#include <amissl/amissl.h>
#include <amissl/tags.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "amisub.h"
#include "md5.h"

struct Library *SocketBase = NULL;

static char g_error[256] = "";

const char *sub_last_error(void)
{
    return g_error[0] ? g_error : "no error";
}

static int fail(int code, const char *msg)
{
    strncpy(g_error, msg, sizeof(g_error) - 1);
    g_error[sizeof(g_error) - 1] = '\0';
    return code;
}

/* ------------------------------------------------------------------ */
/* Zeichensatz und Formatierung                                        */
/* ------------------------------------------------------------------ */

/* Ein Kodepunkt, den Latin-1 nicht kennt, als brauchbarer ASCII-Ersatz.
 *
 * Das ist kein Schoenheitsfehler, sondern in einer Musikbibliothek der
 * Normalfall: Tag-Redakteure setzen den typografischen Apostroph in
 * "Don\u2019t", den Halbgeviertstrich in "a\u2010ha" und die
 * Auslassungspunkte als ein Zeichen. Ohne diese Tabelle stand in der
 * Liste "a?ha" - genau so aufgefallen beim ersten Lauf gegen die echte
 * Bibliothek.
 *
 * Liefert 0, wenn nichts Vernuenftiges einfaellt (dann bleibt es beim
 * Fragezeichen), und -1 fuer Zeichen, die ersatzlos wegfallen sollen. */
static int cp_to_ascii(unsigned long cp)
{
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013:
    case 0x2014: case 0x2015: case 0x2212:
        return '-';             /* Binde-, Gedanken-, Minuszeichen */
    case 0x2018: case 0x2019: case 0x201a: case 0x201b:
    case 0x2032: case 0x02bc:
        return '\'';            /* einfache Anfuehrungs- und Apostrophe */
    case 0x201c: case 0x201d: case 0x201e: case 0x201f:
    case 0x2033:
        return '"';             /* doppelte Anfuehrungszeichen */
    case 0x2026:
        return '.';             /* Auslassungspunkte - nur eines davon */
    case 0x00a0: case 0x2007: case 0x2008: case 0x2009:
    case 0x200a: case 0x202f:
        return ' ';             /* diverse Leerzeichen */
    case 0x200b: case 0x200c: case 0x200d: case 0xfeff:
        return -1;              /* unsichtbar - ersatzlos weglassen */
    default:
        return 0;
    }
}

/* Navidrome liefert UTF-8, der Amiga will Latin-1.
 *
 * Zwei Byte lange Folgen bis U+00FF gehen unmittelbar. Fuer alles
 * darueber versucht cp_to_ascii() einen lesbaren Ersatz; erst wenn auch
 * das nichts hergibt, steht ein Fragezeichen da. Besser ein
 * Fragezeichen als die bekannten Doppelbuchstaben "B\u00c3\u00bcro".
 *
 * Arbeitet an Ort und Stelle. Das geht, weil die Ausgabe nie laenger ist
 * als die Eingabe: jede Mehrbytefolge schrumpft auf hoechstens ein Byte,
 * ASCII bleibt gleich. */
void utf8_to_latin1(char *s)
{
    unsigned char *r = (unsigned char *)s;
    unsigned char *w = (unsigned char *)s;

    while (*r) {
        unsigned long cp;
        int len;

        if (*r < 0x80) {
            *w++ = *r++;
            continue;
        }

        if ((r[0] & 0xe0) == 0xc0 && (r[1] & 0xc0) == 0x80) {
            cp = ((unsigned long)(r[0] & 0x1f) << 6)
               |  (unsigned long)(r[1] & 0x3f);
            len = 2;
        } else if ((r[0] & 0xf0) == 0xe0 && (r[1] & 0xc0) == 0x80
                   && (r[2] & 0xc0) == 0x80) {
            cp = ((unsigned long)(r[0] & 0x0f) << 12)
               | ((unsigned long)(r[1] & 0x3f) << 6)
               |  (unsigned long)(r[2] & 0x3f);
            len = 3;
        } else if ((r[0] & 0xf8) == 0xf0 && (r[1] & 0xc0) == 0x80
                   && (r[2] & 0xc0) == 0x80 && (r[3] & 0xc0) == 0x80) {
            /* Ausserhalb der Basic Multilingual Plane - Emoji und
             * dergleichen. Latin-1 hat dafuer nichts. */
            cp = 0x10000;
            len = 4;
        } else {
            /* Kaputte Folge - genau ein Byte weiter, sonst laeuft die
             * Schleife auf der Stelle und das Programm haengt. */
            *w++ = '?';
            r += 1;
            continue;
        }

        if (cp <= 0xff) {
            *w++ = (unsigned char)cp;
        } else {
            /* int, nicht char: der Rueckgabewert -1 (unsichtbares
             * Zeichen) traegt sonst nur, solange char vorzeichenbehaftet
             * ist - beim 68k-gcc zufaellig ja, anderswo nicht. */
            int sub = cp_to_ascii(cp);
            if (sub > 0) {
                *w++ = (unsigned char)sub;
            } else if (sub == 0) {
                *w++ = '?';
            }
            /* sub < 0: unsichtbares Zeichen, faellt weg */
        }
        r += len;
    }
    *w = '\0';
}

/* "4:07" aus 247, "1:02:03" aus 3723. Ganzzahlig, wie alles hier. */
void duration_text(int secs, char *out, int outsize)
{
    if (secs < 0) {
        secs = 0;
    }
    if (secs >= 3600) {
        sprintf(out, "%d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
    } else {
        sprintf(out, "%d:%02d", secs / 60, secs % 60);
    }
    (void)outsize;              /* Laenge ist durch das Format begrenzt */
}

static void trim(char *s)
{
    char *p = s;
    int n;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'
                     || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    s[n] = '\0';
}

static void copy_field(char *dst, int dstsize, const char *src)
{
    strncpy(dst, src, dstsize - 1);
    dst[dstsize - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Wachsende Liste                                                     */
/* ------------------------------------------------------------------ */

void list_init(struct SubList *l, int itemsize)
{
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
    l->itemsize = itemsize;
}

void list_free(struct SubList *l)
{
    if (l->items) {
        free(l->items);
    }
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

void *list_get(struct SubList *l, int index)
{
    if (index < 0 || index >= l->count) {
        return NULL;
    }
    return (char *)l->items + (long)index * l->itemsize;
}

void *list_add(struct SubList *l)
{
    void *slot;

    if (l->count >= l->capacity) {
        /* Verdoppeln, aber mit 32 anfangen. Eine Albenliste hat oft
         * genau 20 Eintraege - da soll nicht gleich fuer Tausende
         * belegt werden. */
        int newcap = l->capacity ? l->capacity * 2 : 32;
        void *n = realloc(l->items, (long)newcap * l->itemsize);
        if (!n) {
            return NULL;
        }
        l->items = n;
        l->capacity = newcap;
    }
    slot = (char *)l->items + (long)l->count * l->itemsize;
    memset(slot, 0, l->itemsize);
    l->count++;
    return slot;
}

/* ------------------------------------------------------------------ */
/* Einstellungen                                                       */
/* ------------------------------------------------------------------ */

#define PREFS_DIR_ENV    "ENV:AmiSubsonic"
#define PREFS_DIR_ARC    "ENVARC:AmiSubsonic"
#define PREFS_FILE       "AmiSubsonic.prefs"

/* Neben dem Programm. Ein Pfad auf RAM: in den Prefs wird verworfen
 * (siehe prefs_load), der Cache soll den Neustart ueberleben. */
#define CACHE_DIR_DEFAULT "PROGDIR:Cache"

/* "http://navidrome:4533" wird zu host="navidrome", port=4533, https=FALSE. */
int prefs_set_host(struct Prefs *p, const char *value)
{
    const char *s = value;
    char *sep;

    p->https = FALSE;
    if (strnicmp(s, "https://", 8) == 0) {
        p->https = TRUE;
        s += 8;
        p->port = 443;
    } else if (strnicmp(s, "http://", 7) == 0) {
        s += 7;
        p->port = 80;
    }

    copy_field(p->host, sizeof(p->host), s);

    /* Ein Pfad hinter dem Rechnernamen wird verworfen. Navidrome haengt
     * seine API immer unter /rest auf; ein Unterverzeichnis waere ein
     * Reverse-Proxy-Aufbau, den wir bewusst nicht unterstuetzen. */
    sep = strchr(p->host, '/');
    if (sep) {
        *sep = '\0';
    }
    sep = strchr(p->host, ':');
    if (sep) {
        *sep = '\0';
        p->port = atoi(sep + 1);
    }
    if (p->port <= 0) {
        p->port = p->https ? 443 : 4533;
    }
    return SUB_OK;
}

static int prefs_read_file(struct Prefs *p, const char *path)
{
    BPTR fh;
    char line[400];

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        return 0;
    }

    while (FGets(fh, (STRPTR)line, sizeof(line) - 1)) {
        char *eq;

        trim(line);
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        trim(line);
        trim(eq + 1);

        if (stricmp(line, "host") == 0) {
            prefs_set_host(p, eq + 1);
        } else if (stricmp(line, "user") == 0) {
            copy_field(p->user, sizeof(p->user), eq + 1);
        } else if (stricmp(line, "password") == 0) {
            copy_field(p->pass, sizeof(p->pass), eq + 1);
        } else if (stricmp(line, "cache") == 0) {
            copy_field(p->cache, sizeof(p->cache), eq + 1);
        } else if (stricmp(line, "amigaamp") == 0) {
            copy_field(p->amppath, sizeof(p->amppath), eq + 1);
        } else if (stricmp(line, "folder") == 0) {
            copy_field(p->folder, sizeof(p->folder), eq + 1);
        } else if (stricmp(line, "ahiunit") == 0) {
            p->ahiunit = atoi(eq + 1);
            if (p->ahiunit < 0 || p->ahiunit > 3) {
                p->ahiunit = 0;
            }
        } else if (stricmp(line, "visfps") == 0) {
            p->visfps = atoi(eq + 1);
            if (p->visfps != 0 && p->visfps != 15 && p->visfps != 20 &&
                p->visfps != 30 && p->visfps != 60) {
                p->visfps = 30;
            }
        } else if (stricmp(line, "ownscreen") == 0) {
            p->ownscreen = (atoi(eq + 1) != 0);
        } else if (stricmp(line, "screenmode") == 0) {
            /* Die Kennung steht hexadezimal in der Datei - so, wie sie
             * in den Modus-Listen auch auftaucht. */
            p->screenid = (ULONG)strtoul(eq + 1, NULL, 16);
        } else if (stricmp(line, "screensize") == 0) {
            sscanf(eq + 1, "%d %d %d",
                   &p->screenw, &p->screenh, &p->screend);
        }
    }
    Close(fh);
    return 1;
}

int prefs_load(struct Prefs *p)
{
    char path[256];

    memset(p, 0, sizeof(*p));
    p->port = 4533;             /* Navidromes Vorgabe */
    p->visfps = 30;             /* auch fuer alte Prefs ohne die Zeile */

    sprintf(path, "%s/%s", PREFS_DIR_ENV, PREFS_FILE);
    if (!prefs_read_file(p, path)) {
        sprintf(path, "%s/%s", PREFS_DIR_ARC, PREFS_FILE);
        if (!prefs_read_file(p, path)) {
            return fail(SUB_ENOPREFS,
                        "no settings - ENVARC:AmiSubsonic missing");
        }
    }

    if (p->host[0] == '\0') {
        return fail(SUB_ENOPREFS, "no server configured");
    }
    if (p->user[0] == '\0') {
        return fail(SUB_ENOPREFS, "no user name configured");
    }
    /* Ein Cache im RAM ist nie gewollt: er ist nach jedem Neustart weg,
     * und alle Cover werden neu geholt. In WinUAE stand vom Testen noch
     * cache=RAM:AmiSubsonic/Cache in den Prefs, und weil Settings kein
     * Feld dafuer hat, schrieb jedes Speichern die Zeile wieder zurueck
     * (21.9.2026). Also hier verwerfen - das naechste Speichern heilt
     * die Datei dann von selbst. */
    if (p->cache[0] == '\0' || strnicmp(p->cache, "RAM:", 4) == 0) {
        strcpy(p->cache, CACHE_DIR_DEFAULT);
    }
    return SUB_OK;
}

static int prefs_write_one(struct Prefs *p, const char *dir)
{
    BPTR fh, lock;
    char path[256];

    lock = CreateDir((STRPTR)dir);
    if (lock) {
        UnLock(lock);
    }

    sprintf(path, "%s/%s", dir, PREFS_FILE);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        return 0;
    }

    FPrintf(fh, "; AmiSubsonic - Zugang zum Navidrome-/Subsonic-Server.\n");
    FPrintf(fh, "; Von Hand aenderbar. Das Passwort steht im Klartext, weil\n");
    FPrintf(fh, "; Subsonic daraus bei jeder Anfrage md5(passwort+salt)\n");
    FPrintf(fh, "; bilden muss - ein gespeicherter Hash brauechte dasselbe\n");
    FPrintf(fh, "; Salt und waere kein Gewinn.\n");
    FPrintf(fh, "host=%s://%s:%ld\n",
            (LONG)(ULONG)(p->https ? "https" : "http"),
            (LONG)(ULONG)p->host, (LONG)p->port);
    FPrintf(fh, "user=%s\n", (LONG)(ULONG)p->user);
    FPrintf(fh, "password=%s\n", (LONG)(ULONG)p->pass);
    FPrintf(fh, ";\n");
    FPrintf(fh, "; Ablage der einmal geholten Cover. PROGDIR: zeigt auf\n");
    FPrintf(fh, "; das Verzeichnis des Programms. Ein Pfad auf RAM: wird\n");
    FPrintf(fh, "; nicht angenommen, der Cache soll den Neustart ueberleben.\n");
    FPrintf(fh, "cache=%s\n",
            (LONG)(ULONG)(p->cache[0] ? p->cache : CACHE_DIR_DEFAULT));

    FPrintf(fh, ";\n");
    FPrintf(fh, "; Verzeichnis mit eigenen MP3-Dateien. Es wird rekursiv\n");
    FPrintf(fh, "; durchsucht; leer heisst: kein eigener Bestand.\n");
    FPrintf(fh, "folder=%s\n", (LONG)(ULONG)p->folder);
    FPrintf(fh, ";\n");
    FPrintf(fh, "; AHI-Unit fuer die Wiedergabe (0 bis 3). Welcher Modus\n");
    FPrintf(fh, "; dahinter liegt, wird in den AHI-Voreinstellungen\n");
    FPrintf(fh, "; eingestellt, nicht hier.\n");
    FPrintf(fh, "ahiunit=%ld\n", (LONG)p->ahiunit);
    FPrintf(fh, ";\n");
    FPrintf(fh, "; Bilder je Sekunde des Visualizers: 0 (aus), 15, 20, 30, 60.\n");
    FPrintf(fh, "visfps=%ld\n", (LONG)p->visfps);
    FPrintf(fh, ";\n");
    FPrintf(fh, "; Eigener Bildschirm statt Workbench (0/1), dazu die\n");
    FPrintf(fh, "; Modus-Kennung aus dem ASL-Requester und die Masse.\n");
    FPrintf(fh, "; Wirkt erst beim naechsten Start.\n");
    FPrintf(fh, "ownscreen=%ld\n", (LONG)(p->ownscreen ? 1 : 0));
    FPrintf(fh, "screenmode=%08lx\n", (LONG)p->screenid);
    FPrintf(fh, "screensize=%ld %ld %ld\n",
            (LONG)p->screenw, (LONG)p->screenh, (LONG)p->screend);

    Close(fh);
    return 1;
}

int prefs_save(struct Prefs *p)
{
    int arc = prefs_write_one(p, PREFS_DIR_ARC);

    /* ENV: ist auf manchen Systemen nur eine Verknuepfung auf ENVARC:.
     * Dann schreibt der zweite Aufruf dieselbe Datei noch einmal - nicht
     * schoen, aber harmlos. */
    prefs_write_one(p, PREFS_DIR_ENV);

    if (!arc) {
        return fail(SUB_ENOPREFS, "could not save settings");
    }
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Netzwerk                                                            */
/* ------------------------------------------------------------------ */

/* Zeitgrenzen in Sekunden. Ohne sie haengt das GANZE Programm am
 * TCP-Stack: der Verbindungsaufbau zu einem Rechner, der nicht antwortet,
 * laeuft bei Roadshow rund 75 Sekunden. Bei AmiHomeassist war genau das
 * der Unterschied zwischen 76 s und 5,3 s je Fehlversuch. */
#define SUB_CONNECT_SECS  5
#define SUB_IO_SECS      20     /* Cover koennen ein paar hundert KB sein */

/* bsdsocket.library zaehlt Fehler wie BSD und NICHT wie das errno.h der
 * benutzten C-Bibliothek - deshalb stehen die beiden Werte hier selbst. */
#define SUB_EWOULDBLOCK  35
#define SUB_EINPROGRESS  36

/* AmiSSL-Basen. Die drei Namen sind vorgegeben: proto/amisslmaster.h und
 * proto/amissl.h greifen genau darauf zu. AmiSSL v5 spannt zwei
 * Bibliotheksbasen, weil OpenSSL mehr oeffentliche Funktionen hat, als in
 * eine passen - AmiSSLExtBase ist die zweite. */
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;

static SSL_CTX *g_ctx = NULL;
static LONG     g_ssl_errno = 0;

/* Eine Verbindung. ssl == NULL heisst nacktes HTTP. */
struct Conn {
    int  sock;
    SSL *ssl;
};

/* Bibliotheken werden EINMAL geoeffnet und bis zum Programmende offen
 * gehalten - anders als bei AmiHomeassist, wo bsdsocket je Anfrage auf-
 * und wieder zuging.
 *
 * Der Grund ist AmiSSL: dessen Initialisierung ist teuer, und der
 * Zertifikatsspeicher im SSL_CTX wird beim Schliessen weggeworfen. Bei
 * 290 Zertifikaten aus AmiSSL:Certs waere das je Titelwechsel spuerbar.
 * Dafuer muss der Aufrufer am Ende sub_cleanup() rufen. */
static int net_ensure(void)
{
    if (SocketBase) {
        return SUB_OK;
    }
    /* net.lib wird bewusst nicht gelinkt - die Basis oeffnen wir selbst.
     * Beides zusammen stuerzt beim Start ab. */
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) {
        return fail(SUB_ENET, "cannot open bsdsocket.library");
    }
    return SUB_OK;
}

/* Ein Anlauf, AmiSSL fuer eine bestimmte Mindestfassung zu oeffnen.
 * 0 heisst geglueckt. */
static LONG ssl_open(LONG apiversion)
{
    return OpenAmiSSLTags(apiversion,
               AmiSSL_UsesOpenSSLStructs, FALSE,
               AmiSSL_GetAmiSSLBase,      (ULONG)&AmiSSLBase,
               AmiSSL_GetAmiSSLExtBase,   (ULONG)&AmiSSLExtBase,
               AmiSSL_SocketBase,         (ULONG)SocketBase,
               AmiSSL_ErrNoPtr,           (ULONG)&g_ssl_errno,
               TAG_DONE);
}

static int ssl_ensure(void)
{
    if (g_ctx) {
        return SUB_OK;
    }

    AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        return fail(SUB_ENET,
                    "amisslmaster.library missing - AmiSSL not installed");
    }

    /* OpenAmiSSLTags() erledigt InitAmiSSLMaster, OpenAmiSSL und
     * InitAmiSSL in einem Zug; CloseAmiSSL() raeumt entsprechend alles
     * wieder ab.
     *
     * AmiSSL_UsesOpenSSLStructs = FALSE, weil dieser Code keine einzige
     * OpenSSL-Struktur selbst anfasst - nur undurchsichtige Zeiger. Das
     * ist nicht nur sauberer, es erlaubt AmiSSL auch, das Programm
     * spaeter automatisch auf eine neuere OpenSSL-Fassung zu heben, ohne
     * dass neu uebersetzt werden muss.
     *
     * AmiSSL_ErrNoPtr braucht AmiSSL, um Socketfehler ablegen zu koennen;
     * ohne den Tag hat es keinen Platz dafuer. */
    /* ZWEI ANLAEUFE, und der zweite ist der wichtige.
     *
     * Verlangt wird zuerst AMISSL_V3xx - das ist OpenSSL 3.0.3 und
     * heisst in Wahrheit "mindestens AmiSSL 5.1". Auf einer aelteren
     * Installation (AmiSSL 4.x mit OpenSSL 1.1.1, oder ein fruehes 5.0)
     * schlaegt das fehl, obwohl AmiSSL da ist und laeuft.
     *
     * Deshalb der Rueckfall auf AMISSL_V11x. Er ist gefahrlos: dieses
     * Programm benutzt keine einzige Funktion, die es nicht schon in
     * OpenSSL 1.1.1 gibt (nachgesehen, die Liste ist kurz - SSL_CTX_new,
     * TLS_client_method, SSL_set1_host und ein paar mehr). Die
     * angegebene Fassung ist ohnehin nur die UNTERGRENZE: liegt etwas
     * Neueres, nimmt AmiSSL von sich aus das.
     *
     * Am 6.9.2026 stand ein Emulator mit installiertem AmiSSL genau
     * hier - Fehler 2, ohne dass die Meldung sagte, woran es lag. */
    {
        LONG rc3, rc1 = 0;

        rc3 = ssl_open(AMISSL_V3xx);
        if (rc3 != 0) {
            rc1 = ssl_open(AMISSL_V11x);
        }

        if (rc3 != 0 && rc1 != 0) {
            char msg[160];

            sprintf(msg, "cannot start AmiSSL "
                         "(OpenSSL 3: error %ld, OpenSSL 1.1: error %ld, "
                         "master v%ld, Socket %s)",
                    (long)rc3, (long)rc1,
                    (long)(AmiSSLMasterBase
                               ? AmiSSLMasterBase->lib_Version : 0),
                    SocketBase ? "open" : "MISSING");
            CloseLibrary(AmiSSLMasterBase);
            AmiSSLMasterBase = NULL;
            return fail(SUB_ENET, msg);
        }
    }

    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) {
        CloseAmiSSL();
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        return fail(SUB_ENET, "no SSL context");
    }

    /* Der Zertifikatsspeicher liegt als Verzeichnis mit Hash-Namen unter
     * AmiSSL:Certs - genau die Form, die der zweite Parameter erwartet.
     * Der erste (eine Sammeldatei) bleibt deshalb NULL. */
    if (SSL_CTX_load_verify_locations(g_ctx, NULL, "AmiSSL:Certs") != 1) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
        CloseAmiSSL();
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        return fail(SUB_ENET, "cannot read AmiSSL:Certs");
    }

    /* Pruefen, nicht nur verschluesseln. Ueber diese Leitung geht das
     * Passwort - eine ungeprueft angenommene Gegenstelle waere hier
     * ungefaehr so gut wie gar kein TLS.
     *
     * Mindestens TLS 1.2: alles darunter ist seit Jahren gebrochen, und
     * kein Navidrome bietet es an. */
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);

    return SUB_OK;
}

void sub_cleanup(void)
{
    if (g_ctx) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }
    if (AmiSSLMasterBase) {
        /* CloseAmiSSL() gibt AmiSSLBase und AmiSSLExtBase mit frei -
         * die duerfen NICHT einzeln geschlossen werden. */
        CloseAmiSSL();
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

/* Gemerkte Namensaufloesung.
 *
 * Das ist keine Beschleunigung, sondern eine Fehlerbehebung. Die Adresse
 * dieses Projekts ist ein NAME (eine MyFritz-Adresse), also greift die
 * Abkuerzung ueber inet_addr() nicht und jede einzelne Anfrage lief durch
 * gethostbyname(). Das ist die einzige Wartestelle im ganzen Netzweg, die
 * sich NICHT begrenzen laesst - kein Zeitlimit, keine Abbruchmoeglichkeit.
 *
 * Beobachtet: waehrend die Oberflaeche offen war, brach das WLAN weg. Der
 * Amiga selbst lief weiter, aber die Fenster des Programms waren tot,
 * weil der Task in der Namensaufloesung stand und nie in die
 * MUI-Eingabeschleife zurueckkam.
 *
 * Mit dem Zwischenspeicher passiert das hoechstens EINMAL je
 * Programmlauf. Bewusst nur im Speicher und nicht auf Platte: hinter
 * einer MyFritz-Adresse steckt eine wechselnde IP, ein dauerhaft
 * gesicherter Wert waere irgendwann falsch. */
static char  g_dns_host[128] = "";
static ULONG g_dns_addr = 0;

/* Wartet, bis der Socket lesbar (oder schreibbar) ist.
 * 1 = bereit, 0 = Zeit abgelaufen, -1 = Fehler. */
static int sock_wait(int sock, BOOL forwrite, long secs)
{
    fd_set fds;
    struct timeval tv;
    long n;

    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_sec  = secs;
    tv.tv_usec = 0;

    /* Der Zeiger geht als APTR hinein: bsdsocket erwartet
     * 'struct __timeval', eine Typangabe, die kein Header ausfuellt. */
    n = WaitSelect(sock + 1,
                   forwrite ? NULL : (APTR)&fds,
                   forwrite ? (APTR)&fds : NULL,
                   NULL, (APTR)&tv, NULL);
    if (n < 0) {
        return -1;
    }
    return n > 0 ? 1 : 0;
}

static int connect_timeout(int sock, struct sockaddr_in *sa)
{
    LONG nb = 1;
    LONG err = 0;
    socklen_t errlen = sizeof(err);
    int w;

    IoctlSocket(sock, FIONBIO, (APTR)&nb);

    if (connect(sock, (struct sockaddr *)sa, sizeof(*sa)) == 0) {
        return SUB_OK;
    }
    if (Errno() != SUB_EINPROGRESS && Errno() != SUB_EWOULDBLOCK) {
        return fail(SUB_ENET, "connection refused");
    }

    w = sock_wait(sock, TRUE, SUB_CONNECT_SECS);
    if (w == 0) {
        return fail(SUB_ENET, "connection timed out");
    }
    if (w < 0) {
        return fail(SUB_ENET, "connection refused");
    }

    /* Schreibbar heisst nur "fertig", nicht "geglueckt" - abgewiesene
     * Verbindungen melden sich genauso. Der Grund steht in SO_ERROR. */
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (APTR)&err, &errlen) < 0
            || err != 0) {
        return fail(SUB_ENET, "connection refused");
    }
    return SUB_OK;
}

/* Der Socket bleibt nach connect_timeout() nicht blockierend. OpenSSL
 * meldet in dem Fall WANT_READ oder WANT_WRITE statt zu warten, und zwar
 * auch mitten in einem SSL_read: ein TLS-Datensatz kann eine erneute
 * Aushandlung ausloesen, dann will die Bibliothek schreiben, obwohl der
 * Aufrufer liest. Deshalb entscheidet ausschliesslich der gemeldete
 * Fehlercode, worauf gewartet wird - nicht die aufgerufene Funktion.
 *
 * 1 = noch einmal versuchen, 0 = Zeit abgelaufen, -1 = endgueltig. */
static int ssl_retry(struct Conn *c, int ret)
{
    int err = SSL_get_error(c->ssl, ret);

    if (err == SSL_ERROR_WANT_READ) {
        return sock_wait(c->sock, FALSE, SUB_IO_SECS);
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        return sock_wait(c->sock, TRUE, SUB_IO_SECS);
    }
    return -1;
}

static long conn_send(struct Conn *c, const char *buf, long len)
{
    if (c->ssl) {
        return (long)SSL_write(c->ssl, buf, (int)len);
    }
    return send(c->sock, (APTR)buf, len, 0);
}

static long conn_recv(struct Conn *c, char *buf, long len)
{
    if (c->ssl) {
        return (long)SSL_read(c->ssl, buf, (int)len);
    }
    return recv(c->sock, buf, len, 0);
}

static void conn_close(struct Conn *c)
{
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->sock >= 0) {
        CloseSocket(c->sock);
        c->sock = -1;
    }
}

/* TLS-Handshake auf einem bereits verbundenen, nicht blockierenden
 * Socket. */
static int ssl_handshake(struct Conn *c, const char *host)
{
    int rc;

    c->ssl = SSL_new(g_ctx);
    if (!c->ssl) {
        return fail(SUB_ENET, "SSL_new failed");
    }
    SSL_set_fd(c->ssl, c->sock);

    /* SNI. Ohne das kommt hinter einem Reverse Proxy - und die Fritz!Box
     * mit MyFritz ist genau das - das falsche oder gar kein Zertifikat
     * zurueck, weil der Server nicht weiss, welcher Dienst gemeint ist. */
    SSL_set_tlsext_host_name(c->ssl, host);

    /* Prueft zusaetzlich, dass der Name im Zertifikat zu dem passt, den
     * wir angewaehlt haben. SSL_VERIFY_PEER allein pruefte nur die
     * Signaturkette - ein gueltiges Zertifikat fuer irgendeinen anderen
     * Rechner kaeme sonst durch. */
    SSL_set1_host(c->ssl, host);

    for (;;) {
        int w;

        rc = SSL_connect(c->ssl);
        if (rc == 1) {
            return SUB_OK;
        }
        w = ssl_retry(c, rc);
        if (w == 1) {
            continue;
        }
        if (w == 0) {
            return fail(SUB_ENET, "TLS handshake timed out");
        }
        break;
    }

    {
        long v = SSL_get_verify_result(c->ssl);
        char msg[200];

        if (v != X509_V_OK) {
            /* Der haeufigste Fall auf einem Amiga ist eine falsch
             * gestellte Uhr - dann ist jedes Zertifikat entweder noch
             * nicht oder nicht mehr gueltig. Deshalb steht der Hinweis
             * gleich in der Meldung. */
            sprintf(msg, "certificate rejected (reason %ld) - is the clock set?",
                    v);
        } else {
            strcpy(msg, "TLS handshake failed");
        }
        return fail(SUB_ENET, msg);
    }
}

static int recv_all(struct Conn *c, char **out, long *outlen)
{
    long cap = 32768;
    long len = 0;
    char *buf = malloc(cap);
    long n;
    long rounds = 0;

    if (!buf) {
        return fail(SUB_EMEM, "out of memory");
    }

    for (;;) {
        int w;

        /* Notbremse. Mehrere Zweige unten machen "weiter" statt
         * abzubrechen - EWOULDBLOCK trotz gemeldeter Lesbarkeit, ein
         * TLS-Datensatz, der noch nicht vollstaendig ist. Jeder fuer sich
         * ist richtig, zusammen koennen sie im Fehlerfall eine Schleife
         * bilden, die nie endet. Die Zaehlung bricht sie auf; bei einer
         * gesunden Verbindung wird sie nie erreicht. */
        if (++rounds > 20000) {
            free(buf);
            return fail(SUB_ENET, "receiving stalled");
        }

        if (len + 4096 >= cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                return fail(SUB_EMEM, "out of memory");
            }
            buf = nb;
            cap *= 2;
        }

        if (!c->ssl) {
            /* Der Socket ist nicht blockierend, also erst warten, bis
             * wirklich etwas da ist. Sonst kaeme recv() sofort mit
             * EWOULDBLOCK zurueck und die Schleife liefe heiss. */
            w = sock_wait(c->sock, FALSE, SUB_IO_SECS);
            if (w == 0) {
                free(buf);
                return fail(SUB_ENET, "receive timed out");
            }
            if (w < 0) {
                free(buf);
                return fail(SUB_ENET, "receive error");
            }
        }

        n = conn_recv(c, buf + len, cap - len - 1);

        if (n > 0) {
            len += n;
            continue;
        }

        if (c->ssl) {
            /* Bei TLS ist "0 gelesen" nicht gleichbedeutend mit
             * "fertig": SSL_ERROR_ZERO_RETURN heisst sauber beendet,
             * WANT_READ/WANT_WRITE heisst nur "noch nichts da". */
            int err = SSL_get_error(c->ssl, (int)n);

            if (err == SSL_ERROR_ZERO_RETURN) {
                break;
            }
            w = ssl_retry(c, (int)n);
            if (w == 1) {
                continue;
            }
            if (w == 0) {
                free(buf);
                return fail(SUB_ENET, "receive timed out");
            }
            /* Ein abgeschnittener Strom ohne close_notify. Solange
             * Kopfzeilen und Rumpf da sind, ist das brauchbar - viele
             * Server machen es so. */
            break;
        }

        if (n < 0 && Errno() == SUB_EWOULDBLOCK) {
            continue;               /* Fehlalarm - weiter warten */
        }
        break;                      /* 0 = Gegenstelle hat zugemacht */
    }

    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return SUB_OK;
}

/* Ein GET. Der Rumpf kommt als frisch belegter Puffer zurueck, der
 * Aufrufer gibt ihn frei. resp_len zaehlt ohne das angehaengte Nullbyte -
 * der Puffer ist also fuer Text UND fuer Binaerdaten (Cover) brauchbar.
 *
 * HTTP/1.0 mit "Connection: close": damit ist der Rumpf schlicht alles bis
 * zum Verbindungsende. Das spart den Parser fuer Transfer-Encoding:
 * chunked, den man sonst nur fuer Sonderfaelle braeuchte - und Navidrome
 * liefert chunked, sobald man HTTP/1.1 ansagt. */
/* Die Zielangabe steht nicht mehr in struct Prefs, weil nicht jede
 * Anfrage an den eigenen Navidrome geht: die Liedtexte kommen von
 * lrclib.net, einem voellig anderen Rechner. Prefs wuerde dafuer nur
 * missbraucht. */
static int http_get_host(const char *host, int port, BOOL https,
                         const char *path, const char *useragent,
                         char **resp_body, long *resp_len)
{
    struct hostent *he;
    struct sockaddr_in sa;
    struct Conn c;
    in_addr_t addr;
    BOOL used_cache = FALSE;
    int rc;
    long sent, total, n;
    char *req = NULL;
    char *raw = NULL;
    long rawlen = 0;
    char *hdrend;
    int status = 0;

    *resp_body = NULL;
    *resp_len = 0;

    c.sock = -1;
    c.ssl = NULL;

    rc = net_ensure();
    if (rc != SUB_OK) {
        return rc;
    }
    if (https) {
        rc = ssl_ensure();
        if (rc != SUB_OK) {
            return rc;
        }
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    /* Steht in den Einstellungen eine Zahlenadresse, ist der Namensdienst
     * ueberfluessig. Das nimmt die einzige Wartestelle heraus, die sich
     * nicht begrenzen laesst: gethostbyname() blockiert, so lange der
     * Resolver will. */
    addr = inet_addr((STRPTR)host);
    if (addr != INADDR_NONE) {
        sa.sin_addr.s_addr = addr;
    } else if (g_dns_addr != 0 && stricmp(g_dns_host, host) == 0) {
        sa.sin_addr.s_addr = g_dns_addr;    /* schon aufgeloest */
        used_cache = TRUE;
    } else {
        he = gethostbyname((UBYTE *)host);
        if (!he) {
            return fail(SUB_ENET, "cannot resolve host name");
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

        copy_field(g_dns_host, sizeof(g_dns_host), host);
        g_dns_addr = sa.sin_addr.s_addr;
    }

    c.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (c.sock < 0) {
        return fail(SUB_ENET, "socket() failed");
    }

    rc = connect_timeout(c.sock, &sa);
    if (rc != SUB_OK && used_cache) {
        /* Hinter einer MyFritz-Adresse steckt die WAN-Adresse des
         * Anschlusses, und die wechselt - taeglich bei Zwangstrennung.
         * Die gemerkte Adresse zeigt dann ins Leere, und ein Programm,
         * das den Tag ueber laeuft, waere tot bis zum Neustart.
         * Gemessen am 20.9.2026: der Amiga hielt 37.81.44.75 fest,
         * richtig war laengst 37.81.123.130.
         *
         * Also einmal frisch aufloesen und noch einmal versuchen. */
        conn_close(&c);
        g_dns_addr = 0;
        he = gethostbyname((UBYTE *)host);
        if (!he) {
            return fail(SUB_ENET, "cannot resolve host name");
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
        copy_field(g_dns_host, sizeof(g_dns_host), host);
        g_dns_addr = sa.sin_addr.s_addr;

        c.sock = socket(AF_INET, SOCK_STREAM, 0);
        if (c.sock < 0) {
            return fail(SUB_ENET, "socket() failed");
        }
        rc = connect_timeout(c.sock, &sa);
    }
    if (rc != SUB_OK) {
        conn_close(&c);
        return rc;              /* Meldung steht schon */
    }

    if (https) {
        rc = ssl_handshake(&c, host);
        if (rc != SUB_OK) {
            conn_close(&c);
            return rc;
        }
    }

    req = malloc(strlen(path) + strlen(host) + strlen(useragent) + 256);
    if (!req) {
        conn_close(&c);
        return fail(SUB_EMEM, "out of memory");
    }

    /* Der Port gehoert nur dann in die Host-Kopfzeile, wenn er vom
     * Standard abweicht. Reverse Proxies - und die Fritz!Box ist einer -
     * vergleichen den Wert mitunter mit ihrer Konfiguration und weisen
     * "host:443" ab, wo sie "host" erwarten. */
    if ((https && port == 443) || (!https && port == 80)) {
        sprintf(req,
                "GET %s HTTP/1.0\r\n"
                "Host: %s\r\n"
                "User-Agent: %s\r\n"
                "Connection: close\r\n"
                "\r\n",
                path, host, useragent);
    } else {
        sprintf(req,
                "GET %s HTTP/1.0\r\n"
                "Host: %s:%d\r\n"
                "User-Agent: %s\r\n"
                "Connection: close\r\n"
                "\r\n",
                path, host, port, useragent);
    }

    total = (long)strlen(req);
    sent = 0;
    while (sent < total) {
        int w;

        if (c.ssl) {
            n = conn_send(&c, req + sent, total - sent);
            if (n <= 0) {
                w = ssl_retry(&c, (int)n);
                if (w == 1) {
                    continue;
                }
                n = -1;
            }
        } else {
            w = sock_wait(c.sock, TRUE, SUB_IO_SECS);
            if (w > 0) {
                n = conn_send(&c, req + sent, total - sent);
                if (n < 0 && Errno() == SUB_EWOULDBLOCK) {
                    continue;
                }
            } else {
                n = -1;
            }
        }
        if (n <= 0) {
            free(req);
            conn_close(&c);
            return fail(SUB_ENET, "send error");
        }
        sent += n;
    }
    free(req);

    rc = recv_all(&c, &raw, &rawlen);
    conn_close(&c);

    if (rc != SUB_OK) {
        return rc;
    }

    if (sscanf(raw, "HTTP/%*d.%*d %d", &status) != 1) {
        free(raw);
        return fail(SUB_EHTTP, "no valid HTTP response");
    }

    hdrend = strstr(raw, "\r\n\r\n");
    if (!hdrend) {
        free(raw);
        return fail(SUB_EHTTP, "response has no body");
    }
    hdrend += 4;

    if (status < 200 || status > 299) {
        /* Den Anfang des Rumpfes mitnehmen: erst daran sieht man, WER
         * abgelehnt hat - Navidrome selbst oder etwas davor (Fritzbox,
         * Proxy). Nur druckbare Zeichen, Zeilenumbrueche als Leerzeichen,
         * damit die Meldung in eine Statuszeile passt. */
        char msg[160];
        int n, k;

        n = sprintf(msg, "server replied HTTP %d", status);
        if (*hdrend) {
            n += sprintf(msg + n, ": ");
            for (k = 0; hdrend[k] && n < (int)sizeof(msg) - 1 && k < 80; k++) {
                unsigned char ch = (unsigned char)hdrend[k];
                msg[n++] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
            }
            msg[n] = '\0';
        }
        free(raw);
        return fail(SUB_EHTTP, msg);
    }

    /* Der Rumpf wandert an den Anfang des Puffers, damit der Aufrufer
     * genau einen Zeiger bekommt, den er mit free() wieder loswird. */
    *resp_len = rawlen - (hdrend - raw);
    memmove(raw, hdrend, (size_t)*resp_len + 1);
    *resp_body = raw;
    return SUB_OK;
}

static int http_get(struct Prefs *p, const char *path,
                    char **resp_body, long *resp_len)
{
    return http_get_host(p->host, p->port, p->https, path,
                         SUB_CLIENT, resp_body, resp_len);
}

/* ------------------------------------------------------------------ */
/* Minimaler XML-Leser                                                 */
/* ------------------------------------------------------------------ */

/* Ein vollstaendiger XML-Stack waere hier Verschwendung. Subsonic-Antworten
 * sind flach und regelmaessig: die Nutzdaten stecken ausnahmslos in
 * ATTRIBUTEN leerer Elemente, nicht im Textinhalt.
 *
 *   <album id="al-3" name="Sleepwalking" artist="Nina" year="2018"/>
 *
 * Damit reichen zwei Bausteine - "finde das naechste Element mit diesem
 * Namen" und "hol dieses Attribut" - und das Ganze kommt ohne Baum,
 * ohne Rueckverfolgung und ohne nennenswerten Speicher aus. */

/* Zeigt auf das '<' des naechsten Elements mit genau diesem Namen.
 *
 * Der Test auf das Zeichen HINTER dem Namen ist der Kern: ohne ihn faende
 * die Suche nach "album" auch <albumList2> und der Leser liefe in der
 * falschen Ebene weiter. */
static const char *xml_find(const char *from, const char *tag)
{
    int taglen = (int)strlen(tag);

    while (from && (from = strchr(from, '<')) != NULL) {
        if (strncmp(from + 1, tag, taglen) == 0) {
            char after = from[1 + taglen];
            if (after == ' ' || after == '\t' || after == '\n'
                    || after == '\r' || after == '>' || after == '/') {
                return from;
            }
        }
        from++;
    }
    return NULL;
}

/* Haengt einen Unicode-Kodepunkt als UTF-8 an. Die Entitaeten werden also
 * bewusst NACH UTF-8 aufgeloest und erst ganz am Ende in einem Rutsch nach
 * Latin-1 gewandelt - sonst muesste jede Stelle den Zeichensatz einzeln
 * bedenken und &#252; wuerde anders behandelt als ein direkt gesendetes ue. */
static int append_utf8(char *out, int pos, int outsize, unsigned long cp)
{
    if (cp < 0x80) {
        if (pos + 1 < outsize) {
            out[pos++] = (char)cp;
        }
    } else if (cp < 0x800) {
        if (pos + 2 < outsize) {
            out[pos++] = (char)(0xc0 | (cp >> 6));
            out[pos++] = (char)(0x80 | (cp & 0x3f));
        }
    } else {
        if (pos + 3 < outsize) {
            out[pos++] = (char)(0xe0 | (cp >> 12));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[pos++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    return pos;
}

/* Loest die fuenf XML-Entitaeten und Zahlenverweise auf und wandelt
 * anschliessend nach Latin-1. */
static void xml_decode(const char *src, int srclen, char *out, int outsize)
{
    int i = 0, o = 0;

    while (i < srclen && o < outsize - 1) {
        if (src[i] == '&') {
            const char *e = src + i;
            int rest = srclen - i;

            if (rest >= 5 && strncmp(e, "&amp;", 5) == 0) {
                out[o++] = '&'; i += 5; continue;
            }
            if (rest >= 4 && strncmp(e, "&lt;", 4) == 0) {
                out[o++] = '<'; i += 4; continue;
            }
            if (rest >= 4 && strncmp(e, "&gt;", 4) == 0) {
                out[o++] = '>'; i += 4; continue;
            }
            if (rest >= 6 && strncmp(e, "&quot;", 6) == 0) {
                out[o++] = '"'; i += 6; continue;
            }
            if (rest >= 6 && strncmp(e, "&apos;", 6) == 0) {
                out[o++] = '\''; i += 6; continue;
            }
            if (rest >= 4 && e[1] == '#') {
                unsigned long cp = 0;
                int j = 2;
                int hex = (e[2] == 'x' || e[2] == 'X');

                if (hex) {
                    j = 3;
                    while (j < rest && isxdigit((unsigned char)e[j])) {
                        char c = e[j];
                        cp = cp * 16 + (unsigned long)
                             (c <= '9' ? c - '0' : (tolower(c) - 'a' + 10));
                        j++;
                    }
                } else {
                    while (j < rest && e[j] >= '0' && e[j] <= '9') {
                        cp = cp * 10 + (unsigned long)(e[j] - '0');
                        j++;
                    }
                }
                if (j < rest && e[j] == ';' && cp > 0 && cp < 0x10000) {
                    o = append_utf8(out, o, outsize, cp);
                    i += j + 1;
                    continue;
                }
            }
        }
        out[o++] = src[i++];
    }
    out[o] = '\0';
    utf8_to_latin1(out);
}

/* Holt ein Attribut aus dem Element, auf das elem zeigt.
 *
 * Geht die Attributliste Paar fuer Paar durch, statt nach ' name="' zu
 * suchen. Der Unterschied faellt sofort auf: die Suche nach id="
 * traefe in <album id="1" artistId="7"/> je nach Reihenfolge das falsche
 * Attribut, weil artistId auf id endet. */
static BOOL xml_attr(const char *elem, const char *name,
                     char *out, int outsize)
{
    const char *p = elem + 1;
    int namelen = (int)strlen(name);

    out[0] = '\0';

    while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/') {
        p++;                    /* ueber den Elementnamen hinweg */
    }

    for (;;) {
        const char *nstart;
        int nlen;
        char quote;
        const char *vstart;

        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '>' || *p == '/') {
            return FALSE;
        }

        nstart = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p)
                && *p != '>' && *p != '/') {
            p++;
        }
        nlen = (int)(p - nstart);

        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '=') {
            return FALSE;       /* Attribut ohne Wert - hier nie gueltig */
        }
        p++;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        quote = *p;
        if (quote != '"' && quote != '\'') {
            return FALSE;
        }
        p++;
        vstart = p;
        while (*p && *p != quote) {
            p++;
        }
        if (*p != quote) {
            return FALSE;
        }

        if (nlen == namelen && strncmp(nstart, name, (size_t)nlen) == 0) {
            xml_decode(vstart, (int)(p - vstart), out, outsize);
            return TRUE;
        }
        p++;
    }
}

static int xml_attr_int(const char *elem, const char *name)
{
    char buf[32];

    if (!xml_attr(elem, name, buf, sizeof(buf))) {
        return 0;
    }
    return atoi(buf);
}

/* ------------------------------------------------------------------ */
/* Subsonic-Anfragen                                                   */
/* ------------------------------------------------------------------ */

/* Alles, was nicht Buchstabe, Ziffer oder -_.~ ist, wird zu %XX. Betrifft
 * vor allem die Suche ("Nina & Band") und Navidromes IDs, die zwar meist
 * harmlos aussehen, aber nirgends als URL-sicher zugesichert sind. */
static void url_encode(const char *s, char *out, int outsize)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;

    while (*s && o < outsize - 4) {
        unsigned char c = (unsigned char)*s++;

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')
                || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0x0f];
            out[o++] = hex[c & 0x0f];
        }
    }
    out[o] = '\0';
}

/* Acht Hexziffern Salt.
 *
 * Kein rand(): dessen Startwert waere ohne Uhrzeit auf jedem Amiga
 * derselbe. DateStamp() liefert Tage, Minuten und Ticks (1/50 s) seit dem
 * 1.1.1978, der Zaehler trennt zusaetzlich zwei Anfragen innerhalb
 * desselben Ticks. Kryptografische Guete braucht das Salt nicht - es soll
 * nur verhindern, dass immer derselbe Hash ueber die Leitung geht. */
static void make_salt(char *out)
{
    static const char hex[] = "0123456789abcdef";
    static unsigned long counter = 0;
    struct DateStamp ds;
    unsigned long v;
    int i;

    DateStamp(&ds);
    v = ((unsigned long)ds.ds_Days << 20)
      ^ ((unsigned long)ds.ds_Minute << 11)
      ^ ((unsigned long)ds.ds_Tick)
      ^ (++counter * 2654435761UL);

    for (i = 0; i < 8; i++) {
        out[i] = hex[(v >> ((7 - i) * 4)) & 0x0f];
    }
    out[8] = '\0';
}

/* Baut "/rest/<methode>.view?u=..&t=..&s=..&v=..&c=..&f=xml<extra>".
 *
 * extra faengt, wenn vorhanden, mit & an und ist vom Aufrufer bereits
 * URL-kodiert. */
static void build_path(struct Prefs *p, const char *method, const char *extra,
                       char *out, int outsize)
{
    char user[3 * sizeof(p->user)];
    char salt[9];
    char saltpass[sizeof(p->pass) + 16];
    char token[33];

    url_encode(p->user, user, sizeof(user));
    make_salt(salt);

    /* Subsonic will t = md5(passwort + salt), NICHT md5(salt + passwort).
     * Beides laeuft durch, nur eines meldet sich an. */
    sprintf(saltpass, "%s%s", p->pass, salt);
    md5_hex(saltpass, token);

    sprintf(out, "/rest/%s.view?u=%s&t=%s&s=%s&v=%s&c=%s&f=xml%s",
            method, user, token, salt, SUB_API_VERSION, SUB_CLIENT,
            extra ? extra : "");
    (void)outsize;
}

/* Ruft eine Methode auf und prueft die Subsonic-Huelle.
 *
 * Ein Anmeldefehler kommt hier als HTTP 200 mit status="failed" an - wer
 * nur den HTTP-Code prueft, sieht eine leere Liste und sucht den Fehler
 * an der falschen Stelle. */
static int api_call(struct Prefs *p, const char *method, const char *extra,
                    char **body, long *len)
{
    char path[1024];
    const char *resp;
    char status[16];
    int rc;

    if (p->host[0] == '\0' || p->user[0] == '\0') {
        return fail(SUB_ENOPREFS, "settings incomplete");
    }

    build_path(p, method, extra, path, sizeof(path));
    rc = http_get(p, path, body, len);
    if (rc != SUB_OK) {
        return rc;
    }

    resp = xml_find(*body, "subsonic-response");
    if (!resp) {
        free(*body);
        *body = NULL;
        return fail(SUB_EAPI, "no Subsonic response");
    }

    if (xml_attr(resp, "status", status, sizeof(status))
            && stricmp(status, "ok") != 0) {
        const char *err = xml_find(*body, "error");
        char msg[200];

        msg[0] = '\0';
        if (err) {
            char code[16], text[160];
            xml_attr(err, "code", code, sizeof(code));
            xml_attr(err, "message", text, sizeof(text));
            sprintf(msg, "server error %s: %s",
                    code[0] ? code : "?", text[0] ? text : "no details");
        } else {
            strcpy(msg, "server rejected the request");
        }
        free(*body);
        *body = NULL;
        return fail(SUB_EAPI, msg);
    }

    return SUB_OK;
}

int sub_resolve(struct Prefs *p)
{
    struct hostent *he;
    in_addr_t addr;
    int rc;

    if (!p->host[0]) {
        return fail(SUB_ENOPREFS, "no server configured");
    }

    /* net_ensure() MUSS vor jedem Aufruf einer bsdsocket-Funktion
     * stehen, inet_addr() eingeschlossen.
     *
     * Die erste Fassung hatte inet_addr() davor - der Gedanke war, sich
     * das Oeffnen zu sparen, wenn ohnehin eine Zahlenadresse dasteht.
     * Das ist ein Aufruf ueber eine NULL-Basis: der Sprung landet bei
     * 0 minus LVO, und der Arbeitsprozess starb mit "Exception A:
     * line-a emulator, PC FFFFFF4C". Genau diese Signatur - eine PC-
     * Adresse knapp unter dem Adressende - heisst immer "Bibliothek
     * nicht geoeffnet". */
    rc = net_ensure();
    if (rc != SUB_OK) {
        return rc;
    }

    /* Zahlenadresse - nichts aufzuloesen. */
    addr = inet_addr((STRPTR)p->host);
    if (addr != INADDR_NONE) {
        return SUB_OK;
    }
    if (g_dns_addr != 0 && stricmp(g_dns_host, p->host) == 0) {
        return SUB_OK;
    }

    he = gethostbyname((UBYTE *)p->host);
    if (!he) {
        return fail(SUB_ENET, "cannot resolve host name");
    }
    memcpy(&g_dns_addr, he->h_addr_list[0], (size_t)he->h_length);
    copy_field(g_dns_host, sizeof(g_dns_host), p->host);
    return SUB_OK;
}

int sub_ping(struct Prefs *p)
{
    char *body = NULL;
    long len = 0;
    int rc = api_call(p, "ping", NULL, &body, &len);

    if (body) {
        free(body);
    }
    return rc;
}

/* Fuellt ein Song-Element. Wird von drei Methoden benutzt - getAlbum,
 * search3 und getSimilarSongs liefern alle dieselben Attribute. */
static void fill_song(struct Song *s, const char *e)
{
    xml_attr(e, "id",       s->id,       sizeof(s->id));
    xml_attr(e, "title",    s->title,    sizeof(s->title));
    xml_attr(e, "artist",   s->artist,   sizeof(s->artist));
    xml_attr(e, "album",    s->album,    sizeof(s->album));
    xml_attr(e, "albumId",  s->albumid,  sizeof(s->albumid));
    xml_attr(e, "coverArt", s->coverart, sizeof(s->coverart));
    xml_attr(e, "suffix",   s->suffix,   sizeof(s->suffix));
    s->track    = xml_attr_int(e, "track");
    s->year     = xml_attr_int(e, "year");
    s->duration = xml_attr_int(e, "duration");
    s->bitrate  = xml_attr_int(e, "bitRate");
}

static void fill_album(struct Album *a, const char *e)
{
    xml_attr(e, "id",       a->id,       sizeof(a->id));
    xml_attr(e, "name",     a->name,     sizeof(a->name));
    xml_attr(e, "artist",   a->artist,   sizeof(a->artist));
    xml_attr(e, "artistId", a->artistid, sizeof(a->artistid));
    xml_attr(e, "coverArt", a->coverart, sizeof(a->coverart));
    a->year      = xml_attr_int(e, "year");
    a->songcount = xml_attr_int(e, "songCount");
    a->duration  = xml_attr_int(e, "duration");
}

/* Sammelt alle Elemente eines Namens aus der Antwort in die Liste. */
static int collect(char *body, const char *tag, struct SubList *out, int kind)
{
    const char *e = body;

    while ((e = xml_find(e, tag)) != NULL) {
        void *slot = list_add(out);

        if (!slot) {
            return fail(SUB_EMEM, "out of memory");
        }
        if (kind == 0) {
            fill_song((struct Song *)slot, e);
        } else {
            fill_album((struct Album *)slot, e);
        }
        e++;
    }
    return SUB_OK;
}

int sub_get_artists(struct Prefs *p, struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    const char *e;
    int rc;

    rc = api_call(p, "getArtists", NULL, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }

    /* getArtists gruppiert nach Anfangsbuchstaben:
     *   <artists><index name="A"><artist .../></index>...</artists>
     * Die Zwischenebene interessiert nicht - wir sammeln schlicht alle
     * <artist>-Elemente ein, egal wie tief sie haengen. */
    e = body;
    while ((e = xml_find(e, "artist")) != NULL) {
        struct Artist *a = (struct Artist *)list_add(out);

        if (!a) {
            free(body);
            return fail(SUB_EMEM, "out of memory");
        }
        xml_attr(e, "id",   a->id,   sizeof(a->id));
        xml_attr(e, "name", a->name, sizeof(a->name));
        a->albumcount = xml_attr_int(e, "albumCount");
        e++;
    }

    free(body);
    return SUB_OK;
}

int sub_get_albums(struct Prefs *p, const char *type, int size, int offset,
                   struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    char extra[128];
    char enctype[64];
    int rc;

    url_encode(type ? type : "alphabeticalByName", enctype, sizeof(enctype));
    sprintf(extra, "&type=%s&size=%d&offset=%d", enctype, size, offset);

    rc = api_call(p, "getAlbumList2", extra, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    rc = collect(body, "album", out, 1);
    free(body);
    return rc;
}

int sub_get_artist_albums(struct Prefs *p, const char *artistid,
                          struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    char extra[128];
    char encid[96];
    int rc;

    url_encode(artistid, encid, sizeof(encid));
    sprintf(extra, "&id=%s", encid);

    rc = api_call(p, "getArtist", extra, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    rc = collect(body, "album", out, 1);
    free(body);
    return rc;
}

int sub_get_album_songs(struct Prefs *p, const char *albumid,
                        struct Album *info, struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    char extra[128];
    char encid[96];
    const char *e;
    int rc;

    url_encode(albumid, encid, sizeof(encid));
    sprintf(extra, "&id=%s", encid);

    rc = api_call(p, "getAlbum", extra, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }

    /* getAlbum liefert das Album selbst als aeusseres Element mit den
     * Titeln darin. Das erste <album> ist also die Kopfzeile, nicht ein
     * Eintrag einer Liste. */
    if (info) {
        memset(info, 0, sizeof(*info));
        e = xml_find(body, "album");
        if (e) {
            fill_album(info, e);
        }
    }

    rc = collect(body, "song", out, 0);
    free(body);
    return rc;
}

int sub_search_songs(struct Prefs *p, const char *query, int size,
                     int offset, struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    char extra[512];
    char encq[400];
    int rc;

    url_encode(query, encq, sizeof(encq));

    /* artistCount und albumCount ausdruecklich auf 0: sonst schickt
     * Navidrome zu jeder Suche auch Interpreten und Alben mit, die hier
     * niemand liest - unnoetige Bytes ueber eine Leitung, die auf einem
     * A500 zaehlt. */
    sprintf(extra,
            "&query=%s&songCount=%d&songOffset=%d"
            "&artistCount=0&albumCount=0",
            encq, size, offset);

    rc = api_call(p, "search3", extra, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    rc = collect(body, "song", out, 0);
    free(body);
    return rc;
}

int sub_get_similar(struct Prefs *p, const char *songid, int count,
                    struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    char extra[128];
    char encid[96];
    int rc;

    url_encode(songid, encid, sizeof(encid));
    sprintf(extra, "&id=%s&count=%d", encid, count);

    rc = api_call(p, "getSimilarSongs2", extra, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    rc = collect(body, "song", out, 0);
    free(body);
    return rc;
}

/* Die als Favorit markierten Titel.
 *
 * getStarred2 liefert Interpreten, Alben UND Titel in einer Antwort;
 * eingesammelt werden nur die <song>-Elemente. Eine Moeglichkeit, dem
 * Server die anderen beiden abzugewoehnen, gibt es bei diesem Aufruf
 * nicht - anders als bei search3. */
int sub_get_starred_songs(struct Prefs *p, struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    int rc;

    rc = api_call(p, "getStarred2", "", &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }
    rc = collect(body, "song", out, 0);
    free(body);
    return rc;
}

void sub_album_cover_id(const char *albumid, char *out, int outsize)
{
    out[0] = '\0';
    if (albumid && albumid[0]) {
        snprintf(out, outsize, "al-%s", albumid);
    }
}

void sub_album_cover(const struct Song *s, char *out, int outsize)
{
    out[0] = '\0';
    if (!s) {
        return;
    }
    if (!s->albumid[0]) {
        /* Kein albumId in der Antwort - dann bleibt nur die eigene
         * Kennung des Titels. Ein Bild je Titel, aber besser als keins. */
        strncpy(out, s->coverart, outsize - 1);
        out[outsize - 1] = '\0';
        return;
    }
    sub_album_cover_id(s->albumid, out, outsize);
}

int sub_api_raw(struct Prefs *p, const char *method, const char *extra,
                char **body, long *len)
{
    return api_call(p, method, extra ? extra : "", body, len);
}

/* Die Radiostationen des Servers.
 *
 * Eigene Schleife statt collect(): eine Station ist kein Titel und kein
 * Album, sie hat ihre eigenen vier Angaben. */
int sub_get_radios(struct Prefs *p, struct SubList *out)
{
    char *body = NULL;
    long len = 0;
    const char *e;
    int rc;

    rc = api_call(p, "getInternetRadioStations", NULL, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }

    e = body;
    while ((e = xml_find(e, "internetRadioStation")) != NULL) {
        struct Radio *r = (struct Radio *)list_add(out);

        if (!r) {
            free(body);
            return fail(SUB_EMEM, "out of memory");
        }
        xml_attr(e, "id",          r->id,   sizeof(r->id));
        xml_attr(e, "name",        r->name, sizeof(r->name));
        xml_attr(e, "streamUrl",   r->url,  sizeof(r->url));
        xml_attr(e, "homePageUrl", r->home, sizeof(r->home));
        e++;
    }

    free(body);
    return SUB_OK;
}

int sub_get_lyrics(struct Prefs *p, const char *songid, char *out, int outsize)
{
    char *body = NULL;
    long len = 0;
    char extra[128];
    char encid[96];
    const char *e;
    int pos = 0;
    int rc;

    out[0] = '\0';

    url_encode(songid, encid, sizeof(encid));
    sprintf(extra, "&id=%s", encid);

    /* getLyricsBySongId ist eine OpenSubsonic-Erweiterung; Navidrome kann
     * sie, das originale Subsonic nicht. Ein Server ohne sie antwortet mit
     * Fehler 0 oder 70 - das ist kein Grund, den Tab scheitern zu lassen,
     * sondern heisst schlicht "kein Liedtext". */
    rc = api_call(p, "getLyricsBySongId", extra, &body, &len);
    if (rc != SUB_OK) {
        return SUB_OK;          /* Meldung steht in g_error, Text bleibt leer */
    }

    /* <structuredLyrics ...><line start="0" value="..."/>...  Bei
     * unsynchronisierten Texten fehlt start, value gibt es immer. */
    e = body;
    while ((e = xml_find(e, "line")) != NULL) {
        char line[300];

        if (xml_attr(e, "value", line, sizeof(line))) {
            int n = (int)strlen(line);
            if (pos + n + 2 >= outsize) {
                break;
            }
            memcpy(out + pos, line, (size_t)n);
            pos += n;
            out[pos++] = '\n';
        }
        e++;
    }
    out[pos] = '\0';

    free(body);
    return SUB_OK;
}

int sub_stream_url(struct Prefs *p, const char *songid, char *out, int outsize)
{
    char path[1024];
    char extra[128];
    char encid[96];

    if (p->host[0] == '\0') {
        return fail(SUB_ENOPREFS, "no server configured");
    }

    url_encode(songid, encid, sizeof(encid));
    sprintf(extra, "&id=%s", encid);
    build_path(p, "stream", extra, path, sizeof(path));

    if ((int)(strlen(path) + strlen(p->host) + 32) > outsize) {
        return fail(SUB_EMEM, "stream URL too long");
    }

    /* AmigaAMP bekommt die vollstaendige URL und holt den Strom selbst.
     * Die Anmeldung steckt dabei in den Query-Parametern - eine Kopfzeile
     * koennte man ihm nicht mitgeben. */
    sprintf(out, "%s://%s:%d%s",
            p->https ? "https" : "http", p->host, p->port, path);
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Strom vom Server                                                    */
/* ------------------------------------------------------------------ */

/* Anders als api_call() holt das hier NICHT die ganze Antwort. Die
 * Verbindung bleibt offen, und der Aufrufer liest, so viel er gerade
 * braucht. Ein Titel hat leicht 10 MB - die passen auf einem Amiga zwar
 * ins Fast-RAM, aber warten muesste man trotzdem, und beim Umschalten
 * waere alles umsonst geholt. */
int sub_stream_open(struct Prefs *p, const char *songid, long offset,
                    struct SubStream *st)
{
    struct Conn c;
    struct sockaddr_in sa;
    struct hostent *he;
    in_addr_t addr;
    char path[1024];
    char extra[160];
    char encid[96];
    char req[1400];
    char head[1500];
    long n, sent, total, got = 0;
    BOOL used_cache = FALSE;
    int rc, status = 0, rounds = 0;
    char *hdrend;

    memset(st, 0, sizeof(*st));
    st->sock = -1;
    head[0] = '\0';

    if (p->host[0] == '\0') {
        return fail(SUB_ENOPREFS, "no server configured");
    }
    rc = net_ensure();
    if (rc != SUB_OK) {
        return rc;
    }
    if (p->https) {
        rc = ssl_ensure();
        if (rc != SUB_OK) {
            return rc;
        }
    }

    url_encode(songid, encid, sizeof(encid));
    sprintf(extra, "&id=%s&format=raw", encid);
    build_path(p, "stream", extra, path, sizeof(path));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)p->port);

    addr = inet_addr((STRPTR)p->host);
    if (addr != INADDR_NONE) {
        sa.sin_addr.s_addr = addr;
    } else if (g_dns_addr != 0 && stricmp(g_dns_host, p->host) == 0) {
        sa.sin_addr.s_addr = g_dns_addr;
        used_cache = TRUE;
    } else {
        he = gethostbyname((UBYTE *)p->host);
        if (!he) {
            return fail(SUB_ENET, "cannot resolve host name");
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
        copy_field(g_dns_host, sizeof(g_dns_host), p->host);
        g_dns_addr = sa.sin_addr.s_addr;
    }

    c.ssl = NULL;
    c.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (c.sock < 0) {
        return fail(SUB_ENET, "socket() failed");
    }
    rc = connect_timeout(c.sock, &sa);
    if (rc != SUB_OK && used_cache) {
        /* Siehe http_get_host(): die gemerkte Adresse kann veraltet
         * sein, wenn der Anschluss eine neue WAN-Adresse bekommen hat. */
        conn_close(&c);
        g_dns_addr = 0;
        he = gethostbyname((UBYTE *)p->host);
        if (!he) {
            return fail(SUB_ENET, "cannot resolve host name");
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
        copy_field(g_dns_host, sizeof(g_dns_host), p->host);
        g_dns_addr = sa.sin_addr.s_addr;

        c.sock = socket(AF_INET, SOCK_STREAM, 0);
        if (c.sock < 0) {
            return fail(SUB_ENET, "socket() failed");
        }
        rc = connect_timeout(c.sock, &sa);
    }
    if (rc != SUB_OK) {
        conn_close(&c);
        return rc;
    }
    if (p->https) {
        rc = ssl_handshake(&c, p->host);
        if (rc != SUB_OK) {
            conn_close(&c);
            return rc;
        }
    }

    /* HTTP/1.1 mit Range. Bei offset 0 steht trotzdem ein Range-Kopf
     * drin - dann antwortet der Server mit 206 statt 200, und beide
     * Faelle sind unten gleich zu behandeln. */
    sprintf(req,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Range: bytes=%ld-\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, p->host, SUB_CLIENT "/0.2", offset);

    total = (long)strlen(req);
    sent = 0;
    while (sent < total) {
        n = conn_send(&c, req + sent, total - sent);
        if (n <= 0) {
            conn_close(&c);
            return fail(SUB_ENET, "could not send request");
        }
        sent += n;
    }

    /* Kopf lesen, byteweise genug: der Rumpf faengt gleich danach an und
     * darf NICHT verworfen werden. Deshalb wird alles, was ueber den
     * Kopf hinaus schon da ist, in st gemerkt... - einfacher ist es, nur
     * bis zum Kopfende zu lesen und den Rest beim ersten Lesen zu
     * bekommen. Dafuer lesen wir hier in kleinen Schritten. */
    while (got < (long)sizeof(head) - 1 && rounds++ < 4000) {
        n = conn_recv(&c, head + got, 1);
        if (n > 0) {
            got += n;
            head[got] = '\0';
            if (got >= 4 && strcmp(head + got - 4, "\r\n\r\n") == 0) {
                break;
            }
            continue;
        }
        if (c.ssl) {
            int err = SSL_get_error(c.ssl, (int)n);

            if (err == SSL_ERROR_ZERO_RETURN) {
                break;
            }
            if (ssl_retry(&c, (int)n) == 1) {
                continue;
            }
            break;
        }
        if (n < 0 && Errno() == SUB_EWOULDBLOCK) {
            continue;
        }
        break;
    }

    if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1) {
        conn_close(&c);
        return fail(SUB_EHTTP, "no valid HTTP response");
    }
    if (status != 200 && status != 206) {
        char msg[96];

        sprintf(msg, "stream rejected, HTTP %d", status);
        conn_close(&c);
        return fail(SUB_EHTTP, msg);
    }

    /* Gesamtlaenge: bei 206 steht sie hinter dem Schraegstrich im
     * Content-Range, bei 200 im Content-Length. */
    hdrend = strstr(head, "Content-Range:");
    if (hdrend) {
        char *slash = strchr(hdrend, '/');

        if (slash) {
            st->total = atol(slash + 1);
        }
    } else {
        hdrend = strstr(head, "Content-Length:");
        if (hdrend) {
            st->total = atol(hdrend + 15) + offset;
        }
    }

    st->sock = c.sock;
    st->ssl  = c.ssl;
    st->pos  = offset;
    copy_field(st->id, sizeof(st->id), songid);
    return SUB_OK;
}

long sub_stream_read(struct SubStream *st, void *buf, long len)
{
    struct Conn c;
    long n;

    if (!st || st->sock < 0) {
        return -1;
    }
    c.sock = st->sock;
    c.ssl  = st->ssl;

    for (;;) {
        n = conn_recv(&c, (char *)buf, len);
        if (n > 0) {
            st->pos += n;
            return n;
        }
        if (c.ssl) {
            int err = SSL_get_error(c.ssl, (int)n);

            if (err == SSL_ERROR_ZERO_RETURN) {
                return 0;               /* sauberes Ende */
            }
            if (ssl_retry(&c, (int)n) == 1) {
                continue;               /* noch nichts da */
            }
            /* Abgeschnitten. Fuer den Abspieler ist das kein Beinbruch:
             * der Aufrufer setzt mit Range an st->pos wieder auf. */
            return (st->total > 0 && st->pos < st->total) ? -1 : 0;
        }
        if (n < 0 && Errno() == SUB_EWOULDBLOCK) {
            continue;
        }
        return (n == 0) ? 0 : -1;
    }
}

void sub_stream_close(struct SubStream *st)
{
    struct Conn c;

    if (!st || st->sock < 0) {
        return;
    }
    c.sock = st->sock;
    c.ssl  = st->ssl;
    conn_close(&c);
    st->sock = -1;
    st->ssl = NULL;
}

/* ------------------------------------------------------------------ */
/* Strom von einem Radiosender                                         */
/* ------------------------------------------------------------------ */

/* Anders als sub_stream_open() zeigt die Adresse hier NICHT auf den
 * eigenen Server: keine Anmeldung, kein Range, keine Laenge. Gemessen
 * am 21.9.2026 an den acht Sendern dieser Instanz: alle ueber HTTPS,
 * und JEDER leitet zuerst per 302 auf einen anderen Rechner weiter
 * (streams.80s80s.de -> regiocast.streamabc.net). Die Weiterleitung
 * muss also verfolgt werden, jedes Mal mit eigenem Handschlag und
 * eigenem SNI-Namen. */
#define RADIO_HOPS 5

/* "https://rechner:port/pfad" zerlegen. Eine Weiterleitung darf auch
 * nur einen Pfad nennen ("/stream") - dann bleiben Rechner, Port und
 * Verschluesselung, wie sie waren. */
static int radio_split(const char *url, char *host, int hostsize,
                       int *port, BOOL *https, char *path, int pathsize)
{
    const char *s = url;
    const char *slash;
    const char *colon;
    int hl;

    if (s[0] == '/') {
        copy_field(path, pathsize, s);
        return SUB_OK;
    }
    if (strnicmp(s, "https://", 8) == 0) {
        *https = TRUE;
        *port = 443;
        s += 8;
    } else if (strnicmp(s, "http://", 7) == 0) {
        *https = FALSE;
        *port = 80;
        s += 7;
    } else {
        return fail(SUB_EHTTP, "station address is not an http(s) URL");
    }

    slash = strchr(s, '/');
    hl = slash ? (int)(slash - s) : (int)strlen(s);
    if (hl <= 0 || hl >= hostsize) {
        return fail(SUB_EHTTP, "invalid host name in station address");
    }
    memcpy(host, s, hl);
    host[hl] = '\0';

    colon = strchr(host, ':');
    if (colon) {
        *port = atoi(colon + 1);
        host[colon - host] = '\0';
    }
    copy_field(path, pathsize, slash ? slash : "/");
    return SUB_OK;
}

/* Einen Kopfeintrag suchen, Gross-/Kleinschreibung egal: HTTP schreibt
 * sie nicht vor, und die Sender halten es jeder anders ("Location",
 * "location", "icy-metaint"). Der Wert ohne fuehrende Leerzeichen und
 * ohne Zeilenende. */
static BOOL head_field(const char *head, const char *name,
                       char *out, int outsize)
{
    const char *line = head;
    int nl = (int)strlen(name);

    while (line && *line) {
        if (strnicmp(line, name, nl) == 0 && line[nl] == ':') {
            const char *v = line + nl + 1;
            int k = 0;

            while (*v == ' ' || *v == '\t') {
                v++;
            }
            while (v[k] && v[k] != '\r' && v[k] != '\n' &&
                   k < outsize - 1) {
                out[k] = v[k];
                k++;
            }
            out[k] = '\0';
            return TRUE;
        }
        line = strchr(line, '\n');
        if (line) {
            line++;
        }
    }
    return FALSE;
}

/* Den Kopf bis zur Leerzeile lesen, byteweise: was danach kommt, ist
 * schon Musik und darf nicht verloren gehen. */
static long radio_read_head(struct Conn *c, char *head, long size)
{
    long got = 0, n;
    int rounds = 0;

    head[0] = '\0';
    while (got < size - 1 && rounds++ < 4000) {
        n = conn_recv(c, head + got, 1);
        if (n > 0) {
            got += n;
            head[got] = '\0';
            if (got >= 4 && strcmp(head + got - 4, "\r\n\r\n") == 0) {
                break;
            }
            /* Manche Icecast-Abkoemmlinge enden nur mit \n\n. */
            if (got >= 2 && strcmp(head + got - 2, "\n\n") == 0) {
                break;
            }
            continue;
        }
        if (c->ssl) {
            if (SSL_get_error(c->ssl, (int)n) == SSL_ERROR_ZERO_RETURN) {
                break;
            }
            if (ssl_retry(c, (int)n) == 1) {
                continue;
            }
            break;
        }
        if (n < 0 && Errno() == SUB_EWOULDBLOCK) {
            continue;
        }
        break;
    }
    return got;
}

/* Verbinden, ohne g_dns_host anzufassen: der Merkplatz gehoert dem
 * eigenen Server. Wuerde ein Sender ihn ueberschreiben, loeste danach
 * jeder Coverabruf den Navidrome-Namen neu auf - und gethostbyname()
 * blockiert unbegrenzt (AGENTS.md 7). Beim Sender laesst sich das
 * nicht vermeiden: sein Weiterleitungsziel wechselt von Mal zu Mal. */
static int radio_connect(const char *host, int port, BOOL https,
                         struct Conn *c)
{
    struct sockaddr_in sa;
    struct hostent *he;
    in_addr_t addr;
    int rc;

    c->sock = -1;
    c->ssl = NULL;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    addr = inet_addr((STRPTR)host);
    if (addr != INADDR_NONE) {
        sa.sin_addr.s_addr = addr;
    } else {
        he = gethostbyname((UBYTE *)host);
        if (!he) {
            return fail(SUB_ENET, "station: cannot resolve host name");
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
    }

    c->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (c->sock < 0) {
        return fail(SUB_ENET, "socket() failed");
    }
    rc = connect_timeout(c->sock, &sa);
    if (rc == SUB_OK && https) {
        rc = ssl_ensure();
        if (rc == SUB_OK) {
            rc = ssl_handshake(c, host);
        }
    }
    if (rc != SUB_OK) {
        conn_close(c);
    }
    return rc;
}

int sub_radio_open(const char *url, BOOL icy, struct SubStream *st)
{
    struct Conn c;
    char host[128];
    char path[768];
    char req[1100];
    char head[1500];
    char loc[768];
    char val[64];
    int port = 80;
    BOOL https = FALSE;
    int hop, rc, status;
    long n, sent, total;

    memset(st, 0, sizeof(*st));
    st->sock = -1;

    rc = net_ensure();
    if (rc != SUB_OK) {
        return rc;
    }
    rc = radio_split(url, host, sizeof(host), &port, &https,
                     path, sizeof(path));
    if (rc != SUB_OK) {
        return rc;
    }

    for (hop = 0; hop <= RADIO_HOPS; hop++) {
        rc = radio_connect(host, port, https, &c);
        if (rc != SUB_OK) {
            return rc;
        }

        /* HTTP/1.0 aus demselben Grund wie in http_get_host(): dann
         * kommt nie chunked, und der Rumpf ist schlicht der Strom.
         * Der Port gehoert nur in den Host-Kopf, wenn er vom ueblichen
         * abweicht - manche Server vergleichen den Kopf woertlich. */
        if (port == (https ? 443 : 80)) {
            sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\n", path, host);
        } else {
            sprintf(req, "GET %s HTTP/1.0\r\nHost: %s:%d\r\n",
                    path, host, port);
        }
        sprintf(req + strlen(req),
                "User-Agent: %s/0.2\r\n"
                "Accept: */*\r\n"
                "%s"
                "Connection: close\r\n"
                "\r\n",
                SUB_CLIENT, icy ? "Icy-MetaData: 1\r\n" : "");

        total = (long)strlen(req);
        sent = 0;
        while (sent < total) {
            n = conn_send(&c, req + sent, total - sent);
            if (n <= 0) {
                conn_close(&c);
                return fail(SUB_ENET, "station: could not send request");
            }
            sent += n;
        }

        radio_read_head(&c, head, sizeof(head));

        /* Shoutcast antwortet mit "ICY 200 OK" statt "HTTP/1.x 200". */
        status = 0;
        if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1 &&
            sscanf(head, "ICY %d", &status) != 1) {
            conn_close(&c);
            return fail(SUB_EHTTP, "station: no valid HTTP response");
        }

        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            conn_close(&c);
            if (!head_field(head, "location", loc, sizeof(loc))) {
                return fail(SUB_EHTTP, "station: redirect without target");
            }
            rc = radio_split(loc, host, sizeof(host), &port, &https,
                             path, sizeof(path));
            if (rc != SUB_OK) {
                return rc;
            }
            st->hops++;
            continue;
        }
        if (status != 200) {
            char msg[96];

            sprintf(msg, "station refused, HTTP %d", status);
            conn_close(&c);
            return fail(SUB_EHTTP, msg);
        }

        /* Am Inhaltstyp haengt, welcher Dekoder zustaendig ist:
         * audio/mpeg geht an mpega.library, audio/aac(p) an Helix. */
        if (head_field(head, "content-type", val, sizeof(val))) {
            copy_field(st->ctype, sizeof(st->ctype), val);
        }
        if (icy && head_field(head, "icy-metaint", val, sizeof(val))) {
            st->metaint = atol(val);
        }
        if (head_field(head, "icy-br", val, sizeof(val))) {
            st->bitrate = atol(val);
        }

        st->sock = c.sock;
        st->ssl  = c.ssl;
        st->total = 0;              /* ein Sender hat kein Ende */
        return SUB_OK;
    }
    return fail(SUB_EHTTP, "station: too many redirects");
}

/* Ob der Inhaltstyp nach MP3 aussieht. Ohne Angabe wird es versucht -
 * mpega.library sagt dann selbst, dass es kein MPEG ist. */
BOOL sub_radio_is_mp3(const char *ctype)
{
    if (!ctype || !ctype[0]) {
        return TRUE;
    }
    return strnicmp(ctype, "audio/mpeg", 10) == 0 ||
           strnicmp(ctype, "audio/mp3", 9) == 0 ||
           strnicmp(ctype, "audio/x-mpeg", 12) == 0;
}

/* Gemessen am 22.9.2026: beide AAC-Sender dieser Instanz melden
 * "audio/aac" und senden ADTS (HE-AAC). "aacp" ist der alte Name fuer
 * AAC+ und kommt bei Shoutcast-Sendern vor. */
BOOL sub_radio_is_aac(const char *ctype)
{
    if (!ctype) {
        return FALSE;
    }
    return strnicmp(ctype, "audio/aac", 9) == 0 ||
           strnicmp(ctype, "audio/x-aac", 11) == 0;
}

/* Prueft, ob der Server mitten im Titel einsteigen laesst.
 *
 * Davon haengen zwei Dinge des eigenen Abspielers ab: Spulen, und das
 * Wiederaufsetzen nach einem WLAN-Aussetzer. Antwortet der Server mit
 * 206 und einem Content-Range, geht beides; kommt 200, faengt er stur
 * von vorn an, und wir muessten den Anfang wegwerfen.
 *
 * Gelesen wird nur der Kopf der Antwort, nicht der ganze Titel. */
int sub_range_probe(struct Prefs *p, const char *songid, long offset,
                    const char *fmt, int maxbitrate, char *out, int outsize)
{
    struct Conn c;
    struct sockaddr_in sa;
    struct hostent *he;
    in_addr_t addr;
    char path[1024];
    char extra[160];
    char encid[96];
    char req[1400];
    char buf[1500];

    long n, sent, total;
    int rc;

    out[0] = '\0';
    buf[0] = '\0';

    if (p->host[0] == '\0') {
        return fail(SUB_ENOPREFS, "no server configured");
    }
    rc = net_ensure();
    if (rc != SUB_OK) {
        return rc;
    }
    if (p->https) {
        rc = ssl_ensure();
        if (rc != SUB_OK) {
            return rc;
        }
    }

    url_encode(songid, encid, sizeof(encid));
    if (fmt && fmt[0]) {
        sprintf(extra, "&id=%s&format=%s&maxBitRate=%d", encid, fmt,
                maxbitrate > 0 ? maxbitrate : 320);
    } else {
        sprintf(extra, "&id=%s", encid);
    }
    build_path(p, "stream", extra, path, sizeof(path));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)p->port);

    addr = inet_addr((UBYTE *)p->host);
    if (addr != (in_addr_t)-1) {
        sa.sin_addr.s_addr = addr;
    } else {
        he = gethostbyname((UBYTE *)p->host);
        if (!he) {
            return fail(SUB_ENET, "cannot resolve host name");
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
    }

    c.ssl = NULL;
    c.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (c.sock < 0) {
        return fail(SUB_ENET, "socket() failed");
    }
    rc = connect_timeout(c.sock, &sa);
    if (rc != SUB_OK) {
        conn_close(&c);
        return rc;
    }
    if (p->https) {
        rc = ssl_handshake(&c, p->host);
        if (rc != SUB_OK) {
            conn_close(&c);
            return rc;
        }
    }

    sprintf(req,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Range: bytes=%ld-\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, p->host, SUB_CLIENT "/0.1", offset);

    total = (long)strlen(req);
    sent = 0;
    while (sent < total) {
        n = conn_send(&c, req + sent, total - sent);
        if (n <= 0) {
            conn_close(&c);
            return fail(SUB_ENET, "could not send request");
        }
        sent += n;
    }

    /* Wie in recv_all(): ein einzelnes SSL_read liefert oft erst einmal
     * WANT_READ, weil der Datensatz noch unterwegs ist. Einmal lesen und
     * aufgeben waere der haeufigste Anfaengerfehler mit AmiSSL. */
    {
        long got = 0;
        int rounds = 0;

        while (got < (long)sizeof(buf) - 1 && rounds++ < 200) {
            n = conn_recv(&c, buf + got, (long)sizeof(buf) - 1 - got);
            if (n > 0) {
                got += n;
                if (strstr(buf, "\r\n\r\n")) {
                    break;          /* Kopf vollstaendig, Rumpf egal */
                }
                buf[got] = '\0';
                continue;
            }
            if (c.ssl) {
                int err = SSL_get_error(c.ssl, (int)n);

                if (err == SSL_ERROR_ZERO_RETURN) {
                    break;
                }
                if (ssl_retry(&c, (int)n) == 1) {
                    continue;
                }
                break;
            }
            if (n < 0 && Errno() == SUB_EWOULDBLOCK) {
                continue;
            }
            break;
        }
        conn_close(&c);
        if (got <= 0) {
            return fail(SUB_ENET, "no response");
        }
        buf[got] = '\0';
    }

    /* Nur den Kopf zurueckgeben - der Rumpf sind die ersten Bytes des
     * Titels und interessiert hier nicht. */
    {
        char *e = strstr(buf, "\r\n\r\n");
        int len;

        if (e) {
            *e = '\0';
        }
        len = (int)strlen(buf);
        if (len > outsize - 1) {
            len = outsize - 1;
        }
        memcpy(out, buf, (size_t)len);
        out[len] = '\0';
    }
    return SUB_OK;
}

int sub_get_cover(struct Prefs *p, const char *coverartid, int maxsize,
                  const char *path)
{
    char *body = NULL;
    long len = 0;
    char reqpath[1024];
    char extra[128];
    char encid[96];
    BPTR fh;
    int rc;

    if (!coverartid || coverartid[0] == '\0') {
        return fail(SUB_EAPI, "no cover available");
    }

    url_encode(coverartid, encid, sizeof(encid));
    if (maxsize > 0) {
        sprintf(extra, "&id=%s&size=%d", encid, maxsize);
    } else {
        sprintf(extra, "&id=%s", encid);
    }

    build_path(p, "getCoverArt", extra, reqpath, sizeof(reqpath));

    /* Hier absichtlich kein api_call(): die Antwort ist im Erfolgsfall ein
     * JPEG oder PNG, kein XML. Nur wenn etwas schiefgeht, kommt eine
     * subsonic-response - danach wird unten geschaut. */
    rc = http_get(p, reqpath, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }

    if (len > 20 && xml_find(body, "subsonic-response") != NULL) {
        free(body);
        return fail(SUB_EAPI, "server returned no image");
    }

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        free(body);
        return fail(SUB_EMEM, "cannot write cover");
    }
    if (Write(fh, body, len) != len) {
        Close(fh);
        free(body);
        return fail(SUB_EMEM, "cover written incompletely");
    }
    Close(fh);
    free(body);
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Cover-Cache                                                         */
/* ------------------------------------------------------------------ */

/* Der Dateiname IST die Cover-ID. Das haelt den spaeteren Abgleich
 * einfach: was der Server nennt und hier fehlt, wird geholt; was hier
 * liegt und der Server nicht mehr nennt, kann weg. Ein Index waere eine
 * zweite Wahrheit, die veralten kann.
 *
 * Gefiltert wird nur, was in einem AmigaDOS-Namen nicht vorkommen darf.
 * Die Platten hier laufen auf PFS3 (107 Zeichen), Navidromes IDs sind
 * rund 22 Zeichen lang - Kuerzen ist also nicht noetig, und ein Hash
 * waere nur ein Name, den man nicht mehr zurueckuebersetzen kann. */
void cache_path(struct Prefs *p, const char *coverartid,
                char *out, int outsize)
{
    const char *dir = (p && p->cache[0]) ? p->cache : CACHE_DIR_DEFAULT;
    int n, i;

    if (!out || outsize <= 0) {
        return;
    }
    out[0] = '\0';
    if (!coverartid || coverartid[0] == '\0') {
        return;
    }

    n = (int)strlen(dir);
    if (n + (int)strlen(coverartid) + 6 > outsize) {
        return;                 /* passt nicht - der Aufrufer sieht "" */
    }

    strcpy(out, dir);
    /* Ein Verzeichnisname darf schon auf ':' enden (z.B. "T:"), dann
     * gehoert KEIN weiterer Schraegstrich dazwischen. */
    if (n > 0 && out[n - 1] != ':' && out[n - 1] != '/') {
        out[n++] = '/';
        out[n] = '\0';
    }

    for (i = 0; coverartid[i]; i++) {
        char c = coverartid[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[n++] = c;
        } else {
            out[n++] = '_';
        }
    }
    out[n] = '\0';
    strcat(out, ".jpg");
}

/* Legt ein Verzeichnis samt ALLER fehlenden Stufen davor an.
 *
 * CreateDir() legt nur die letzte Stufe an. Gemessen am 21.9.2026 in
 * WinUAE: cache=RAM:AmiSubsonic/Cache, nach einem Neustart gab es
 * RAM:AmiSubsonic nicht mehr - jedes Cover scheiterte, und die
 * Statuszeile meldete "network not responding", obwohl Radio und Listen
 * liefen. Also Stufe fuer Stufe: an jedem '/' hinter dem Doppelpunkt
 * einmal anhalten und anlegen, was fehlt. */
static BOOL mkdir_all(const char *dir)
{
    char buf[256];
    char *s;
    BPTR lock;
    int n = (int)strlen(dir);

    if (n <= 0 || n >= (int)sizeof(buf)) {
        return FALSE;
    }
    strcpy(buf, dir);
    if (buf[n - 1] == '/') {
        buf[n - 1] = '\0';         /* "Work:x/" wie "Work:x" */
    }

    s = strchr(buf, ':');
    s = s ? s + 1 : buf;
    for (;;) {
        char *slash = strchr(s, '/');
        char keep = 0;

        if (slash) {
            keep = *slash;
            *slash = '\0';
        }
        if (buf[0] && buf[strlen(buf) - 1] != ':') {
            lock = Lock((STRPTR)buf, ACCESS_READ);
            if (!lock) {
                lock = CreateDir((STRPTR)buf);
            }
            if (!lock) {
                return FALSE;
            }
            UnLock(lock);
        }
        if (!slash) {
            break;
        }
        *slash = keep;
        s = slash + 1;
    }
    return TRUE;
}

int cache_ensure(struct Prefs *p)
{
    const char *dir = (p && p->cache[0]) ? p->cache : CACHE_DIR_DEFAULT;
    BPTR lock;
    char msg[160];

    lock = Lock((STRPTR)dir, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return SUB_OK;
    }

    if (!mkdir_all(dir)) {
        /* Den Pfad nennen - sonst sucht man den Fehler im Netz. */
        sprintf(msg, "cannot create cache directory %.100s", dir);
        return fail(SUB_EMEM, msg);
    }
    return SUB_OK;
}

BOOL cache_have(struct Prefs *p, const char *coverartid)
{
    char path[256];
    BPTR lock;

    cache_path(p, coverartid, path, sizeof(path));
    if (path[0] == '\0') {
        return FALSE;
    }

    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) {
        return FALSE;
    }
    UnLock(lock);
    return TRUE;
}

int sub_get_cover_cached(struct Prefs *p, const char *coverartid,
                         int maxsize, char *out, int outsize)
{
    int rc;

    if (!out || outsize <= 0) {
        return fail(SUB_EMEM, "cache path too long");
    }
    cache_path(p, coverartid, out, outsize);
    if (out[0] == '\0') {
        return fail(SUB_EAPI, "no cover available");
    }

    if (cache_have(p, coverartid)) {
        return SUB_OK;
    }

    rc = cache_ensure(p);
    if (rc != SUB_OK) {
        return rc;
    }

    rc = sub_get_cover(p, coverartid, maxsize, out);
    if (rc != SUB_OK) {
        /* Eine halb geschriebene Datei waere schlimmer als gar keine:
         * beim naechsten Start laege sie da und wuerde nie erneuert. */
        DeleteFile((STRPTR)out);
        out[0] = '\0';
        return rc;
    }
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Liedtexte von lrclib.net                                            */
/* ------------------------------------------------------------------ */

/* Navidrome liefert nur, was in den Dateien steckt - eingebettete
 * USLT-Tags oder eine .lrc daneben. Bei einer gewachsenen Sammlung ist
 * das fast nie vorhanden. Feishin holt die Texte deshalb aus dem Netz
 * (Genius, lrclib.net, NetEase, SimpMusic).
 *
 * Von den vieren taugt fuer einen Amiga nur lrclib.net: ein schlichtes
 * GET ohne Schluessel und ohne Anmeldung. Genius muesste man aus HTML
 * herausschneiden, NetEase verlangt verschluesselte Parameter - beides
 * auf 68k weder sinnvoll noch wartbar.
 *
 * Die Antwort ist JSON. Ein vollstaendiger JSON-Leser waere dafuer
 * genauso ueberzogen wie ein XML-Stack oben: gebraucht werden genau zwei
 * Felder, beide Zeichenketten auf oberster Ebene. */

#define LRC_HOST "lrclib.net"

/* Der Wert einer Zeichenketten-Eigenschaft der obersten Ebene.
 *
 * Loest dabei die JSON-Maskierungen auf - \n vor allem, denn genau daran
 * haengen die Zeilenumbrueche des Liedtextes. \u-Folgen werden ueber
 * UTF-8 gefuehrt und ganz am Ende zusammen mit dem Rest nach Latin-1
 * gewandelt, damit es nur eine Stelle dafuer gibt. */
static BOOL json_string(const char *body, const char *key,
                        char *out, int outsize)
{
    char pattern[64];
    const char *p;
    int o = 0;

    out[0] = '\0';

    sprintf(pattern, "\"%s\":", key);
    p = strstr(body, pattern);
    if (!p) {
        return FALSE;
    }
    p += strlen(pattern);

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return FALSE;           /* null oder eine Zahl - kein Text */
    }
    p++;

    while (*p && *p != '"' && o < outsize - 4) {
        if (*p != '\\') {
            out[o++] = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'n':  out[o++] = '\n'; p++; break;
        case 'r':  p++; break;      /* CR wegwerfen, LF genuegt */
        case 't':  out[o++] = ' ';  p++; break;
        case 'b': case 'f': p++; break;
        case 'u': {
            unsigned long cp = 0;
            int i;

            p++;
            for (i = 0; i < 4 && isxdigit((unsigned char)*p); i++, p++) {
                char ch = *p;
                cp = cp * 16 + (unsigned long)
                     (ch <= '9' ? ch - '0' : (tolower(ch) - 'a' + 10));
            }
            o = append_utf8(out, o, outsize, cp);
            break;
        }
        default:
            /* \" \\ \/ und alles andere steht fuer sich selbst. */
            if (*p) {
                out[o++] = *p++;
            }
            break;
        }
    }
    out[o] = '\0';
    utf8_to_latin1(out);
    return TRUE;
}

int sub_get_lyrics_net(const char *artist, const char *title,
                       const char *album, int duration,
                       char *plain, int plainsize,
                       char *synced, int syncedsize)
{
    char path[900];
    char eartist[200], etitle[300], ealbum[300];
    char *body = NULL;
    long len = 0;
    int rc, attempt;

    if (plain)  { plain[0] = '\0'; }
    if (synced) { synced[0] = '\0'; }

    if (!artist || !title || !artist[0] || !title[0]) {
        return fail(SUB_EAPI, "artist or title missing");
    }

    url_encode(artist, eartist, sizeof(eartist));
    url_encode(title, etitle, sizeof(etitle));
    url_encode(album ? album : "", ealbum, sizeof(ealbum));

    /* Album und Dauer sind laut lrclib optional, verbessern den Treffer
     * aber deutlich - ohne sie kommt bei haeufigen Titelnamen leicht die
     * Fassung einer anderen Aufnahme zurueck. */
    if (duration > 0) {
        sprintf(path, "/api/get?artist_name=%s&track_name=%s"
                      "&album_name=%s&duration=%d",
                eartist, etitle, ealbum, duration);
    } else {
        sprintf(path, "/api/get?artist_name=%s&track_name=%s&album_name=%s",
                eartist, etitle, ealbum);
    }

    /* lrclib bittet in seiner Doku ausdruecklich um einen aussagekraeftigen
     * User-Agent - der Dienst ist kostenlos und ohne Anmeldung, das ist
     * die Gegenleistung. */
    /* lrclib ist kostenlos und lehnt unter Last mit HTTP 503 ab ("Server
     * is busy. Please retry ... Server Overload", gesehen am 19.9.2026).
     * Also EIN zweiter Versuch nach 2 s - mehr nicht, der Dienst bittet
     * um Schonung, und solange wartet der Netzprozess, nicht die
     * Oberflaeche. */
    for (attempt = 0; ; attempt++) {
        rc = http_get_host(LRC_HOST, 443, TRUE, path,
                           SUB_CLIENT "/0.1 (AmigaOS Subsonic client)",
                           &body, &len);
        if (rc == SUB_OK || attempt >= 1
            || !strstr(sub_last_error(), "HTTP 503")) {
            break;
        }
        Delay(100);                         /* 2 s, 50 Ticks je Sekunde */
    }
    if (rc != SUB_OK && strstr(sub_last_error(), "HTTP 503")) {
        return fail(SUB_EBUSY, "lrclib.net is busy");
    }
    if (rc != SUB_OK) {
        /* 404 heisst schlicht "kein Text bekannt" und ist kein Fehler,
         * ueber den sich jemand aergern muesste. */
        if (strstr(sub_last_error(), "404")) {
            return fail(SUB_EAPI, "lrclib.net does not know this track");
        }
        /* Alles andere mit Absender: "Server antwortet mit HTTP 503"
         * allein las sich wie ein Fehler des eigenen Navidrome, kam aber
         * (vermutlich) von lrclib, das unter Last kurz ablehnt. */
        {
            char msg[256];

            strcpy(msg, "Lyrics (lrclib.net): ");
            strncat(msg, sub_last_error(), sizeof(msg) - strlen(msg) - 1);
            return fail(rc, msg);
        }
    }

    if (synced && syncedsize > 0) {
        json_string(body, "syncedLyrics", synced, syncedsize);
    }
    if (plain && plainsize > 0) {
        json_string(body, "plainLyrics", plain, plainsize);
    }

    free(body);

    if ((!plain || !plain[0]) && (!synced || !synced[0])) {
        return fail(SUB_EAPI, "lrclib.net has no lyrics for this track");
    }
    return SUB_OK;
}

int sub_get_song(struct Prefs *p, const char *songid, struct Song *out)
{
    char *body = NULL;
    long len = 0;
    char extra[128];
    char encid[96];
    const char *e;
    int rc;

    memset(out, 0, sizeof(*out));

    url_encode(songid, encid, sizeof(encid));
    sprintf(extra, "&id=%s", encid);

    rc = api_call(p, "getSong", extra, &body, &len);
    if (rc != SUB_OK) {
        return rc;
    }

    e = xml_find(body, "song");
    if (!e) {
        free(body);
        return fail(SUB_EAPI, "track not found");
    }
    fill_song(out, e);
    free(body);
    return SUB_OK;
}

/* Vermutung aus der Adresse allein. Seit AAC spielt (22.9.2026) nur
 * noch ein Hinweis in der Senderliste des CLI; ob es geht, entscheidet
 * der Content-Type beim Abspielen. Gemessen am 21.9.2026: beide
 * AAC-Sender dieser Instanz enden auf ".../stream/aacp". */
BOOL sub_radio_url_aac(const char *url)
{
    const char *s;

    for (s = url; *s; s++) {
        if (strnicmp(s, "aac", 3) == 0 || strnicmp(s, ".m4a", 4) == 0 ||
            strnicmp(s, ".mp4", 4) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}
