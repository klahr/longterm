import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0

Page {
    id: page

    property var session
    property Item sessionPage
    property Item terminalView

    allowedOrientations: Orientation.All

    // Replaces the session page itself, so going back leads to the start page
    function switchTo(otherSession) {
        pageStack.pop(sessionPage, PageStackAction.Immediate)
        pageStack.replace(Qt.resolvedUrl("SessionPage.qml"), { session: otherSession }, PageStackAction.Immediate)
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
                onClicked: pageStack.animatorReplace(Qt.resolvedUrl("SettingsPage.qml"))

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Settings")
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
                    text: qsTr("Close connection")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            SectionHeader {
                text: qsTr("Switch to")
                visible: sessionManager.count > 1
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
