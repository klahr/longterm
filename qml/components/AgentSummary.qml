import QtQuick 2.0
import Sailfish.Silica 1.0

// How many sessions' coding agents are working, waiting for input or done
Label {
    readonly property var parts: {
        var list = []
        if (sessionManager.waitingCount > 0)
            list.push(qsTr("%n waiting", "", sessionManager.waitingCount))
        if (sessionManager.workingCount > 0)
            list.push(qsTr("%n working", "", sessionManager.workingCount))
        if (sessionManager.doneCount > 0)
            list.push(qsTr("%n done", "", sessionManager.doneCount))
        return list
    }

    visible: parts.length > 0
    text: parts.join(" · ")
    truncationMode: TruncationMode.Fade
    // The colors of StatusDot, the one that needs attention first
    color: sessionManager.waitingCount > 0 ? "#e5484d" : sessionManager.workingCount > 0 ? "#3b82f6" : "#30a46c"
}
