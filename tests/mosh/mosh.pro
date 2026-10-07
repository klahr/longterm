TEMPLATE = app
TARGET = moshtest
QT += quick
CONFIG += console link_pkgconfig
PKGCONFIG += libcrypto zlib

ROOT = $$PWD/../..
INCLUDEPATH += $$ROOT/src
SOURCES += main.cpp \
    $$ROOT/src/colorschemes.cpp \
    $$ROOT/src/moshclient.cpp \
    $$ROOT/src/terminal.cpp
HEADERS += $$ROOT/src/colorschemes.h \
    $$ROOT/src/moshclient.h \
    $$ROOT/src/terminal.h

LIBVTERM_SRC = $$ROOT/3rdparty/libvterm
LIBVTERM_GEN = $$OUT_PWD/libvterm-gen
INCLUDEPATH += $$LIBVTERM_SRC/include $$LIBVTERM_SRC/src $$LIBVTERM_GEN
QMAKE_CFLAGS += -std=c99

LIBVTERM_PATCHES = $$ROOT/3rdparty/patches/libvterm
LIBVTERM_PATCHED = parser.c screen.c state.c
LIBVTERM_SOURCES = $$files($$LIBVTERM_SRC/src/*.c)
for(file, LIBVTERM_PATCHED) {
    !system(mkdir -p $$LIBVTERM_GEN/src && cp $$LIBVTERM_SRC/src/$$file $$LIBVTERM_GEN/src/$$file \
            && patch -s $$LIBVTERM_GEN/src/$$file $$LIBVTERM_PATCHES/$${file}.patch): \
        error(Could not patch libvterm $$file)
    LIBVTERM_SOURCES -= $$LIBVTERM_SRC/src/$$file
    LIBVTERM_SOURCES += $$LIBVTERM_GEN/src/$$file
}
SOURCES += $$LIBVTERM_SOURCES

for(table, $$list(DECdrawing uk)) {
    !system(mkdir -p $$LIBVTERM_GEN/encoding && perl -CSD $$LIBVTERM_SRC/tbl2inc_c.pl \
            $$LIBVTERM_SRC/src/encoding/$${table}.tbl > $$LIBVTERM_GEN/encoding/$${table}.inc): \
        error(Could not generate libvterm $$table table)
}
