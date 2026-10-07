import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    property var session

    canAccept: pathField.text.trim().length > 0 && !/\/$/.test(pathField.text.trim())
    onAccepted: session.files.download(pathField.text.trim())

    Column {
        width: parent.width
        spacing: Theme.paddingMedium

        DialogHeader {
            title: qsTr("Download file")
            acceptText: qsTr("Download")
        }

        TextField {
            id: pathField
            width: parent.width
            label: qsTr("File on the server")
            placeholderText: qsTr("Path, from the home folder or starting with /")
            // Starts in the shell's folder when it says which one that is
            text: session.terminal.workingDirectory.length > 0 ? session.terminal.workingDirectory + "/" : ""
            focus: true
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.enabled: dialog.canAccept
            EnterKey.onClicked: dialog.accept()
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("Saved in the Downloads folder")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryHighlightColor
        }
    }
}
