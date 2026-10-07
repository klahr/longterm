import QtQuick 2.0
import Sailfish.Silica 1.0

// Sets the code that unlocks the app
Dialog {
    id: dialog

    canAccept: codeField.text.length >= 4 && codeField.text === repeatField.text
    onAccepted: appSettings.setLockCode(codeField.text)

    Column {
        width: parent.width

        DialogHeader {
            title: qsTr("Lock code")
        }

        PasswordField {
            id: codeField
            width: parent.width
            label: qsTr("Code, at least 4 characters")
            placeholderText: qsTr("Code")
            inputMethodHints: Qt.ImhDigitsOnly
            focus: true
            EnterKey.iconSource: "image://theme/icon-m-enter-next"
            EnterKey.onClicked: repeatField.focus = true
        }

        PasswordField {
            id: repeatField
            width: parent.width
            label: repeatField.text.length > 0 && repeatField.text !== codeField.text ? qsTr("The codes differ") : qsTr("Again")
            placeholderText: qsTr("The same code again")
            errorHighlight: repeatField.text.length > 0 && repeatField.text !== codeField.text
            inputMethodHints: Qt.ImhDigitsOnly
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.enabled: dialog.canAccept
            EnterKey.onClicked: dialog.accept()
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("Keys and passwords are in the device keychain either way. The code keeps others from using "
                       + "connections and saved hosts on an unlocked phone.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryHighlightColor
        }
    }
}
