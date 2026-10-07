import QtQuick 2.0
import Sailfish.Silica 1.0

// Asks for the current code before the lock is turned off
Dialog {
    id: dialog

    property bool codeOk

    canAccept: codeOk
    onAccepted: appSettings.setLockCode("")

    Column {
        width: parent.width

        DialogHeader {
            title: qsTr("Turn off lock")
        }

        PasswordField {
            id: codeField
            width: parent.width
            label: qsTr("Current lock code")
            placeholderText: label
            inputMethodHints: Qt.ImhDigitsOnly
            focus: true
            // Codes are at least 4 characters, so shorter ones are not worth hashing
            onTextChanged: dialog.codeOk = text.length >= 4 && appSettings.checkLockCode(text)
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.enabled: dialog.canAccept
            EnterKey.onClicked: dialog.accept()
        }
    }
}
