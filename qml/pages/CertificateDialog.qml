import QtQuick 2.0
import Sailfish.Silica 1.0

// An OpenSSH certificate for a key, the line of its -cert.pub file
Dialog {
    id: dialog

    property string keyId
    property string certificate
    readonly property string problem: keyStore.checkCertificate(keyId, certificateArea.text)

    canAccept: problem.length === 0
    onAccepted: keyStore.setCertificate(keyId, certificateArea.text)

    Column {
        width: parent.width

        DialogHeader {
            title: qsTr("Certificate")
        }

        TextArea {
            id: certificateArea
            width: parent.width
            label: qsTr("OpenSSH certificate")
            placeholderText: qsTr("Paste the line of the key's -cert.pub file, empty for none")
            text: dialog.certificate
            font.family: "Monospace"
            font.pixelSize: Theme.fontSizeExtraSmall
            wrapMode: TextEdit.WrapAnywhere
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

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("Servers that trust your certificate authority accept the key with it, without the key in "
                       + "authorized_keys.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryHighlightColor
        }
    }
}
