import QtQuick 2.0
import QtQml 2.2
import QtFeedback 5.0
import Sailfish.Silica 1.0
import Nemo.Notifications 1.0
import Nemo.DBus 2.0
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
        bellNotification.remoteActions = [ showAction(session) ]
        bellNotification.publish()
    }

    // A notification per session, publishing again replaces the previous message
    property var messageNotifications: ({})

    function notify(session, title, body) {
        var page = pageStack.currentPage
        if (Qt.application.state === Qt.ApplicationActive && page && page.session === session) {
            if (appSettings.bellVibrate)
                bellEffect.play()
            return
        }
        if (!appSettings.bellNotify)
            return
        var key = String(session)
        var notification = messageNotifications[key]
        if (!notification) {
            notification = messageNotificationComponent.createObject(app)
            messageNotifications[key] = notification
        }
        var name = session.name.length > 0 ? session.name : session.user + "@" + session.host
        var text = title.length > 0 && body.length > 0 ? title + ": " + body : title + body
        notification.summary = name
        notification.previewSummary = name
        notification.body = text
        notification.previewBody = text
        notification.remoteActions = [ showAction(session) ]
        notification.publish()
    }

    // Tapping a notification calls showSession over D-Bus
    function showAction(session) {
        return {
            "name": "default",
            "displayName": qsTr("Show"),
            "service": "rs.r8.longterm",
            "path": "/rs/r8/longterm",
            "iface": "rs.r8.longterm",
            "method": "showSession",
            "arguments": [ String(session) ]
        }
    }

    function showSession(key) {
        activate()
        var session = null
        for (var i = 0; i < sessionManager.count; ++i) {
            if (String(sessionManager.sessionAt(i)) === key)
                session = sessionManager.sessionAt(i)
        }
        // Closed since it notified
        if (!session)
            return
        var current = pageStack.currentPage
        if (current && current.showSession !== undefined && current.connectToHost === undefined) {
            current.showSession(session)
            return
        }
        var sessionsPage = pageStack.find(function(page) { return page.connectToHost !== undefined })
        if (!sessionsPage)
            return
        if (current !== sessionsPage)
            pageStack.pop(sessionsPage, PageStackAction.Immediate)
        sessionsPage.showSession(session)
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

    DBusAdaptor {
        service: "rs.r8.longterm"
        iface: "rs.r8.longterm"
        path: "/rs/r8/longterm"
        xml: "  <interface name=\"rs.r8.longterm\">\n"
           + "    <method name=\"showSession\">\n"
           + "      <arg name=\"key\" type=\"s\" direction=\"in\"/>\n"
           + "    </method>\n"
           + "  </interface>\n"

        function showSession(key) {
            app.showSession(key)
        }
    }

    Component {
        id: messageNotificationComponent
        Notification {
            appName: "Longterm"
            appIcon: "longterm"
        }
    }

    Instantiator {
        model: sessionManager

        delegate: Connections {
            target: model.session.terminal
            onBell: app.bell(model.session)
            onNotificationRequested: app.notify(model.session, title, body)
            onClipboardRequested: {
                if (appSettings.remoteClipboard)
                    Clipboard.text = text
            }
        }
    }
}
