import QtQuick 2.0
import Sailfish.Silica 1.0

HighlightImage {
    property string systemId

    readonly property var logos: ({
        "almalinux": "almalinux",
        "arch": "archlinux",
        "centos": "centos",
        "debian": "debian",
        "fedora": "fedora",
        "gentoo": "gentoo",
        "kali": "kalilinux",
        "linuxmint": "linuxmint",
        "macos": "apple",
        "nixos": "nixos",
        "ol": "oracle",
        "opensuse": "opensuse",
        "opensuse-leap": "opensuse",
        "opensuse-tumbleweed": "opensuse",
        "raspbian": "raspberrypi",
        "rhel": "redhat",
        "rocky": "rockylinux",
        "sles": "opensuse",
        "ubuntu": "ubuntu",
        "windows": "windows",
        "dragonfly": "unix",
        "freebsd": "unix",
        "illumos": "unix",
        "netbsd": "unix",
        "omnios": "unix",
        "openbsd": "unix",
        "openindiana": "unix",
        "solaris": "unix"
    })
    readonly property string logo: {
        var ids = systemId.split(" ")
        for (var i = 0; i < ids.length; ++i) {
            if (logos[ids[i]])
                return logos[ids[i]]
        }
        return systemId.length > 0 ? "linux" : ""
    }

    width: Theme.iconSizeSmallPlus
    height: width
    sourceSize.width: width
    sourceSize.height: height
    fillMode: Image.PreserveAspectFit
    source: logo.length > 0 ? Qt.resolvedUrl("../images/systems/" + logo + ".svg") : ""
    color: logo === "apple" ? Theme.primaryColor : "transparent"
}
