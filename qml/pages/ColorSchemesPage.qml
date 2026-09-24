import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    allowedOrientations: Orientation.All

    SilicaListView {
        anchors.fill: parent
        model: colorSchemes

        header: PageHeader {
            title: qsTr("Color scheme")
        }

        delegate: ListItem {
            id: delegate

            readonly property bool current: model.schemeId === appSettings.terminalColorScheme
            readonly property var schemePalette: model.palette

            contentHeight: preview.height + nameLabel.height + 3 * Theme.paddingMedium
            onClicked: appSettings.terminalColorScheme = model.schemeId

            Label {
                id: nameLabel
                anchors {
                    top: parent.top
                    topMargin: Theme.paddingMedium
                }
                x: Theme.horizontalPageMargin
                text: model.name
                color: delegate.current || delegate.highlighted ? Theme.highlightColor : Theme.primaryColor
            }

            Icon {
                anchors {
                    right: parent.right
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: nameLabel.verticalCenter
                }
                visible: delegate.current
                source: "image://theme/icon-s-installed"
            }

            Rectangle {
                id: preview

                anchors {
                    top: nameLabel.bottom
                    topMargin: Theme.paddingMedium
                }
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                height: previewColumn.height + 2 * Theme.paddingMedium
                radius: Theme.paddingSmall
                color: model.background

                Column {
                    id: previewColumn

                    x: Theme.paddingMedium
                    y: Theme.paddingMedium
                    width: parent.width - 2 * Theme.paddingMedium
                    spacing: Theme.paddingSmall

                    Text {
                        text: "user@host:~$ ls --color"
                        color: model.foreground
                        font.family: "Monospace"
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }

                    Row {
                        Repeater {
                            model: 16

                            Rectangle {
                                width: previewColumn.width / 16
                                height: Theme.paddingLarge
                                color: delegate.schemePalette[index]
                            }
                        }
                    }
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
