import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    property string keyId
    // Such keys are copied as they are, protected by their own passphrase
    property bool encrypted

    canAccept: encrypted || passphraseField.text === repeatField.text
    onAccepted: keyStore.exportPrivateKey(keyId, passphraseField.text)

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            DialogHeader {
                acceptText: qsTr("Copy")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: dialog.encrypted
                      ? qsTr("Copies the private key to the clipboard in OpenSSH format, still protected by its passphrase.")
                      : qsTr("Copies the private key to the clipboard in OpenSSH format. Anyone who gets it "
                             + "can log in wherever the key is accepted, so set a passphrase unless the "
                             + "clipboard goes straight into a trusted place.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
            }

            PasswordField {
                id: passphraseField
                width: parent.width
                visible: !dialog.encrypted
                label: qsTr("Passphrase, optional")
                placeholderText: qsTr("Passphrase")
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: repeatField.focus = true
            }

            PasswordField {
                id: repeatField
                width: parent.width
                visible: !dialog.encrypted
                label: dialog.canAccept ? qsTr("Passphrase again") : qsTr("The passphrases differ")
                placeholderText: qsTr("Passphrase again")
                errorHighlight: !dialog.canAccept
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.enabled: dialog.canAccept
                EnterKey.onClicked: dialog.accept()
            }
        }
    }
}
