# Needs an SSH server on 127.0.0.1 with the account tester/secret, mosh-server,
# tmux and nc, see main.cpp. LIBSSH_BUILD is a build of 3rdparty/libssh.
TEMPLATE = app
TARGET = sshtest
QT += quick network concurrent testlib
CONFIG += console link_pkgconfig
PKGCONFIG += libcrypto zlib

ROOT = $$PWD/../..
INCLUDEPATH += $$PWD/stubs $$ROOT/src
SOURCES += main.cpp \
    secretvault_stub.cpp \
    $$ROOT/src/appsettings.cpp \
    $$ROOT/src/backup.cpp \
    $$ROOT/src/colorschemes.cpp \
    $$ROOT/src/hoststore.cpp \
    $$ROOT/src/sessionmanager.cpp \
    $$ROOT/src/keystore.cpp \
    $$ROOT/src/moshclient.cpp \
    $$ROOT/src/sftpbrowser.cpp \
    $$ROOT/src/sftpengine.cpp \
    $$ROOT/src/sshagent.cpp \
    $$ROOT/src/sshsession.cpp \
    $$ROOT/src/terminal.cpp
HEADERS += $$ROOT/src/appsettings.h \
    $$ROOT/src/backup.h \
    $$ROOT/src/colorschemes.h \
    $$ROOT/src/hoststore.h \
    $$ROOT/src/sessionmanager.h \
    $$ROOT/src/keystore.h \
    $$ROOT/src/moshclient.h \
    $$ROOT/src/secretvault.h \
    $$ROOT/src/sftpbrowser.h \
    $$ROOT/src/sftpengine.h \
    $$ROOT/src/sshagent.h \
    $$ROOT/src/sshsession.h \
    $$ROOT/src/terminal.h

INCLUDEPATH += $$ROOT/3rdparty/libssh/include $$LIBSSH_BUILD/include
LIBS += -L$$LIBSSH_BUILD/lib -lssh
QMAKE_RPATHDIR += $$LIBSSH_BUILD/lib

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
