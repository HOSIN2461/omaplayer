import QtQuick
import QtQuick.Controls

// A flat square button showing a Font Awesome glyph. Font Awesome 7 is
// installed with fonts-fa-solid on Omarchy; a monochrome glyph keeps the bar
// consistent across light/dark content.
Button {
    id: control

    property string glyph: ""
    property string tip: ""

    implicitWidth: 34
    implicitHeight: 34

    contentItem: Text {
        text: control.glyph
        font.family: "Font Awesome 7 Free Solid"
        font.pixelSize: 15
        color: hovered ? Colors.overlayText : Colors.textDim
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Colors.radius
        color: control.hovered || control.pressed ? Colors.hover : "transparent"
    }

    ToolTip.visible: control.hovered && control.tip !== ""
    ToolTip.text: control.tip
    ToolTip.delay: 600
}