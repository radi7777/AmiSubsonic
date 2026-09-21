/* MD5 (RFC 1321) - wird fuer die Subsonic-Anmeldung gebraucht.
 *
 * Subsonic schickt das Passwort nicht im Klartext, sondern als
 * t = md5(passwort + salt) zusammen mit s = salt. Der Amiga muss den
 * Hash also selbst rechnen; eine md5.library gibt es hier nicht.
 *
 * Reine Ganzzahlarithmetik auf 32 Bit - passt zum Zielprofil (68020,
 * keine FPU) und ist auf jedem Amiga schnell genug: die Eingabe ist ein
 * Passwort plus acht Zeichen Salt, also ein einziger 64-Byte-Block.
 */

#ifndef MD5_H
#define MD5_H

struct MD5Ctx {
    unsigned long state[4];
    unsigned long count[2];         /* Laenge in Bits, low/high */
    unsigned char buffer[64];
};

void md5_init(struct MD5Ctx *ctx);
void md5_update(struct MD5Ctx *ctx, const unsigned char *data, unsigned long len);
void md5_final(struct MD5Ctx *ctx, unsigned char digest[16]);

/* Bequemlichkeit: hasht einen String und schreibt die 32 Hexziffern
 * kleingeschrieben nach out (braucht 33 Bytes). Genau die Form, die
 * Subsonic im Parameter t erwartet. */
void md5_hex(const char *s, char *out);

#endif
