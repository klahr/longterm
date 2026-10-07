import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    canAccept: passphraseField.text.length >= 8 && passphraseField.text === repeatField.text
    onAccepted: backup.exportBackup(passphraseField.text)

    Column {
        width: parent.width

        DialogHeader {
            title: qsTr("Back up")
            acceptText: qsTr("Save")
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("The backup holds your private keys and saved passwords, encrypted with this passphrase. "
                       + "It goes to Documents/Longterm. Without the passphrase it cannot be restored.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryHighlightColor
        }

        PasswordField {
            id: passphraseField
            width: parent.width
            label: qsTr("Passphrase, at least 8 characters")
            placeholderText: qsTr("Passphrase")
            focus: true
            EnterKey.iconSource: "image://theme/icon-m-enter-next"
            EnterKey.onClicked: repeatField.focus = true
        }

        PasswordField {
            id: repeatField
            width: parent.width
            label: repeatField.text.length > 0 && repeatField.text !== passphraseField.text
                   ? qsTr("The passphrases differ") : qsTr("Again")
            placeholderText: qsTr("The same passphrase again")
            errorHighlight: repeatField.text.length > 0 && repeatField.text !== passphraseField.text
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.enabled: dialog.canAccept
            EnterKey.onClicked: dialog.accept()
        }
    }
}
