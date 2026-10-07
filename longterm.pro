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
DEFINES += APP_VERSION=\\\"$$VERSION\\\"
QT += network concurrent
PKGCONFIG += sailfishsecrets zlib

SOURCES += src/longterm.cpp \
    src/appsettings.cpp \
    src/backup.cpp \
    src/colorschemes.cpp \
    src/hostlist.cpp \
    src/hoststore.cpp \
    src/keystore.cpp \
    src/knownhosts.cpp \
    src/moshclient.cpp \
    src/qrimageprovider.cpp \
    src/secretvault.cpp \
    src/sessionfilter.cpp \
    src/sessionmanager.cpp \
    src/sftpbrowser.cpp \
    src/sftpengine.cpp \
    src/sshagent.cpp \
    src/sshsession.cpp \
    src/terminal.cpp \
    src/terminalview.cpp

HEADERS += src/appsettings.h \
    src/backup.h \
    src/colorschemes.h \
    src/hostlist.h \
    src/hoststore.h \
    src/keystore.h \
    src/knownhosts.h \
    src/moshclient.h \
    src/qrimageprovider.h \
    src/secretvault.h \
    src/sessionfilter.h \
    src/sessionmanager.h \
    src/sftpbrowser.h \
    src/sftpengine.h \
    src/sshagent.h \
    src/sshsession.h \
    src/terminal.h \
    src/terminalview.h

# libvterm (MIT) is small enough to compile straight into the app
LIBVTERM_SRC = $$PWD/3rdparty/libvterm
LIBVTERM_GEN = $$OUT_PWD/libvterm-gen
# The patched copies below include their neighbours from src
INCLUDEPATH += $$LIBVTERM_SRC/include $$LIBVTERM_SRC/src $$LIBVTERM_GEN
QMAKE_CFLAGS += -std=c99

# libvterm 0.3.3 reads and writes outside its buffers on some output from the
# server and on resizes, and its mirror is no longer maintained. qmake makes
# patched copies of the affected files, see 3rdparty/patches/libvterm.
LIBVTERM_PATCHES = $$PWD/3rdparty/patches/libvterm
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

# QR codes for host key fingerprints (MIT), compiled into the app
INCLUDEPATH += $$PWD/3rdparty/qrcodegen
SOURCES += $$PWD/3rdparty/qrcodegen/qrcodegen.cpp
HEADERS += $$PWD/3rdparty/qrcodegen/qrcodegen.hpp

# libssh is not available on the device, so it is built from 3rdparty/libssh
# and shipped as a private shared library in /usr/share/$${TARGET}/lib.
# It needs its server code even here: only that hands channels the server
# opens, for agent forwarding and remote port forwards, to the client's
# callbacks.
LIBSSH_SRC = $$PWD/3rdparty/libssh
LIBSSH_BUILD = $$OUT_PWD/libssh-build

libssh.target = $$LIBSSH_BUILD/lib/libssh.so
libssh.commands = cmake -S $$LIBSSH_SRC -B $$LIBSSH_BUILD \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_SERVER=ON -DWITH_GSSAPI=OFF -DWITH_PCAP=OFF \
    -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF \
    -DWITH_DEBUG_CALLTRACE=OFF && \
    cmake --build $$LIBSSH_BUILD --parallel
# These objects include generated libssh headers. One rule each, qmake
# escapes spaces in a target name.
sshsession_libssh.target = sshsession.o
sshsession_libssh.depends = $$libssh.target
keystore_libssh.target = keystore.o
keystore_libssh.depends = $$libssh.target
knownhosts_libssh.target = knownhosts.o
knownhosts_libssh.depends = $$libssh.target
sshagent_libssh.target = sshagent.o
sshagent_libssh.depends = $$libssh.target
sftpengine_libssh.target = sftpengine.o
sftpengine_libssh.depends = $$libssh.target
QMAKE_EXTRA_TARGETS += libssh sshsession_libssh keystore_libssh knownhosts_libssh sshagent_libssh sftpengine_libssh
PRE_TARGETDEPS += $$libssh.target

INCLUDEPATH += $$LIBSSH_SRC/include $$LIBSSH_BUILD/include
LIBS += -L$$LIBSSH_BUILD/lib -lssh
# The forwarded agent signs with OpenSSL directly, libssh has no call for it,
# and mosh encrypts with AES-OCB from it
PKGCONFIG += libcrypto
QMAKE_RPATHDIR += /usr/share/$${TARGET}/lib

libssh_install.path = /usr/share/$${TARGET}/lib
libssh_install.extra = mkdir -p $(INSTALL_ROOT)$$libssh_install.path && \
    cp -P $$LIBSSH_BUILD/lib/libssh.so.* $(INSTALL_ROOT)$$libssh_install.path/
INSTALLS += libssh_install

licenses.path = /usr/share/$${TARGET}/licenses
licenses.extra = mkdir -p $(INSTALL_ROOT)$$licenses.path && \
    cp $$PWD/LICENSE $(INSTALL_ROOT)$$licenses.path/longterm.txt && \
    cp $$LIBSSH_SRC/COPYING $(INSTALL_ROOT)$$licenses.path/libssh.txt && \
    cp $$LIBVTERM_SRC/LICENSE $(INSTALL_ROOT)$$licenses.path/libvterm.txt && \
    cp $$PWD/3rdparty/qrcodegen/LICENSE $(INSTALL_ROOT)$$licenses.path/qrcodegen.txt
INSTALLS += licenses

DISTFILES += qml/longterm.qml \
    3rdparty/patches/libvterm/*.patch \
    qml/components/AgentSummary.qml \
    qml/components/KeyButton.qml \
    qml/components/Keys.js \
    qml/components/StatusDot.qml \
    qml/components/SystemIcon.qml \
    qml/images/systems/* \
    qml/cover/CoverPage.qml \
    qml/pages/AboutPage.qml \
    qml/pages/BackupDialog.qml \
    qml/pages/CertificateDialog.qml \
    qml/pages/ColorSchemesPage.qml \
    qml/pages/DownloadFileDialog.qml \
    qml/pages/EditSessionDialog.qml \
    qml/pages/ExportKeyDialog.qml \
    qml/pages/FilesPage.qml \
    qml/pages/GenerateKeyDialog.qml \
    qml/pages/HistoryPage.qml \
    qml/pages/HostPage.qml \
    qml/pages/ImportHostsDialog.qml \
    qml/pages/ImportKeyDialog.qml \
    qml/pages/ImportSchemeDialog.qml \
    qml/pages/InstallKeyPage.qml \
    qml/pages/KeyPage.qml \
    qml/pages/KeysPage.qml \
    qml/pages/KnownHostPage.qml \
    qml/pages/KnownHostsPage.qml \
    qml/pages/LicensePage.qml \
    qml/pages/LockCodeDialog.qml \
    qml/pages/LockPage.qml \
    qml/pages/RestoreDialog.qml \
    qml/pages/SessionMenuPage.qml \
    qml/pages/SessionPage.qml \
    qml/pages/SessionsPage.qml \
    qml/pages/SettingsPage.qml \
    qml/pages/ShareUploadPage.qml \
    qml/pages/SnippetDialog.qml \
    qml/pages/SnippetPickerPage.qml \
    qml/pages/SnippetsPage.qml \
    qml/pages/TextPage.qml \
    qml/pages/ToolbarKeysPage.qml \
    rpm/longterm.changes \
    rpm/longterm.spec \
    translations/*.ts \
    longterm.desktop \
    fonts/LICENSE.md \
    LICENSE \
    README.md \
    icons/longterm.svg

# Source Code Pro (SIL Open Font License 1.1, see fonts/LICENSE.md) and the
# Nerd Fonts symbols (MIT, icons under their own licenses in fonts/NerdFonts-README.md)
fonts.files = fonts/SourceCodePro-Medium.ttf fonts/SourceCodePro-Bold.ttf fonts/LICENSE.md \
    fonts/SymbolsNerdFontMono-Regular.ttf fonts/NerdFonts-LICENSE.txt fonts/NerdFonts-README.md
fonts.path = /usr/share/$${TARGET}/fonts
INSTALLS += fonts

SAILFISHAPP_ICONS = 86x86 108x108 128x128 172x172

# Keeps translations/longterm.ts up to date as the source for translators.
# Add translations/longterm-<lang>.ts files to TRANSLATIONS to ship them.
CONFIG += sailfishapp_i18n
