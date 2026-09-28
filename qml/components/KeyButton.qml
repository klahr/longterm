import QtQuick 2.0
import Sailfish.Silica 1.0

// One key of the toolbar above the keyboard, see Keys.js
BackgroundItem {
    property var key
    // The terminal view the modifiers latch on
    property Item view

    readonly property bool latched: view !== null
                                    && ((key.modifier === "ctrl" && view.ctrlLatched)
                                        || (key.modifier === "alt" && view.altLatched))

    height: Theme.itemSizeExtraSmall
    highlighted: down || latched

    Label {
        anchors.centerIn: parent
        text: key.label
        font.pixelSize: Theme.fontSizeSmall
        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
    }
}
