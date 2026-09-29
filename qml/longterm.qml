import QtQuick 2.0
import QtQml 2.2
import QtFeedback 5.0
import Sailfish.Silica 1.0
import Nemo.Notifications 1.0
import "pages"

ApplicationWindow {
    id: app

    _defaultLabelFormat: Text.PlainText

    // When each session last notified, so a stream of bells makes one notification
    property var lastBellNotification: ({})

    function bell(session) {
        var page = pageStack.currentPage
        if (Qt.application.state === Qt.ApplicationActive && page && page.session === session) {
            if (appSettings.bellVibrate)
                bellEffect.play()
            return
        }
        if (!appSettings.bellNotify)
            return
        var key = String(session)
        var now = Date.now()
        if (lastBellNotification[key] !== undefined && now - lastBellNotification[key] < 10000)
            return
        lastBellNotification[key] = now
        var name = session.name.length > 0 ? session.name : session.user + "@" + session.host
        bellNotification.summary = name
        bellNotification.previewSummary = name
        bellNotification.body = qsTr("Terminal bell")
        bellNotification.previewBody = bellNotification.body
        bellNotification.publish()
    }

    initialPage: Component { SessionsPage { } }
    cover: Qt.resolvedUrl("cover/CoverPage.qml")
    allowedOrientations: defaultAllowedOrientations

    ThemeEffect {
        id: bellEffect
        effect: ThemeEffect.PressStrong
    }

    Notification {
        id: bellNotification
        appName: "Longterm"
        appIcon: "longterm"
    }

    Instantiator {
        model: sessionManager

        delegate: Connections {
            target: model.session.terminal
            onBell: app.bell(model.session)
            onClipboardRequested: {
                if (appSettings.remoteClipboard)
                    Clipboard.text = text
            }
        }
    }
}
