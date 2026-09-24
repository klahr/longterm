import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property string name
    property string publicKey
    property string fingerprint

    allowedOrientations: Orientation.All

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: page.width
            spacing: Theme.paddingLarge

            PageHeader {
                title: page.name
                description: page.fingerprint
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("Add this line to ~/.ssh/authorized_keys on the server")
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: page.publicKey
                wrapMode: Text.WrapAnywhere
                font.family: "Monospace"
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Copy public key")
                onClicked: Clipboard.text = page.publicKey
            }
        }

        VerticalScrollDecorator {}
    }
}
