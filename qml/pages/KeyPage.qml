import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property string keyId
    property string name
    property string publicKey
    property string fingerprint
    // Kept with its passphrase, asked for on every connect
    property bool encrypted
    property bool privateKeyCopied

    allowedOrientations: Orientation.All

    Connections {
        target: keyStore
        onPrivateKeyExported: {
            if (keyId !== page.keyId)
                return
            Clipboard.text = privateKey
            page.privateKeyCopied = true
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Export private key")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("ExportKeyDialog.qml"),
                                                  { keyId: page.keyId, encrypted: page.encrypted })
            }
        }

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

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: page.privateKeyCopied || keyStore.errorString.length > 0
                text: keyStore.errorString.length > 0 ? keyStore.errorString
                                                      : qsTr("The private key is on the clipboard")
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
            }
        }

        VerticalScrollDecorator {}
    }
}
