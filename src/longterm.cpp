#include <QFontDatabase>
#include <QGuiApplication>
#include <QQuickView>
#include <QtQml>

#include <sailfishapp.h>

#include "appsettings.h"
#include "colorschemes.h"
#include "hoststore.h"
#include "keystore.h"
#include "secretvault.h"
#include "sessionmanager.h"
#include "sshsession.h"
#include "terminal.h"
#include "terminalview.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));

    const int fontId = QFontDatabase::addApplicationFont(
                SailfishApp::pathTo(QStringLiteral("fonts/SourceCodePro-Medium.ttf")).toLocalFile());
    // Same family, so bold cells get the real bold face instead of a synthesized one
    QFontDatabase::addApplicationFont(
                SailfishApp::pathTo(QStringLiteral("fonts/SourceCodePro-Bold.ttf")).toLocalFile());
    const QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
    const QString terminalFontFamily = fontFamilies.isEmpty() ? QStringLiteral("Monospace") : fontFamilies.first();

    qmlRegisterUncreatableType<SshSession>("rs.r8.longterm", 1, 0, "SshSession",
                                           QStringLiteral("Sessions are created by sessionManager"));
    qmlRegisterUncreatableType<Terminal>("rs.r8.longterm", 1, 0, "Terminal",
                                         QStringLiteral("Terminals belong to a session"));
    qmlRegisterType<TerminalView>("rs.r8.longterm", 1, 0, "TerminalView");

    AppSettings appSettings;
    ColorSchemes colorSchemes;
    SecretVault vault;
    KeyStore keyStore(&vault);
    HostStore hostStore(&vault);
    SessionManager sessionManager(&vault, &hostStore);

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    view->rootContext()->setContextProperty(QStringLiteral("appSettings"), &appSettings);
    view->rootContext()->setContextProperty(QStringLiteral("colorSchemes"), &colorSchemes);
    view->rootContext()->setContextProperty(QStringLiteral("hostStore"), &hostStore);
    view->rootContext()->setContextProperty(QStringLiteral("terminalFontFamily"), terminalFontFamily);
    view->rootContext()->setContextProperty(QStringLiteral("keyStore"), &keyStore);
    view->rootContext()->setContextProperty(QStringLiteral("sessionManager"), &sessionManager);
    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
