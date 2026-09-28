import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    property var session

    onAccepted: {
        session.name = nameField.text.trim()
        session.startupScript = scriptArea.text
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            DialogHeader {
                acceptText: qsTr("Save")
            }

            TextField {
                id: nameField
                width: parent.width
                label: qsTr("Name")
                placeholderText: session.user + "@" + session.host
                text: session.name
                focus: true
                inputMethodHints: Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: scriptArea.focus = true
            }

            TextArea {
                id: scriptArea
                width: parent.width
                label: qsTr("Startup script")
                placeholderText: label
                text: session.startupScript
                font.family: terminalFontFamily
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("Typed into the shell on every connect. Saved unencrypted, so keep passwords out of it.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }
        }

        VerticalScrollDecorator {}
    }
}
