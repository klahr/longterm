import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0
import "../components"
import "../components/Keys.js" as Keys

Page {
    id: page

    readonly property var session: sessionList.currentItem ? sessionList.currentItem.session : null
    readonly property Item currentView: sessionList.currentItem ? sessionList.currentItem.terminalView : null
    property bool searching
    property bool keyboardHidden

    function startSearch() {
        searching = true
        searchField.forceActiveFocus()
    }

    function stopSearch() {
        searching = false
        searchField.text = ""
        if (currentView)
            currentView.clearSelection()
        showKeyboard()
    }

    function findNext(backwards) {
        if (currentView && searchField.text.length > 0)
            searchField.errorHighlight = !currentView.find(searchField.text, backwards)
    }

    function showKeyboard() {
        var item = sessionList.currentItem
        if (!item || searching || keyboardHidden || item.session.prompting)
            return
        item.terminalView.forceActiveFocus()
        Qt.inputMethod.show()
    }

    function showSession(otherSession) {
        visibleSessions.keep = otherSession
        showIndex(visibleSessions.indexOf(otherSession))
    }

    function showFirstSession() {
        if (visibleSessions.count === 0 && sessionManager.count > 0)
            visibleSessions.keep = sessionManager.sessionAt(0)
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
        if (status !== PageStatus.Active)
            return
        if (searching)
            searchField.forceActiveFocus()
        else
            showKeyboard()
    }

    // Disconnected sessions are left out of the swipe, except the one on screen
    SessionFilter {
        id: visibleSessions
        source: sessionManager
    }

    // The session is removed when its shell exits, so leave once none are left to show
    Connections {
        target: visibleSessions
        onCountChanged: {
            if (visibleSessions.count === 0 && page.status === PageStatus.Active)
                pageStack.navigateBack()
        }
    }

    // A keyboard swiped down stays down until the terminal or the toolbar is tapped
    Connections {
        target: Qt.inputMethod
        onVisibleChanged: {
            if (Qt.inputMethod.visible)
                page.keyboardHidden = false
            else if (page.status === PageStatus.Active && Qt.application.state === Qt.ApplicationActive)
                keyboardHideTimer.restart()
        }
    }

    Connections {
        target: Qt.application
        onStateChanged: {
            if (Qt.application.state === Qt.ApplicationActive && page.status === PageStatus.Active)
                page.showKeyboard()
        }
    }

    Timer {
        id: keyboardHideTimer
        interval: 300
        onTriggered: {
            if (page.status === PageStatus.Active && Qt.application.state === Qt.ApplicationActive
                    && !Qt.inputMethod.visible)
                page.keyboardHidden = true
        }
    }

    // Fills the display notch margin and the name bar in the session's colors
    Rectangle {
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: searchBar.top
        }
        color: sessionList.currentItem ? colorSchemes.background(sessionList.currentItem.schemeId) : "transparent"
    }

    ListView {
        id: sessionList

        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: searchBar.top
            // Keep text and overlays out from under the display notch
            topMargin: page.orientation === Orientation.Portrait ? Screen.topCutout.height : 0
            leftMargin: page.orientation === Orientation.Landscape ? Screen.topCutout.height : 0
            rightMargin: page.orientation === Orientation.LandscapeInverted ? Screen.topCutout.height : 0
        }
        clip: true
        orientation: ListView.Horizontal
        snapMode: ListView.SnapOneItem
        highlightRangeMode: ListView.StrictlyEnforceRange
        // The default speed takes seconds for a full-width page when the indicators switch sessions
        highlightMoveDuration: 250
        highlightMoveVelocity: -1
        cacheBuffer: width
        interactive: count > 1
        // Not pulling past the first session leaves that swipe to the page stack for going back
        boundsBehavior: currentIndex === 0 ? Flickable.StopAtBounds : Flickable.DragAndOvershootBounds
        model: visibleSessions

        // Only once the swipe settles, as dropping the session left behind mid-swipe would shift the view
        onMovementEnded: visibleSessions.keep = page.session
        onCurrentItemChanged: {
            if (!moving && currentItem)
                visibleSessions.keep = currentItem.session
            if (currentItem && page.status === PageStatus.Active)
                page.showKeyboard()
        }

        delegate: Item {
            id: delegateItem

            readonly property var session: model.session
            readonly property string schemeId: session.colorScheme.length > 0 ? session.colorScheme
                                                                              : appSettings.terminalColorScheme
            readonly property alias terminalView: terminalView

            width: sessionList.width
            height: sessionList.height

            Rectangle {
                id: nameBar

                width: parent.width
                height: nameLabel.implicitHeight + Theme.paddingSmall
                // Swiping between sessions shows each bar in its own scheme
                color: colorSchemes.background(delegateItem.schemeId)

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
                        color: colorSchemes.foreground(delegateItem.schemeId)
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
                colorScheme: delegateItem.schemeId

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
                        property int draggedLines
                        property bool dragged
                        property bool selecting
                        property real lastY
                        property real lastTime
                        property real velocity

                        anchors.fill: parent
                        // Keep the page and the session swipe from taking over a selection or a scroll
                        preventStealing: selecting || dragged
                        onPressed: {
                            flickTimer.stop()
                            startY = mouse.y
                            draggedLines = 0
                            dragged = false
                            lastY = mouse.y
                            lastTime = Date.now()
                            velocity = 0
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
                            // A tap that wobbles a little should not scroll
                            if (!dragged) {
                                if (Math.abs(mouse.y - startY) < Theme.startDragDistance)
                                    return
                                dragged = true
                                startY = mouse.y
                            }
                            var lines = Math.round((mouse.y - startY) / terminalView.cellHeight)
                            terminalView.scroll(lines - draggedLines)
                            draggedLines = lines
                            var now = Date.now()
                            if (now > lastTime) {
                                var current = (mouse.y - lastY) / terminalView.cellHeight * 1000 / (now - lastTime)
                                velocity = 0.6 * current + 0.4 * velocity
                                lastY = mouse.y
                                lastTime = now
                            }
                        }
                        onDoubleClicked: terminalView.selectWordAt(mouse.x, mouse.y)
                        onReleased: {
                            selecting = false
                            if (dragged && Date.now() - lastTime < 100 && Math.abs(velocity) > 10) {
                                flickTimer.velocity = velocity
                                flickTimer.remainder = 0
                                flickTimer.offset = terminalView.scrollOffset
                                flickTimer.start()
                            }
                        }
                        onCanceled: selecting = false
                        onClicked: {
                            if (dragged)
                                return
                            if (terminalView.hasSelection) {
                                terminalView.clearSelection()
                                return
                            }
                            page.keyboardHidden = false
                            terminalView.forceActiveFocus()
                            Qt.inputMethod.show()
                        }

                        Timer {
                            id: flickTimer

                            property real velocity
                            property real remainder
                            property int offset

                            interval: 16
                            repeat: true
                            onTriggered: {
                                if (terminalView.scrollOffset === 0 && offset !== 0) {
                                    stop()
                                    return
                                }
                                remainder += velocity * interval / 1000
                                var lines = remainder < 0 ? Math.ceil(remainder) : Math.floor(remainder)
                                remainder -= lines
                                velocity *= 0.95
                                if (lines !== 0) {
                                    var before = terminalView.scrollOffset
                                    terminalView.scroll(lines)
                                    if (terminalView.scrollOffset === before || terminalView.scrollOffset === 0)
                                        stop()
                                }
                                offset = terminalView.scrollOffset
                                if (Math.abs(velocity) < 2)
                                    stop()
                            }
                        }
                    }
                }

                Rectangle {
                    id: statusOverlay

                    readonly property string status: {
                        switch (session.state) {
                        case SshSession.Connecting: return session.prompting ? session.promptText : qsTr("Connecting")
                        // mosh keeps the session through the silence, this only says how long it has been
                        case SshSession.Connected:
                            return session.silentSeconds > 0
                                    ? qsTr("No contact with the server for %n seconds", "", session.silentSeconds) : ""
                        default: return session.errorString.length > 0 ? session.errorString : qsTr("Disconnected")
                        }
                    }
                    readonly property bool canReconnect: session.state === SshSession.Disconnected
                    readonly property bool expanded: canReconnect || session.prompting

                    anchors {
                        top: parent.top
                        horizontalCenter: parent.horizontalCenter
                        margins: Theme.paddingSmall
                    }
                    width: expanded ? parent.width - 2 * Theme.paddingLarge
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
                            wrapMode: statusOverlay.expanded ? Text.Wrap : Text.NoWrap
                            truncationMode: statusOverlay.expanded ? TruncationMode.None : TruncationMode.Fade
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

                        TextField {
                            id: promptField

                            function submit() {
                                session.answerPrompt(text, rememberPromptSwitch.checked)
                                text = ""
                                terminalView.forceActiveFocus()
                            }

                            width: parent.width
                            visible: session.prompting
                            echoMode: session.promptEcho ? TextInput.Normal : TextInput.Password
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            placeholderText: session.promptLabel.length > 0 ? session.promptLabel : qsTr("Answer")
                            label: placeholderText
                            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                            EnterKey.onClicked: submit()
                            onVisibleChanged: {
                                if (visible && sessionList.currentItem === delegateItem) {
                                    forceActiveFocus()
                                    Qt.inputMethod.show()
                                }
                            }
                        }

                        TextSwitch {
                            id: rememberPromptSwitch
                            visible: session.prompting && session.promptCanRemember
                            text: qsTr("Remember password")
                            description: qsTr("Kept in the device keychain")
                            checked: true
                        }

                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: session.prompting
                            spacing: Theme.paddingLarge

                            Button {
                                text: qsTr("Cancel")
                                onClicked: session.disconnectFromHost()
                            }

                            Button {
                                text: qsTr("OK")
                                onClicked: promptField.submit()
                            }
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

                        // Re-read whenever the selection changes
                        readonly property string url: terminalView.hasSelection ? terminalView.selectedUrl() : ""

                        Repeater {
                            model: {
                                var actions = []
                                if (selectionActions.url.length > 0)
                                    actions.push({ label: qsTr("Open"), action: "open" })
                                actions.push({ label: qsTr("Copy"), action: "copy" })
                                actions.push({ label: qsTr("Paste"), action: "paste" })
                                actions.push({ label: qsTr("Clear"), action: "clear" })
                                return actions
                            }

                            BackgroundItem {
                                width: actionLabel.width + 2 * Theme.paddingLarge
                                height: Theme.itemSizeExtraSmall
                                onClicked: {
                                    if (modelData.action === "open") {
                                        Qt.openUrlExternally(selectionActions.url)
                                        terminalView.clearSelection()
                                    } else if (modelData.action === "copy") {
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

                // Shows that newer output is hidden below, and goes back to it
                Rectangle {
                    anchors {
                        right: parent.right
                        bottom: parent.bottom
                        margins: Theme.paddingMedium
                    }
                    width: latestRow.width + 2 * Theme.paddingMedium
                    height: Theme.itemSizeExtraSmall
                    radius: height / 2
                    color: Theme.rgba(latestArea.pressed ? Theme.highlightBackgroundColor : Theme.highlightDimmerColor, 0.9)
                    visible: terminalView.scrollOffset > 0 && !terminalView.hasSelection

                    Row {
                        id: latestRow

                        anchors.centerIn: parent
                        spacing: Theme.paddingSmall

                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            source: "image://theme/icon-m-down"
                        }

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: terminalView.scrollOffset === 1 ? qsTr("1 line below") : qsTr("%1 lines below").arg(terminalView.scrollOffset)
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.highlightColor
                        }
                    }

                    MouseArea {
                        id: latestArea
                        anchors.fill: parent
                        onClicked: terminalView.scrollOffset = 0
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
        id: searchBar

        anchors {
            left: parent.left
            right: parent.right
            bottom: keyBar.top
        }
        height: page.searching ? searchField.height : 0
        visible: page.searching

        SearchField {
            id: searchField

            width: parent.width - 3 * Theme.itemSizeSmall
            placeholderText: qsTr("Find in scrollback")
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            EnterKey.iconSource: "image://theme/icon-m-up"
            EnterKey.onClicked: page.findNext(true)
            onTextChanged: {
                errorHighlight = false
                if (page.currentView)
                    page.currentView.clearSelection()
                page.findNext(true)
            }
        }

        IconButton {
            width: Theme.itemSizeSmall
            anchors.verticalCenter: parent.verticalCenter
            icon.source: "image://theme/icon-m-up"
            onClicked: page.findNext(true)
        }

        IconButton {
            width: Theme.itemSizeSmall
            anchors.verticalCenter: parent.verticalCenter
            icon.source: "image://theme/icon-m-down"
            onClicked: page.findNext(false)
        }

        IconButton {
            width: Theme.itemSizeSmall
            anchors.verticalCenter: parent.verticalCenter
            icon.source: "image://theme/icon-m-close"
            onClicked: page.stopSearch()
        }
    }

    Item {
        id: keyBar

        readonly property var keys: Keys.selected(appSettings.toolbarKeys)
        // Keys share the width while they fit, beyond that the row scrolls
        readonly property real keyWidth: Math.max(width / (keys.length + 1), Theme.itemSizeExtraSmall * 0.75)

        function press(key) {
            var view = page.currentView
            if (!view)
                return
            if (key.modifier === "ctrl")
                view.ctrlLatched = !view.ctrlLatched
            else if (key.modifier === "alt")
                view.altLatched = !view.altLatched
            else if (key.action === "menu")
                pageStack.animatorPush(Qt.resolvedUrl("SessionMenuPage.qml"),
                                       { session: page.session, sessionPage: page, terminalView: view })
            else if (key.key !== undefined)
                view.sendKey(key.key)
            else
                view.sendText(key.text)
        }

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: Theme.itemSizeExtraSmall

        Flickable {
            anchors {
                left: parent.left
                right: menuKey.left
                top: parent.top
                bottom: parent.bottom
            }
            clip: true
            contentWidth: keyRow.width
            flickableDirection: Flickable.HorizontalFlick
            interactive: contentWidth > width
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: keyRow

                Repeater {
                    model: keyBar.keys

                    KeyButton {
                        width: keyBar.keyWidth
                        key: modelData
                        view: page.currentView
                        onClicked: keyBar.press(modelData)
                    }
                }
            }
        }

        KeyButton {
            id: menuKey

            anchors.right: parent.right
            width: keyBar.keyWidth
            key: ({ label: "\u22ee", action: "menu" })
            view: page.currentView
            onClicked: keyBar.press(key)
        }

        MouseArea {
            anchors.fill: parent
            enabled: page.keyboardHidden
            onClicked: {
                page.keyboardHidden = false
                page.showKeyboard()
            }
        }
    }
}
