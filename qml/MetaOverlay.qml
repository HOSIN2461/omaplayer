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

    // Text pill button in the player's visual language: translucent dark
    // backing, accent glow on hover, gentle squish on press.
    component MetaButton: Button {
        id: control
        implicitHeight: 26
        background: Rectangle {
            radius: 13
            color: control.hovered || control.pressed ? Colors.hover : "#26ffffff"
            border.color: control.hovered ? Colors.borderGlow : Colors.border
            border.width: 1
            scale: control.pressed ? 0.93 : 1
            Behavior on color { ColorAnimation { duration: 110 } }
            Behavior on scale { NumberAnimation { duration: 110; easing.type: Easing.OutCubic } }
        }
        contentItem: Text {
            text: control.text
            color: control.pressed ? Colors.accent
                 : control.hovered ? Colors.overlayText
                 : Colors.overlayText
            font.pixelSize: 11
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            Behavior on color { ColorAnimation { duration: 110 } }
        }
    }

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
        color: "#44000000"
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
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.bottomMargin: 68

        // Same translucent gradient backing as the control bar.
        gradient: Gradient {
            GradientStop { position: 0.00; color: "#9c4a5a6e" }
            GradientStop { position: 0.30; color: "#c90d0d12" }
            GradientStop { position: 1.00; color: "#e60d0d12" }
        }
        border.color: "#3dffffff"
        radius: Colors.radius

        // === ok: real data ===
        Loader {
            anchors.fill: parent
            active: meta.info.state === "ok"
            sourceComponent: RowLayout {
                spacing: 14
                Item { Layout.preferredWidth: 4 }

                Image {
                    id: posterImg
                    Layout.preferredWidth: 148
                    Layout.preferredHeight: 218
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 14
                    Layout.bottomMargin: 14
                    source: meta.info.posterUrl ?? ""
                    fillMode: Image.PreserveAspectCrop
                    clip: true
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
                    spacing: 4

                    Text {
                        text: meta.info.title ?? ""
                        color: Colors.overlayText
                        font.pointSize: 16
                        font.bold: true
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: (meta.info.subtitle ?? "").length > 0
                        text: meta.info.subtitle ?? ""
                        color: Colors.accent
                        font.pixelSize: 12
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
                        font.pixelSize: 11
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: (meta.info.overview ?? "").length > 0
                        text: meta.info.overview ?? ""
                        color: Colors.overlayText
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.preferredHeight: implicitHeight
                    }
                    Row {
                        spacing: 6
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        visible: (meta.info.linkUrl ?? "").length > 0
                        MetaButton {
                            text: meta.info.linkUrl.indexOf("imdb.com") >= 0 ? qsTr("IMDb megnyitása")
                                                                             : qsTr("TMDb megnyitása")
                            onClicked: Qt.openUrlExternally(meta.info.linkUrl)
                        }
                        MetaButton {
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
                    font.family: "Font Awesome 7 Free Solid"
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
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
                MetaButton {
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
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                anchors.centerIn: parent
            }
        }

        implicitHeight: 246
    }
}