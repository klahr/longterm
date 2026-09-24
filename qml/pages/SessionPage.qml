import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0

Page {
    id: page

    property var session

    function showKeyboard() {
        terminalView.forceActiveFocus()
        Qt.inputMethod.show()
    }

    allowedOrientations: Orientation.All

    onStatusChanged: {
        if (status === PageStatus.Active)
            showKeyboard()
    }

    // The session is removed when its shell exits, so leave before it goes away
    Connections {
        target: session
        onShellExited: pageStack.pop(pageStack.previousPage(page))
    }

    // Keep the keyboard open while the session is on screen
    Connections {
        target: Qt.inputMethod
        onVisibleChanged: {
            if (!Qt.inputMethod.visible && page.status === PageStatus.Active)
                keyboardRestoreTimer.restart()
        }
    }

    Timer {
        id: keyboardRestoreTimer
        interval: 300
        onTriggered: {
            if (page.status === PageStatus.Active && !Qt.inputMethod.visible)
                page.showKeyboard()
        }
    }

    TerminalView {
        id: terminalView

        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: keyBar.top
        }
        terminal: session.terminal
        fontFamily: terminalFontFamily
        fontPixelSize: appSettings.terminalFontSize > 0 ? appSettings.terminalFontSize : Theme.fontSizeExtraSmall
        colorScheme: appSettings.terminalColorScheme

        PinchArea {
            property int startSize

            anchors.fill: parent
            onPinchStarted: startSize = terminalView.fontPixelSize
            onPinchUpdated: {
                var size = Math.round(startSize * pinch.scale)
                appSettings.terminalFontSize = Math.max(Theme.fontSizeTiny / 2, Math.min(Theme.fontSizeHuge, size))
            }

            MouseArea {
                property real startY
                property int startOffset
                property bool dragged
                property bool selecting

                anchors.fill: parent
                // Keep the page from taking a selection drag as a back swipe
                preventStealing: selecting
                onPressed: {
                    startY = mouse.y
                    startOffset = terminalView.scrollOffset
                    dragged = false
                }
                onPressAndHold: {
                    if (dragged)
                        return
                    selecting = true
                    terminalView.startSelection(mouse.x, mouse.y)
                }
                onPositionChanged: {
                    if (selecting) {
                        terminalView.updateSelection(mouse.x, mouse.y)
                        return
                    }
                    var lines = Math.round((mouse.y - startY) / terminalView.cellHeight)
                    if (lines !== 0)
                        dragged = true
                    terminalView.scrollOffset = startOffset + lines
                }
                onDoubleClicked: terminalView.selectWordAt(mouse.x, mouse.y)
                onReleased: selecting = false
                onCanceled: selecting = false
                onClicked: {
                    if (dragged)
                        return
                    if (terminalView.hasSelection) {
                        terminalView.clearSelection()
                        return
                    }
                    terminalView.forceActiveFocus()
                    Qt.inputMethod.show()
                }
            }
        }

        Rectangle {
            readonly property string status: {
                switch (session.state) {
                case SshSession.Connecting: return qsTr("Connecting")
                case SshSession.Connected: return ""
                default: return session.errorString.length > 0 ? session.errorString : qsTr("Disconnected")
                }
            }

            anchors {
                top: parent.top
                horizontalCenter: parent.horizontalCenter
                margins: Theme.paddingSmall
            }
            width: Math.min(statusLabel.implicitWidth + 2 * Theme.paddingMedium, parent.width - 2 * Theme.paddingSmall)
            height: statusLabel.height + 2 * Theme.paddingSmall
            radius: Theme.paddingSmall
            color: Theme.rgba(Theme.highlightDimmerColor, 0.9)
            visible: status.length > 0 && !terminalView.hasSelection

            Label {
                id: statusLabel
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.paddingMedium
                horizontalAlignment: Text.AlignHCenter
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
                text: parent.status
            }
        }

        Rectangle {
            anchors {
                top: parent.top
                right: parent.right
                margins: Theme.paddingSmall
            }
            width: selectionActions.width
            height: selectionActions.height
            radius: Theme.paddingSmall
            color: Theme.rgba(Theme.highlightDimmerColor, 0.9)
            visible: terminalView.hasSelection

            Row {
                id: selectionActions

                Repeater {
                    model: [
                        { label: qsTr("Copy"), action: "copy" },
                        { label: qsTr("Paste"), action: "paste" },
                        { label: qsTr("Clear"), action: "clear" }
                    ]

                    BackgroundItem {
                        width: actionLabel.width + 2 * Theme.paddingLarge
                        height: Theme.itemSizeExtraSmall
                        onClicked: {
                            if (modelData.action === "copy") {
                                Clipboard.text = terminalView.selectedText()
                                terminalView.clearSelection()
                            } else if (modelData.action === "paste") {
                                terminalView.paste(Clipboard.text)
                            } else {
                                terminalView.clearSelection()
                            }
                        }

                        Label {
                            id: actionLabel
                            anchors.centerIn: parent
                            text: modelData.label
                            color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                        }
                    }
                }
            }
        }
    }

    Row {
        id: keyBar

        readonly property var keys: [
            { label: "Esc", key: Qt.Key_Escape },
            { label: "Tab", key: Qt.Key_Tab },
            { label: "Ctrl", modifier: "ctrl" },
            { label: "←", key: Qt.Key_Left },
            { label: "↑", key: Qt.Key_Up },
            { label: "↓", key: Qt.Key_Down },
            { label: "→", key: Qt.Key_Right },
            { label: "|", text: "|" },
            { label: "/", text: "/" },
            { label: "-", text: "-" },
            { label: "\u22ee", action: "menu" }
        ]

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        Repeater {
            model: keyBar.keys

            BackgroundItem {
                readonly property bool latched: modelData.modifier === "ctrl" && terminalView.ctrlLatched

                width: keyBar.width / keyBar.keys.length
                height: Theme.itemSizeExtraSmall
                highlighted: down || latched
                onClicked: {
                    if (modelData.modifier === "ctrl")
                        terminalView.ctrlLatched = !terminalView.ctrlLatched
                    else if (modelData.action === "menu")
                        pageStack.animatorPush(Qt.resolvedUrl("SessionMenuPage.qml"),
                                               { session: page.session, sessionPage: page, terminalView: terminalView })
                    else if (modelData.key !== undefined)
                        terminalView.sendKey(modelData.key)
                    else
                        terminalView.sendText(modelData.text)
                }

                Label {
                    anchors.centerIn: parent
                    text: modelData.label
                    font.pixelSize: Theme.fontSizeSmall
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }
        }
    }
}
