#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQuickView>
#include <QtQml>

#include <sailfishapp.h>

#include "appsettings.h"
#include "backup.h"
#include "colorschemes.h"
#include "hostlist.h"
#include "hoststore.h"
#include "keystore.h"
#include "qrimageprovider.h"
#include "knownhosts.h"
#include "secretvault.h"
#include "sessionfilter.h"
#include "sessionmanager.h"
#include "sftpbrowser.h"
#include "sshsession.h"
#include "terminal.h"
#include "terminalview.h"
#include "version.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    app->setApplicationVersion(QStringLiteral(APP_VERSION));

    const int fontId = QFontDatabase::addApplicationFont(
                SailfishApp::pathTo(QStringLiteral("fonts/SourceCodePro-Medium.ttf")).toLocalFile());
    // Same family, so bold cells get the real bold face instead of a synthesized one
    QFontDatabase::addApplicationFont(
                SailfishApp::pathTo(QStringLiteral("fonts/SourceCodePro-Bold.ttf")).toLocalFile());
    const QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
    const QString terminalFontFamily = fontFamilies.isEmpty() ? QStringLiteral("Monospace") : fontFamilies.first();
    // Icons for prompts such as starship and powerlevel10k, used where the
    // terminal font has no glyph of its own
    const QStringList symbolFamilies = QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFont(
                SailfishApp::pathTo(QStringLiteral("fonts/SymbolsNerdFontMono-Regular.ttf")).toLocalFile()));
    // Fonts with cells of one width, the bundled one first
    QStringList terminalFonts { terminalFontFamily };
    const QFontDatabase fontDatabase;
    for (const QString &family : fontDatabase.families()) {
        if (fontDatabase.isFixedPitch(family) && !terminalFonts.contains(family) && !symbolFamilies.contains(family))
            terminalFonts.append(family);
    }
    if (!symbolFamilies.isEmpty()) {
        for (const QString &family : terminalFonts)
            QFont::insertSubstitution(family, symbolFamilies.first());
    }

    qmlRegisterUncreatableType<SshSession>("rs.r8.longterm", 1, 0, "SshSession",
                                           QStringLiteral("Sessions are created by sessionManager"));
    qmlRegisterUncreatableType<Terminal>("rs.r8.longterm", 1, 0, "Terminal",
                                         QStringLiteral("Terminals belong to a session"));
    qmlRegisterUncreatableType<SftpBrowser>("rs.r8.longterm", 1, 0, "SftpBrowser",
                                            QStringLiteral("File browsers belong to a session"));
    qmlRegisterType<TerminalView>("rs.r8.longterm", 1, 0, "TerminalView");
    qmlRegisterType<SessionFilter>("rs.r8.longterm", 1, 0, "SessionFilter");

    AppSettings appSettings;
    ColorSchemes colorSchemes;
    SecretVault vault;
    KeyStore keyStore(&vault);
    HostStore hostStore(&vault);
    HostList hostList;
    hostList.setSource(&hostStore);
    QObject::connect(&keyStore, &KeyStore::keyRemoved, &hostStore, &HostStore::forgetKey);
    SessionManager sessionManager(&vault, &hostStore, &keyStore, &appSettings);
    KnownHosts knownHosts;
    Backup backup(&vault, &hostStore, &keyStore, &appSettings, &colorSchemes);

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    view->engine()->addImageProvider(QStringLiteral("qr"), new QrImageProvider);
    view->rootContext()->setContextProperty(QStringLiteral("appSettings"), &appSettings);
    view->rootContext()->setContextProperty(QStringLiteral("colorSchemes"), &colorSchemes);
    view->rootContext()->setContextProperty(QStringLiteral("hostStore"), &hostStore);
    view->rootContext()->setContextProperty(QStringLiteral("hostList"), &hostList);
    view->rootContext()->setContextProperty(QStringLiteral("terminalFontFamily"), terminalFontFamily);
    view->rootContext()->setContextProperty(QStringLiteral("terminalFonts"), terminalFonts);
    view->rootContext()->setContextProperty(QStringLiteral("keyStore"), &keyStore);
    view->rootContext()->setContextProperty(QStringLiteral("knownHosts"), &knownHosts);
    view->rootContext()->setContextProperty(QStringLiteral("backup"), &backup);
    view->rootContext()->setContextProperty(QStringLiteral("sessionManager"), &sessionManager);
    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
