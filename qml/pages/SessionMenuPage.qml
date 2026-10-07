import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import Sailfish.Share 1.0
import rs.r8.longterm 1.0

Page {
    id: page

    property var session
    property Item sessionPage
    property Item terminalView
    property string scrollbackMessage

    allowedOrientations: Orientation.All

    function switchTo(otherSession) {
        sessionPage.showSession(otherSession)
        pageStack.pop()
    }

    ShareAction {
        id: shareAction
        mimeType: "text/plain"
        title: qsTr("Share scrollback")
    }

    // Into the shell's folder when it says which one that is, otherwise the home folder
    Component {
        id: uploadPickerComponent

        ContentPickerPage {
            title: qsTr("Upload file")
            onSelectedContentPropertiesChanged: session.files.upload(selectedContentProperties.filePath,
                                                                     session.terminal.workingDirectory, false)
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width

            PageHeader {
                title: session.name.length > 0 ? session.name : session.user + "@" + session.host
                description: session.user + "@" + session.host + (session.port !== 22 ? ":" + session.port : "")
            }

            BackgroundItem {
                enabled: Clipboard.hasText && session.state === SshSession.Connected
                onClicked: {
                    page.terminalView.paste(Clipboard.text)
                    pageStack.pop()
                }

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Paste")
                    color: !parent.enabled ? Theme.secondaryColor
                                           : parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: {
                    page.sessionPage.startSearch()
                    pageStack.pop()
                }

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Find")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: pageStack.animatorReplace(Qt.resolvedUrl("SnippetPickerPage.qml"),
                                                     { session: page.session, terminalView: page.terminalView })

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Snippets")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: pageStack.animatorReplace(Qt.resolvedUrl("HistoryPage.qml"),
                                                     { session: page.session, terminalView: page.terminalView })

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("History")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: {
                    var path = page.session.saveScrollback()
                    page.scrollbackMessage = path.length > 0 ? qsTr("Saved to %1").arg(path) : page.session.errorString
                }

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Save scrollback")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: {
                    var name = (session.name.length > 0 ? session.name : session.host).replace(/[\/:*?"<>|]/g, "_")
                    shareAction.resources = [ { "name": name + ".txt", "data": page.session.scrollbackText(),
                                                "type": "text/plain" } ]
                    shareAction.trigger()
                }

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Share scrollback")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: page.scrollbackMessage.length > 0
                text: page.scrollbackMessage
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }

            BackgroundItem {
                enabled: session.filesAvailable
                onClicked: pageStack.animatorReplace(Qt.resolvedUrl("FilesPage.qml"),
                                                     { session: page.session, startPath: session.terminal.workingDirectory })

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Files")
                    color: !parent.enabled ? Theme.secondaryColor
                                           : parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                enabled: session.filesAvailable
                onClicked: pageStack.animatorReplace(uploadPickerComponent)

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Upload file")
                    color: !parent.enabled ? Theme.secondaryColor
                                           : parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                enabled: session.filesAvailable
                onClicked: pageStack.animatorReplace(Qt.resolvedUrl("DownloadFileDialog.qml"), { session: page.session })

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Download file")
                    color: !parent.enabled ? Theme.secondaryColor
                                           : parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: pageStack.animatorReplace(Qt.resolvedUrl("SettingsPage.qml"))

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Settings")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: pageStack.animatorReplace(Qt.resolvedUrl("EditSessionDialog.qml"), { session: page.session })

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Edit session")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                visible: session.state === SshSession.Disconnected && !session.hostKeyMismatch
                onClicked: {
                    page.session.reconnect()
                    pageStack.pop()
                }

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Reconnect")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                visible: session.state !== SshSession.Disconnected
                onClicked: {
                    page.session.disconnectFromHost()
                    pageStack.pop()
                }

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Disconnect")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            BackgroundItem {
                onClicked: {
                    sessionManager.closeSession(page.session)
                    pageStack.pop(pageStack.previousPage(page.sessionPage))
                }

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Remove")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            SectionHeader {
                text: qsTr("Switch to")
            }

            BackgroundItem {
                onClicked: pageStack.pop(pageStack.previousPage(page.sessionPage))

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Home")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            Repeater {
                model: sessionManager

                BackgroundItem {
                    readonly property var otherSession: model.session

                    visible: otherSession !== page.session
                    onClicked: page.switchTo(otherSession)

                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        text: otherSession.name.length > 0 ? otherSession.name
                                                           : otherSession.user + "@" + otherSession.host
                        truncationMode: TruncationMode.Fade
                        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
