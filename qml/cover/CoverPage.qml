import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0

CoverBackground {
    id: cover

    readonly property int maxVisible: Math.max(1, Math.floor((height - header.height - 2 * Theme.paddingLarge)
                                                             / (Theme.fontSizeSmall + Theme.paddingMedium)) - 1)

    Column {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.paddingLarge
        visible: sessionManager.count === 0
        spacing: Theme.paddingSmall

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Longterm")
            color: Theme.highlightColor
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("No connections")
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
        }
    }

    Column {
        x: Theme.paddingLarge
        y: Theme.paddingLarge
        width: parent.width - 2 * Theme.paddingLarge
        visible: sessionManager.count > 0
        spacing: Theme.paddingMedium

        Label {
            id: header
            width: parent.width
            text: sessionManager.count === 1 ? qsTr("1 connection")
                                             : qsTr("%1 connections").arg(sessionManager.count)
            truncationMode: TruncationMode.Fade
            color: Theme.highlightColor
        }

        Repeater {
            model: sessionManager

            Row {
                readonly property var session: model.session

                visible: index < cover.maxVisible
                width: parent.width
                spacing: Theme.paddingSmall

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.paddingMedium
                    height: width
                    radius: width / 2
                    color: session.state === SshSession.Connected ? Theme.highlightColor
                         : session.state === SshSession.Connecting ? Theme.secondaryHighlightColor
                         : session.errorString.length > 0 ? Theme.errorColor
                         : Theme.secondaryColor
                    opacity: session.state === SshSession.Connecting ? 0.6 : 1.0
                }

                Label {
                    width: parent.width - Theme.paddingMedium - parent.spacing
                    text: session.name.length > 0 ? session.name : session.user + "@" + session.host
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeSmall
                    color: session.state === SshSession.Connected ? Theme.primaryColor : Theme.secondaryColor
                }
            }
        }

        Label {
            visible: sessionManager.count > cover.maxVisible
            width: parent.width
            text: qsTr("+%1 more").arg(sessionManager.count - cover.maxVisible)
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryColor
        }
    }
}
