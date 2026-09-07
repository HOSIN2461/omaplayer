import QtQuick
import Omaplayer

// Lightweight notification stack. Anchor the host anywhere (typically
// bottom-right, above the control bar); toasts fade in/out and stack
// upward from the bottom edge.
//
//   Toast {
//       id: toasts
//       anchors.right: parent.right
//       anchors.bottom: bar.top
//   }
//   toasts.show("Sikeres mentés", "ok")
//
// Types drive the accent: "info", "ok", "err". Pass a custom duration in
// ms (0 keeps the card until clicked).
Item {
    id: root

    property int maxToasts: 4
    property int defaultDuration: 3400
    property int cardWidth: 280

    width: cardWidth
    height: stack.childrenRect.height

    function show(text, type, duration) {
        const dur = duration === undefined ? root.defaultDuration : duration
        const socket = type === "err" ? "#ff5c5c"
                    : type === "ok"  ? "#3ecf8e"
                    : Colors.accent
        if (stack.children.length >= root.maxToasts)
            stack.children[0].destroy()

        const card = Qt.createQmlObject(`
            import QtQuick 2.15
            import Omaplayer 1.0
            Rectangle {
                id: card
                width: root.cardWidth
                height: 34
                radius: 11
                color: "#1d1d24"
                border.color: "#45454f"
                border.width: 1
                opacity: 0
                Behavior on opacity { NumberAnimation { duration: 140 } }
                Behavior on height { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                MouseArea {
                    anchors.fill: parent
                    onClicked: card.opacity = 0
                }
                Rectangle {
                    width: 3
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    height: parent.height - 12
                    radius: 2
                    color: "${socket}"
                }
                Text {
                    id: tip
                    width: parent.width - 22
                    anchors { left: parent.left; leftMargin: 14; top: parent.top; topMargin: 8 }
                    text: ${JSON.stringify(text)}
                    color: Colors.overlayText
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                Timer {
                    interval: ${dur}
                    repeat: false
                    running: ${dur > 0}
                    onTriggered: card.opacity = 0
                }
                onOpacityChanged: {
                    if (opacity === 0)
                        card.destroy()
                }
                Component.onCompleted: {
                    height = Math.max(34, tip.height + 16)
                    opacity = 1
                }
            }`, stack, "/ToastCard.qml")
    }

    Column {
        id: stack
        anchors.bottom: parent.bottom
        width: parent.width
        spacing: 8
    }
}