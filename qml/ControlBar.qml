import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omaplayer

Rectangle {
    id: bar

    required property MpvCore mpv

    // Who is hovering anything inside the bar — the public flag Main.qml uses
    // so the auto-hide timer stays off while the pointer is here.
    readonly property bool anywhereHovered: barArea.containsMouse
    readonly property bool dragActive: seek.dragging

    height: 54
    radius: Colors.radius
    color: Colors.overlay
    border.color: Colors.border

    opacity: exposed ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    // Not visible while faded: lets clicks pass through to the gesture layer.
    visible: opacity > 0
    property bool exposed: true

    function show() { exposed = true; barTimerRestart() }
    function hide() { exposed = false }

    // Lightweight: ask the root to restart its auto-hide timer. In this
    // component's file scope the root is reachable via `window` parent chain;
    // expose it through the first caller instead — simplest is to let Main.qml
    // bump the timer itself on any mouse event over the bar. The MouseArea
    // below does that.
    function barTimerRestart() {
        var win = bar.parent
        while (win && !win.reTouch)
            win = win.parent
        if (win)
            win.reTouch()
    }

    MouseArea {
        id: barArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.topMargin: 7
        anchors.bottomMargin: 7
        spacing: 10

        // --- play / pause / stop -----------------------------------------
        IconButton {
            id: playBtn
            glyph: mpv.playing ? "\uF04C" : "\uF04B"   // FA pause / play
            tip: mpv.playing ? qsTr("Pause") : qsTr("Play")
            onClicked: mpv.togglePause()
        }

        IconButton {
            id: stopBtn
            glyph: "\uF04D"                                 // FA stop
            tip: qsTr("Stop")
            onClicked: mpv.stop()
        }

        // --- time + seek ---------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            color: "transparent"

            Row {
                id: times
                anchors.left: parent.left
                anchors.top: parent.top
                spacing: 4

                Text {
                    text: fmtTime(mpv.position)
                    color: Colors.overlayText
                    font.pixelSize: 11
                }
                Text {
                    text: " / " + fmtTime(mpv.duration)
                    color: Colors.textDim
                    font.pixelSize: 11
                }
            }

            Rectangle {
                id: track
                y: 12
                height: 6
                radius: 3
                color: Colors.border
                anchors {
                    left: parent.left
                    right: parent.right
                }
            }

            Rectangle {
                id: fill
                height: track.height
                radius: track.radius
                color: Colors.accent
                width: track.width * (mpv.duration > 0
                                       ? clampRatio(mpv.position / mpv.duration)
                                       : 0)
                anchors {
                    left: track.left
                    verticalCenter: track.verticalCenter
                }
            }

            // invisible wide strip for hover + drag
            MouseArea {
                id: seek
                property bool dragging: false
                property real target: 0.0

                anchors.fill: track
                anchors.topMargin: -16
                anchors.bottomMargin: -16
                hoverEnabled: true

                onPressed: mouse => {
                    dragging = true
                    setFromMouse(mouse.x)
                    mpv.pause()
                }
                onPositionChanged: mouse => {
                    if (dragging)
                        setFromMouse(mouse.x)
                    seekTip.position = clampRatio(mouse.x / parent.width)
                }
                onReleased: {
                    if (dragging) {
                        mpv.seek(target)
                        dragging = false
                    }
                }

                function setFromMouse(x) {
                    const ratio = clampRatio(x / parent.width)
                    target = ratio * mpv.duration
                    seekTip.position = ratio
                }
            }

            // Hover preview bubble.
            Rectangle {
                id: seekTip
                property real position: 0
                visible: seek.containsMouse && !seek.dragging && mpv.duration > 0
                width: 90
                height: 24
                radius: 5
                color: Colors.chrome
                border.color: Colors.border

                x: clampSeq(0, parent.width - width, track.x + position * track.width - width / 2)
                y: -32

                Text {
                    anchors.centerIn: parent
                    text: fmtTime(seekTip.position * mpv.duration)
                    color: Colors.overlayText
                    font.pixelSize: 12
                }
            }
        }

        // --- volume -------------------------------------------------------------
        IconButton {
            id: volBtn
            glyph: mpv.muted ? "\uF6A9"
                 : mpv.volume < 1 ? "\uF026"
                 : mpv.volume < 50 ? "\uF027"
                 : "\uF028"
            tip: qsTr("Mute")
            onClicked: mpv.toggleMute()
        }

        Slider {
            id: volSlider
            Layout.preferredWidth: 100
            from: 0
            to: 150
            value: mpv.volume
            onMoved: mpv.setVolume(value)
            ToolTip.visible: hovered || activeFocus
            ToolTip.text: Math.round(value) + " %"
        }
    }

    function clampRatio(x) { return Math.max(0, Math.min(1, x)) }
    function clampSeq(min, max, v) { return Math.max(min, Math.min(max, v)) }
    function fmtTime(s) {
        if (!s || !isFinite(s) || s < 0)
            return "--:--"
        const total = Math.round(s)
        const h = Math.floor(total / 3600)
        const m = Math.floor((total % 3600) / 60)
        const sec = total % 60
        const z = n => (n < 10 ? "0" : "") + n
        return (h > 0 ? h + ":" : "") + z(m) + ":" + z(sec)
    }
}