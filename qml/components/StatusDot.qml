import QtQuick 2.0
import Sailfish.Silica 1.0
import rs.r8.longterm 1.0

Rectangle {
    property var session

    width: Theme.paddingMedium
    height: width
    radius: width / 2
    color: session.state === SshSession.Connected ? Theme.highlightColor
         : session.state === SshSession.Connecting ? Theme.secondaryHighlightColor
         : session.errorString.length > 0 ? Theme.errorColor
         : Theme.secondaryColor
    opacity: session.state === SshSession.Connecting ? 0.6 : 1.0
}
