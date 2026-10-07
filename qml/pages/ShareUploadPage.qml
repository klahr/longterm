import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0

// Where files shared from another app go: pick a connection, then the folder
Page {
    id: page

    property var paths: []

    allowedOrientations: Orientation.All

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: sessionManager

        header: Column {
            width: listView.width

            PageHeader {
                title: qsTr("Upload %n file(s)", "", page.paths.length)
                description: qsTr("Choose the connection to upload to")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: page.paths.map(function(path) { return path.split("/").pop() }).join(", ")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
                bottomPadding: Theme.paddingLarge
            }
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No connections")
            hintText: qsTr("Connect to a host first, then share again")
        }

        delegate: ListItem {
            id: item

            readonly property var session: model.session

            contentHeight: Theme.itemSizeSmall
            onClicked: {
                if (session.state === SshSession.Disconnected && !session.hostKeyMismatch)
                    session.reconnect()
                pageStack.replace(Qt.resolvedUrl("FilesPage.qml"), { session: session, pendingUploads: page.paths })
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                text: item.session.name.length > 0 ? item.session.name : item.session.user + "@" + item.session.host
                truncationMode: TruncationMode.Fade
                color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
            }
        }

        VerticalScrollDecorator {}
    }
}
