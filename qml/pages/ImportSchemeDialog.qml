import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

// Paste a scheme or pick its file: iTerm2, Alacritty, Windows Terminal, Xresources or base16
Dialog {
    id: dialog

    property string filePath
    // Why what was given cannot be imported
    readonly property string problem: filePath.length > 0 ? colorSchemes.checkFile(filePath)
                                                          : textArea.text.trim().length > 0 ? colorSchemes.check(textArea.text) : ""

    canAccept: (textArea.text.trim().length > 0 || filePath.length > 0) && problem.length === 0

    onAccepted: {
        var id = filePath.length > 0 ? colorSchemes.importSchemeFile(nameField.text, filePath)
                                     : colorSchemes.importScheme(nameField.text, textArea.text)
        if (id.length > 0)
            appSettings.terminalColorScheme = id
    }

    Component {
        id: pickerComponent

        FilePickerPage {
            title: qsTr("Scheme file")
            onSelectedContentPropertiesChanged: {
                dialog.filePath = selectedContentProperties.filePath
                if (nameField.text.length === 0)
                    nameField.text = selectedContentProperties.fileName.replace(/\.[^.]*$/, "")
            }
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width

            DialogHeader {
                title: qsTr("Import color scheme")
                acceptText: qsTr("Import")
            }

            TextField {
                id: nameField
                width: parent.width
                label: qsTr("Name")
                placeholderText: label
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: textArea.focus = true
            }

            ValueButton {
                label: qsTr("File")
                value: dialog.filePath.length > 0 ? dialog.filePath.split("/").pop() : qsTr("None, paste it below")
                onClicked: pageStack.animatorPush(pickerComponent)
            }

            TextArea {
                id: textArea
                width: parent.width
                visible: dialog.filePath.length === 0
                label: qsTr("Scheme")
                placeholderText: qsTr("Paste an iTerm2, Alacritty, Windows Terminal, Xresources or base16 scheme")
                font.family: terminalFontFamily
                font.pixelSize: Theme.fontSizeExtraSmall
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: text.length > 0
                text: dialog.problem
                wrapMode: Text.Wrap
                color: Theme.errorColor
            }
        }

        VerticalScrollDecorator {}
    }
}
