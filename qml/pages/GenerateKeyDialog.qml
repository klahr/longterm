import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    canAccept: nameField.text.trim().length > 0
    onAccepted: keyStore.generateKey(nameField.text, ["ed25519", "ecdsa", "rsa"][typeComboBox.currentIndex])

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

        ComboBox {
            id: typeComboBox
            width: parent.width
            label: qsTr("Type")
            menu: ContextMenu {
                MenuItem { text: qsTr("Ed25519") }
                MenuItem { text: qsTr("ECDSA P-256") }
                MenuItem { text: qsTr("RSA 3072") }
            }
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("Ed25519 suits almost every server. Pick RSA for older devices that do not accept it. "
                       + "The private key is kept in the device keychain.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryHighlightColor
        }
    }
}
