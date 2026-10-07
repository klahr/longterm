import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

Dialog {
    id: dialog

    property string filePath

    canAccept: filePath.length > 0 && passphraseField.text.length > 0
    onAccepted: backup.importBackup(filePath, passphraseField.text)

    Component {
        id: pickerComponent

        FilePickerPage {
            title: qsTr("Backup file")
            nameFilters: [ "*.ltbackup" ]
            onSelectedContentPropertiesChanged: dialog.filePath = selectedContentProperties.filePath
        }
    }

    Column {
        width: parent.width

        DialogHeader {
            title: qsTr("Restore")
            acceptText: qsTr("Restore")
        }

        ValueButton {
            label: qsTr("Backup file")
            value: dialog.filePath.length > 0 ? dialog.filePath.split("/").pop() : qsTr("Choose")
            onClicked: pageStack.animatorPush(pickerComponent)
        }

        PasswordField {
            id: passphraseField
            width: parent.width
            label: qsTr("Passphrase")
            placeholderText: label
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.enabled: dialog.canAccept
            EnterKey.onClicked: dialog.accept()
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("Hosts in the backup replace the ones they were saved from, keys that are here already stay.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryHighlightColor
        }
    }
}
