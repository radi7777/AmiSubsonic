/* MD5 nach RFC 1321. Kompakte Fassung, gemeinfreier Algorithmus.
 *
 * ACHTUNG bei 'unsigned long': auf dem 68k ist das 32 Bit, also genau
 * richtig. Trotzdem maskiert der Code jede Addition mit 0xffffffffUL -
 * das kostet auf 32 Bit nichts und macht die Datei auf einem 64-Bit-Wirt
 * uebersetzbar, was fuer einen Selbsttest auf dem Mac praktisch ist.
 */

#include <string.h>
#include "md5.h"

#define M32(x) ((x) & 0xffffffffUL)

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROL(x, n) M32(((x) << (n)) | (M32(x) >> (32 - (n))))

#define STEP(f, a, b, c, d, x, s, ac) \
    (a) = M32((a) + f((b), (c), (d)) + (x) + (unsigned long)(ac)); \
    (a) = ROL((a), (s)); \
    (a) = M32((a) + (b));

static void md5_block(unsigned long state[4], const unsigned char block[64])
{
    unsigned long a = state[0], b = state[1], c = state[2], d = state[3];
    unsigned long x[16];
    int i;

    /* MD5 liest little-endian - der 68k ist big-endian, also von Hand
     * zusammensetzen statt den Puffer umzudeuten. Ein Cast auf
     * (unsigned long *) waere hier still falsch UND wuerde auf dem 68000
     * bei ungerader Adresse zusaetzlich einen Adressfehler ausloesen. */
    for (i = 0; i < 16; i++) {
        x[i] =  (unsigned long)block[i * 4]
             | ((unsigned long)block[i * 4 + 1] << 8)
             | ((unsigned long)block[i * 4 + 2] << 16)
             | ((unsigned long)block[i * 4 + 3] << 24);
    }

    STEP(F, a, b, c, d, x[ 0],  7, 0xd76aa478UL)
    STEP(F, d, a, b, c, x[ 1], 12, 0xe8c7b756UL)
    STEP(F, c, d, a, b, x[ 2], 17, 0x242070dbUL)
    STEP(F, b, c, d, a, x[ 3], 22, 0xc1bdceeeUL)
    STEP(F, a, b, c, d, x[ 4],  7, 0xf57c0fafUL)
    STEP(F, d, a, b, c, x[ 5], 12, 0x4787c62aUL)
    STEP(F, c, d, a, b, x[ 6], 17, 0xa8304613UL)
    STEP(F, b, c, d, a, x[ 7], 22, 0xfd469501UL)
    STEP(F, a, b, c, d, x[ 8],  7, 0x698098d8UL)
    STEP(F, d, a, b, c, x[ 9], 12, 0x8b44f7afUL)
    STEP(F, c, d, a, b, x[10], 17, 0xffff5bb1UL)
    STEP(F, b, c, d, a, x[11], 22, 0x895cd7beUL)
    STEP(F, a, b, c, d, x[12],  7, 0x6b901122UL)
    STEP(F, d, a, b, c, x[13], 12, 0xfd987193UL)
    STEP(F, c, d, a, b, x[14], 17, 0xa679438eUL)
    STEP(F, b, c, d, a, x[15], 22, 0x49b40821UL)

    STEP(G, a, b, c, d, x[ 1],  5, 0xf61e2562UL)
    STEP(G, d, a, b, c, x[ 6],  9, 0xc040b340UL)
    STEP(G, c, d, a, b, x[11], 14, 0x265e5a51UL)
    STEP(G, b, c, d, a, x[ 0], 20, 0xe9b6c7aaUL)
    STEP(G, a, b, c, d, x[ 5],  5, 0xd62f105dUL)
    STEP(G, d, a, b, c, x[10],  9, 0x02441453UL)
    STEP(G, c, d, a, b, x[15], 14, 0xd8a1e681UL)
    STEP(G, b, c, d, a, x[ 4], 20, 0xe7d3fbc8UL)
    STEP(G, a, b, c, d, x[ 9],  5, 0x21e1cde6UL)
    STEP(G, d, a, b, c, x[14],  9, 0xc33707d6UL)
    STEP(G, c, d, a, b, x[ 3], 14, 0xf4d50d87UL)
    STEP(G, b, c, d, a, x[ 8], 20, 0x455a14edUL)
    STEP(G, a, b, c, d, x[13],  5, 0xa9e3e905UL)
    STEP(G, d, a, b, c, x[ 2],  9, 0xfcefa3f8UL)
    STEP(G, c, d, a, b, x[ 7], 14, 0x676f02d9UL)
    STEP(G, b, c, d, a, x[12], 20, 0x8d2a4c8aUL)

    STEP(H, a, b, c, d, x[ 5],  4, 0xfffa3942UL)
    STEP(H, d, a, b, c, x[ 8], 11, 0x8771f681UL)
    STEP(H, c, d, a, b, x[11], 16, 0x6d9d6122UL)
    STEP(H, b, c, d, a, x[14], 23, 0xfde5380cUL)
    STEP(H, a, b, c, d, x[ 1],  4, 0xa4beea44UL)
    STEP(H, d, a, b, c, x[ 4], 11, 0x4bdecfa9UL)
    STEP(H, c, d, a, b, x[ 7], 16, 0xf6bb4b60UL)
    STEP(H, b, c, d, a, x[10], 23, 0xbebfbc70UL)
    STEP(H, a, b, c, d, x[13],  4, 0x289b7ec6UL)
    STEP(H, d, a, b, c, x[ 0], 11, 0xeaa127faUL)
    STEP(H, c, d, a, b, x[ 3], 16, 0xd4ef3085UL)
    STEP(H, b, c, d, a, x[ 6], 23, 0x04881d05UL)
    STEP(H, a, b, c, d, x[ 9],  4, 0xd9d4d039UL)
    STEP(H, d, a, b, c, x[12], 11, 0xe6db99e5UL)
    STEP(H, c, d, a, b, x[15], 16, 0x1fa27cf8UL)
    STEP(H, b, c, d, a, x[ 2], 23, 0xc4ac5665UL)

    STEP(I, a, b, c, d, x[ 0],  6, 0xf4292244UL)
    STEP(I, d, a, b, c, x[ 7], 10, 0x432aff97UL)
    STEP(I, c, d, a, b, x[14], 15, 0xab9423a7UL)
    STEP(I, b, c, d, a, x[ 5], 21, 0xfc93a039UL)
    STEP(I, a, b, c, d, x[12],  6, 0x655b59c3UL)
    STEP(I, d, a, b, c, x[ 3], 10, 0x8f0ccc92UL)
    STEP(I, c, d, a, b, x[10], 15, 0xffeff47dUL)
    STEP(I, b, c, d, a, x[ 1], 21, 0x85845dd1UL)
    STEP(I, a, b, c, d, x[ 8],  6, 0x6fa87e4fUL)
    STEP(I, d, a, b, c, x[15], 10, 0xfe2ce6e0UL)
    STEP(I, c, d, a, b, x[ 6], 15, 0xa3014314UL)
    STEP(I, b, c, d, a, x[13], 21, 0x4e0811a1UL)
    STEP(I, a, b, c, d, x[ 4],  6, 0xf7537e82UL)
    STEP(I, d, a, b, c, x[11], 10, 0xbd3af235UL)
    STEP(I, c, d, a, b, x[ 2], 15, 0x2ad7d2bbUL)
    STEP(I, b, c, d, a, x[ 9], 21, 0xeb86d391UL)

    state[0] = M32(state[0] + a);
    state[1] = M32(state[1] + b);
    state[2] = M32(state[2] + c);
    state[3] = M32(state[3] + d);
}

void md5_init(struct MD5Ctx *ctx)
{
    ctx->state[0] = 0x67452301UL;
    ctx->state[1] = 0xefcdab89UL;
    ctx->state[2] = 0x98badcfeUL;
    ctx->state[3] = 0x10325476UL;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
}

void md5_update(struct MD5Ctx *ctx, const unsigned char *data, unsigned long len)
{
    unsigned long have = (ctx->count[0] >> 3) & 0x3f;
    unsigned long need = 64 - have;
    unsigned long i;

    ctx->count[0] = M32(ctx->count[0] + (len << 3));
    if (ctx->count[0] < M32(len << 3)) {
        ctx->count[1]++;
    }
    ctx->count[1] = M32(ctx->count[1] + (len >> 29));

    if (len >= need) {
        memcpy(ctx->buffer + have, data, need);
        md5_block(ctx->state, ctx->buffer);
        for (i = need; i + 63 < len; i += 64) {
            md5_block(ctx->state, data + i);
        }
        have = 0;
    } else {
        i = 0;
    }
    memcpy(ctx->buffer + have, data + i, len - i);
}

void md5_final(struct MD5Ctx *ctx, unsigned char digest[16])
{
    static const unsigned char pad[64] = { 0x80 };
    unsigned char lenbits[8];
    unsigned long have;
    int i;

    for (i = 0; i < 4; i++) {
        lenbits[i]     = (unsigned char)((ctx->count[0] >> (i * 8)) & 0xff);
        lenbits[i + 4] = (unsigned char)((ctx->count[1] >> (i * 8)) & 0xff);
    }

    have = (ctx->count[0] >> 3) & 0x3f;
    md5_update(ctx, pad, (have < 56) ? (56 - have) : (120 - have));
    md5_update(ctx, lenbits, 8);

    for (i = 0; i < 4; i++) {
        digest[i * 4]     = (unsigned char)((ctx->state[i]      ) & 0xff);
        digest[i * 4 + 1] = (unsigned char)((ctx->state[i] >>  8) & 0xff);
        digest[i * 4 + 2] = (unsigned char)((ctx->state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = (unsigned char)((ctx->state[i] >> 24) & 0xff);
    }
}

void md5_hex(const char *s, char *out)
{
    static const char hex[] = "0123456789abcdef";
    struct MD5Ctx ctx;
    unsigned char d[16];
    int i;

    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char *)s, (unsigned long)strlen(s));
    md5_final(&ctx, d);

    for (i = 0; i < 16; i++) {
        out[i * 2]     = hex[(d[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[d[i] & 0x0f];
    }
    out[32] = '\0';
}
