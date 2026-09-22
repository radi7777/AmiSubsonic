# AmiSubsonic - Cross-Build mit m68k-amigaos-gcc (AmigaPorts-Toolchain).
#
# Zielprofil: 68020 aufwaerts, MIT UND OHNE FPU. Deshalb -mcpu=68020 und
# sonst nichts. Zielplattform der Entwicklung ist zwar PiStorm (68040 mit
# FPU), aber dieses Programm rechnet nirgends mit Fliesskomma: der
# Gradient interpoliert ganzzahlig, die Hauptfarbe kommt aus einem
# Histogramm auf Bytes, Zeiten sind Sekunden. Eine FPU vorauszusetzen
# wuerde also nichts einbringen und nur die Maschinen ausschliessen, auf
# denen keine steckt.
#
# RTG/Truecolor ist davon unberuehrt - das ist eine Anforderung an die
# Grafikkarte, nicht an die CPU.
#
#   make            → AmiSubsonicCLI (spaeter zusaetzlich AmiSubsonic)
#   make check-fpu  → prueft, dass kein FPU-Opcode im Binary steckt
#   make push       → schiebt das Binary auf den Amiga

TOOLCHAIN ?= $(HOME)/opt/m68k-amigaos-gcc-16.2
CC         = $(TOOLCHAIN)/bin/m68k-amigaos-gcc
OBJDUMP    = $(TOOLCHAIN)/bin/m68k-amigaos-objdump
AR         = $(TOOLCHAIN)/bin/m68k-amigaos-ar
AMIGALIB   = $(TOOLCHAIN)/m68k-amigaos/lib/libamiga.a

CPU        = 68020

# -MMD -MP schreibt je Objekt eine .d-Datei mit den Headern, die es
# einbindet. Ohne das uebersetzt make nach einer Header-Aenderung nichts
# neu und man testet stillschweigend das alte Binary weiter.
CFLAGS  = -mcpu=$(CPU) -Os -fomit-frame-pointer -noixemul -MMD -MP \
          -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign \
          -Ivendor/mui/include -Ivendor/mpega/include -I.

# -noixemul waehlt libnix statt newlib - kein ixemul.library als
# Voraussetzung.
#
# ACHTUNG: hier steht bewusst KEIN -lamiga. amiga.lib bringt ein eigenes
# _sprintf mit, die RawDoFmt-Fassung, und dort ist %d SECHZEHN Bit breit.
# sprintf("%d", 400) schreibt dann "0" und schiebt alle folgenden
# Argumente um zwei Bytes. In diesem Programm waere das besonders
# heimtueckisch: die Anfrage ginge mit kaputtem Port und verschobenem
# Token raus, und Navidrome antwortete mit "Wrong username or password".
# Aus amiga.lib brauchen wir nur DoMethod() fuer MUI - das holen wir uns
# unten als einzelnes Objekt heraus.
LDFLAGS = -noixemul -s -lgcc

CORE_OBJS = amisub.o md5.o cover.o audio.o ring.o local.o aacsize.o aacsize_sbr.o

# Helix AAC (RealNetworks, RPSL - siehe vendor/helix-aac/README.amiga).
# Eigene Regel mit eigenen Flags: -DARDUINO waehlt dort die normale
# stdlib, die Ersatz-Header in amiga/ fangen <Arduino.h>/<pgmspace.h>
# ab, und die Warnungen sind aus - es ist fremder Code, den wir mit
# Absicht NICHT anfassen (ausser dem 68k-Block in assembly.h).
# -fwrapv: Helix schiebt vorzeichenbehaftete Werte ueber das Vorzeichen
# hinaus (UBSan auf dem Mac, 22.9.2026) - formal undefiniert, gemeint ist
# Zweierkomplement. So bleibt gcc beim Gemeinten, auch bei -O3.
AAC_DIR   = vendor/helix-aac
AAC_SRCS  = $(wildcard $(AAC_DIR)/*.c)
AAC_OBJS  = $(patsubst $(AAC_DIR)/%.c,build/aac/%.o,$(AAC_SRCS))
AAC_FLAGS = -mcpu=$(CPU) -Os -fomit-frame-pointer -noixemul -MMD -MP -w -fwrapv \
            -DARDUINO -I$(AAC_DIR)/amiga -I$(AAC_DIR)

CLI_OBJS  = $(CORE_OBJS) cli.o $(AAC_OBJS) build/DoMethod.o
GUI_OBJS  = $(CORE_OBJS) gui.o panel.o tracklist.o albumgrid.o sidebar.o \
            tabs.o visual.o \
            player.o netjob.o muistubs.o $(AAC_OBJS) \
            build/DoMethod.o build/DoSuperMethod.o

all: AmiSubsonicCLI AmiSubsonic AmiSubsonic.info

# Das Workbench-Icon, aus AmiSubsonic_icon128.png. Braucht Python mit
# PIL - das gibt es auf diesem Mac nur unter /usr/local/bin/python3.
AmiSubsonic.info: appicon.py AmiSubsonic_icon128.png
	/usr/local/bin/python3 appicon.py

AmiSubsonicCLI: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJS) $(LDFLAGS)

AmiSubsonic: $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(GUI_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Der Visualizer rechnet je Bild eine FFT und zeichnet einige tausend
# Bildpunkte - als einziger Teil dieses Programms lohnt sich hier
# Tempo vor Groesse. Gemessen auf der PiStorm (22.9.2026, 300 Bilder):
# Spektrum 1,09 -> 0,75 ms, Zeichnen 1,12 -> 0,97 ms je Bild. Die
# Pruefsumme ueber alle 300 Bildpuffer ist mit -Os und -O3 dieselbe.
visual.o: visual.c
	$(CC) $(filter-out -Os,$(CFLAGS)) -O3 -fwrapv -c -o $@ $<

# Eigene Datei, aber mit Helix' Flags - nur dort kennt man die Groessen.
aacsize.o aacsize_sbr.o: %.o: %.c
	$(CC) $(AAC_FLAGS) -c -o $@ $<

build/aac/%.o: $(AAC_DIR)/%.c
	@mkdir -p build/aac
	$(CC) $(AAC_FLAGS) -c -o $@ $<

# Nur die beiden gebrauchten Objekte aus amiga.lib, statt der ganzen
# Bibliothek - siehe die sprintf-Warnung oben bei LDFLAGS. DoMethod fuer
# MUIM_Notify, DoSuperMethod fuer den Dispatcher der eigenen Klasse.
build/DoMethod.o: $(AMIGALIB)
	@mkdir -p build
	cd build && $(AR) x $(AMIGALIB) DoMethod.o

build/DoSuperMethod.o: $(AMIGALIB)
	@mkdir -p build
	cd build && $(AR) x $(AMIGALIB) DoSuperMethod.o

# Harte Zusicherung fuers Zielprofil: kein einziger FPU-Opcode.
# Die Pruefung steckt in checkfpu.py und nicht in einem awk-Einzeiler,
# weil gcc Sprungtabellen von switch mitten in den Code legt: objdump
# zerlegt deren Bytes als Befehle und meldet dann ein 'ftstp', das nie
# ausgefuehrt wird.
check-fpu: AmiSubsonicCLI AmiSubsonic
	@python3 checkfpu.py $(OBJDUMP) AmiSubsonicCLI AmiSubsonic

push: AmiSubsonicCLI AmiSubsonic AmiSubsonic.info
	python3 push.py AmiSubsonicCLI AmiSubsonic AmiSubsonic.info

-include $(CORE_OBJS:.o=.d) cli.d $(GUI_OBJS:.o=.d) $(AAC_OBJS:.o=.d)

clean:
	rm -rf *.o *.d build AmiSubsonicCLI AmiSubsonic

.PHONY: all clean check-fpu push
