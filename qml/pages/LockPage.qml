import QtQuick 2.0
import Sailfish.Silica 1.0

// Covers everything until the lock code is typed
Page {
    id: page

    signal unlocked()

    property int failures
    property bool waiting

    function tryCode() {
        if (waiting || codeField.text.length === 0)
            return
        if (appSettings.checkLockCode(codeField.text)) {
            codeField.text = ""
            failures = 0
            unlocked()
            return
        }
        codeField.text = ""
        // Guessing gets slow after a few tries
        if (++failures >= 5) {
            waiting = true
            waitTimer.start()
        }
    }

    backNavigation: false
    forwardNavigation: false
    allowedOrientations: Orientation.All

    onStatusChanged: {
        if (status === PageStatus.Active)
            codeField.forceActiveFocus()
    }

    Timer {
        id: waitTimer
        interval: 30000
        onTriggered: page.waiting = false
    }

    Column {
        anchors.centerIn: parent
        width: parent.width
        spacing: Theme.paddingLarge

        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            source: "image://theme/icon-l-lock"
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: page.waiting ? qsTr("Too many wrong codes, wait a moment")
                               : page.failures > 0 ? qsTr("Wrong code") : qsTr("Longterm is locked")
            color: page.failures > 0 ? Theme.errorColor : Theme.highlightColor
            font.pixelSize: Theme.fontSizeLarge
        }

        PasswordField {
            id: codeField
            width: parent.width
            enabled: !page.waiting
            label: qsTr("Lock code")
            placeholderText: label
            inputMethodHints: Qt.ImhDigitsOnly
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.onClicked: page.tryCode()
        }
    }
}
