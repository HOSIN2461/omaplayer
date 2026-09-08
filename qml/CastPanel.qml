import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omaplayer

// Cast drawer (left edge): lists LAN targets — DLNA renderers, Google Cast
// (Chromecast-kompatibilis) and AirPlay receivers — so the user can pick one and
// start/stop casting without hunting through the tiny context menu. Same
// in-window rule as the other drawers — plain Item, no native popup,
// keyboard focus (G/L/Esc) stays on the main window.
Item {
    id: panel

    visible: false
    z: 50

    // The CastManager singleton lives in Main.qml; it is passed in by id.
    // NOTE: the property is named `manager`, NOT `cast` — binding it as
    // `cast: cast` inside Main.qml would self-reference and null it out.
    property CastManager manager
    property MpvCore mpv

    function open() {
        visible = true
        manager.startDiscovery()
    }
    function close() { visible = false }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: Colors.overlay
        border.color: Colors.border
        border.width: 1
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 6
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        anchors.bottomMargin: 6
        spacing: 6

        // --- header: title / refreshing state / close ----------------------
        Row {
            Layout.fillWidth: true
            spacing: 4

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Kivetítés")
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Colors.overlayText
                elide: Text.ElideMiddle
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: manager.discovering
                text: qsTr("keresés…")
                font.pixelSize: 10
                color: Colors.accent
            }
            Item { Layout.fillWidth: true }

            IconButton {
                implicitWidth: 26
                implicitHeight: 26
                glyph: "\uF00D"                       // FA xmark
                tip: qsTr("Bezárás (Esc)")
                onClicked: close()
            }
        }

        // --- active cast info ----------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            visible: manager.castUrl.length > 0
            height: 34
            radius: 8
            color: "#16ffffff"
            border.color: Colors.accent
            border.width: 1

            Row {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\uF6C4"                     // FA display-arrow-up
                    font.family: "Font Awesome 7 Free Solid"
                    font.pixelSize: 12
                    color: Colors.accent
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Vetítés: %1").arg(manager.activeDeviceName)
                    font.pixelSize: 12
                    color: Colors.overlayText
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("áll")
                    font.pixelSize: 11
                    color: Colors.accent
                }
            }
        }

        // --- preparing (transcoding) card ----------------------------------
        // While ffmpeg converts for Cast/AirPlay nothing streams yet — show
        // the target + a real progress bar + cancel, so the state is clear.
        Rectangle {
            Layout.fillWidth: true
            visible: manager.converting || manager.hlsBusy
            height: 64
            radius: 8
            color: "#16ffffff"
            border.color: Colors.accent
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                anchors.topMargin: 8
                anchors.bottomMargin: 8
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        Layout.fillWidth: true
                        text: manager.hlsBusy
                              ? qsTr("Élő indítása: %1").arg(manager.activeDeviceName)
                              : qsTr("Előkészítés: %1").arg(manager.activeDeviceName)
                        font.pixelSize: 12
                        color: Colors.overlayText
                        elide: Text.ElideRight
                    }
                    Text {
                        text: manager.hlsBusy
                              ? qsTr("élő")
                              : "%1%".arg(Math.round(manager.convertProgress * 100))
                        font.pixelSize: 11
                        color: Colors.accent
                        font.weight: Font.DemiBold
                    }
                    Rectangle {
                        Layout.preferredWidth: 62
                        Layout.preferredHeight: 24
                        radius: 12
                        color: cancelHov.containsMouse ? Colors.hover : "transparent"
                        border.color: Colors.accent
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("mégse")
                            font.pixelSize: 11
                            color: Colors.accent
                        }
                        MouseArea {
                            id: cancelHov
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: manager.stopCast()
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 5
                    radius: 3
                    color: "#22ffffff"
                    visible: !manager.hlsBusy
                    Rectangle {
                        width: parent.width * manager.convertProgress
                        height: parent.height
                        radius: 3
                        color: Colors.accent
                        Behavior on width { NumberAnimation { duration: 200 } }
                    }
                }
                Text {
                    visible: manager.hlsBusy
                    Layout.fillWidth: true
                    text: qsTr("Néhány másodperc; tekerés visszafelé korlátos")
                    font.pixelSize: 10
                    color: Colors.textDim
                }
            }
        }

        // --- AirPlay PIN row ------------------------------------------------
        // The TV shows a PIN (pair-pin-start); typing it here finishes HAP
        // pairing, then playback starts automatically.
        Rectangle {
            Layout.fillWidth: true
            visible: manager.airplayPairing
            height: 64
            radius: 8
            color: "#16ffffff"
            border.color: Colors.accent
            border.width: 1
            // Fresh PIN per round: never append to a stale entry.
            onVisibleChanged: if (visible) pinField.text = ""

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                anchors.topMargin: 8
                anchors.bottomMargin: 8
                spacing: 4

                Text {
                    Layout.fillWidth: true
                    text: qsTr("PIN a tévé képernyőjéről:")
                    font.pixelSize: 12
                    color: Colors.overlayText
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        id: pinField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 28
                        placeholderText: "1234"
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: RegularExpressionValidator {
                            regularExpression: /[0-9]{0,8}/
                        }
                        font.pixelSize: 13
                        onAccepted: manager.finishAirPlayPair(text)
                    }
                    Rectangle {
                        Layout.preferredWidth: 52
                        Layout.preferredHeight: 28
                        radius: 14
                        color: Colors.accent
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("OK")
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: "#0b0b0e"
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: manager.finishAirPlayPair(pinField.text)
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }
            }
        }

        // --- renderer list --------------------------------------------------
        // ListView (like the playlist), NOT Flickable+Repeater: a bare
        // Flickable swallows row taps as potential drags, so vetít clicks
        // never reached onClicked.
        ListView {
            id: devList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: manager.devices
            // Empty-state text sits on top while there is nothing to show.
            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                visible: !manager.discovering && devList.count === 0
                text: qsTr("Nem találunk eszközt a hálózaton (DLNA / Google Cast / AirPlay).")
                font.pixelSize: 11
                color: Colors.textDim
                wrapMode: Text.WordWrap
            }

            delegate: Rectangle {
                required property var modelData
                required property int index
                width: devList.width
                        height: 40
                        radius: 8
                        color: devMouse.containsMouse
                               ? Colors.hover
                               : (modelData.name === manager.activeDeviceName
                                  ? "#26ffffff" : "transparent")
                        border.color: modelData.name === manager.activeDeviceName
                                      ? Colors.accent : "transparent"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8

                            Rectangle {
                                width: 30; height: 30; radius: 8
                                Layout.alignment: Qt.AlignVCenter
                                color: "#10ffffff"
                                Text {
                                    anchors.centerIn: parent
                                    text: "\uF26C"     // FA desktop
                                    font.family: "Font Awesome 7 Free Solid"
                                    font.pixelSize: 12
                                    color: modelData.name === manager.activeDeviceName
                                            ? Colors.accent : Colors.overlayText
                                }
                            }
                            Column {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 2
                                Text {
                                    width: parent.width
                                    text: modelData.name
                                    font.pixelSize: 12
                                    color: devMouse.containsMouse ? "#fff" : Colors.overlayText
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    text: (modelData.type === "googlecast" ? "Google Cast · "
                                        : modelData.type === "airplay" ? "AirPlay · "
                                        : "DLNA · ") + modelData.host + ":" + modelData.port
                                    font.pixelSize: 10
                                    color: Colors.textDim
                                    elide: Text.ElideRight
                                }
                            }

                            // Pill button in the house style (cf. the "Kihagyás"
                            // button): solid accent for "kapcsolódás",
                            // outlined for the active "lekapcsolódás" state.
                            // Visual only — the whole row's MouseArea below
                            // handles the click.
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: 96
                                Layout.preferredHeight: 26
                                radius: 13
                                color: modelData.name === manager.activeDeviceName
                                       ? "transparent"
                                       : (devMouse.containsMouse || devMouse.pressed
                                          ? Colors.accentGlow : Colors.accent)
                                border.color: modelData.name === manager.activeDeviceName
                                              ? Colors.accent : "transparent"
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: 110 } }
                                Text {
                                    anchors.centerIn: parent
                                    // While preparing (transcode) or pairing
                                    // nothing streams yet — neutral "…"
                                    // instead of "lekapcsolódás" (click still
                                    // cancels via stopCast).
                                    text: {
                                        if (modelData.name !== manager.activeDeviceName)
                                            return qsTr("kapcsolódás")
                                        if (manager.converting || manager.airplayPairing)
                                            return "…"
                                        return qsTr("lekapcsolódás")
                                    }
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: modelData.name === manager.activeDeviceName
                                           ? Colors.accent : "#0b0b0e"
                                }
                            }
                        }

                        MouseArea {
                            id: devMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                const idx = index
                                if (modelData.name === manager.activeDeviceName) {
                                    manager.stopCast()
                                    panel.close()
                                } else {
                                    manager.stopCast()
                                    // requestCast (not cast): defers past the
                                    // click handler — cast() blocks on the
                                    // network, which is fatal inside JS eval.
                                    manager.requestCast(index, mpv.filePath,
                                                        mpv.position)
                                    // AirPlay may need a PIN typed here, so
                                    // keep the panel open for those rows.
                                    if (modelData.type !== "airplay")
                                        panel.close()
                                }
                            }
                            cursorShape: Qt.PointingHandCursor
                        }
                    } // delegate
        } // devList

        // --- scan / refresh -------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            height: 30
            radius: 15
            color: scanMouse.containsMouse ? Colors.hover : "#18ffffff"
            border.color: Colors.border
            border.width: 1
            Text {
                anchors.centerIn: parent
                text: manager.discovering
                      ? qsTr("Keresés…") : qsTr("Rendererek keresése")
                font.pixelSize: 11
                color: scanMouse.containsMouse ? Colors.overlayText : Colors.textDim
            }
            MouseArea {
                id: scanMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: manager.startDiscovery()
                cursorShape: Qt.PointingHandCursor
            }
        }
    }
}