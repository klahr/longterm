import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components/Keys.js" as Keys

Page {
    id: page

    allowedOrientations: Orientation.All

    // Keeps the toolbar in catalog order whatever order keys are switched in
    function setKey(id, shown) {
        var current = appSettings.toolbarKeys
        var keys = []
        for (var i = 0; i < Keys.all.length; ++i) {
            var key = Keys.all[i].id
            if (key === id ? shown : current.indexOf(key) >= 0)
                keys.push(key)
        }
        appSettings.toolbarKeys = keys
    }

    SilicaListView {
        anchors.fill: parent
        model: Keys.all

        header: PageHeader {
            title: qsTr("Toolbar keys")
            description: qsTr("The row scrolls when the keys do not fit")
        }

        delegate: TextSwitch {
            text: modelData.label
            automaticCheck: false
            checked: appSettings.toolbarKeys.indexOf(modelData.id) >= 0
            onClicked: page.setKey(modelData.id, !checked)
        }

        VerticalScrollDecorator {}
    }
}
