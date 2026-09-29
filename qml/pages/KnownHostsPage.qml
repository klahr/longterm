import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    allowedOrientations: Orientation.All

    Component.onCompleted: knownHosts.reload()

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: knownHosts

        header: PageHeader {
            title: qsTr("Known hosts")
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No known hosts")
            hintText: qsTr("Host keys are saved the first time you connect")
        }

        delegate: ListItem {
            id: delegate

            contentHeight: Theme.itemSizeMedium
            menu: Component {
                ContextMenu {
                    MenuItem {
                        text: qsTr("Delete")
                        onClicked: {
                            // The delegate's context is gone when the remorse timer fires
                            var hosts = knownHosts
                            var line = model.line
                            delegate.remorseDelete(function() { hosts.remove(line) })
                        }
                    }
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin

                Label {
                    width: parent.width
                    text: model.hosts
                    truncationMode: TruncationMode.Fade
                    color: delegate.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    text: model.keyType + " " + model.fingerprint
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: delegate.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
