import QtQuick 2.0
import Sailfish.Silica 1.0

// Snippets for the session's host and for all hosts, a tap types one
Page {
    id: page

    property var session
    property Item terminalView

    readonly property var snippets: {
        var list = []
        var all = appSettings.snippets
        for (var i = 0; i < all.length; ++i) {
            if (all[i].hostId.length === 0 || all[i].hostId === session.hostId)
                list.push({ index: i, name: all[i].name, text: all[i].text, run: all[i].run, hostId: all[i].hostId })
        }
        return list
    }

    function send(snippet) {
        // Bracketed, so text with several lines is not run line by line
        terminalView.paste(snippet.text)
        if (snippet.run)
            terminalView.sendKey(Qt.Key_Return)
        pageStack.pop()
    }

    allowedOrientations: Orientation.All

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: page.snippets

        PullDownMenu {
            MenuItem {
                text: qsTr("Manage snippets")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("SnippetsPage.qml"))
            }
            MenuItem {
                text: qsTr("New snippet")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("SnippetDialog.qml"), { hostId: session.hostId })
            }
        }

        header: PageHeader {
            title: qsTr("Snippets")
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No snippets")
            hintText: qsTr("Pull down to add commands you type often")
        }

        delegate: ListItem {
            id: item

            contentHeight: Theme.itemSizeMedium
            onClicked: page.send(modelData)

            Column {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter

                Label {
                    width: parent.width
                    text: modelData.name.length > 0 ? modelData.name : modelData.text
                    truncationMode: TruncationMode.Fade
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                Label {
                    width: parent.width
                    text: (modelData.run ? "⏎ " : "") + modelData.text.replace(/\n/g, " ↵ ")
                    truncationMode: TruncationMode.Fade
                    font.family: terminalFontFamily
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryColor
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
