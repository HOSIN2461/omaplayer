import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omaplayer

// DLNA cast drawer (left edge): lists the MediaRenderers found on the LAN so
// the user can pick one and start/stop Casting without hunting through the tiny
// context menu. Same in-window rule as the other drawers — plain Item, no
// native popup, keyboard focus (G/L/Esc) stays on the main window.
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
                text: qsTr("Kivetítés (DLNA)")
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

        // --- renderer list --------------------------------------------------
        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: devCol.width
            contentHeight: devCol.implicitHeight

            Column {
                id: devCol
                width: devFlick.width
                spacing: 4

                // Placeholder while the list is empty.
                Text {
                    width: devCol.width
                    visible: !manager.discovering && manager.devices.length === 0
                    text: qsTr("Nem találunk renderert a hálózaton.")
                    font.pixelSize: 11
                    color: Colors.textDim
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: manager.devices
                    delegate: Rectangle {
                        required property var modelData
                        width: devCol.width
                        height: 40
                        radius: 8
                        color: devMouse.containsMouse
                               ? (modelData.name === manager.activeDeviceName
                                  ? Colors.accent : Colors.hover)
                               : (modelData.name === manager.activeDeviceName
                                  ? "#26ffffff" : "transparent")
                        border.color: modelData.name === manager.activeDeviceName
                                      ? Colors.accent : "transparent"
                        border.width: 1

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8

                            Rectangle {
                                width: 30; height: 30; radius: 8
                                anchors.verticalCenter: parent.verticalCenter
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
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                Text {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    color: devMouse.containsMouse ? "#fff" : Colors.overlayText
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: modelData.host + ":" + modelData.port
                                    font.pixelSize: 10
                                    color: Colors.textDim
                                }
                            }
                            Item { width: parent.width }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.name === manager.activeDeviceName
                                      ? qsTr("áll") : qsTr("vetít")
                                font.pixelSize: 11
                                color: Colors.accent
                                font.weight: Font.DemiBold
                            }
                        }

                        MouseArea {
                            id: devMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                const idx = model.index
                                if (modelData.name === manager.activeDeviceName) {
                                    manager.stopCast()
                                    panel.close()
                                } else {
                                    manager.stopCast()
                                    manager.cast(idx, mpv.filePath, mpv.position)
                                    panel.close()
                                }
                            }
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }
            }
        }

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