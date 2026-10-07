import QtQuick 2.0
import Sailfish.Silica 1.0

// Adds a snippet, or edits the one at index
Dialog {
    id: dialog

    property int index: -1
    // Preselects a host for a new snippet
    property string hostId

    canAccept: textArea.text.length > 0

    // Index 0 is every host, the rest follow hostStore order
    readonly property string selectedHostId: hostComboBox.currentIndex > 0
                                             ? hostStore.hostIdAt(hostComboBox.currentIndex - 1) : ""

    Component.onCompleted: {
        if (index >= 0) {
            var snippet = appSettings.snippets[index]
            nameField.text = snippet.name
            textArea.text = snippet.text
            runSwitch.checked = snippet.run
            hostId = snippet.hostId
        }
        hostComboBox.currentIndex = hostId.length > 0 ? hostStore.indexOf(hostId) + 1 : 0
    }

    onAccepted: {
        var snippets = appSettings.snippets
        var snippet = {
            name: nameField.text.trim(),
            text: textArea.text,
            hostId: selectedHostId,
            run: runSwitch.checked
        }
        if (index >= 0)
            snippets[index] = snippet
        else
            snippets.push(snippet)
        appSettings.snippets = snippets
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width

            DialogHeader {
                title: dialog.index >= 0 ? qsTr("Edit snippet") : qsTr("New snippet")
            }

            TextField {
                id: nameField
                width: parent.width
                label: qsTr("Name")
                placeholderText: qsTr("Name, the text itself when empty")
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: textArea.focus = true
            }

            TextArea {
                id: textArea
                width: parent.width
                label: qsTr("Text")
                placeholderText: qsTr("Command or text to type")
                font.family: terminalFontFamily
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            TextSwitch {
                id: runSwitch
                text: qsTr("Press Enter after it")
                description: qsTr("Runs the command right away instead of leaving it to edit")
            }

            ComboBox {
                id: hostComboBox
                width: parent.width
                label: qsTr("Host")
                menu: ContextMenu {
                    MenuItem { text: qsTr("All hosts") }
                    Repeater {
                        model: hostStore
                        MenuItem { text: model.name }
                    }
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
