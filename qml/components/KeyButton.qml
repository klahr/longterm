import QtQuick 2.0
import Sailfish.Silica 1.0

// One key of the toolbar above the keyboard, see Keys.js
BackgroundItem {
    id: button

    property var key
    // The terminal view the modifiers latch on
    property Item view

    readonly property bool latched: view !== null
                                    && ((key.modifier === "ctrl" && (view.ctrlLatched || view.ctrlLocked))
                                        || (key.modifier === "alt" && (view.altLatched || view.altLocked)))
    readonly property bool locked: view !== null
                                   && ((key.modifier === "ctrl" && view.ctrlLocked)
                                       || (key.modifier === "alt" && view.altLocked))

    // The arrows key sends one arrow for each step dragged
    property real dragX
    property real dragY
    readonly property real dragStep: Theme.itemSizeExtraSmall / 2

    height: Theme.itemSizeExtraSmall
    highlighted: down || latched
    // The row of keys scrolls sideways, which would take the drag
    preventStealing: key.action === "arrows"

    onPressed: {
        dragX = mouse.x
        dragY = mouse.y
    }
    onPositionChanged: {
        if (key.action !== "arrows" || !view)
            return
        while (Math.abs(mouse.x - dragX) >= dragStep || Math.abs(mouse.y - dragY) >= dragStep) {
            var dx = mouse.x - dragX
            var dy = mouse.y - dragY
            if (Math.abs(dx) >= Math.abs(dy)) {
                view.sendKey(dx > 0 ? Qt.Key_Right : Qt.Key_Left)
                dragX += dx > 0 ? dragStep : -dragStep
            } else {
                view.sendKey(dy > 0 ? Qt.Key_Down : Qt.Key_Up)
                dragY += dy > 0 ? dragStep : -dragStep
            }
        }
    }

    Label {
        anchors.centerIn: parent
        text: key.label
        font.pixelSize: Theme.fontSizeSmall
        font.underline: button.locked
        font.bold: button.locked
        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
    }
}
