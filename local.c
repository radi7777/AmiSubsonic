/* AmiSubsonic - eigene MP3-Dateien finden, siehe local.h. */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "local.h"

static char g_why[160] = "";

const char *local_last_error(void)
{
    return g_why[0] ? g_why : "no error";
}

static ULONG now_cs(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 6000UL + (ULONG)ds.ds_Tick * 2UL;
}

/* Endet der Name auf .mp3? Gross- und Kleinschreibung ist auf dem Amiga
 * egal, also wird sie hier auch nicht beachtet. */
static BOOL is_mp3(const char *name)
{
    int n = (int)strlen(name);

    if (n < 5) {
        return FALSE;
    }
    return stricmp(name + n - 4, ".mp3") == 0;
}

/* Dateiname ohne Endung als vorlaeufiger Titel. Die richtigen Angaben
 * kommen aus dem ID3-Etikett, das ist die naechste Stufe - bis dahin ist
 * der Dateiname besser als eine leere Zeile. */
static void title_from_name(const char *name, char *out, int outsize)
{
    int n = (int)strlen(name);

    if (n > 4 && stricmp(name + n - 4, ".mp3") == 0) {
        n -= 4;
    }
    if (n > outsize - 1) {
        n = outsize - 1;
    }
    memcpy(out, name, (size_t)n);
    out[n] = '\0';
}

/* Ein Verzeichnis abarbeiten und in seine Unterverzeichnisse absteigen.
 *
 * Rekursion mit einem FileInfoBlock JE EBENE: ExNext() merkt sich den
 * Stand im FIB, ein gemeinsamer waere also nach dem ersten Abstieg
 * verloren. Der FIB muss ueber AllocDosObject kommen - er verlangt
 * Langwort-Ausrichtung, die ein Stapelplatz nicht garantiert.
 *
 * Tiefe: jede Ebene kostet rund 260 Bytes Stapel plus den FIB auf der
 * Halde. Der Netzprozess hat 64 KB, das reicht fuer weit mehr Ebenen,
 * als eine Musiksammlung je hat. */
static int scan_dir(const char *dir, struct SubList *out,
                    struct ScanStat *st, int depth)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    char child[256];
    int rc = SUB_OK;

    if (depth > 12) {
        return SUB_OK;              /* so tief liegt keine Sammlung */
    }

    lock = Lock((STRPTR)dir, ACCESS_READ);
    if (!lock) {
        sprintf(g_why, "cannot access %.100s", dir);
        return SUB_EAPI;
    }

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        strcpy(g_why, "out of memory");
        return SUB_EMEM;
    }

    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        sprintf(g_why, "Examine failed on %.100s", dir);
        return SUB_EAPI;
    }
    st->dirs++;

    while (ExNext(lock, fib)) {
        /* Der Pfad wird hier von Hand gebaut statt mit AddPart(), damit
         * die Laenge sicher begrenzt bleibt. Ein zu langer Pfad wird
         * uebersprungen statt abgeschnitten - eine abgeschnittene
         * Datei liesse sich spaeter nicht oeffnen. */
        int dl = (int)strlen(dir);
        int nl = (int)strlen(fib->fib_FileName);

        if (dl + nl + 2 > (int)sizeof(child)) {
            st->skipped++;
            continue;
        }
        strcpy(child, dir);
        if (dl > 0 && dir[dl - 1] != ':' && dir[dl - 1] != '/') {
            strcat(child, "/");
        }
        strcat(child, fib->fib_FileName);

        if (fib->fib_DirEntryType > 0) {
            rc = scan_dir(child, out, st, depth + 1);
            if (rc != SUB_OK) {
                break;
            }
            if (st->full) {
                break;
            }
            continue;
        }

        if (!is_mp3(fib->fib_FileName)) {
            st->skipped++;
            continue;
        }

        if (st->files >= LOCAL_MAX_FILES) {
            st->full = TRUE;
            break;
        }

        {
            struct Song *s = (struct Song *)list_add(out);

            if (!s) {
                strcpy(g_why, "out of memory for the list");
                rc = SUB_EMEM;
                break;
            }
            memset(s, 0, sizeof(*s));
            s->lalbum = -1;
            strncpy(s->path, child, sizeof(s->path) - 1);
            title_from_name(fib->fib_FileName, s->title, sizeof(s->title));
            strcpy(s->suffix, "mp3");
            st->files++;
        }
    }

    /* ERROR_NO_MORE_ENTRIES ist das normale Ende, alles andere ein
     * echter Fehler - ein unlesbares Unterverzeichnis soll den ganzen
     * Durchgang aber nicht abbrechen. */
    if (rc == SUB_OK && IoErr() != ERROR_NO_MORE_ENTRIES) {
        sprintf(g_why, "read error in %.100s (%ld)", dir, IoErr());
    }

    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return rc;
}

BOOL local_tags(struct Song *s);    /* steht weiter unten */

int local_scan(const char *root, struct SubList *out, struct ScanStat *st)
{
    ULONG t0;
    int rc;

    memset(st, 0, sizeof(*st));
    g_why[0] = '\0';

    if (!root || !root[0]) {
        strcpy(g_why, "no folder set");
        return SUB_ENOPREFS;
    }

    t0 = now_cs();
    rc = scan_dir(root, out, st, 0);
    st->cs = (LONG)(now_cs() - t0);

    /* Die Etiketten erst NACH dem Suchen lesen, in einem zweiten
     * Durchgang. So steht die Liste zuerst vollstaendig, und die Zeiten
     * fuer Suchen und Lesen lassen sich getrennt messen - beim ersten
     * Anlauf war unklar, welcher Teil wie lange braucht. */
    t0 = now_cs();
    if (rc == SUB_OK && out->count > 0 && out->items) {
        int i;

        for (i = 0; i < out->count; i++) {
            local_tags((struct Song *)list_get(out, i));
        }
    }
    st->tag_cs = (LONG)(now_cs() - t0);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ID3-Etiketten und Spieldauer                                        */
/* ------------------------------------------------------------------ */

/* Ein ID3v2-Etikett steht VOR der Musik und ist meist 30 bis 50 KB gross,
 * weil das Cover darin steckt. Gelesen wird deshalb nur der Kopf und
 * danach gezielt die Textfelder - das ganze Etikett einzulesen waere bei
 * 200 Dateien ein Vielfaches an Arbeit.
 *
 * Aufbau (ID3v2.3/2.4):
 *   "ID3" | Version 2 | Flags 1 | Groesse 4 (je sieben nutzbare Bits)
 *   dann Rahmen: Kennung 4 | Groesse 4 | Flags 2 | Inhalt
 * In 2.3 ist die Rahmengroesse eine normale 32-Bit-Zahl, in 2.4
 * ebenfalls "synchsafe". Beides kommt vor, beides wird hier behandelt. */

#define ID3_READ_MAX  (64L * 1024L)     /* mehr Kopf braucht niemand */

static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16)
         | ((ULONG)p[2] << 8)  |  (ULONG)p[3];
}

static ULONG synchsafe(const UBYTE *p)
{
    return ((ULONG)(p[0] & 0x7f) << 21) | ((ULONG)(p[1] & 0x7f) << 14)
         | ((ULONG)(p[2] & 0x7f) << 7)  |  (ULONG)(p[3] & 0x7f);
}

/* Text aus einem ID3v2-Rahmen nach Latin-1.
 *
 * Das erste Byte sagt, wie kodiert wurde: 0 = Latin-1, 1 = UTF-16 mit
 * Bytefolgemarke, 2 = UTF-16BE, 3 = UTF-8. Ohne diese Umsetzung stuenden
 * in der Liste Fragezeichen und Nullbytes - UTF-16 hat vor jedem
 * lateinischen Buchstaben eine Null. */
static void id3_text(const UBYTE *src, LONG len, char *out, int outsize)
{
    int enc, i, o = 0;
    BOOL be = TRUE;

    out[0] = '\0';
    if (len <= 1) {
        return;
    }
    enc = src[0];
    src++;
    len--;

    if (enc == 1 || enc == 2) {
        if (enc == 1 && len >= 2) {
            if (src[0] == 0xff && src[1] == 0xfe) { be = FALSE; }
            src += 2;
            len -= 2;
        }
        for (i = 0; i + 1 < len && o < outsize - 1; i += 2) {
            ULONG c = be ? (((ULONG)src[i] << 8) | src[i + 1])
                         : (((ULONG)src[i + 1] << 8) | src[i]);

            if (c == 0) {
                break;
            }
            /* Alles oberhalb von Latin-1 wird zu einem Fragezeichen -
             * besser als ein zerhacktes Zeichen. */
            out[o++] = (c < 256) ? (char)c : '?';
        }
    } else if (enc == 3) {
        /* UTF-8: die Folgen fuer Latin-1 sind hoechstens zwei Bytes
         * lang, alles Laengere wird zum Fragezeichen. */
        for (i = 0; i < len && o < outsize - 1; i++) {
            UBYTE c = src[i];

            if (c == 0) {
                break;
            }
            if (c < 0x80) {
                out[o++] = (char)c;
            } else if ((c & 0xe0) == 0xc0 && i + 1 < len) {
                ULONG v = ((ULONG)(c & 0x1f) << 6) | (src[i + 1] & 0x3f);

                out[o++] = (v < 256) ? (char)v : '?';
                i++;
            } else {
                out[o++] = '?';
                while (i + 1 < len && (src[i + 1] & 0xc0) == 0x80) {
                    i++;
                }
            }
        }
    } else {
        for (i = 0; i < len && o < outsize - 1; i++) {
            if (src[i] == 0) {
                break;
            }
            out[o++] = (char)src[i];
        }
    }

    out[o] = '\0';

    /* Nachlaufende Leerzeichen kommen in alten Etiketten haeufig vor. */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) {
        out[--o] = '\0';
    }
}

/* Bitraten- und Frequenztabellen fuer MPEG 1/2 Layer III. */
static const LONG g_rate_v1l3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const LONG g_rate_v2l3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};
static const LONG g_freq_v1[4] = { 44100, 48000, 32000, 0 };
static const LONG g_freq_v2[4] = { 22050, 24000, 16000, 0 };

/* Spieldauer aus dem ersten Rahmenkopf.
 *
 * Bei fester Bitrate ist das schlicht Bytes mal acht durch Bitrate. Bei
 * wechselnder steht im ersten Rahmen ein "Xing"- oder "Info"-Feld mit der
 * Zahl der Rahmen; dann ist die Dauer Rahmen mal 1152 durch Abtastrate.
 * Ohne diese Unterscheidung waeren VBR-Dateien um Faktoren daneben. */
static void mpeg_duration(const UBYTE *buf, LONG len, LONG filebytes,
                          LONG offset, struct Song *s)
{
    LONG i;

    for (i = 0; i + 4 < len && i < 8192; i++) {
        LONG ver, layer, brix, frix, rate, freq, samples;
        const UBYTE *h = buf + i;

        if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0) {
            continue;
        }
        ver   = (h[1] >> 3) & 3;        /* 3 = MPEG1, 2 = MPEG2, 0 = 2.5 */
        layer = (h[1] >> 1) & 3;        /* 1 = Layer III */
        brix  = (h[2] >> 4) & 15;
        frix  = (h[2] >> 2) & 3;

        if (ver == 1 || layer != 1 || brix == 0 || brix == 15 || frix == 3) {
            continue;                   /* kein brauchbarer Kopf */
        }

        rate = (ver == 3) ? g_rate_v1l3[brix] : g_rate_v2l3[brix];
        freq = (ver == 3) ? g_freq_v1[frix]   : g_freq_v2[frix];
        if (rate <= 0 || freq <= 0) {
            continue;
        }
        samples = (ver == 3) ? 1152 : 576;

        s->bitrate = (int)rate;

        /* Xing/Info steht im ersten Rahmen, hinter dem Kopf und einem
         * Zwischenraum, dessen Laenge von Kanaelen und Version abhaengt.
         * Statt das auszurechnen, wird schlicht in den naechsten 200
         * Bytes danach gesucht. */
        {
            LONG j;

            for (j = i + 4; j + 12 < len && j < i + 200; j++) {
                if ((buf[j] == 'X' && buf[j+1] == 'i' && buf[j+2] == 'n'
                        && buf[j+3] == 'g')
                    || (buf[j] == 'I' && buf[j+1] == 'n' && buf[j+2] == 'f'
                        && buf[j+3] == 'o')) {
                    ULONG flags = be32(buf + j + 4);

                    if (flags & 1) {    /* Zahl der Rahmen ist dabei */
                        ULONG frames = be32(buf + j + 8);

                        if (frames > 0) {
                            s->duration = (int)((frames * (ULONG)samples)
                                                / (ULONG)freq);
                            return;
                        }
                    }
                }
            }
        }

        /* Feste Bitrate: aus der Dateigroesse rechnen. Der Kopf und ein
         * etwaiges ID3v1-Etikett am Ende fallen dabei kaum ins Gewicht. */
        if (filebytes > offset) {
            s->duration = (int)(((filebytes - offset) / 125L) / rate);
        }
        return;
    }
}

BOOL local_tags(struct Song *s)
{
    BPTR fh;
    UBYTE *buf;
    LONG got, filebytes = 0;
    LONG id3len = 0;
    BOOL ok = FALSE;

    if (!s || !s->path[0]) {
        return FALSE;
    }

    fh = Open((STRPTR)s->path, MODE_OLDFILE);
    if (!fh) {
        return FALSE;
    }

    /* Dateigroesse fuer die Dauer bei fester Bitrate. */
    if (Seek(fh, 0, OFFSET_END) >= 0) {
        filebytes = Seek(fh, 0, OFFSET_BEGINNING);
    }

    buf = AllocVec(ID3_READ_MAX, MEMF_ANY);
    if (!buf) {
        Close(fh);
        return FALSE;
    }

    got = Read(fh, buf, ID3_READ_MAX);
    if (got < 10) {
        FreeVec(buf);
        Close(fh);
        return FALSE;
    }

    if (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        LONG pos = 10;
        int major = buf[3];

        id3len = (LONG)synchsafe(buf + 6) + 10;

        /* Ein Erweiterungskopf steht vor den Rahmen und ist zu
         * ueberspringen (Bit 6 der Flags). */
        if ((buf[5] & 0x40) && pos + 4 <= got) {
            pos += (LONG)((major >= 4) ? synchsafe(buf + pos)
                                       : be32(buf + pos));
        }

        while (pos + 10 <= got && pos + 10 <= id3len) {
            char fid[5];
            ULONG flen;

            memcpy(fid, buf + pos, 4);
            fid[4] = '\0';
            if (fid[0] == '\0') {
                break;                  /* Auffuellbytes, Ende der Rahmen */
            }
            flen = (major >= 4) ? synchsafe(buf + pos + 4)
                                : be32(buf + pos + 4);
            if (flen == 0 || pos + 10 + (LONG)flen > got) {
                break;
            }

            if (strcmp(fid, "TIT2") == 0) {
                id3_text(buf + pos + 10, (LONG)flen, s->title,
                         sizeof(s->title));
                ok = TRUE;
            } else if (strcmp(fid, "TPE1") == 0) {
                id3_text(buf + pos + 10, (LONG)flen, s->artist,
                         sizeof(s->artist));
            } else if (strcmp(fid, "TALB") == 0) {
                id3_text(buf + pos + 10, (LONG)flen, s->album,
                         sizeof(s->album));
            } else if (strcmp(fid, "TYER") == 0 || strcmp(fid, "TDRC") == 0) {
                char tmp[16];

                id3_text(buf + pos + 10, (LONG)flen, tmp, sizeof(tmp));
                s->year = atoi(tmp);
            } else if (strcmp(fid, "TRCK") == 0) {
                char tmp[16];

                id3_text(buf + pos + 10, (LONG)flen, tmp, sizeof(tmp));
                s->track = atoi(tmp);   /* "3/12" ergibt 3 */
            }
            pos += 10 + (LONG)flen;
        }
    }

    /* Die Musik faengt hinter dem Etikett an - dort steht der erste
     * Rahmenkopf, aus dem die Dauer kommt. */
    if (id3len > 0 && id3len < got) {
        mpeg_duration(buf + id3len, got - id3len, filebytes, id3len, s);
    } else if (id3len == 0) {
        mpeg_duration(buf, got, filebytes, 0, s);
    } else {
        /* Das Etikett ist groesser als der gelesene Block - gezielt
         * nachlesen statt alles einzuziehen. */
        if (Seek(fh, id3len, OFFSET_BEGINNING) >= 0) {
            LONG n = Read(fh, buf, 4096);

            if (n > 4) {
                mpeg_duration(buf, n, filebytes, id3len, s);
            }
        }
    }

    /* ID3v1 als Rueckfall: 128 Bytes am Dateiende, feste Feldlaengen. */
    if (!ok && filebytes > 128) {
        if (Seek(fh, filebytes - 128, OFFSET_BEGINNING) >= 0
                && Read(fh, buf, 128) == 128
                && buf[0] == 'T' && buf[1] == 'A' && buf[2] == 'G') {
            char tmp[31];

            memcpy(tmp, buf + 3, 30);  tmp[30] = '\0';
            if (tmp[0]) { strncpy(s->title,  tmp, sizeof(s->title) - 1);  ok = TRUE; }
            memcpy(tmp, buf + 33, 30); tmp[30] = '\0';
            if (tmp[0]) { strncpy(s->artist, tmp, sizeof(s->artist) - 1); }
            memcpy(tmp, buf + 63, 30); tmp[30] = '\0';
            if (tmp[0]) { strncpy(s->album,  tmp, sizeof(s->album) - 1);  }
            memcpy(tmp, buf + 93, 4);  tmp[4]  = '\0';
            if (tmp[0]) { s->year = atoi(tmp); }
            if (buf[125] == 0 && buf[126] != 0) {
                s->track = buf[126];    /* ID3v1.1 */
            }
        }
    }

    FreeVec(buf);
    Close(fh);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Aus Titeln Alben machen                                             */
/* ------------------------------------------------------------------ */

/* Das Verzeichnis eines Titels, also der Pfad ohne den Dateinamen. Es
 * ist das zweite Merkmal neben dem Album-Etikett: zwei Alben koennen
 * denselben Namen tragen ("Greatest Hits"), liegen dann aber in
 * verschiedenen Verzeichnissen. */
static void dir_of(const char *path, char *out, int outsize)
{
    int n = (int)strlen(path);

    while (n > 0 && path[n - 1] != '/' && path[n - 1] != ':') {
        n--;
    }
    /* Den Trenner mitnehmen, wenn es ein Doppelpunkt ist - "Stuff:" ist
     * ein gueltiger Pfad, "Stuff" nicht. */
    if (n > 0 && path[n - 1] == '/') {
        n--;
    }
    if (n > outsize - 1) {
        n = outsize - 1;
    }
    memcpy(out, path, (size_t)n);
    out[n] = '\0';
}

/* Der letzte Teil eines Pfades, also der Verzeichnisname. Er springt
 * ein, wenn ein Titel kein Album-Etikett hat - bei einer Sammlung aus
 * Verzeichnissen ist das fast immer der Albumname. */
static const char *lastpart(const char *path)
{
    const char *p = path + strlen(path);

    while (p > path && p[-1] != '/' && p[-1] != ':') {
        p--;
    }
    return p;
}

/* Sortierung fuer die Gruppierung: erst Verzeichnis, dann Album, dann
 * Titelnummer, dann Titel. Damit stehen die Titel eines Albums
 * hintereinander UND in der richtigen Reihenfolge. */
static int group_cmp(const void *a, const void *b)
{
    const struct Song *x = (const struct Song *)a;
    const struct Song *y = (const struct Song *)b;
    char dx[192], dy[192];
    int c;

    dir_of(x->path, dx, sizeof(dx));
    dir_of(y->path, dy, sizeof(dy));
    c = stricmp(dx, dy);
    if (c != 0) {
        return c;
    }
    c = stricmp(x->album, y->album);
    if (c != 0) {
        return c;
    }
    if (x->track != y->track) {
        return x->track - y->track;
    }
    return stricmp(x->title, y->title);
}

int local_group(struct SubList *songs, struct SubList *albums)
{
    int i;
    struct Album *cur = NULL;
    char curdir[192] = "";
    char curalb[SUB_ALBUM_LEN] = "";

    if (!songs || !albums) {
        return SUB_EAPI;
    }
    if (songs->count <= 0 || !songs->items) {
        return SUB_OK;
    }

    qsort(songs->items, (size_t)songs->count,
          (size_t)songs->itemsize, group_cmp);

    for (i = 0; i < songs->count; i++) {
        struct Song *s = (struct Song *)list_get(songs, i);
        char dir[192];

        if (!s) {
            continue;
        }
        dir_of(s->path, dir, sizeof(dir));

        if (!cur || stricmp(dir, curdir) != 0
                 || stricmp(s->album, curalb) != 0) {
            cur = (struct Album *)list_add(albums);
            if (!cur) {
                strcpy(g_why, "out of memory for the albums");
                return SUB_EMEM;
            }
            memset(cur, 0, sizeof(*cur));

            strncpy(cur->name,
                    s->album[0] ? s->album : lastpart(dir),
                    sizeof(cur->name) - 1);
            strncpy(cur->artist, s->artist, sizeof(cur->artist) - 1);
            strncpy(cur->dir, dir, sizeof(cur->dir) - 1);
            cur->year  = s->year;
            cur->first = i;
            cur->count = 0;

            strcpy(curdir, dir);
            strncpy(curalb, s->album, sizeof(curalb) - 1);
            curalb[sizeof(curalb) - 1] = '\0';
        }

        /* Verschiedene Interpreten im selben Album: das ist eine
         * Zusammenstellung. Genau so steht es auch beim Server. */
        if (cur->artist[0] && s->artist[0]
                && stricmp(cur->artist, s->artist) != 0) {
            strcpy(cur->artist, "Various Artists");
        }
        if (!cur->year && s->year) {
            cur->year = s->year;
        }
        s->lalbum = albums->count - 1;
        cur->count++;
        cur->songcount = cur->count;
        cur->duration += s->duration;
    }
    return SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Cover fuer ein eigenes Album                                        */
/* ------------------------------------------------------------------ */

/* Zuerst wird im Verzeichnis nachgesehen. Das ist ein Dateizugriff und
 * damit viel billiger, als in jedem Titel das Etikett zu durchsuchen -
 * bei zwanzig Titeln je Album neunzehnmal umsonst.
 *
 * Erst wenn dort nichts liegt, wird das eingebettete Bild (APIC) aus dem
 * ersten Titel geholt und EINMAL in den Cover-Cache geschrieben.
 * Danach ist es eine Datei wie jede andere, und Bildwand, Miniaturen und
 * Farbverlauf arbeiten unveraendert weiter. */
static BOOL file_there(const char *dir, const char *name,
                       char *out, int outsize)
{
    BPTR fh;
    int dl = (int)strlen(dir);

    if (dl + (int)strlen(name) + 2 > outsize) {
        return FALSE;
    }
    strcpy(out, dir);
    if (dl > 0 && dir[dl - 1] != ':' && dir[dl - 1] != '/') {
        strcat(out, "/");
    }
    strcat(out, name);

    fh = Open((STRPTR)out, MODE_OLDFILE);
    if (!fh) {
        return FALSE;
    }
    Close(fh);
    return TRUE;
}

/* Das eingebettete Bild aus dem ID3v2-Etikett in eine Datei schreiben.
 *
 * Der APIC-Rahmen ist aufgebaut als: Kodierung 1 | MIME-Typ mit
 * Nullbyte | Bildart 1 | Beschreibung mit Nullbyte | Bilddaten. Die
 * Beschreibung ist bei UTF-16 mit ZWEI Nullbytes beendet - wer das
 * uebersieht, schreibt zwei Bytes Muell vor den Dateianfang, und kein
 * Datatype erkennt das Bild mehr. */
static BOOL apic_to_file(const char *mp3, const char *dest)
{
    BPTR fh, out;
    UBYTE *buf;
    LONG got, pos = 10, id3len;
    BOOL ok = FALSE;
    int major;

    fh = Open((STRPTR)mp3, MODE_OLDFILE);
    if (!fh) {
        return FALSE;
    }
    buf = AllocVec(ID3_READ_MAX, MEMF_ANY);
    if (!buf) {
        Close(fh);
        return FALSE;
    }
    got = Read(fh, buf, ID3_READ_MAX);
    if (got < 20 || buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') {
        FreeVec(buf);
        Close(fh);
        return FALSE;
    }
    major  = buf[3];
    id3len = (LONG)synchsafe(buf + 6) + 10;
    if ((buf[5] & 0x40) && pos + 4 <= got) {
        pos += (LONG)((major >= 4) ? synchsafe(buf + pos) : be32(buf + pos));
    }

    while (pos + 10 <= got && pos + 10 <= id3len) {
        char fid[5];
        ULONG flen;

        memcpy(fid, buf + pos, 4);
        fid[4] = '\0';
        if (fid[0] == '\0') {
            break;
        }
        flen = (major >= 4) ? synchsafe(buf + pos + 4) : be32(buf + pos + 4);
        if (flen == 0 || pos + 10 + (LONG)flen > got) {
            break;
        }

        if (strcmp(fid, "APIC") == 0) {
            UBYTE *p = buf + pos + 10;
            LONG left = (LONG)flen;
            int enc = p[0];
            LONG i = 1;

            while (i < left && p[i] != 0) { i++; }   /* MIME-Typ */
            i++;
            if (i < left) { i++; }                   /* Bildart */
            if (enc == 1 || enc == 2) {
                while (i + 1 < left && !(p[i] == 0 && p[i + 1] == 0)) {
                    i += 2;
                }
                i += 2;
            } else {
                while (i < left && p[i] != 0) { i++; }
                i++;
            }

            if (i < left) {
                out = Open((STRPTR)dest, MODE_NEWFILE);
                if (out) {
                    ok = (Write(out, p + i, left - i) == left - i);
                    Close(out);
                    if (!ok) {
                        DeleteFile((STRPTR)dest);
                    }
                }
            }
            break;
        }
        pos += 10 + (LONG)flen;
    }

    FreeVec(buf);
    Close(fh);
    return ok;
}

BOOL local_cover(struct Album *al, struct Song *first,
                 const char *cachedir, char *out, int outsize)
{
    static const char *names[] = {
        "folder.jpg", "cover.jpg", "front.jpg", "folder.png", "cover.png",
        "Folder.jpg", "Cover.jpg", "Front.jpg"
    };
    unsigned int i;
    ULONG h = 5381;
    const char *c;

    if (!al || !out || outsize < 32) {
        return FALSE;
    }

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (file_there(al->dir, names[i], out, outsize)) {
            return TRUE;
        }
    }

    if (!first || !first->path[0] || !cachedir || !cachedir[0]) {
        return FALSE;
    }

    /* Streuwert ueber das Verzeichnis: er ist die Kennung im Cache, so
     * wie beim Server die Cover-ID. Gleicher Ordner heisst gleiche
     * Datei, und beim naechsten Start ist sie schon da. */
    for (c = al->dir; *c; c++) {
        h = h * 33 + (ULONG)(unsigned char)*c;
    }
    sprintf(out, "%s/lo-%08lx.jpg", cachedir, (unsigned long)h);

    {
        BPTR fh = Open((STRPTR)out, MODE_OLDFILE);

        if (fh) {
            Close(fh);
            return TRUE;            /* schon beim letzten Mal geholt */
        }
    }
    return apic_to_file(first->path, out);
}
