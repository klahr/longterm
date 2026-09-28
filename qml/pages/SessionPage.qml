import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0
import "../components"

Page {
    id: page

    readonly property var session: sessionList.currentItem ? sessionList.currentItem.session : null
    readonly property Item currentView: sessionList.currentItem ? sessionList.currentItem.terminalView : null

    function showKeyboard() {
        if (!currentView)
            return
        currentView.forceActiveFocus()
        Qt.inputMethod.show()
    }

    function showSession(otherSession) {
        showIndex(sessionManager.indexOf(otherSession))
    }

    function showFirstSession() {
        showIndex(0)
    }

    function showIndex(index) {
        if (index < 0 || index >= sessionList.count)
            return
        sessionList.currentIndex = index
        sessionList.positionViewAtIndex(index, ListView.Beginning)
    }

    allowedOrientations: Orientation.All
    // With several sessions the swipe switches between them, so only the first session swipes back
    backNavigation: !sessionList.interactive || sessionList.currentIndex === 0

    onStatusChanged: {
        if (status === PageStatus.Active)
            showKeyboard()
    }

    // The session is removed when its shell exits, so leave once none are left to show
    Connections {
        target: sessionManager
        onCountChanged: {
            if (sessionManager.count === 0 && page.status === PageStatus.Active)
                pageStack.navigateBack()
        }
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

    ListView {
        id: sessionList

        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: keyBar.top
            // Keep text and overlays out from under the display notch
            topMargin: page.orientation === Orientation.Portrait ? Screen.topCutout.height : 0
            leftMargin: page.orientation === Orientation.Landscape ? Screen.topCutout.height : 0
            rightMargin: page.orientation === Orientation.LandscapeInverted ? Screen.topCutout.height : 0
        }
        clip: true
        orientation: ListView.Horizontal
        snapMode: ListView.SnapOneItem
        highlightRangeMode: ListView.StrictlyEnforceRange
        cacheBuffer: width
        interactive: count > 1
        // Not pulling past the first session leaves that swipe to the page stack for going back
        boundsBehavior: currentIndex === 0 ? Flickable.StopAtBounds : Flickable.DragAndOvershootBounds
        model: sessionManager

        onCurrentItemChanged: {
            if (currentItem && page.status === PageStatus.Active)
                page.showKeyboard()
        }

        delegate: Item {
            readonly property var session: model.session
            readonly property alias terminalView: terminalView

            width: sessionList.width
            height: sessionList.height

            Item {
                id: nameBar

                width: parent.width
                height: nameLabel.implicitHeight + Theme.paddingSmall

                Row {
                    anchors.centerIn: parent
                    spacing: Theme.paddingSmall

                    StatusDot {
                        anchors.verticalCenter: parent.verticalCenter
                        session: model.session
                    }

                    Label {
                        id: nameLabel

                        width: Math.min(implicitWidth, nameBar.width - 2 * Theme.pageStackIndicatorWidth)
                        truncationMode: TruncationMode.Fade
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.highlightColor
                        text: session.name.length > 0 ? session.name : session.user + "@" + session.host
                    }
                }
            }

            TerminalView {
                id: terminalView

                anchors {
                    top: nameBar.bottom
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
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
                    id: statusOverlay

                    readonly property string status: {
                        switch (session.state) {
                        case SshSession.Connecting: return qsTr("Connecting")
                        case SshSession.Connected: return ""
                        default: return session.errorString.length > 0 ? session.errorString : qsTr("Disconnected")
                        }
                    }
                    readonly property bool canReconnect: session.state === SshSession.Disconnected

                    anchors {
                        top: parent.top
                        horizontalCenter: parent.horizontalCenter
                        margins: Theme.paddingSmall
                    }
                    width: canReconnect ? parent.width - 2 * Theme.paddingLarge
                                        : Math.min(statusLabel.implicitWidth + 2 * Theme.paddingMedium,
                                                   parent.width - 2 * Theme.paddingSmall)
                    height: statusColumn.height + 2 * Theme.paddingSmall
                    radius: Theme.paddingSmall
                    color: Theme.rgba(Theme.highlightDimmerColor, 0.9)
                    visible: status.length > 0 && !terminalView.hasSelection

                    Column {
                        id: statusColumn

                        x: Theme.paddingMedium
                        y: Theme.paddingSmall
                        width: parent.width - 2 * Theme.paddingMedium
                        spacing: Theme.paddingSmall

                        Label {
                            id: statusLabel
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: statusOverlay.canReconnect ? Text.Wrap : Text.NoWrap
                            truncationMode: statusOverlay.canReconnect ? TruncationMode.None : TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.highlightColor
                            text: statusOverlay.status
                        }

                        Label {
                            width: parent.width
                            visible: session.hostKeyMismatch
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WrapAnywhere
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.secondaryHighlightColor
                            text: qsTr("The server's key is not the one saved earlier. Only trust it if you know "
                                       + "the server was reinstalled or its keys changed. New key: %1")
                                  .arg(session.serverFingerprint)
                        }

                        Button {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: statusOverlay.canReconnect
                            text: session.hostKeyMismatch ? qsTr("Trust new key") : qsTr("Reconnect")
                            onClicked: {
                                if (session.hostKeyMismatch)
                                    session.trustNewHostKey()
                                else
                                    session.reconnect()
                            }
                        }
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
        }
    }

    // Lit like the page stack's own indicators while there are sessions to swipe to
    Repeater {
        model: [
            { visible: sessionList.currentIndex > 0, x: 0, step: -1 },
            { visible: sessionList.currentIndex < sessionList.count - 1, x: page.width, step: 1 }
        ]

        Item {
            x: modelData.x - width / 2
            width: Theme.pageStackIndicatorWidth
            height: page.isPortrait ? Theme.itemSizeLarge : Theme.itemSizeSmall
            visible: modelData.visible

            GlassItem {
                anchors.centerIn: parent
                color: indicatorArea.pressed ? Theme.highlightColor : Theme.lightPrimaryColor
                backgroundColor: palette.backgroundGlowColor
                radius: 0.22
                falloffRadius: 0.18
            }

            MouseArea {
                id: indicatorArea

                x: modelData.step < 0 ? parent.width / 2 : parent.width / 2 - width
                width: Theme.itemSizeSmall
                height: parent.height
                onClicked: sessionList.currentIndex += modelData.step
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
                readonly property bool latched: modelData.modifier === "ctrl" && page.currentView !== null && page.currentView.ctrlLatched

                width: keyBar.width / keyBar.keys.length
                height: Theme.itemSizeExtraSmall
                highlighted: down || latched
                onClicked: {
                    var view = page.currentView
                    if (!view)
                        return
                    if (modelData.modifier === "ctrl")
                        view.ctrlLatched = !view.ctrlLatched
                    else if (modelData.action === "menu")
                        pageStack.animatorPush(Qt.resolvedUrl("SessionMenuPage.qml"),
                                               { session: page.session, sessionPage: page, terminalView: view })
                    else if (modelData.key !== undefined)
                        view.sendKey(modelData.key)
                    else
                        view.sendText(modelData.text)
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
