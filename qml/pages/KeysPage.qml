import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page

    allowedOrientations: Orientation.All

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: keyStore

        header: PageHeader {
            title: qsTr("Keys")
            description: keyStore.errorString
        }

        PullDownMenu {
            busy: keyStore.busy
            MenuItem {
                text: qsTr("Import key")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("ImportKeyDialog.qml"))
            }
            MenuItem {
                text: qsTr("Generate key")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("GenerateKeyDialog.qml"))
            }
        }

        ViewPlaceholder {
            enabled: listView.count === 0 && !keyStore.busy
            text: qsTr("No keys")
            hintText: qsTr("Pull down to generate or import a key")
        }

        delegate: ListItem {
            id: delegate

            contentHeight: Theme.itemSizeMedium
            menu: Component {
                ContextMenu {
                    MenuItem {
                        text: qsTr("Copy public key")
                        onClicked: Clipboard.text = model.publicKey
                    }
                    MenuItem {
                        text: qsTr("Delete")
                        onClicked: {
                            // The delegate's context is gone when the remorse timer fires
                            var store = keyStore
                            var keyId = model.keyId
                            delegate.remorseDelete(function() { store.removeKey(keyId) })
                        }
                    }
                }
            }
            onClicked: pageStack.animatorPush(Qt.resolvedUrl("KeyPage.qml"), {
                name: model.name,
                publicKey: model.publicKey,
                fingerprint: model.fingerprint
            })

            Column {
                anchors.verticalCenter: parent.verticalCenter
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin

                Label {
                    width: parent.width
                    text: model.name
                    truncationMode: TruncationMode.Fade
                    color: delegate.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    text: model.fingerprint
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: delegate.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
