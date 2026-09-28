import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    property var session
    // Applied on accept like the other fields, empty follows the app's scheme
    property string colorScheme: session.colorScheme

    onAccepted: {
        session.name = nameField.text.trim()
        session.startupScript = scriptArea.text
        session.colorScheme = colorScheme
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

            ValueButton {
                label: qsTr("Color scheme")
                value: dialog.colorScheme.length > 0
                       ? colorSchemes.name(dialog.colorScheme)
                       : qsTr("App default (%1)").arg(colorSchemes.name(appSettings.terminalColorScheme))
                onClicked: {
                    var picker = pageStack.push(Qt.resolvedUrl("ColorSchemesPage.qml"),
                                                { forSession: true, currentScheme: dialog.colorScheme })
                    picker.schemePicked.connect(function(schemeId) { dialog.colorScheme = schemeId })
                }
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
