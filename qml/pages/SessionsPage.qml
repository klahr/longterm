import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0
import "../components"

Page {
    id: page

    allowedOrientations: Orientation.All

    // Hosts without a key or saved password ask for it in the session
    function connectToHost(hostId) {
        var session = sessionManager.openHost(hostId)
        if (session)
            showSession(session)
    }

    function showSession(session) {
        updateAttachedPage()
        pageStack.nextPage(page).showSession(session)
        pageStack.navigateForward()
    }

    // The sessions sit to the right of this page, so swiping forward leads to the first of them
    function updateAttachedPage() {
        var attached = pageStack.nextPage(page)
        if (sessionManager.count === 0) {
            if (attached)
                pageStack.popAttached()
        } else if (attached) {
            attached.showFirstSession()
        } else {
            pageStack.pushAttached(Qt.resolvedUrl("SessionPage.qml"))
        }
    }

    onStatusChanged: {
        if (status === PageStatus.Active)
            updateAttachedPage()
    }

    Connections {
        target: sessionManager
        onCountChanged: {
            if (page.status === PageStatus.Active)
                page.updateAttachedPage()
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("About")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("AboutPage.qml"))
            }
            MenuItem {
                text: qsTr("Settings")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("SettingsPage.qml"))
            }
            MenuItem {
                text: qsTr("Keys")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("KeysPage.qml"))
            }
            MenuItem {
                text: qsTr("New host")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("HostPage.qml"))
            }
        }

        ViewPlaceholder {
            enabled: sessionManager.count === 0 && hostStore.count === 0
            text: qsTr("No hosts")
            hintText: qsTr("Pull down to add a host")
        }

        Column {
            id: column

            width: parent.width

            PageHeader {
                title: qsTr("Longterm")
                description: hostStore.errorString
            }

            SectionHeader {
                text: qsTr("Connections")
                visible: sessionManager.count > 0
            }

            Repeater {
                model: sessionManager

                ListItem {
                    id: sessionItem

                    readonly property var session: model.session

                    width: column.width
                    contentHeight: Theme.itemSizeMedium
                    menu: Component {
                        ContextMenu {
                            MenuItem {
                                visible: sessionItem.session.state === SshSession.Disconnected
                                         && !sessionItem.session.hostKeyMismatch
                                text: qsTr("Reconnect")
                                onClicked: sessionItem.session.reconnect()
                            }
                            MenuItem {
                                visible: sessionItem.session.state !== SshSession.Disconnected
                                text: qsTr("Disconnect")
                                onClicked: sessionItem.session.disconnectFromHost()
                            }
                            MenuItem {
                                text: qsTr("Edit")
                                onClicked: pageStack.animatorPush(Qt.resolvedUrl("EditSessionDialog.qml"),
                                                                  { session: sessionItem.session })
                            }
                            MenuItem {
                                text: qsTr("Remove")
                                onClicked: {
                                    // The delegate and its context are gone by the time the remorse
                                    // timer fires, so capture everything the callback needs
                                    var manager = sessionManager
                                    var session = sessionItem.session
                                    sessionItem.remorseDelete(function() { manager.closeSession(session) })
                                }
                            }
                        }
                    }
                    onClicked: {
                        if (session.state === SshSession.Disconnected && !session.hostKeyMismatch)
                            session.reconnect()
                        page.showSession(session)
                    }

                    SystemIcon {
                        id: sessionIcon
                        x: Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        systemId: sessionItem.session.systemId
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        x: sessionIcon.x + sessionIcon.width + Theme.paddingMedium
                        width: thumbnail.x - x - Theme.paddingMedium

                        Row {
                            width: parent.width
                            spacing: Theme.paddingMedium

                            StatusDot {
                                anchors.verticalCenter: parent.verticalCenter
                                session: sessionItem.session
                            }

                            Label {
                                width: parent.width - Theme.paddingMedium - parent.spacing
                                text: session.name.length > 0 ? session.name : session.user + "@" + session.host
                                truncationMode: TruncationMode.Fade
                                color: sessionItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                            }
                        }
                        Label {
                            x: Theme.paddingMedium + Theme.paddingMedium
                            width: parent.width - x
                            text: {
                                switch (session.state) {
                                case SshSession.Connecting: return qsTr("Connecting")
                                case SshSession.Connected: return qsTr("Connected")
                                default: return session.errorString.length > 0 ? session.errorString : qsTr("Disconnected")
                                }
                            }
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: session.state === SshSession.Connected ? "#4caf50"
                                 : sessionItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                        }
                    }

                    // What the session shows right now, in miniature
                    TerminalView {
                        id: thumbnail

                        anchors {
                            right: parent.right
                            rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        height: sessionItem.contentHeight - 2 * Theme.paddingSmall
                        // Shaped like the device held upright, as the session is usually seen
                        width: Math.round(height * Math.min(Screen.width, Screen.height) / Math.max(Screen.width, Screen.height))
                        preview: true
                        radius: Theme.paddingSmall
                        terminal: sessionItem.session.terminal
                        fontFamily: terminalFontFamily
                        fontPixelSize: appSettings.terminalFontSize > 0 ? appSettings.terminalFontSize : Theme.fontSizeExtraSmall
                        colorScheme: sessionItem.session.colorScheme.length > 0 ? sessionItem.session.colorScheme
                                                                                : appSettings.terminalColorScheme
                        opacity: sessionItem.session.state === SshSession.Connected ? 1.0 : 0.5
                    }
                }
            }

            SectionHeader {
                text: qsTr("Hosts")
                visible: hostStore.count > 0
            }

            Repeater {
                model: hostStore

                ListItem {
                    id: hostItem

                    width: column.width
                    contentHeight: Theme.itemSizeMedium
                    menu: Component {
                        ContextMenu {
                            MenuItem {
                                text: qsTr("Edit")
                                onClicked: pageStack.animatorPush(Qt.resolvedUrl("HostPage.qml"),
                                                                  { hostId: model.hostId })
                            }
                            MenuItem {
                                text: qsTr("Delete")
                                onClicked: {
                                    var store = hostStore
                                    var hostId = model.hostId
                                    hostItem.remorseDelete(function() { store.removeHost(hostId) })
                                }
                            }
                        }
                    }
                    onClicked: page.connectToHost(model.hostId)

                    SystemIcon {
                        id: hostIcon
                        x: Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        systemId: model.systemId
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        x: hostIcon.x + hostIcon.width + Theme.paddingMedium
                        width: parent.width - x - Theme.horizontalPageMargin

                        Label {
                            width: parent.width
                            text: model.name
                            truncationMode: TruncationMode.Fade
                            color: hostItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                        }
                        Label {
                            width: parent.width
                            text: model.user + "@" + model.address + (model.port !== 22 ? ":" + model.port : "")
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: hostItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                        }
                    }
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
