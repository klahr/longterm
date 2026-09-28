import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    // How many hosts were read, set once accepted
    property int imported

    canAccept: configArea.text.trim().length > 0
    onAccepted: imported = hostStore.importConfig(configArea.text)

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            DialogHeader {
                acceptText: qsTr("Import")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("Paste hosts in ~/.ssh/config format. Host, HostName, Port, User, ProxyJump, "
                           + "ForwardAgent and LocalForward are read, hosts with the same name are updated. "
                           + "Keys and passwords are set on each host afterwards.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
            }

            TextArea {
                id: configArea
                width: parent.width
                label: qsTr("ssh_config")
                placeholderText: "Host example\n    HostName example.com\n    User me"
                text: Clipboard.text.indexOf("Host") >= 0 ? Clipboard.text : ""
                font.family: "Monospace"
                font.pixelSize: Theme.fontSizeExtraSmall
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }
        }
    }
}
