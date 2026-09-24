import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0

Page {
    id: page

    allowedOrientations: Orientation.All

    function connectToHost(hostId, needsPassword) {
        if (needsPassword) {
            pageStack.animatorPush(Qt.resolvedUrl("HostPage.qml"), { hostId: hostId })
            return
        }
        var session = sessionManager.openHost(hostId)
        if (session)
            pageStack.animatorPush(Qt.resolvedUrl("SessionPage.qml"), { session: session })
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
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
                                text: qsTr("Close")
                                onClicked: {
                                    // The delegate is gone by the time the remorse timer fires
                                    var session = sessionItem.session
                                    sessionItem.remorseDelete(function() { sessionManager.closeSession(session) })
                                }
                            }
                        }
                    }
                    onClicked: pageStack.animatorPush(Qt.resolvedUrl("SessionPage.qml"), { session: session })

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin

                        Label {
                            width: parent.width
                            text: session.name.length > 0 ? session.name : session.user + "@" + session.host
                            truncationMode: TruncationMode.Fade
                            color: sessionItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                        }
                        Label {
                            width: parent.width
                            text: {
                                switch (session.state) {
                                case SshSession.Connecting: return qsTr("Connecting")
                                case SshSession.Connected: return qsTr("Connected")
                                default: return session.errorString.length > 0 ? session.errorString : qsTr("Disconnected")
                                }
                            }
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: sessionItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                        }
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
                                    var hostId = model.hostId
                                    hostItem.remorseDelete(function() {
                                        hostStore.removeHost(hostId)
                                    })
                                }
                            }
                        }
                    }
                    onClicked: page.connectToHost(model.hostId, model.keyId.length === 0 && !model.hasPassword)

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin

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
