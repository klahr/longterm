import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    // Picking for one session offers "App default", as an empty id, and reports
    // the choice instead of changing the app's scheme
    property bool forSession
    property string currentScheme: appSettings.terminalColorScheme

    signal schemePicked(string schemeId)

    function pick(schemeId) {
        if (!forSession) {
            appSettings.terminalColorScheme = schemeId
            return
        }
        schemePicked(schemeId)
        pageStack.pop()
    }

    allowedOrientations: Orientation.All

    SilicaListView {
        anchors.fill: parent
        model: colorSchemes

        PullDownMenu {
            MenuItem {
                text: qsTr("Import scheme")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("ImportSchemeDialog.qml"))
            }
        }

        header: Column {
            width: page.width

            PageHeader {
                title: qsTr("Color scheme")
            }

            ListItem {
                id: defaultItem

                visible: page.forSession
                contentHeight: Theme.itemSizeSmall
                onClicked: page.pick("")

                Label {
                    id: defaultLabel
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.horizontalPageMargin
                    width: parent.width - 3 * Theme.horizontalPageMargin - defaultIcon.width
                    text: qsTr("App default (%1)").arg(colorSchemes.name(appSettings.terminalColorScheme))
                    truncationMode: TruncationMode.Fade
                    color: page.currentScheme.length === 0 || defaultItem.highlighted ? Theme.highlightColor
                                                                                      : Theme.primaryColor
                }

                Icon {
                    id: defaultIcon
                    anchors {
                        right: parent.right
                        rightMargin: Theme.horizontalPageMargin
                        verticalCenter: parent.verticalCenter
                    }
                    visible: page.currentScheme.length === 0
                    source: "image://theme/icon-s-installed"
                }
            }
        }

        delegate: ListItem {
            id: delegate

            readonly property bool current: model.schemeId === page.currentScheme
            readonly property var schemePalette: model.palette

            contentHeight: preview.height + nameLabel.height + 3 * Theme.paddingMedium
            onClicked: page.pick(model.schemeId)
            menu: model.custom ? removeMenu : null

            Component {
                id: removeMenu

                ContextMenu {
                    MenuItem {
                        text: qsTr("Delete")
                        onClicked: {
                            var schemes = colorSchemes
                            var id = model.schemeId
                            delegate.remorseDelete(function() { schemes.removeScheme(id) })
                        }
                    }
                }
            }

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
