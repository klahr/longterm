import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0

Rectangle {
    property var session

    // While connected, what a program reported with "OSC 777;longterm-status" wins
    readonly property string activity: session.state === SshSession.Connected ? session.terminal.activity : ""

    width: Theme.paddingMedium
    height: width
    radius: width / 2
    readonly property color activityColor: activity === "working" ? "#3b82f6"
                                         : activity === "waiting" ? "#e5484d"
                                         : activity === "done" ? "#30a46c"
                                         : "transparent"
    readonly property string activityText: activity === "working" ? qsTr("Working...")
                                         : activity === "waiting" ? qsTr("Waiting for input...")
                                         : activity === "done" ? qsTr("Done")
                                         : ""

    color: activity.length > 0 ? activityColor
         : session.state === SshSession.Connected ? Theme.highlightColor
         : session.state === SshSession.Connecting ? Theme.secondaryHighlightColor
         : session.errorString.length > 0 ? Theme.errorColor
         : Theme.secondaryColor
    opacity: session.state === SshSession.Connecting ? 0.6 : 1.0
}
