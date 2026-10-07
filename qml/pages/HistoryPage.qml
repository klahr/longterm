import QtQuick 2.0
import Sailfish.Silica 1.0

// Lines typed in this session, the newest first. A tap types one to edit,
// the menu runs it or keeps it as a snippet.
Page {
    id: page

    property var session
    property Item terminalView

    readonly property var lines: session.history.slice().reverse()

    function send(line, run) {
        terminalView.paste(line)
        if (run)
            terminalView.sendKey(Qt.Key_Return)
        pageStack.pop()
    }

    allowedOrientations: Orientation.All

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: page.lines

        header: PageHeader {
            title: qsTr("History")
            description: session.name.length > 0 ? session.name : session.user + "@" + session.host
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("Nothing typed yet")
            hintText: qsTr("Commands typed in this session show up here")
        }

        delegate: ListItem {
            id: item

            onClicked: page.send(modelData, false)
            menu: ContextMenu {
                MenuItem {
                    text: qsTr("Run")
                    onClicked: page.send(modelData, true)
                }
                MenuItem {
                    text: qsTr("Save as snippet")
                    onClicked: {
                        var snippets = appSettings.snippets
                        snippets.push({ name: "", text: modelData, hostId: session.hostId, run: true })
                        appSettings.snippets = snippets
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                text: modelData
                truncationMode: TruncationMode.Fade
                font.family: terminalFontFamily
                font.pixelSize: Theme.fontSizeSmall
                color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
            }
        }

        VerticalScrollDecorator {}
    }
}
