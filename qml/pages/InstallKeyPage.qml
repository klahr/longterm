import QtQuick 2.0
import Sailfish.Silica 1.0

// Adds the key to a host's authorized_keys, like ssh-copy-id
Page {
    id: page

    property string keyId
    property string keyName

    // Index follows hostStore order
    readonly property string hostId: hostComboBox.currentIndex >= 0 ? hostStore.hostIdAt(hostComboBox.currentIndex) : ""

    function install() {
        var session = sessionManager.installKey(hostId, keyId, useSwitch.checked)
        if (!session)
            return
        // The login and the result show in the session, the start page keeps sessions to its right
        var startPage = pageStack.find(function(other) { return other.connectToHost !== undefined })
        pageStack.pop(startPage, PageStackAction.Immediate)
        startPage.showSession(session)
    }

    allowedOrientations: Orientation.All

    Column {
        width: parent.width
        spacing: Theme.paddingMedium

        PageHeader {
            title: qsTr("Install key")
            description: page.keyName
        }

        ComboBox {
            id: hostComboBox
            width: parent.width
            label: qsTr("Host")
            currentIndex: -1
            menu: ContextMenu {
                Repeater {
                    model: hostStore
                    MenuItem { text: model.name }
                }
            }
        }

        TextSwitch {
            id: useSwitch
            checked: true
            text: qsTr("Log in with this key from now on")
            description: qsTr("Once the key is in place. A saved password for the host is forgotten.")
        }

        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            text: qsTr("Logs in the way the host is set up, with its password if it has no key, and adds the public "
                       + "key to ~/.ssh/authorized_keys there.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryHighlightColor
        }

        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Install")
            enabled: page.hostId.length > 0
            onClicked: page.install()
        }
    }
}
