.pragma library

// The keys the toolbar above the keyboard can show, in toolbar order.
// Ids are kept in the settings, so they must not change.
var all = [
    { id: "esc", label: "Esc", key: Qt.Key_Escape },
    { id: "tab", label: "Tab", key: Qt.Key_Tab },
    { id: "ctrl", label: "Ctrl", modifier: "ctrl" },
    { id: "alt", label: "Alt", modifier: "alt" },
    { id: "left", label: "←", key: Qt.Key_Left },
    { id: "up", label: "↑", key: Qt.Key_Up },
    { id: "down", label: "↓", key: Qt.Key_Down },
    { id: "right", label: "→", key: Qt.Key_Right },
    { id: "home", label: "Home", key: Qt.Key_Home },
    { id: "end", label: "End", key: Qt.Key_End },
    { id: "pgup", label: "PgUp", key: Qt.Key_PageUp },
    { id: "pgdn", label: "PgDn", key: Qt.Key_PageDown },
    { id: "del", label: "Del", key: Qt.Key_Delete },
    { id: "pipe", label: "|", text: "|" },
    { id: "slash", label: "/", text: "/" },
    { id: "dash", label: "-", text: "-" },
    { id: "tilde", label: "~", text: "~" },
    { id: "f1", label: "F1", key: Qt.Key_F1 },
    { id: "f2", label: "F2", key: Qt.Key_F2 },
    { id: "f3", label: "F3", key: Qt.Key_F3 },
    { id: "f4", label: "F4", key: Qt.Key_F4 },
    { id: "f5", label: "F5", key: Qt.Key_F5 },
    { id: "f6", label: "F6", key: Qt.Key_F6 },
    { id: "f7", label: "F7", key: Qt.Key_F7 },
    { id: "f8", label: "F8", key: Qt.Key_F8 },
    { id: "f9", label: "F9", key: Qt.Key_F9 },
    { id: "f10", label: "F10", key: Qt.Key_F10 },
    { id: "f11", label: "F11", key: Qt.Key_F11 },
    { id: "f12", label: "F12", key: Qt.Key_F12 }
]

function selected(ids) {
    var keys = []
    for (var i = 0; i < all.length; ++i) {
        if (ids.indexOf(all[i].id) >= 0)
            keys.push(all[i])
    }
    return keys
}
