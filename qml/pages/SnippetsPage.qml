import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    allowedOrientations: Orientation.All

    function hostName(hostId) {
        if (hostId.length === 0)
            return qsTr("All hosts")
        var host = hostStore.host(hostId)
        return host.name !== undefined ? host.name : qsTr("Deleted host")
    }

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: appSettings.snippets

        PullDownMenu {
            MenuItem {
                text: qsTr("New snippet")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("SnippetDialog.qml"))
            }
        }

        header: PageHeader {
            title: qsTr("Snippets")
            description: qsTr("Typed from the Snip key or the session menu")
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No snippets")
            hintText: qsTr("Pull down to add one")
        }

        delegate: ListItem {
            id: item

            contentHeight: Theme.itemSizeMedium
            onClicked: pageStack.animatorPush(Qt.resolvedUrl("SnippetDialog.qml"), { index: index })
            menu: ContextMenu {
                MenuItem {
                    text: qsTr("Delete")
                    onClicked: {
                        var position = index
                        item.remorseDelete(function() {
                            var snippets = appSettings.snippets
                            snippets.splice(position, 1)
                            appSettings.snippets = snippets
                        })
                    }
                }
            }

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
                    text: page.hostName(modelData.hostId)
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryColor
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
