import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    property string title
    property string license
    property url source
    property string text

    allowedOrientations: Orientation.All

    Component.onCompleted: {
        var request = new XMLHttpRequest()
        request.onreadystatechange = function() {
            if (request.readyState === XMLHttpRequest.DONE)
                page.text = request.responseText
        }
        request.open("GET", source)
        request.send()
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: page.width

            PageHeader {
                title: page.title
                description: page.license
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: page.text.length > 0 ? page.text : qsTr("The license text is not installed")
                wrapMode: Text.Wrap
                font.family: terminalFontFamily
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.secondaryHighlightColor
            }
        }

        VerticalScrollDecorator {}
    }
}
