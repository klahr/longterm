import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    readonly property int defaultFontSize: Theme.fontSizeExtraSmall

    allowedOrientations: Orientation.All

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: page.width

            PageHeader {
                title: qsTr("Settings")
            }

            SectionHeader {
                text: qsTr("Terminal")
            }

            ValueButton {
                label: qsTr("Color scheme")
                value: colorSchemes.name(appSettings.terminalColorScheme)
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("ColorSchemesPage.qml"))
            }

            Slider {
                width: parent.width
                label: qsTr("Font size")
                minimumValue: Math.round(Theme.fontSizeTiny / 2)
                maximumValue: Theme.fontSizeHuge
                stepSize: 1
                value: appSettings.terminalFontSize > 0 ? appSettings.terminalFontSize : page.defaultFontSize
                valueText: value === page.defaultFontSize ? qsTr("%1 px (default)").arg(value) : qsTr("%1 px").arg(value)
                onDownChanged: {
                    if (!down)
                        appSettings.terminalFontSize = value === page.defaultFontSize ? 0 : value
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: qsTr("You can also pinch the terminal to change the font size.")
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryHighlightColor
            }
        }

        VerticalScrollDecorator {}
    }
}
