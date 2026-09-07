import QtQuick
import Omaplayer

// Live playback statistics overlay (in the spirit of mpv's stats script):
// render FPS, nominal video FPS, resolution, codec + pixel format, A/V sync,
// live video/audio bitrates, the active hardware decoder, dropped frames and
// container. Values are polled fresh every 500 ms only while the overlay is on
// screen (toggle: Ctrl+I), so nothing runs in idle. Glass styling matches the
// control bar.
Item {
    id: statsRoot

    required property MpvCore mpv

    property bool open: false
    property var data: ({})

    visible: open && Object.keys(data).length > 0
    width: 250
    height: col.height + 22
    z: 4

    x: parent.width - width - 14
    y: 10

    function rows(d) {
        const out = []
        const num = (x, dec) => x === undefined || x === null
                         ? ""
                         : Number(x).toFixed(dec)
        if (d.avsync !== undefined)
            out.push([qsTr("A/V szinkron"),
                      (d.avsync > 0 ? "+" : "") + num(d.avsync, 3) + " mp"])
        if (d.fps)
            out.push([qsTr("Megjelenítés"), num(d.fps, 2) + " FPS"])
        if (d.videoFps)
            out.push([qsTr("Videó képfrissítés"), num(d.videoFps, 3) + " FPS"])
        if (d.w && d.h)
            out.push([qsTr("Felbontás"), d.w + "\u00D7" + d.h])
        if (d.videoCodec)
            out.push([qsTr("Videó kodek"), d.videoCodec])
        if (d.pixelFormat)
            out.push([qsTr("Pixelformátum"), d.pixelFormat])
        if (d.videoBitrate)
            out.push([qsTr("Videó bitráta"), num(d.videoBitrate / 1000, 1) + " Mbps"])
        if (d.audioBitrate)
            out.push([qsTr("Hang bitráta"), num(d.audioBitrate / 1000, 1) + " Mbps"])
        if (d.hwdec)
            out.push([qsTr("Hardveres dekódolás"), d.hwdec])
        if (d.dropped)
            out.push([qsTr("Dobott kockák"), d.dropped])
        if (d.container)
            out.push([qsTr("Konténer"), d.container])
        if (d.buffering)
            out.push([qsTr("Puffer"), d.buffering + " %"])
        return out
    }

    Timer {
        interval: 500
        repeat: true
        running: statsRoot.open && statsRoot.visible && mpv.mediaReady()
        onTriggered: statsRoot.data = mpv.stats()
    }

    Rectangle {
        anchors.fill: parent
        radius: 14
        gradient: Gradient {
            GradientStop { position: 0.0;  color: "#b04a5a6e" }
            GradientStop { position: 0.30; color: "#e00d0d12" }
            GradientStop { position: 1.0;  color: "#f20d0d12" }
        }
        border.color: "#3dffffff"
        border.width: 1
    }

    Column {
        id: col
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 11
        anchors.bottomMargin: 11
        spacing: 3

        Repeater {
            model: statsRoot.rows(statsRoot.data)
            delegate: Row {
                width: parent.width
                spacing: 8
                Text {
                    text: modelData[0] + ":"
                    color: Colors.textDim
                    font.pixelSize: 11
                    width: 126
                    elide: Text.ElideRight
                }
                Text {
                    text: modelData[1]
                    color: Colors.overlayText
                    font.pixelSize: 11
                    width: parent.width - 126
                    elide: Text.ElideRight
                }
            }
        }
    }
}