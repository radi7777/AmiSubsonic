#!/usr/bin/env python3
"""Macht aus AmiSubsonic_icon128.png das Workbench-Icon AmiSubsonic.info.

Nach dem Muster von appicon.py aus AmiHomeassist, zwei Fassungen in
einer Datei, wie auf dem Amiga ueblich:

  klassisch   64x64 in den vier Workbench-Grundfarben (grau, schwarz,
              weiss, blau). Die sieht eine alte icon.library - Workbench
              3.1 im Emulator zeigt NUR diese.

  GlowIcon    dieselbe Zeichnung mit 256 Farben, Farbe 0 durchsichtig,
              und im angewaehlten Zustand mit Lichtkranz. icon.library
              ab V44 (OS 3.2 auf dem A500) zeigt diese.

Anders als bei AmiHomeassist 256 statt 16 Farben: das Bild ist ein
Farbverlauf, mit 16 Farben waeren daraus breite Streifen geworden.
Das Format erlaubt bis zu 256 (Tiefe 8 im IMAG-Kopf).

    python3 appicon.py     schreibt AmiSubsonic.info und eine Vorschau
"""

import struct

from PIL import Image

SRC = "AmiSubsonic_icon128.png"
SIZE = 64                  # vom Anwender gewaehlt, 21.9.2026
NOPOS = -2147483648

WB_GREY, WB_BLACK, WB_WHITE, WB_BLUE = 0, 1, 2, 3
WB_PAL = [(0x95, 0x95, 0x95), (0, 0, 0), (255, 255, 255), (0x3B, 0x67, 0xA2)]

GLOW_T = 0                 # durchsichtig
GLOW_RING = 255            # Lichtkranz im angewaehlten Zustand
RING_RGB = (0xFF, 0xE0, 0x80)


def load():
    im = Image.open(SRC).convert("RGBA")
    return im.resize((SIZE, SIZE), Image.LANCZOS)


def grids(im):
    """(klassisch, glow, glow-Palette)"""
    px = im.load()

    # 254 Farben fuer das Bild (1..254), 0 durchsichtig, 255 Lichtkranz.
    rgb = Image.new("RGB", im.size, (0, 0, 0))
    rgb.paste(im, mask=im.split()[3])
    q = rgb.quantize(colors=254, method=Image.MEDIANCUT, dither=Image.NONE)
    qpal = q.getpalette()[:254 * 3]
    qpal += [0] * (254 * 3 - len(qpal))
    qpx = q.load()

    pal = [WB_PAL[0]]
    pal += [tuple(qpal[i * 3:i * 3 + 3]) for i in range(254)]
    pal += [RING_RGB]

    classic = [[WB_GREY] * SIZE for _ in range(SIZE)]
    glow = [[GLOW_T] * SIZE for _ in range(SIZE)]
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            if a < 128:
                continue
            glow[y][x] = qpx[x, y] + 1
            lum = (r * 3 + g * 6 + b) // 10
            if lum < 50:
                classic[y][x] = WB_BLACK
            elif lum > 190:
                classic[y][x] = WB_WHITE
            else:
                classic[y][x] = WB_BLUE
    return classic, glow, pal


def glow_selected(gn):
    """Angewaehlt: jeder freie Punkt am Rand der Zeichnung leuchtet."""
    out = [row[:] for row in gn]
    for y in range(SIZE):
        for x in range(SIZE):
            if gn[y][x] != GLOW_T:
                continue
            if any(gn[y + dy][x + dx] != GLOW_T
                   for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                   if 0 <= y + dy < SIZE and 0 <= x + dx < SIZE):
                out[y][x] = GLOW_RING
    return out


def rle_pack(values, depth):
    """ByteRun1 ueber einen Bitstrom: 8-Bit-Steuerbytes, Werte depth Bit."""
    stream = []
    i, n = 0, len(values)
    while i < n:
        run = 1
        while i + run < n and values[i + run] == values[i] and run < 128:
            run += 1
        if run >= 2:
            stream.append((257 - run, 8))
            stream.append((values[i], depth))
            i += run
        else:
            lits = []
            while i < n and len(lits) < 128:
                if i + 1 < n and values[i + 1] == values[i]:
                    break
                lits.append(values[i])
                i += 1
            stream.append((len(lits) - 1, 8))
            stream += [(v, depth) for v in lits]

    buf, acc, nbits = bytearray(), 0, 0
    for v, w in stream:
        acc = (acc << w) | (v & ((1 << w) - 1))
        nbits += w
        while nbits >= 8:
            nbits -= 8
            buf.append((acc >> nbits) & 0xFF)
    if nbits:
        buf.append((acc << (8 - nbits)) & 0xFF)
    return bytes(buf)


def imag_chunk(grid, pal):
    depth = max(1, (len(pal) - 1).bit_length())
    img = rle_pack([p for row in grid for p in row], depth)
    palbytes = bytes(c for rgb in pal for c in rgb)
    body = struct.pack(">BBBBBBHH", GLOW_T, len(pal) - 1, 0x01 | 0x02, 1, 0,
                       depth, len(img) - 1, len(palbytes) - 1)
    body += img + palbytes
    return (b"IMAG" + struct.pack(">I", len(body)) + body
            + (b"\0" if len(body) & 1 else b""))


def glow_form(glow, pal):
    face = struct.pack(">BBBBH", SIZE - 1, SIZE - 1, 1, 0x11,
                       len(pal) * 3 - 1)
    body = (b"ICON"
            + b"FACE" + struct.pack(">I", len(face)) + face
            + imag_chunk(glow, pal)
            + imag_chunk(glow_selected(glow), pal))
    return b"FORM" + struct.pack(">I", len(body)) + body


def planar(g, depth):
    words = (SIZE + 15) // 16
    out = bytearray()
    for plane in range(depth):
        for y in range(SIZE):
            bits = 0
            for x in range(SIZE):
                if (g[y][x] >> plane) & 1:
                    bits |= 1 << (words * 16 - 1 - x)
            out += bits.to_bytes(words * 2, "big")
    return bytes(out)


def image_header(depth):
    return struct.pack(">hhhhhIBBI", 0, 0, SIZE, SIZE, depth, 1, 0x03, 0x00, 0)


def build_info(classic, glow, pal):
    gadget = struct.pack(">IhhhhHHH", 0, 0, 0, SIZE, SIZE, 0x0006, 0x0001, 0x0001)
    gadget += struct.pack(">IIIIIHI", 1, 1, 0, 0, 0, 0, 0)
    gadget = gadget[:44]

    do = struct.pack(">HH", 0xE310, 1) + gadget
    do += bytes([3, 0])                       # WBTOOL
    do += struct.pack(">II", 0, 0)
    do += struct.pack(">ii", NOPOS, NOPOS)
    do += struct.pack(">III", 0, 0, 0)
    assert len(do) == 78, len(do)

    out = bytearray(do)
    out += image_header(2) + planar(classic, 2)
    out += image_header(2) + planar(classic, 2)
    out += glow_form(glow, pal)
    return bytes(out)


def main():
    classic, glow, pal = grids(load())
    with open("AmiSubsonic.info", "wb") as f:
        f.write(build_info(classic, glow, pal))

    # Vorschau: klassisch | GlowIcon | angewaehlt, je vierfach
    z = 4
    sel = glow_selected(glow)
    prev = Image.new("RGB", (SIZE * 3 * z, SIZE * z), WB_PAL[0])
    p = prev.load()
    for n, (g, pl) in enumerate(((classic, WB_PAL), (glow, pal), (sel, pal))):
        for y in range(SIZE):
            for x in range(SIZE):
                c = pl[g[y][x]]
                for dy in range(z):
                    for dx in range(z):
                        p[(n * SIZE + x) * z + dx, y * z + dy] = c
    prev.save("/tmp/amisubsonic_icon_preview.png")
    print("AmiSubsonic.info geschrieben, Vorschau in "
          "/tmp/amisubsonic_icon_preview.png")


if __name__ == "__main__":
    main()
