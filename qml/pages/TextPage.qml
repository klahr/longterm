import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property var session
    property Item terminalView
    readonly property string text: session.scrollbackText()

    allowedOrientations: Orientation.All

    onStatusChanged: {
        if (status === PageStatus.Active && !flickable.scrolled) {
            flickable.scrolled = true
            flickable.scrollToBottom()
            textArea.forceActiveFocus()
        }
    }

    SilicaFlickable {
        id: flickable

        property bool scrolled

        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Copy all")
                onClicked: Clipboard.text = page.text
            }
        }

        Column {
            id: column

            width: parent.width

            PageHeader {
                title: qsTr("Text")
                description: session.name.length > 0 ? session.name : session.user + "@" + session.host
            }

            TextArea {
                id: textArea

                width: parent.width
                readOnly: true
                focusOnClick: true
                softwareInputPanelEnabled: false
                labelVisible: false
                text: page.text
                textLeftMargin: 0
                textRightMargin: 0
                font.family: terminalView.fontFamily
                font.pixelSize: terminalView.fontPixelSize
            }
        }

        VerticalScrollDecorator {}
    }
}
