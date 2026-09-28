import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    // Empty for a new host or a one-off connection, otherwise the host being edited
    property string hostId
    property bool hasSavedPassword

    allowedOrientations: Orientation.All

    readonly property bool canConnect: addressField.text.length > 0 && userField.text.length > 0
                                       && portField.acceptableInput && forwardsValid
    readonly property bool canSave: canConnect && nameField.text.trim().length > 0

    // Index 0 is password authentication, the rest follow keyStore order
    readonly property string keyId: authComboBox.currentIndex > 0
                                    ? keyStore.keyIdAt(authComboBox.currentIndex - 1) : ""
    // Index 0 is a direct connection, the rest follow hostStore order
    readonly property string jumpHostId: jumpComboBox.currentIndex > 0
                                         ? hostStore.hostIdAt(jumpComboBox.currentIndex - 1) : ""
    readonly property var localForwards: {
        var entries = forwardsField.text.split(/[\s,]+/)
        var forwards = []
        for (var i = 0; i < entries.length; ++i) {
            if (entries[i].length > 0)
                forwards.push(entries[i])
        }
        return forwards
    }
    readonly property bool forwardsValid: {
        for (var i = 0; i < localForwards.length; ++i) {
            if (!/^\d{1,5}:(\[[^\]]+\]|[^:\[\]]+):\d{1,5}$/.test(localForwards[i]))
                return false
        }
        return true
    }

    function save() {
        return hostStore.saveHost(hostId, nameField.text, addressField.text,
                                  parseInt(portField.text), userField.text, keyId,
                                  passwordField.text, rememberSwitch.checked, jumpHostId,
                                  agentSwitch.checked, localForwards)
    }

    function connect() {
        if (!canConnect)
            return
        var id = canSave ? save() : ""
        var session
        // Saved hosts bring their jump host and forwards along
        if (id.length > 0)
            session = sessionManager.openHost(id, keyId.length === 0 ? passwordField.text : "")
        else
            session = sessionManager.openSession(nameField.text.trim(), addressField.text, parseInt(portField.text),
                                                 userField.text, passwordField.text, keyId)
        // The start page keeps the sessions attached to its right
        var startPage = pageStack.previousPage(page)
        pageStack.pop(startPage, PageStackAction.Immediate)
        startPage.showSession(session)
    }

    Component.onCompleted: {
        if (hostId.length === 0)
            return
        var host = hostStore.host(hostId)
        nameField.text = host.name
        addressField.text = host.address
        portField.text = host.port
        userField.text = host.user
        authComboBox.currentIndex = host.keyId.length > 0 ? keyStore.indexOf(host.keyId) + 1 : 0
        hasSavedPassword = host.hasPassword
        rememberSwitch.checked = host.hasPassword
        jumpComboBox.currentIndex = host.jumpHostId.length > 0 ? hostStore.indexOf(host.jumpHostId) + 1 : 0
        agentSwitch.checked = host.forwardAgent
        forwardsField.text = host.localForwards.join(", ")
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: page.width
            spacing: Theme.paddingMedium

            PageHeader {
                title: page.hostId.length > 0 ? qsTr("Edit host") : qsTr("New host")
            }

            TextField {
                id: nameField
                width: parent.width
                label: qsTr("Name, to save the host")
                placeholderText: qsTr("Name")
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: addressField.focus = true
            }

            TextField {
                id: addressField
                width: parent.width
                label: qsTr("Address")
                placeholderText: label
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: portField.focus = true
            }

            TextField {
                id: portField
                width: parent.width
                label: qsTr("Port")
                placeholderText: label
                text: "22"
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: 65535 }
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: userField.focus = true
            }

            TextField {
                id: userField
                width: parent.width
                label: qsTr("Username")
                placeholderText: label
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: passwordField.focus = true
            }

            ComboBox {
                id: authComboBox
                width: parent.width
                label: qsTr("Authentication")
                menu: ContextMenu {
                    MenuItem { text: qsTr("Password") }
                    Repeater {
                        model: keyStore
                        MenuItem { text: qsTr("Key: %1").arg(model.name) }
                    }
                }
            }

            PasswordField {
                id: passwordField
                width: parent.width
                visible: page.keyId.length === 0
                placeholderText: page.hasSavedPassword ? qsTr("Saved password") : qsTr("Password")
                label: page.hasSavedPassword ? qsTr("Leave empty to use the saved password") : qsTr("Password")
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.enabled: page.canConnect
                EnterKey.onClicked: page.connect()
            }

            TextSwitch {
                id: rememberSwitch
                visible: page.keyId.length === 0 && nameField.text.trim().length > 0
                text: qsTr("Remember password")
                description: qsTr("Kept in the device keychain")
                checked: true
            }

            SectionHeader {
                text: qsTr("Advanced")
            }

            ComboBox {
                id: jumpComboBox
                width: parent.width
                label: qsTr("Jump host")
                description: qsTr("Reach this host through another saved host, which needs a key or a saved password")
                menu: ContextMenu {
                    MenuItem { text: qsTr("None") }
                    Repeater {
                        model: hostStore
                        MenuItem { text: model.name }
                    }
                }
            }

            TextSwitch {
                id: agentSwitch
                visible: page.keyId.length > 0
                text: qsTr("Forward agent")
                description: qsTr("Lets the server log in to other hosts with this key while connected. Only turn on for servers you trust.")
            }

            TextField {
                id: forwardsField
                width: parent.width
                label: page.forwardsValid ? qsTr("Local port forwards, localPort:host:port")
                                          : qsTr("Use localPort:host:port, separated by commas")
                placeholderText: qsTr("Port forwards, e.g. 8080:localhost:80")
                errorHighlight: !page.forwardsValid
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.paddingLarge

                Button {
                    text: qsTr("Save")
                    enabled: page.canSave
                    onClicked: {
                        page.save()
                        pageStack.pop()
                    }
                }

                Button {
                    text: qsTr("Connect")
                    enabled: page.canConnect
                    onClicked: page.connect()
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
