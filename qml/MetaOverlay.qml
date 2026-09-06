import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omaplayer

// Pause-overlay info card: show name/series, episode mark, year, rating,
// genres and synopsis plus a deep link to the TMDb (or IMDb) page.
// Data comes from the unified `meta` (MetadataInfo) provider — Jellyfin items
// cost nothing (the server already enriched them), local files need the
// optional TMDb key. The card appears a moment after pause and vanishes
// immediately on resume.
Item {
    id: cardRoot

    required property var meta
    required property bool playing
    signal openSettings()

    visible: false
    z: 3

    // --- visibility: appear on pause (slightly delayed), vanish on resume --
    property bool showable: !meta.busy
                            && !playing
                            && meta.overlayEnabled
                            && meta.info !== undefined
                            && meta.info.state !== undefined
                            && meta.info.state.length > 0
                            && meta.info.state !== "error"

    Timer {
        id: appearDelay
        interval: 600
        running: cardRoot.showable
        repeat: false
        onTriggered: cardRoot.visible = cardRoot.showable
    }
    onPlayingChanged: {
        if (playing)
            visible = false
        else if (showable)
            appearDelay.restart()
    }
    onShowableChanged: {
        if (!showable)
            visible = false
        else
            appearDelay.restart()
    }

    // --- backdrop ----------------------------------------------------------
    Rectangle {
        anchors.fill: parent
        color: "#99000000"
        visible: cardRoot.visible && (!!meta.info.backdropUrl)
    }
    Image {
        anchors.fill: parent
        source: meta.info.backdropUrl ?? ""
        fillMode: Image.PreserveAspectCrop
        opacity: 0.30
        visible: cardRoot.visible && (!!meta.info.backdropUrl)
    }

    // --- card --------------------------------------------------------------
    Rectangle {
        id: metaCard
        visible: cardRoot.visible

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.bottomMargin: 84

        color: Colors.overlay
        border.color: Colors.border
        radius: Colors.radius

        // === ok: real data ===
        Loader {
            anchors.fill: parent
            active: meta.info.state === "ok"
            sourceComponent: RowLayout {
                spacing: 14
                Item { Layout.preferredWidth: 4 }

                Image {
                    Layout.preferredWidth: 112
                    Layout.preferredHeight: 168
                    Layout.alignment: Qt.AlignVCenter
                    source: meta.info.posterUrl ?? ""
                    fillMode: Image.PreserveAspectCrop
                    clip: true
                    visible: source.length > 0
                    Rectangle {
                        anchors.fill: parent
                        visible: parent.source.length === 0
                        color: Colors.track
                        Text {
                            anchors.centerIn: parent
                            text: "\uF03E"
                            font.pointSize: 26
                            color: Colors.textDim
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 3

                    Text {
                        text: meta.info.title ?? ""
                        color: Colors.overlayText
                        font.pointSize: 17
                        font.bold: true
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: (meta.info.subtitle ?? "").length > 0
                        text: meta.info.subtitle ?? ""
                        color: Colors.textDim
                        font.pointSize: 13
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: (meta.info.year ?? 0) > 0 || (meta.info.rating ?? 0) > 0
                                    || (meta.info.genres ?? "").length > 0
                        text: {
                            const bits = []
                            if (meta.info.year) bits.push(meta.info.year)
                            if (meta.info.rating) bits.push("\u2605 " + Number(meta.info.rating).toFixed(1))
                            if (meta.info.genres) bits.push(meta.info.genres)
                            return bits.join("   •   ")
                        }
                        color: Colors.textDim
                        font.pointSize: 12
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: (meta.info.overview ?? "").length > 0
                        text: meta.info.overview ?? ""
                        color: Colors.overlayText
                        font.pointSize: 12
                        wrapMode: Text.WordWrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.preferredHeight: implicitHeight
                    }
                    Row {
                        spacing: 6
                        visible: (meta.info.linkUrl ?? "").length > 0
                        Button {
                            text: meta.info.linkUrl.indexOf("imdb.com") >= 0 ? qsTr("IMDb megnyitása")
                                                                             : qsTr("TMDb megnyitása")
                            onClicked: Qt.openUrlExternally(meta.info.linkUrl)
                        }
                        Button {
                            text: qsTr("Bezárás")
                            onClicked: cardRoot.visible = false
                        }
                    }
                }
                Item { Layout.preferredWidth: 4 }
            }
        }

        // === needkey: TMDb key required for local files ===
        Loader {
            anchors.fill: parent
            active: meta.info.state === "needkey"
            sourceComponent: RowLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 10
                Text {
                    text: "\uF0C3"
                    font.pointSize: 18
                    color: Colors.accent
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: qsTr("Ehhez a fájlhoz metaadatok")
                        color: Colors.overlayText
                        font.pointSize: 14
                        font.bold: true
                    }
                    Text {
                        text: qsTr("Lokális fájlok info megjelenítéséhez adj meg ingyenes TMDB API kulcsot a beállításokban.")
                        color: Colors.textDim
                        font.pointSize: 12
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
                Button {
                    text: qsTr("Beállítások")
                    onClicked: cardRoot.openSettings()
                }
            }
        }

        // === notfound ===
        Loader {
            anchors.fill: parent
            active: meta.info.state === "notfound"
            sourceComponent: Text {
                text: qsTr("Ehhez a fájlhoz nem találtam metaadatot.")
                color: Colors.textDim
                font.pointSize: 13
                horizontalAlignment: Text.AlignHCenter
                anchors.centerIn: parent
            }
        }

        implicitHeight: 200
    }
}