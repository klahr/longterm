import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    property var session

    onAccepted: session.name = nameField.text.trim()

    Column {
        width: parent.width
        spacing: Theme.paddingMedium

        DialogHeader {
            acceptText: qsTr("Rename")
        }

        TextField {
            id: nameField
            width: parent.width
            label: qsTr("Name")
            placeholderText: session.user + "@" + session.host
            text: session.name
            focus: true
            inputMethodHints: Qt.ImhNoPredictiveText
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.onClicked: dialog.accept()
        }
    }
}
