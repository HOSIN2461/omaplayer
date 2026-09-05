import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import Omaplayer

// Floating "pill" control bar. The outer Item is a full-width positioning
// container; the visible bar slides down and fades out when hidden, giving a
// soft floaty reveal instead of a hard pop.
//
// Layout (IINA-style): volume top-left, transport buttons centered, settings
// gear + hamburger menu top-right; the film timeline sits at the very bottom
// with the time readout centered underneath it.
Item {
    id: bar

    required property MpvCore mpv

    // Public show/hide state — Main.qml drives these.
    property bool exposed: true

    // Who is hovering anything inside the bar — the public flag Main.qml uses
    // so the auto-hide timer stays off while the pointer is here.
    readonly property bool anywhereHovered: barArea.containsMouse
    readonly property bool dragActive: seek.dragging

    height: 112

    opacity: exposed ? 1 : 0
    y: exposed ? 0 : 122
    enabled: exposed
    Behavior on opacity { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
    Behavior on y { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }

    function show() { exposed = true; reTouch() }
    function hide() { exposed = false }

    // Calls a named function on the window/scope that owns it (first ancestor
    // that has it) — the same walking trick reTouch() uses.
    function callMain(name, btn) {
        var w = bar.parent
        while (w && !w[name])
            w = w.parent
        if (w)
            w[name](btn)
    }

    // Restart the root auto-hide timer (see Main.qml) so an interaction while
    // the bar is up resets the countdown.
    function reTouch() {
        var win = bar.parent
        while (win && !win.reTouch)
            win = win.parent
        if (win)
            win.reTouch()
    }

    // --- the floating pill -------------------------------------------------
    Rectangle {
        id: pill
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        anchors.topMargin: 12
        anchors.bottomMargin: 8
        radius: Colors.radius
        color: Colors.overlay
        border.color: Colors.border
        border.width: 1
    }

    // Soft drop shadow that lifts the pill off the video.
    MultiEffect {
        anchors.fill: pill
        source: pill
        z: -1
        shadowEnabled: true
        shadowColor: "#000000"
        shadowBlur: 0.14
        shadowOpacity: 0.55
        shadowVerticalOffset: 6
    }

    MouseArea {
        id: barArea
        anchors.fill: pill
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    Column {
        anchors.fill: pill
        anchors.leftMargin: 14
        anchors.rightMargin: 10
        anchors.topMargin: 8
        anchors.bottomMargin: 6

        // --- control row ====  volume .. transport .. gear/menu ------
        RowLayout {
            spacing: 10

            // Volume (top-left, above the timeline).
            IconButton {
                id: volBtn
                glyph: mpv.muted ? "\uF6A9"
                     : mpv.volume < 1 ? "\uF026"
                     : mpv.volume < 50 ? "\uF027"
                     : "\uF028"
                tip: qsTr("Némítás")
                onClicked: mpv.toggleMute()
            }

            Slider {
                id: volSlider
                Layout.preferredWidth: 96
                from: 0
                to: 150
                value: mpv.volume
                onMoved: mpv.setVolume(value)
                ToolTip.visible: hovered || activeFocus
                ToolTip.text: Math.round(value) + " %"

                background: Item {
                    implicitHeight: 18
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: 5
                        radius: height / 2
                        color: Colors.track
                    }
                    Rectangle {
                        width: parent.width * volSlider.visualPosition
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        height: 5
                        radius: height / 2
                        color: Colors.accent
                    }
                }

                handle: Rectangle {
                    x: volSlider.leftPadding + volSlider.visualPosition * (volSlider.availableWidth - width)
                    y: volSlider.topPadding + (volSlider.availableHeight - height) / 2
                    width: 14
                    height: 14
                    radius: width / 2
                    color: volSlider.hovered || volSlider.dragging ? "#ffffff" : Colors.hover
                    border.color: Colors.accent
                    border.width: 2
                    Behavior on color { ColorAnimation { duration: 110 } }
                }
            }

            Item { Layout.fillWidth: true }

            // Transport — centered.
            IconButton {
                id: playBtn
                glyph: mpv.playing ? "\uF04C" : "\uF04B"   // FA pause / play
                tip: mpv.playing ? qsTr("Szünet") : qsTr("Lejátszás")
                onClicked: mpv.togglePause()
            }

            IconButton {
                id: stopBtn
                glyph: "\uF04D"                                 // FA stop
                tip: qsTr("Leállítás")
                onClicked: mpv.stop()
            }

            Item { Layout.fillWidth: true }

            // Settings gear + hamburger menu — top-right.
            IconButton {
                id: gearBtn
                glyph: "\uF013"                                 // FA cog
                tip: qsTr("Beállítások")
                onClicked: bar.callMain("openSettingsAt", gearBtn)
            }

            IconButton {
                id: menuBtn
                glyph: "\uF0C9"                                 // FA bars: playlist
                tip: qsTr("Lejátszási lista")
                onClicked: bar.callMain("openPlaylistAt", menuBtn)
            }
        }

        // --- film timeline, right at the bottom ----------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 4

            // Thin resting track that thickens while hovering/dragging.
            Rectangle {
                id: track
                y: parent.height / 2 - 2
                height: 4
                radius: height / 2
                color: Colors.track
                Behavior on height { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                anchors {
                    left: parent.left
                    right: parent.right
                }
            }

            Rectangle {
                id: fill
                height: track.height
                radius: track.height / 2
                color: Colors.accent
                width: track.width * (mpv.duration > 0
                                       ? clampRatio(mpv.position / mpv.duration)
                                       : 0)
                Behavior on width { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                anchors {
                    left: track.left
                    verticalCenter: track.verticalCenter
                }
            }

            Rectangle {
                id: thumb
                visible: seek.containsMouse || seek.dragging
                width: Colors.thumbSize
                height: Colors.thumbSize
                radius: width / 2
                color: "#ffffff"
                border.color: Colors.accent
                border.width: 2
                x: seekBoxWidth() - width / 2
                y: track.y + track.height / 2 - height / 2

                function seekBoxWidth() {
                    return track.x + (mpv.duration > 0
                        ? clampRatio(mpv.position / mpv.duration) * track.width
                        : 0)
                }
            }

            // invisible wide strip for hover + drag
            MouseArea {
                id: seek
                property bool dragging: false
                property real target: 0.0

                anchors.fill: parent
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
                width: 92
                height: 24
                radius: height / 2
                color: Colors.chrome
                border.color: Colors.border
                opacity: 1
                Behavior on opacity { NumberAnimation { duration: 90 } }

                x: clampSeq(0, parent.width - width, track.x + position * track.width - width / 2)
                y: 0

                Text {
                    anchors.centerIn: parent
                    text: fmtTime(seekTip.position * mpv.duration)
                    color: Colors.overlayText
                    font.pixelSize: 12
                }
            }
        }

        // --- time readout, centered under the timeline ----------------------
        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 1
            text: fmtTime(mpv.position) + " / " + fmtTime(mpv.duration)
            color: Colors.textDim
            font.pixelSize: 11
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