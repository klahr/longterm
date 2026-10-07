import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    readonly property var components: [
        {
            name: "libssh",
            description: qsTr("SSH protocol, version %1").arg("0.12.2"),
            license: "GNU LGPL 2.1",
            url: "https://www.libssh.org/",
            file: "../../licenses/libssh.txt"
        },
        {
            name: "libvterm",
            description: qsTr("Terminal emulation, version %1, with memory safety fixes").arg("0.3.3"),
            license: "MIT",
            url: "https://www.leonerd.org.uk/code/libvterm/",
            file: "../../licenses/libvterm.txt"
        },
        {
            name: "Source Code Pro",
            description: qsTr("Terminal font"),
            license: "SIL Open Font License 1.1",
            url: "https://github.com/adobe-fonts/source-code-pro",
            file: "../../fonts/LICENSE.md"
        },
        {
            name: "Nerd Fonts",
            description: qsTr("Symbols for prompts and file listings, the icon sets keep their own licenses"),
            license: "MIT",
            url: "https://www.nerdfonts.com/",
            file: "../../fonts/NerdFonts-LICENSE.txt"
        },
        {
            name: "QR Code generator",
            description: qsTr("QR codes of host key fingerprints, by Project Nayuki"),
            license: "MIT",
            url: "https://www.nayuki.io/page/qr-code-generator-library",
            file: "../../licenses/qrcodegen.txt"
        },
        {
            name: "Devicon",
            description: qsTr("Operating system logos, which are trademarks of their owners"),
            license: "MIT",
            url: "https://devicon.dev/",
            file: "../images/systems/LICENSE"
        }
    ]

    allowedOrientations: Orientation.All

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: page.width

            PageHeader {
                title: qsTr("About")
            }

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Theme.iconSizeExtraLarge
                height: width
                sourceSize.width: width
                sourceSize.height: height
                source: "/usr/share/icons/hicolor/172x172/apps/longterm.png"
            }

            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Longterm"
                font.pixelSize: Theme.fontSizeExtraLarge
                color: Theme.highlightColor
            }

            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Version %1").arg(Qt.application.version)
                color: Theme.secondaryHighlightColor
            }

            Item {
                width: parent.width
                height: Theme.paddingLarge
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("An SSH terminal for Sailfish OS")
                wrapMode: Text.Wrap
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                text: "© 2026 Joachim Klahr"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
            }

            Item {
                width: parent.width
                height: Theme.paddingLarge
            }

            ButtonLayout {
                Button {
                    text: qsTr("License")
                    onClicked: pageStack.animatorPush(Qt.resolvedUrl("LicensePage.qml"), {
                        title: "Longterm",
                        license: "GNU GPL 3",
                        source: Qt.resolvedUrl("../../licenses/longterm.txt")
                    })
                }
                Button {
                    text: qsTr("Source code")
                    onClicked: Qt.openUrlExternally("https://github.com/klahr/longterm")
                }
            }

            SectionHeader {
                text: qsTr("Third-party components")
            }

            Repeater {
                model: page.components

                ListItem {
                    id: componentItem

                    width: column.width
                    contentHeight: Theme.itemSizeLarge
                    menu: Component {
                        ContextMenu {
                            MenuItem {
                                text: qsTr("Open website")
                                onClicked: Qt.openUrlExternally(modelData.url)
                            }
                        }
                    }
                    onClicked: pageStack.animatorPush(Qt.resolvedUrl("LicensePage.qml"), {
                        title: modelData.name,
                        license: modelData.license,
                        source: Qt.resolvedUrl(modelData.file)
                    })

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin

                        Label {
                            width: parent.width
                            text: modelData.name
                            truncationMode: TruncationMode.Fade
                            color: componentItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                        }
                        Label {
                            width: parent.width
                            text: modelData.description
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: componentItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                        }
                        Label {
                            width: parent.width
                            text: modelData.license
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: componentItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                        }
                    }
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
