import QtQuick 2.0
import QtQml 2.2
import QtFeedback 5.0
import Sailfish.Silica 1.0
import Nemo.Notifications 1.0
import Nemo.DBus 2.0
import Sailfish.Share 1.0
import "pages"

ApplicationWindow {
    id: app

    _defaultLabelFormat: Text.PlainText

    // When the app went to the background, 0 while it is in front
    property real backgroundSince: 0

    function lock() {
        if (appSettings.locked || !appSettings.lockEnabled)
            return
        appSettings.locked = true
        var lockPage = pageStack.push(Qt.resolvedUrl("pages/LockPage.qml"), {}, PageStackAction.Immediate)
        lockPage.unlocked.connect(function() {
            appSettings.locked = false
            pageStack.pop(undefined, PageStackAction.Immediate)
        })
    }

    Component.onCompleted: lock()

    Connections {
        target: Qt.application
        onStateChanged: {
            if (Qt.application.state !== Qt.ApplicationActive) {
                if (app.backgroundSince === 0)
                    app.backgroundSince = Date.now()
                // Locking right away keeps the app switcher from showing what is behind
                if (appSettings.lockDelay === 0)
                    app.lock()
                return
            }
            if (app.backgroundSince > 0 && Date.now() - app.backgroundSince >= appSettings.lockDelay * 60000)
                app.lock()
            app.backgroundSince = 0
        }
    }

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

    // replies are maps with label and keys, each a button on the notification
    function notify(session, title, body, replies) {
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
        var actions = [ showAction(session) ]
        for (var i = 0; replies !== undefined && i < replies.length; ++i) {
            actions.push({
                "name": "reply" + i,
                "displayName": replies[i].label,
                "service": "rs.r8.longterm",
                "path": "/rs/r8/longterm",
                "iface": "rs.r8.longterm",
                "method": "reply",
                "arguments": [ String(session), replies[i].keys ]
            })
        }
        notification.remoteActions = actions
        notification.publish()
    }

    function reply(key, keys) {
        // Answering needs the app unlocked, like typing does
        if (appSettings.locked) {
            activate()
            return
        }
        for (var i = 0; i < sessionManager.count; ++i) {
            if (String(sessionManager.sessionAt(i)) === key)
                sessionManager.sessionAt(i).sendInput(keys)
        }
    }

    // Only what the app downloaded itself
    function openLocalFile(path) {
        var downloads = StandardPaths.download
        if (path.indexOf(downloads + "/") === 0 && path.indexOf("/../") < 0)
            Qt.openUrlExternally("file://" + path.split("/").map(encodeURIComponent).join("/"))
    }

    function transferFinished(session, name, upload, localPath, error) {
        var notification = transferNotificationComponent.createObject(app)
        notification.summary = error.length > 0 ? (upload ? qsTr("Upload of %1 failed").arg(name)
                                                          : qsTr("Download of %1 failed").arg(name))
                                                : (upload ? qsTr("Uploaded %1").arg(name) : qsTr("Downloaded %1").arg(name))
        notification.previewSummary = notification.summary
        notification.body = error.length > 0 ? error : localPath
        notification.previewBody = notification.body
        // A download opens in the app for its type, an upload shows its session
        if (!upload && error.length === 0) {
            notification.remoteActions = [ {
                "name": "default",
                "displayName": qsTr("Open"),
                "service": "rs.r8.longterm",
                "path": "/rs/r8/longterm",
                "iface": "rs.r8.longterm",
                "method": "openFile",
                "arguments": [ localPath ]
            } ]
        } else {
            notification.remoteActions = [ showAction(session) ]
        }
        notification.publish()
        // Published notifications stay without it, each transfer has one of its own
        notification.destroy()
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
        if (appSettings.locked)
            return
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

    // Files shared from other apps, see X-Share-Methods in the desktop file
    ShareProvider {
        method: "upload"
        capabilities: [ "*/*" ]

        onTriggered: {
            var paths = []
            for (var i = 0; i < resources.length; ++i) {
                var resource = resources[i]
                var path = resource.filePath !== undefined && resource.filePath.length > 0
                        ? resource.filePath
                        : sessionManager.writeSharedText(resource.name, resource.data)
                if (path.length > 0)
                    paths.push(path)
            }
            app.activate()
            if (paths.length > 0 && !appSettings.locked)
                pageStack.push(Qt.resolvedUrl("pages/ShareUploadPage.qml"), { paths: paths })
        }
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
           + "    <method name=\"reply\">\n"
           + "      <arg name=\"key\" type=\"s\" direction=\"in\"/>\n"
           + "      <arg name=\"keys\" type=\"s\" direction=\"in\"/>\n"
           + "    </method>\n"
           + "    <method name=\"openFile\">\n"
           + "      <arg name=\"path\" type=\"s\" direction=\"in\"/>\n"
           + "    </method>\n"
           + "  </interface>\n"

        function showSession(key) {
            app.showSession(key)
        }

        function reply(key, keys) {
            app.reply(key, keys)
        }

        function openFile(path) {
            app.openLocalFile(path)
        }
    }

    Component {
        id: messageNotificationComponent
        Notification {
            appName: "Longterm"
            appIcon: "longterm"
        }
    }

    Component {
        id: transferNotificationComponent
        Notification {
            appName: "Longterm"
            appIcon: "longterm"
            isTransient: true
        }
    }

    Instantiator {
        model: sessionManager

        delegate: Connections {
            target: model.session.files
            onTransferFinished: {
                // Opening it is what was asked for, a notification would only repeat it
                if (open && error.length === 0)
                    app.openLocalFile(localPath)
                else
                    app.transferFinished(model.session, name, upload, localPath, error)
            }
        }
    }

    Instantiator {
        model: sessionManager

        delegate: Connections {
            target: model.session.terminal
            onBell: app.bell(model.session)
            onNotificationRequested: app.notify(model.session, title, body)
            onQuestionAsked: app.notify(model.session, title, body, replies)
            onClipboardRequested: {
                if (appSettings.remoteClipboard)
                    Clipboard.text = text
            }
        }
    }
}
