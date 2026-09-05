import QtQuick
import QtQuick.Controls

// A round (pill) button showing a Font Awesome glyph. Font Awesome 7 is
// installed with fonts-fa-solid on Omarchy; a monochrome glyph keeps the bar
// consistent across light/dark content. Modern feel comes from the pill shape,
// the soft accent glow on hover and the pressed "squish".
Button {
    id: control

    property string glyph: ""
    property string tip: ""

    implicitWidth: 38
    implicitHeight: 38

    contentItem: Text {
        text: control.glyph
        font.family: "Font Awesome 7 Free Solid"
        font.pixelSize: 16
        color: control.pressed ? Colors.accent
             : control.hovered ? Colors.overlayText
             : Colors.textDim
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        Behavior on color { ColorAnimation { duration: 110 } }
    }

    background: Rectangle {
        radius: control.width / 2
        color: control.hovered || control.pressed ? Colors.hover : "transparent"
        border.color: control.hovered ? Colors.borderGlow : "transparent"
        border.width: 1
        scale: control.pressed ? 0.90 : 1
        Behavior on scale { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }
        Behavior on color { ColorAnimation { duration: 110 } }
    }

    ToolTip.visible: control.hovered && control.tip !== ""
    ToolTip.text: control.tip
    ToolTip.delay: 600
}