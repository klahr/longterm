# NOTICE:
#
# Application name defined in TARGET has a corresponding QML filename.
# If name defined in TARGET is changed, the following needs to be done
# to match new name:
#   - corresponding QML filename must be changed
#   - desktop icon filename must be changed
#   - desktop filename must be changed
#   - icon definition filename in desktop file must be changed
#   - translation filenames have to be changed

# The name of your application
TARGET = longterm

CONFIG += sailfishapp
PKGCONFIG += sailfishsecrets

SOURCES += src/longterm.cpp \
    src/appsettings.cpp \
    src/colorschemes.cpp \
    src/hoststore.cpp \
    src/keystore.cpp \
    src/secretvault.cpp \
    src/sessionmanager.cpp \
    src/sshsession.cpp \
    src/terminal.cpp \
    src/terminalview.cpp

HEADERS += src/appsettings.h \
    src/colorschemes.h \
    src/hoststore.h \
    src/keystore.h \
    src/secretvault.h \
    src/sessionmanager.h \
    src/sshsession.h \
    src/terminal.h \
    src/terminalview.h

# libvterm (MIT) is small enough to compile straight into the app
LIBVTERM_SRC = $$PWD/3rdparty/libvterm
LIBVTERM_GEN = $$OUT_PWD/libvterm-gen
INCLUDEPATH += $$LIBVTERM_SRC/include $$LIBVTERM_GEN
SOURCES += $$files($$LIBVTERM_SRC/src/*.c)
QMAKE_CFLAGS += -std=c99

# The git tree only has the character set tables as .tbl sources, generate
# the .inc files encoding.c includes the same way upstream's Makefile does.
# Targets are relative to the build directory, as qmake's include scanner
# writes them that way too.
vterm_decdrawing.target = libvterm-gen/encoding/DECdrawing.inc
vterm_decdrawing.depends = $$LIBVTERM_SRC/src/encoding/DECdrawing.tbl
vterm_decdrawing.commands = mkdir -p libvterm-gen/encoding && \
    perl -CSD $$LIBVTERM_SRC/tbl2inc_c.pl $$vterm_decdrawing.depends > $$vterm_decdrawing.target
vterm_uk.target = libvterm-gen/encoding/uk.inc
vterm_uk.depends = $$LIBVTERM_SRC/src/encoding/uk.tbl
vterm_uk.commands = mkdir -p libvterm-gen/encoding && \
    perl -CSD $$LIBVTERM_SRC/tbl2inc_c.pl $$vterm_uk.depends > $$vterm_uk.target
vterm_encoding.target = encoding.o
vterm_encoding.depends = $$vterm_decdrawing.target $$vterm_uk.target
QMAKE_EXTRA_TARGETS += vterm_decdrawing vterm_uk vterm_encoding

# libssh is not available on the device, so it is built from 3rdparty/libssh
# and shipped as a private shared library in /usr/share/$${TARGET}/lib
LIBSSH_SRC = $$PWD/3rdparty/libssh
LIBSSH_BUILD = $$OUT_PWD/libssh-build

libssh.target = $$LIBSSH_BUILD/lib/libssh.so
libssh.commands = cmake -S $$LIBSSH_SRC -B $$LIBSSH_BUILD \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_SERVER=OFF -DWITH_GSSAPI=OFF -DWITH_PCAP=OFF \
    -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF \
    -DWITH_DEBUG_CALLTRACE=OFF && \
    cmake --build $$LIBSSH_BUILD --parallel
# These objects include generated libssh headers. One rule each, qmake
# escapes spaces in a target name.
sshsession_libssh.target = sshsession.o
sshsession_libssh.depends = $$libssh.target
keystore_libssh.target = keystore.o
keystore_libssh.depends = $$libssh.target
QMAKE_EXTRA_TARGETS += libssh sshsession_libssh keystore_libssh
PRE_TARGETDEPS += $$libssh.target

INCLUDEPATH += $$LIBSSH_SRC/include $$LIBSSH_BUILD/include
LIBS += -L$$LIBSSH_BUILD/lib -lssh
QMAKE_RPATHDIR += /usr/share/$${TARGET}/lib

libssh_install.path = /usr/share/$${TARGET}/lib
libssh_install.extra = mkdir -p $(INSTALL_ROOT)$$libssh_install.path && \
    cp -P $$LIBSSH_BUILD/lib/libssh.so.* $(INSTALL_ROOT)$$libssh_install.path/
INSTALLS += libssh_install

DISTFILES += qml/longterm.qml \
    qml/cover/CoverPage.qml \
    qml/pages/ColorSchemesPage.qml \
    qml/pages/GenerateKeyDialog.qml \
    qml/pages/HostPage.qml \
    qml/pages/ImportKeyDialog.qml \
    qml/pages/KeyPage.qml \
    qml/pages/KeysPage.qml \
    qml/pages/SessionMenuPage.qml \
    qml/pages/SessionPage.qml \
    qml/pages/SessionsPage.qml \
    qml/pages/SettingsPage.qml \
    rpm/longterm.changes.in \
    rpm/longterm.changes.run.in \
    rpm/longterm.spec \
    translations/*.ts \
    longterm.desktop \
    fonts/LICENSE.md \
    LICENSE \
    README.md \
    icons/longterm.svg

# Development convenience: prefill the connect form password from ./passwd.
# The value ends up in the binary, so keep the file out of release builds.
exists($$PWD/passwd) {
    DEFINES += LONGTERM_DEV_PASSWORD=\\\"$$cat($$PWD/passwd)\\\"
}

# Source Code Pro (SIL Open Font License 1.1, see fonts/LICENSE.md)
fonts.files = fonts/SourceCodePro-Medium.ttf fonts/SourceCodePro-Bold.ttf fonts/LICENSE.md
fonts.path = /usr/share/$${TARGET}/fonts
INSTALLS += fonts

SAILFISHAPP_ICONS = 86x86 108x108 128x128 172x172

# to disable building translations every time, comment out the
# following CONFIG line
CONFIG += sailfishapp_i18n

# German translation is enabled as an example. If you aren't
# planning to localize your app, remember to comment out the
# following TRANSLATIONS line. And also do not forget to
# modify the localized app name in the the .desktop file.
TRANSLATIONS += translations/longterm-de.ts
