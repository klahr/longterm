import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    readonly property int defaultFontSize: Theme.fontSizeExtraSmall
    property string hostsMessage

    Connections {
        target: backup
        onExported: page.hostsMessage = error.length > 0 ? error : qsTr("Saved the backup to %1").arg(path)
        onImported: page.hostsMessage = error.length > 0 ? error : summary
    }

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

            ComboBox {
                width: parent.width
                label: qsTr("Font")
                description: qsTr("Symbols for prompts such as starship come from Nerd Fonts whatever the font")
                currentIndex: Math.max(0, terminalFonts.indexOf(appSettings.terminalFontFamily.length > 0
                                                                ? appSettings.terminalFontFamily : terminalFontFamily))
                menu: ContextMenu {
                    Repeater {
                        model: terminalFonts
                        MenuItem {
                            text: modelData
                            font.family: modelData
                        }
                    }
                }
                onCurrentIndexChanged: appSettings.terminalFontFamily = currentIndex > 0 ? terminalFonts[currentIndex] : ""
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
                text: qsTr("Toolbar at the top")
                description: qsTr("Instead of above the keyboard")
                automaticCheck: false
                checked: appSettings.toolbarAtTop
                onClicked: appSettings.toolbarAtTop = !checked
            }

            TextSwitch {
                text: qsTr("Connection name in landscape")
                description: qsTr("Turn off for one more row of terminal when the phone is on its side")
                automaticCheck: false
                checked: appSettings.nameBarInLandscape
                onClicked: appSettings.nameBarInLandscape = !checked
            }

            ValueButton {
                label: qsTr("Snippets")
                value: appSettings.snippets.length
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("SnippetsPage.qml"))
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

            TextSwitch {
                text: qsTr("Show typing ahead over mosh")
                description: qsTr("On slow connections, typed text shows underlined until the server echoes it")
                automaticCheck: false
                checked: appSettings.moshPrediction
                onClicked: appSettings.moshPrediction = !checked
            }

            ValueButton {
                label: qsTr("Known hosts")
                value: knownHosts.count
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("KnownHostsPage.qml"))
            }

            SectionHeader {
                text: qsTr("Security")
            }

            TextSwitch {
                text: qsTr("Lock the app")
                description: qsTr("Asks for a code before showing connections, also after a while in the background")
                automaticCheck: false
                checked: appSettings.lockEnabled
                onClicked: {
                    if (checked)
                        appSettings.setLockCode("")
                    else
                        pageStack.animatorPush(Qt.resolvedUrl("LockCodeDialog.qml"))
                }
            }

            ComboBox {
                width: parent.width
                visible: appSettings.lockEnabled
                label: qsTr("Lock after")
                readonly property var minutes: [0, 1, 5, 15, 60]
                currentIndex: Math.max(0, minutes.indexOf(appSettings.lockDelay))
                menu: ContextMenu {
                    MenuItem { text: qsTr("Leaving the app") }
                    MenuItem { text: qsTr("1 minute") }
                    MenuItem { text: qsTr("5 minutes") }
                    MenuItem { text: qsTr("15 minutes") }
                    MenuItem { text: qsTr("1 hour") }
                }
                onCurrentIndexChanged: appSettings.lockDelay = minutes[currentIndex]
            }

            SectionHeader {
                text: qsTr("Hosts")
            }

            ValueButton {
                label: qsTr("Copy hosts as ssh_config")
                enabled: hostStore.count > 0
                onClicked: {
                    Clipboard.text = hostStore.exportConfig()
                    page.hostsMessage = hostStore.count === 1 ? qsTr("Copied 1 host to the clipboard")
                                                              : qsTr("Copied %1 hosts to the clipboard").arg(hostStore.count)
                }
            }

            ValueButton {
                label: qsTr("Import from ssh_config")
                onClicked: {
                    var dialog = pageStack.push(Qt.resolvedUrl("ImportHostsDialog.qml"))
                    dialog.accepted.connect(function() {
                        page.hostsMessage = dialog.imported === 1 ? qsTr("Read 1 host")
                                                                   : qsTr("Read %1 hosts").arg(dialog.imported)
                    })
                }
            }

            ValueButton {
                label: qsTr("Back up everything")
                description: qsTr("Hosts, keys, saved passwords, snippets and settings in one encrypted file")
                enabled: !backup.busy
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("BackupDialog.qml"))
            }

            ValueButton {
                label: qsTr("Restore a backup")
                enabled: !backup.busy
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("RestoreDialog.qml"))
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
