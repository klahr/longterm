import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import rs.r8.longterm 1.0

Page {
    id: page

    property var session
    // The folder to start in, the home folder when empty
    property string startPath
    readonly property var files: session.files

    function formatSize(bytes) {
        if (bytes < 1024)
            return qsTr("%1 B").arg(bytes)
        var units = [ qsTr("%1 kB"), qsTr("%1 MB"), qsTr("%1 GB"), qsTr("%1 TB") ]
        var value = bytes / 1024
        var unit = 0
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024
            ++unit
        }
        return units[unit].arg(value < 10 ? value.toFixed(1) : Math.round(value))
    }

    function upload(path) {
        var name = decodeURIComponent(String(path).split("/").pop())
        if (files.contains(name))
            Remorse.popupAction(page, qsTr("Replacing %1").arg(name), function() { files.upload(path, "", true) })
        else
            files.upload(path, "", false)
    }

    allowedOrientations: Orientation.All

    Component.onCompleted: files.open(startPath)

    Component {
        id: pickerComponent

        ContentPickerPage {
            title: qsTr("Upload to %1").arg(files.path)
            onSelectedContentPropertiesChanged: page.upload(selectedContentProperties.filePath)
        }
    }

    Component {
        id: nameDialogComponent

        Dialog {
            id: nameDialog

            property string title
            property string name
            // Called with the name typed
            property var done

            canAccept: nameField.text.trim().length > 0 && nameField.text.indexOf("/") < 0
            onAccepted: done(nameField.text.trim())

            Column {
                width: parent.width

                DialogHeader {
                    title: nameDialog.title
                }

                TextField {
                    id: nameField
                    width: parent.width
                    label: qsTr("Name")
                    placeholderText: label
                    text: nameDialog.name
                    focus: true
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.enabled: nameDialog.canAccept
                    EnterKey.onClicked: nameDialog.accept()
                }
            }
        }
    }

    SilicaListView {
        id: listView

        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: transferPanel.top
        }
        clip: true
        model: files

        PullDownMenu {
            busy: files.loading

            MenuItem {
                text: qsTr("Upload file")
                enabled: session.filesAvailable && files.path.length > 0
                onClicked: pageStack.animatorPush(pickerComponent)
            }

            MenuItem {
                text: qsTr("New folder")
                enabled: session.filesAvailable && files.path.length > 0
                onClicked: pageStack.animatorPush(nameDialogComponent, {
                    title: qsTr("New folder"),
                    done: function(name) { files.makeDirectory(name) }
                })
            }

            MenuItem {
                text: qsTr("Home folder")
                enabled: session.filesAvailable
                onClicked: files.open("")
            }

            MenuItem {
                text: qsTr("Refresh")
                enabled: session.filesAvailable
                onClicked: files.refresh()
            }
        }

        header: Column {
            width: listView.width

            PageHeader {
                title: files.path.length > 0 ? (files.path === "/" ? "/" : files.path.split("/").pop()) : qsTr("Files")
                description: files.path
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: text.length > 0
                text: !session.filesAvailable ? qsTr("Files need the SSH connection, connect again to use them")
                                              : files.errorString
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.errorColor
                bottomPadding: Theme.paddingMedium
            }

            BackgroundItem {
                visible: files.path.length > 0 && files.path !== "/"
                enabled: session.filesAvailable
                onClicked: files.up()

                Icon {
                    id: upIcon
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    source: "image://theme/icon-m-back"
                }

                Label {
                    anchors {
                        left: upIcon.right
                        leftMargin: Theme.paddingMedium
                        verticalCenter: parent.verticalCenter
                    }
                    text: qsTr("Parent folder")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }
        }

        delegate: ListItem {
            id: item

            readonly property string path: files.childPath(model.name)

            contentHeight: Theme.itemSizeMedium
            enabled: session.filesAvailable
            onClicked: {
                if (model.directory)
                    files.open(path)
                else
                    openMenu()
            }

            menu: ContextMenu {
                MenuItem {
                    visible: !model.directory
                    text: qsTr("Download")
                    onClicked: files.download(item.path)
                }

                MenuItem {
                    text: qsTr("Rename")
                    onClicked: {
                        var oldName = model.name
                        pageStack.animatorPush(nameDialogComponent, {
                            title: qsTr("Rename"),
                            name: oldName,
                            done: function(name) { files.rename(oldName, name) }
                        })
                    }
                }

                MenuItem {
                    text: qsTr("Delete")
                    onClicked: {
                        var name = model.name
                        item.remorseDelete(function() { files.remove(name) })
                    }
                }
            }

            Icon {
                id: icon
                x: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                source: model.directory ? "image://theme/icon-m-file-folder" : "image://theme/icon-m-file-other"
            }

            Column {
                anchors {
                    left: icon.right
                    leftMargin: Theme.paddingMedium
                    right: parent.right
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }

                Label {
                    width: parent.width
                    text: model.name
                    truncationMode: TruncationMode.Fade
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                Label {
                    width: parent.width
                    text: {
                        var details = []
                        if (!model.directory)
                            details.push(page.formatSize(model.size))
                        if (model.link)
                            details.push(qsTr("link"))
                        details.push(Format.formatDate(model.modified, Formatter.DurationElapsed))
                        return details.join(" · ")
                    }
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                }
            }
        }

        ViewPlaceholder {
            enabled: listView.count === 0 && !files.loading && files.path.length > 0
            text: qsTr("Empty folder")
            hintText: qsTr("Pull down to upload a file")
        }

        BusyIndicator {
            anchors.centerIn: parent
            size: BusyIndicatorSize.Large
            running: files.loading && listView.count === 0
        }

        VerticalScrollDecorator {}
    }

    // Transfers keep going after leaving the page, the notification says when they are done
    Column {
        id: transferPanel

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        Repeater {
            model: files.transfers

            Item {
                width: transferPanel.width
                height: progressBar.height

                ProgressBar {
                    id: progressBar

                    anchors {
                        left: parent.left
                        right: cancelButton.left
                    }
                    minimumValue: 0
                    maximumValue: Math.max(1, modelData.total)
                    value: modelData.bytes
                    indeterminate: index > 0 || modelData.total === 0
                    label: index > 0 ? qsTr("Waiting: %1").arg(modelData.name)
                                     : (modelData.upload ? qsTr("Uploading %1, %2 of %3") : qsTr("Downloading %1, %2 of %3"))
                                       .arg(modelData.name).arg(page.formatSize(modelData.bytes)).arg(page.formatSize(modelData.total))
                }

                IconButton {
                    id: cancelButton
                    anchors {
                        right: parent.right
                        rightMargin: Theme.paddingMedium
                        verticalCenter: parent.verticalCenter
                    }
                    icon.source: "image://theme/icon-m-clear"
                    onClicked: files.cancel(modelData.id)
                }
            }
        }
    }
}
