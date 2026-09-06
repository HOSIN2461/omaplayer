import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Omaplayer

// Floating "liquid glass" control bar. The outer Item is a full-width
// positioning container; the glass pill slides down and fades out when
// hidden, giving a soft floaty reveal instead of a hard pop.
//
// Layout (IINA-style row): volume slider on the left, then rewind /
// play-pause / stop / forward; the playlist + settings sit at the right
// edge. The film timeline sits at the very bottom of the pill with the
// elapsed time readout at its left end and the total duration at its right
// end. The glass look is a translucent gradient + top sheen + soft drop
// shadow; all sliders and buttons use the blue accent.
Item {
    id: bar

    required property MpvCore mpv

    // Public show/hide state — Main.qml drives these.
    property bool exposed: true

    // Who is hovering anything inside the bar — the public flag Main.qml uses
    // so the auto-hide timer stays off while the pointer is here.
    readonly property bool anywhereHovered: barArea.containsMouse
    readonly property bool dragActive: seek.dragging

    // Height follows the window so a small floating window keeps its video
    // visible; the compact layout (84px) still fits every control.
    height: Math.min(104, Math.max(82, parent.height * 0.32))

    opacity: exposed ? 1 : 0
    y: exposed ? 0 : 124
    enabled: exposed
    Behavior on opacity { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
    Behavior on y { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }

    function show() { exposed = true; if (bar.onRetouch) bar.onRetouch() }
    function hide() { exposed = false }

    // Direct function references injected by Main.qml. Walking up the parent
    // chain to find root methods is unreliable (the contentItem is the last
    // Item in that chain, so the ApplicationWindow object is never reached),
    // which is why the settings/playlist popups silently never opened before.
    property var onSettings: null
    property var onPlaylist: null
    property var onJellyfin: null
    property var onRetouch: null
    // Flash an action into the top-left indicator (glyph, label).
    property var onFlash: null
    function flash(glyph, label) { if (bar.onFlash) bar.onFlash(glyph, label) }

    // Icon by explicit (volume, muted) so the flash never reads the async
    // mpv mirror (mpv.volume/mpv.muted update on a later property event).
    function volumeGlyph(vol, muted) {
        if (muted) return "\uF6A9"
        return vol < 1 ? "\uF026"
             : vol < 50 ? "\uF027"
             : "\uF028"
    }

    // --- the liquid-glass pill -------------------------------------------
    Rectangle {
        id: pill
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 8
        anchors.bottomMargin: 4
        radius: 18
        // Frosted gradient: a light blue-grey sheen on top melting into the
        // dark body, with video faintly visible through the translucent core.
        gradient: Gradient {
            GradientStop { position: 0.0;  color: "#9c4a5a6e" }
            GradientStop { position: 0.30; color: "#c90d0d12" }
            GradientStop { position: 1.0;  color: "#e60d0d12" }
        }
        border.color: "#3dffffff"
        border.width: 1
    }

    // Soft drop shadow that lifts the pill off the video.
    MultiEffect {
        anchors.fill: pill
        source: pill
        z: -1
        shadowEnabled: true
        shadowColor: "#000000"
        shadowBlur: 0.16
        shadowOpacity: 0.6
        shadowVerticalOffset: 6
    }

    MouseArea {
        id: barArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    Column {
        anchors.fill: pill
        anchors.leftMargin: 12
        anchors.rightMargin: 8
        anchors.topMargin: 4
        anchors.bottomMargin: 2
        spacing: 1

        // --- control row: volume .. rewind/play/stop/forward .. right-edge
        // playlist/settings. The core play/stop + gear/menu always stay; the
        // volume slider and the prev/next pair collapse on a narrow window.
        // This row flexes to fill the pill so the buttons always sit vertically
        // centered while the scrubber stays pinned to the bottom edge.
        Item {
            width: parent.width
            height: parent.height - 23

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 2
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                IconButton {
                    id: volBtn
                    visible: pill.width >= 484
                    implicitWidth: 34
                    implicitHeight: 34
                    glyph: bar.volumeGlyph(mpv.volume, mpv.muted)
                    tip: qsTr("Némítás (M)")
                    onClicked: {
                        const muted = !mpv.muted
                        mpv.toggleMute()
                        bar.flash(bar.volumeGlyph(mpv.volume, muted),
                                  muted ? qsTr("Némítva") : qsTr("Hang"))
                    }
                }

                Slider {
                    id: volSlider
                    visible: pill.width >= 484
                    width: 92
                    height: 40
                    from: 0
                    to: 150
                    value: mpv.volume
                    onMoved: {
                        mpv.setVolume(value)
                        bar.flash(bar.volumeGlyph(value, mpv.muted), Math.round(value) + " %")
                    }

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
                            Behavior on width { NumberAnimation { duration: 90 } }
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
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                IconButton {
                    id: prevBtn
                    visible: pill.width >= 324
                    glyph: "\uF048"                                 // FA backward-step
                    tip: qsTr("Előző (P)")
                    onClicked: {
                        mpv.playlistPrevious()
                        bar.flash("\uF048", qsTr("Előző"))
                    }
                }

                IconButton {
                    id: playBtn
                    implicitWidth: 40
                    implicitHeight: 40
                    glyph: mpv.playing ? "\uF04C" : "\uF04B"   // FA pause / play
                    tip: mpv.playing ? qsTr("Szünet (Szóköz)") : qsTr("Lejátszás (Szóköz)")
                    onClicked: {
                        const willPause = mpv.playing
                        mpv.togglePause()
                        bar.flash(willPause ? "\uF04C" : "\uF04B",
                                  willPause ? qsTr("Szünet") : qsTr("Lejátszás"))
                    }
                }

                IconButton {
                    id: nextBtn
                    visible: pill.width >= 324
                    glyph: "\uF051"                                 // FA forward-step
                    tip: qsTr("Következő (N)")
                    onClicked: {
                        mpv.playlistNext()
                        bar.flash("\uF051", qsTr("Következő"))
                    }
                }
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                IconButton {
                    id: menuBtn
                    implicitWidth: 34
                    implicitHeight: 34
                    glyph: "\uF00B"                                 // FA list-ul: playlist
                    tip: qsTr("Lejátszási lista (L)")
                    onClicked: {
                        if (bar.onPlaylist) bar.onPlaylist()
                        bar.flash("\uF00B", qsTr("Lejátszási lista"))
                    }
                }

                IconButton {
                    id: jellyBtn
                    implicitWidth: 34
                    implicitHeight: 34
                    glyph: "\uF03D"                                 // FA video: media
                    tip: qsTr("Jellyfin (J)")
                    onClicked: {
                        if (bar.onJellyfin) bar.onJellyfin()
                        bar.flash("\uF03D", qsTr("Jellyfin"))
                    }
                }

                IconButton {
                    id: gearBtn
                    implicitWidth: 34
                    implicitHeight: 34
                    glyph: "\uF013"                                 // FA cog
                    tip: qsTr("Beállítások (G)")
                    onClicked: {
                        if (bar.onSettings) bar.onSettings()
                        bar.flash("\uF013", qsTr("Beállítások"))
                    }
                }
            }
        }
        // --- scrubber: timeline with elapsed time at the left end and total
        // duration at the right end; draggable to seek in the media ---------
        Row {
            width: parent.width
            height: 22
            spacing: 8

            Text {
                width: 52
                anchors.verticalCenter: parent.verticalCenter
                text: fmtTime(mpv.position)
                color: Colors.textDim
                font.pixelSize: 11
                horizontalAlignment: Text.AlignLeft
            }

            Item {
                id: scrubWrap
                width: parent.width - 52 - 52 - 16
                height: parent.height

                property real targetRatio: 0.0

                // Hover/drag preview bubble floating above the track.
                Rectangle {
                    id: seekTip
                    visible: seek.containsMouse && mpv.duration > 0
                    width: 92
                    height: 22
                    radius: height / 2
                    color: Colors.overlay
                    border.color: Colors.border

                    x: clampSeq(0, scrubWrap.width - width, scrubWrap.targetRatio * track.width - width / 2)
                    y: -2

                    Text {
                        anchors.centerIn: parent
                        text: fmtTime(scrubWrap.targetRatio * mpv.duration)
                        color: Colors.overlayText
                        font.pixelSize: 11
                    }
                }

                // Resting track; thickens while hovering/dragging.
                Rectangle {
                    id: track
                    y: parent.height / 2 + 2
                    height: (seek.containsMouse || seek.dragging) ? 8 : 5
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

                // Full-height invisible strip for hover + drag.
                MouseArea {
                    id: seek
                    property bool dragging: false
                    property real target: 0.0
                    property bool pausedForSeek: false

                    anchors.fill: parent
                    hoverEnabled: true

                    onPressed: mouse => {
                        dragging = true
                        setFromMouse(mouse.x)
                        // Pause only while dragging so the position preview stays
                        // stable; resume right after the seek if it was playing.
                        pausedForSeek = mpv.playing
                        mpv.pause()
                    }
                    onPositionChanged: mouse => {
                        if (dragging)
                            setFromMouse(mouse.x)
                        else
                            scrubWrap.targetRatio = clampRatio(mouse.x / parent.width)
                    }
                    onReleased: {
                        if (dragging)
                            finishSeek()
                    }
                    onCanceled: {
                        if (dragging)
                            finishSeek()
                    }

                    function finishSeek() {
                        mpv.seek(target)
                        if (pausedForSeek)
                            mpv.play()
                        pausedForSeek = false
                        dragging = false
                        bar.flash("\uF017", fmtTime(target))
                    }

                    function setFromMouse(x) {
                        const ratio = clampRatio(x / parent.width)
                        target = ratio * mpv.duration
                        scrubWrap.targetRatio = ratio
                    }
                }
            }

            Text {
                width: 52
                anchors.verticalCenter: parent.verticalCenter
                text: fmtTime(mpv.duration)
                color: Colors.textDim
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    function clampRatio(x) { return Math.max(0, Math.min(1, x)) }
    function clampSeq(a, v, b) { return Math.max(a, Math.min(v, b)) }
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