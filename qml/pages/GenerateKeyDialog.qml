import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    canAccept: nameField.text.trim().length > 0
    onAccepted: keyStore.generateKey(nameField.text)

    Column {
        width: parent.width
        spacing: Theme.paddingMedium

        DialogHeader {
            acceptText: qsTr("Generate")
        }

        TextField {
            id: nameField
            width: parent.width
            label: qsTr("Name")
            placeholderText: label
            text: "longterm"
            inputMethodHints: Qt.ImhNoPredictiveText
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.enabled: dialog.canAccept
            EnterKey.onClicked: dialog.accept()
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("Creates an Ed25519 key. The private key is kept in the device keychain.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryHighlightColor
        }
    }
}
