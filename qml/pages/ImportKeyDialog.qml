import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    readonly property string validationError: keyStore.validatePrivateKey(keyArea.text, passphraseField.text)

    canAccept: nameField.text.trim().length > 0 && validationError.length === 0
    onAccepted: keyStore.importKey(nameField.text, keyArea.text, passphraseField.text)

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            DialogHeader {
                acceptText: qsTr("Import")
            }

            TextField {
                id: nameField
                width: parent.width
                label: qsTr("Name")
                placeholderText: label
                inputMethodHints: Qt.ImhNoPredictiveText
            }

            TextArea {
                id: keyArea
                width: parent.width
                label: qsTr("Private key")
                placeholderText: "-----BEGIN OPENSSH PRIVATE KEY-----"
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                font.family: "Monospace"
                font.pixelSize: Theme.fontSizeTiny
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Paste from clipboard")
                enabled: Clipboard.hasText
                onClicked: keyArea.text = Clipboard.text
            }

            PasswordField {
                id: passphraseField
                width: parent.width
                label: qsTr("Passphrase, if the key has one")
                placeholderText: label
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: keyArea.text.length > 0 && dialog.validationError.length > 0
                text: dialog.validationError
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.errorColor
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("The key is stored in the device keychain without its passphrase.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
            }
        }

        VerticalScrollDecorator {}
    }
}
