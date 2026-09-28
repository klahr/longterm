import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    readonly property int defaultFontSize: Theme.fontSizeExtraSmall
    property string hostsMessage

    allowedOrientations: Orientation.All

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: page.width

            PageHeader {
                title: qsTr("Settings")
            }

            SectionHeader {
                text: qsTr("Terminal")
            }

            ValueButton {
                label: qsTr("Color scheme")
                value: colorSchemes.name(appSettings.terminalColorScheme)
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("ColorSchemesPage.qml"))
            }

            Slider {
                width: parent.width
                label: qsTr("Font size")
                minimumValue: Math.round(Theme.fontSizeTiny / 2)
                maximumValue: Theme.fontSizeHuge
                stepSize: 1
                value: appSettings.terminalFontSize > 0 ? appSettings.terminalFontSize : page.defaultFontSize
                valueText: value === page.defaultFontSize ? qsTr("%1 px (default)").arg(value) : qsTr("%1 px").arg(value)
                onDownChanged: {
                    if (!down)
                        appSettings.terminalFontSize = value === page.defaultFontSize ? 0 : value
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("You can also pinch the terminal to change the font size.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }

            ValueButton {
                label: qsTr("Toolbar keys")
                value: appSettings.toolbarKeys.length
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("ToolbarKeysPage.qml"))
            }

            TextSwitch {
                text: qsTr("Vibrate on bell")
                automaticCheck: false
                checked: appSettings.bellVibrate
                onClicked: appSettings.bellVibrate = !checked
            }

            TextSwitch {
                text: qsTr("Notify on bell")
                description: qsTr("When the session ringing is not on screen")
                automaticCheck: false
                checked: appSettings.bellNotify
                onClicked: appSettings.bellNotify = !checked
            }

            TextSwitch {
                text: qsTr("Let servers set the clipboard")
                description: qsTr("Programs such as tmux and vim can copy to the device clipboard (OSC 52). "
                                  + "They cannot read it.")
                automaticCheck: false
                checked: appSettings.remoteClipboard
                onClicked: appSettings.remoteClipboard = !checked
            }

            SectionHeader {
                text: qsTr("Connections")
            }

            TextSwitch {
                text: qsTr("Reconnect automatically")
                description: qsTr("After the network drops or changes, for example between WLAN and mobile data. "
                                  + "Programs on the server keep running only inside tmux or screen.")
                automaticCheck: false
                checked: appSettings.autoReconnect
                onClicked: appSettings.autoReconnect = !checked
            }

            ValueButton {
                label: qsTr("Known hosts")
                value: knownHosts.count
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("KnownHostsPage.qml"))
            }

            SectionHeader {
                text: qsTr("Hosts")
            }

            ValueButton {
                label: qsTr("Copy hosts as ssh_config")
                enabled: hostStore.count > 0
                onClicked: {
                    Clipboard.text = hostStore.exportConfig()
                    page.hostsMessage = qsTr("Copied %n host(s) to the clipboard", "", hostStore.count)
                }
            }

            ValueButton {
                label: qsTr("Import from ssh_config")
                onClicked: {
                    var dialog = pageStack.push(Qt.resolvedUrl("ImportHostsDialog.qml"))
                    dialog.accepted.connect(function() {
                        page.hostsMessage = qsTr("Read %n host(s)", "", dialog.imported)
                    })
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: page.hostsMessage.length > 0
                text: page.hostsMessage
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }
        }

        VerticalScrollDecorator {}
    }
}
