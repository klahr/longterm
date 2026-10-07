import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    // Empty for a new host or a one-off connection, otherwise the host being edited
    property string hostId
    property bool hasSavedPassword

    allowedOrientations: Orientation.All

    readonly property bool canConnect: addressField.text.length > 0 && userField.text.length > 0
                                       && portField.acceptableInput && forwardsValid && remoteForwardsValid
                                       && socksValid && environmentValid
                                       && (keepAliveField.text.length === 0 || keepAliveField.acceptableInput)
                                       && (timeoutField.text.length === 0 || timeoutField.acceptableInput)
    readonly property bool canSave: canConnect && nameField.text.trim().length > 0

    // Index 0 is password authentication, the rest follow keyStore order
    readonly property string keyId: authComboBox.currentIndex > 0
                                    ? keyStore.keyIdAt(authComboBox.currentIndex - 1) : ""
    // Index 0 is a direct connection, the rest follow hostStore order
    readonly property string jumpHostId: jumpComboBox.currentIndex > 0
                                         ? hostStore.hostIdAt(jumpComboBox.currentIndex - 1) : ""
    readonly property var localForwards: splitList(forwardsField.text)
    readonly property bool forwardsValid: validForwards(localForwards)
    readonly property var remoteForwards: splitList(remoteForwardsField.text)
    readonly property bool remoteForwardsValid: validForwards(remoteForwards)
    readonly property var socksPorts: splitList(socksField.text)
    readonly property bool socksValid: {
        for (var i = 0; i < socksPorts.length; ++i) {
            if (!/^\d{1,5}$/.test(socksPorts[i]) || socksPorts[i] < 1 || socksPorts[i] > 65535)
                return false
        }
        return true
    }
    // One NAME=value a line, values may have spaces
    readonly property var environment: {
        var lines = environmentArea.text.split("\n")
        var variables = []
        for (var i = 0; i < lines.length; ++i) {
            var line = lines[i].trim()
            if (line.length > 0)
                variables.push(line)
        }
        return variables
    }
    readonly property bool environmentValid: {
        for (var i = 0; i < environment.length; ++i) {
            if (!/^[A-Za-z_][A-Za-z0-9_]*=/.test(environment[i]))
                return false
        }
        return true
    }

    function splitList(text) {
        var entries = text.split(/[\s,]+/)
        var list = []
        for (var i = 0; i < entries.length; ++i) {
            if (entries[i].length > 0)
                list.push(entries[i])
        }
        return list
    }

    // "port:host:port", where host may be a bracketed IPv6 address
    function validForwards(forwards) {
        for (var i = 0; i < forwards.length; ++i) {
            var match = /^(\d{1,5}):(\[[^\]]+\]|[^:\[\]]+):(\d{1,5})$/.exec(forwards[i])
            if (!match || match[1] < 1 || match[1] > 65535 || match[3] < 1 || match[3] > 65535)
                return false
        }
        return true
    }

    function save() {
        var options = {
            jumpHostId: jumpHostId,
            forwardAgent: agentSwitch.checked,
            localForwards: localForwards,
            remoteForwards: remoteForwards,
            dynamicForwards: socksPorts,
            environment: environment,
            tmuxSession: tmuxSwitch.checked ? tmuxField.text.trim() : "",
            keepAliveInterval: keepAliveField.text.length > 0 ? parseInt(keepAliveField.text) : 0,
            connectTimeout: timeoutField.text.length > 0 ? parseInt(timeoutField.text) : 0,
            mosh: moshSwitch.checked
        }
        return hostStore.saveHost(hostId, nameField.text, addressField.text,
                                  parseInt(portField.text), userField.text, keyId,
                                  passwordField.text, rememberSwitch.checked, options)
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
        remoteForwardsField.text = host.remoteForwards.join(", ")
        socksField.text = host.dynamicForwards.join(", ")
        environmentArea.text = host.environment.join("\n")
        tmuxSwitch.checked = host.tmuxSession.length > 0
        if (host.tmuxSession.length > 0)
            tmuxField.text = host.tmuxSession
        keepAliveField.text = host.keepAliveInterval > 0 ? host.keepAliveInterval : ""
        timeoutField.text = host.connectTimeout > 0 ? host.connectTimeout : ""
        moshSwitch.checked = host.mosh
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

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: nameField.text.trim().length === 0
                text: qsTr("These take effect for saved hosts, give the host a name to use them.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }

            TextSwitch {
                id: moshSwitch
                text: qsTr("Use mosh")
                description: qsTr("The session stays through network changes and sleep, and typing does not wait for "
                                  + "the network. Needs mosh-server on the host and UDP ports 60000 to 61000 open to it, "
                                  + "without mosh-server it connects over SSH.")
            }

            TextSwitch {
                id: tmuxSwitch
                text: qsTr("Attach to tmux")
                description: qsTr("Opens a tmux session instead of a plain shell, or the one running already, so "
                                  + "what runs in it is still there after disconnecting")
            }

            TextField {
                id: tmuxField
                width: parent.width
                visible: tmuxSwitch.checked
                label: qsTr("tmux session")
                placeholderText: label
                text: "main"
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
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
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: remoteForwardsField.focus = true
            }

            TextField {
                id: remoteForwardsField
                width: parent.width
                label: page.remoteForwardsValid ? qsTr("Remote port forwards, remotePort:host:port")
                                                : qsTr("Use remotePort:host:port, separated by commas")
                placeholderText: qsTr("Remote forwards, e.g. 9000:localhost:8000")
                description: qsTr("The server listens on the remote port and passes connections on to the host and "
                                  + "port as seen from the phone")
                errorHighlight: !page.remoteForwardsValid
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: socksField.focus = true
            }

            TextField {
                id: socksField
                width: parent.width
                label: page.socksValid ? qsTr("SOCKS proxy ports") : qsTr("Use port numbers, separated by commas")
                placeholderText: qsTr("SOCKS proxy port, e.g. 1080")
                description: qsTr("A SOCKS proxy on the phone, for apps that connect out through the server")
                errorHighlight: !page.socksValid
                inputMethodHints: Qt.ImhDigitsOnly
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: environmentArea.focus = true
            }

            TextArea {
                id: environmentArea
                width: parent.width
                label: page.environmentValid ? qsTr("Environment variables, NAME=value on each line")
                                             : qsTr("Use NAME=value, one on each line")
                placeholderText: qsTr("Environment, e.g. LANG=en_US.UTF-8")
                errorHighlight: !page.environmentValid
                font.family: terminalFontFamily
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("Servers only take the variables their AcceptEnv setting lists, often LANG and LC_*.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }

            TextField {
                id: keepAliveField
                width: parent.width
                label: qsTr("Keepalive interval in seconds")
                placeholderText: qsTr("Keepalive interval, 60 seconds")
                description: qsTr("How often an idle connection sends something, so routers do not drop it")
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: 3600 }
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: timeoutField.focus = true
            }

            TextField {
                id: timeoutField
                width: parent.width
                label: qsTr("Connect timeout in seconds")
                placeholderText: qsTr("Connect timeout, 15 seconds")
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: 300 }
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
