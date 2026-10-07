import QtQuick 2.0
import Sailfish.Silica 1.0

// A trusted host key, to compare with the server, for example with
// ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub on it
Page {
    id: page

    property string hosts
    property string keyType
    property string fingerprint

    allowedOrientations: Orientation.All

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingLarge

            PageHeader {
                title: page.hosts
                description: page.keyType
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: page.fingerprint
                wrapMode: Text.WrapAnywhere
                font.family: "Monospace"
                color: Theme.highlightColor
            }

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(page.width, page.height) * 0.7
                height: width
                sourceSize.width: width
                sourceSize.height: height
                smooth: false
                source: "image://qr/" + encodeURIComponent(page.fingerprint)
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("On the server, ssh-keygen -lf with the host key's .pub file in /etc/ssh shows the same "
                           + "fingerprint. A phone or computer that reads QR codes can compare it with this one.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Copy fingerprint")
                onClicked: Clipboard.text = page.fingerprint
            }
        }

        VerticalScrollDecorator {}
    }
}
