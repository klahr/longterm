import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0
import "../components"

CoverBackground {
    id: cover

    readonly property int maxVisible: Math.max(1, Math.floor((height - header.height - 2 * Theme.paddingLarge
                                                              - (sessionManager.workingCount + sessionManager.waitingCount
                                                                 + sessionManager.doneCount > 0 ? Theme.fontSizeSmall : 0))
                                                             / (Theme.fontSizeSmall + Theme.paddingMedium)) - 1)

    Column {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.paddingLarge
        visible: sessionManager.count === 0 || appSettings.locked
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
            text: appSettings.locked ? qsTr("Locked") : qsTr("No connections")
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
        }
    }

    Column {
        x: Theme.paddingLarge
        y: Theme.paddingLarge
        width: parent.width - 2 * Theme.paddingLarge
        visible: sessionManager.count > 0 && !appSettings.locked
        spacing: Theme.paddingMedium

        Label {
            id: header
            width: parent.width
            text: sessionManager.count === 1 ? qsTr("1 connection")
                                             : qsTr("%1 connections").arg(sessionManager.count)
            truncationMode: TruncationMode.Fade
            color: Theme.highlightColor
        }

        AgentSummary {
            width: parent.width
            font.pixelSize: Theme.fontSizeExtraSmall
        }

        Repeater {
            model: sessionManager

            Row {
                readonly property var session: model.session

                visible: index < cover.maxVisible
                width: parent.width
                spacing: Theme.paddingSmall

                StatusDot {
                    anchors.verticalCenter: parent.verticalCenter
                    session: parent.session
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
