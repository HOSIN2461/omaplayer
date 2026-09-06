import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Omaplayer

ApplicationWindow {
    id: root

    visible: true
    width: 640
    height: 400
    minimumWidth: 320
    minimumHeight: 200
    color: "black"
    property MpvCore mpv: MpvCore {}
    property bool autoPip: Qt.application.arguments.indexOf("--pip") >= 0
    // The player is a compact floating window by design (float + pin via the
    // Hyprland rule); the much bigger "windowed" mode below would feel like a
    // second PiP, so there is no dedicated PiP toggle any more.
    property int pipW: 360
    property int pipH: 203

    Component.onCompleted: {
        // --pip is accepted for backwards compatibility; the player is now
        // always a compact floating window (float + pin via Hyprland rule).
    }

    title: mpv.mediaTitle.length > 0 ? mpv.mediaTitle : qsTr("Omaplayer")

    MpvVideoItem {
        id: video

        anchors.fill: parent
    }

    // One control surface: a translucent bar pinned to the bottom. It fades
    // away after moments of no interaction while video plays, and returns on
    // any input. It sits above the gesture layer so clicks on controls do not
    // double-toggle playback.
    ControlBar {
        id: bar
        mpv: root.mpv

        z: 2 // above the gesture layer so seek/buttons get the pointer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        // Direct wiring — the old parent-chain walk from the bar could not
        // reach these Main.qml methods and silently did nothing.
        onSettings: () => openSettings()
        onPlaylist: () => openPlaylist()
        onRetouch: () => reTouch()
    }

    // Auto-hide: fade the bar away after idle, keep it while the pointer or a
    // seek drag is on it. It stays up while paused so controls remain handy.
    Timer {
        id: barTimer
        interval: 3200
        running: mpv.playing && bar.exposed
        repeat: true
        onTriggered: {
            if (!bar.anywhereHovered && !bar.dragActive)
                bar.hide()
        }
    }

    function reTouch() {
        barTimer.restart()
    }

    // --- interaction layer, below the control bar -----------------------------
    MouseArea {
        id: gestures

        anchors.fill: parent
        z: 1
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true

        onClicked: mouse => {
            bar.show()
            if (urlDialog.visible) { urlDialog.close(); return }
            if (updatePopup.visible) { updatePopup.close(); return }
            if (settingsMenu.visible) { settingsMenu.close(); return }
            if (playlistPanel.visible) { playlistPanel.close(); return }
            if (contextMenu.visible) { contextMenu.close(); return }
            if (mouse.button === Qt.RightButton)
                contextMenu.openAt(mouse.x, mouse.y)
            else
                mpv.togglePause()
        }
        onDoubleClicked: mouse => {
            if (mouse.button === Qt.LeftButton)
                root.toggleFullscreen()
        }
        // Celluloid-style wheel: vertical scroll = volume, horizontal (or
        // Shift+scroll) = seek. A pip floating window is small, so volume is
        // the most-used gesture; the seek timeline stays precise for seeking.
        onWheel: wheel => {
            const horiz = wheel.angleDelta.x !== 0 || (wheel.modifiers & Qt.ShiftModifier)
            const delta = horiz ? wheel.angleDelta.x !== 0 ? wheel.angleDelta.x : wheel.angleDelta.y
                                : wheel.angleDelta.y
            if (horiz)
                mpv.seekRelative(delta > 0 ? 10 : -10)
            else
                mpv.setVolume(Math.max(0, Math.min(150, mpv.volume + delta / 8)))
        }

        // Any mouse movement over the player surfaces the controls; they fade
        // away again once the auto-hide timer elapses while idle.
        onPositionChanged: mouse => {
            bar.show()
        }
    }

    // --- context menu (custom popup — the Qt Controls Menu turned out to
    // render an empty box with delegates, so this is 100% ours) ----------------
    component MenuRow: Rectangle {
        id: row
        property string rowText: ""
        property string glyph: ""
        property var onActivate: null

        width: contextMenu.width
        height: 32
        radius: 7
        color: mouse.containsMouse ? Colors.hover : "transparent"
        Behavior on color { ColorAnimation { duration: 90 } }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            onClicked: {
                contextMenu.close()
                if (row.onActivate)
                    row.onActivate()
            }
        }

        RowLayout {
            anchors.fill: parent
            spacing: 10
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Text {
                text: row.glyph
                font.family: "Font Awesome 7 Free Solid"
                font.pixelSize: 14
                color: mouse.containsMouse ? Colors.accent : Colors.textDim
                Behavior on color { ColorAnimation { duration: 90 } }
            }

            Text {
                text: row.rowText
                font.pixelSize: 13
                color: mouse.containsMouse ? Colors.overlayText : Colors.textDim
                verticalAlignment: Text.AlignVCenter
                Behavior on color { ColorAnimation { duration: 90 } }
            }

            Item { Layout.fillWidth: true }
        }
    }

    // Labelled value slider used by the settings panel (brightness, contrast,
    // saturation, gamma, subtitle size, audio delay).
    component ValueSlider: RowLayout {
        id: vs
        property string vsLabel: ""
        property int vsMin: -100
        property int vsMax: 100
        property int vsStep: 1
        property double vsValue: 0
        property bool vsInteger: true
        property var onChanged: null

        Text {
            text: vs.vsLabel
            color: Colors.overlayText
            font.pixelSize: 12
            Layout.preferredWidth: 84
        }
        Slider {
            id: vsSlider
            Layout.fillWidth: true
            from: vs.vsMin
            to: vs.vsMax
            stepSize: vs.vsStep
            value: vs.vsValue
            onMoved: if (vs.onChanged) vs.onChanged(value)

            background: Item {
                implicitHeight: 16
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: 4
                    radius: height / 2
                    color: Colors.track
                }
                Rectangle {
                    width: parent.width * vsSlider.visualPosition
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    height: 4
                    radius: height / 2
                    color: Colors.accent
                }
            }
            handle: Rectangle {
                x: vsSlider.leftPadding + vsSlider.visualPosition * (vsSlider.availableWidth - width)
                y: vsSlider.topPadding + (vsSlider.availableHeight - height) / 2
                width: 13
                height: 13
                radius: width / 2
                color: vsSlider.hovered || vsSlider.dragging ? "#ffffff" : Colors.hover
                border.color: Colors.accent
                border.width: 2
                Behavior on color { ColorAnimation { duration: 110 } }
            }
        }
        Text {
            text: vsInteger ? Math.round(vsSlider.value).toString()
                            : vsSlider.value.toFixed(1)
            color: Colors.textDim
            font.pixelSize: 11
            Layout.preferredWidth: 40
            horizontalAlignment: Text.AlignRight
        }
    }

    // A labelled on/off pill used all over the settings panes.
    component ToggleRow: RowLayout {
        id: tr
        property string trLabel: ""
        property bool trValue: false
        property var onToggled: null

        Text {
            text: tr.trLabel
            color: Colors.overlayText
            font.pixelSize: 12
            Layout.fillWidth: true
        }
        Rectangle {
            id: trPill
            Layout.preferredWidth: 58
            Layout.preferredHeight: 26
            radius: 13
            color: tr.trValue ? Colors.accent : Colors.hover
            Behavior on color { ColorAnimation { duration: 100 } }
            Text {
                anchors.centerIn: parent
                text: tr.trValue ? qsTr("Be") : qsTr("Ki")
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: tr.trValue ? "#0b0b0e" : Colors.textDim
            }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onClicked: if (tr.onToggled) tr.onToggled(!tr.trValue)
            }
        }
    }

    // A horizontal row of selectable pill buttons (aspect, crop, rotation…).
    component SegmentRow: Row {
        id: seg
        property var segItems: []
        property var segCurrent: ""
        property var onPick: null
        spacing: 4

        Repeater {
            model: seg.segItems
            Rectangle {
                required property var modelData
                height: 24
                radius: 12
                width: segItemWidth(modelData.label)
                color: (seg.segCurrent === modelData.value)
                       ? Colors.accent : (segHover.containsMouse ? Colors.hover : "transparent")
                Behavior on color { ColorAnimation { duration: 90 } }
                Text {
                    anchors.centerIn: parent
                    text: modelData.label
                    font.pixelSize: 11
                    color: (seg.segCurrent === modelData.value) ? "#0b0b0e" : Colors.textDim
                }
                MouseArea {
                    id: segHover
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: if (seg.onPick) seg.onPick(modelData.value)
                }
            }
        }

        function segItemWidth(label) {
            return Math.min(74, Math.max(34, label.length * 8 + 18))
        }
    }

    // A section header inside the settings panes.
    component SectionLabel: Text {
        font.pixelSize: 11
        font.weight: Font.DemiBold
        color: Colors.accent
    }

    // A track/subtitle dropdown: a compact list of selectable entries.
    component TrackPicker: Column {
        id: tp
        property string tpLabel: ""
        property var tpModel: []
        property int tpCurrentId: -1
        property var onPick: null
        property bool tpOpen: false
        property int itemHeight: 26
        spacing: 5
        width: parent ? parent.width : 0

        Row {
            width: parent.width
            Text {
                text: tp.tpLabel
                color: Colors.overlayText
                font.pixelSize: 12
                Layout.fillWidth: true
                width: parent.width - 90
            }
            Text {
                text: tp.currentLabel()
                color: Colors.textDim
                font.pixelSize: 12
                anchors.right: parent.right
                elide: Text.ElideRight
                width: 110
                horizontalAlignment: Text.AlignRight
            }
        }

        Column {
            width: parent.width
            spacing: 3
            visible: tp.tpOpen
            Repeater {
                model: tp.tpModel
                Rectangle {
                    required property var modelData
                    height: tp.itemHeight
                    radius: 6
                    width: parent ? parent.width : 0
                    color: (tp.tpCurrentId === modelData.id)
                           ? Colors.selection : (rowHover.containsMouse ? Colors.hover : "transparent")
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 6
                        spacing: 6
                        Text {
                            text: modelData.selected ? "\uF00C" : "  "
                            color: Colors.accent
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: modelData.title
                            color: (tp.tpCurrentId === modelData.id) ? Colors.overlayText : Colors.textDim
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width
                        }
                    }
                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (tp.onPick) tp.onPick(modelData.id)
                            tp.tpOpen = false
                        }
                    }
                }
            }
        }

        function currentLabel() {
            for (var i = 0; i < tp.tpModel.length; i++)
                if (tp.tpModel[i].id === tp.tpCurrentId)
                    return tp.tpModel[i].title
            return "--"
        }
    }

    // A row of colour swatches; tapping one reports its #AARRGGBB. Active
    // swatch gets an accent ring so the current value is visibly selected.
    component ColorSwatches: Row {
        id: sw
        property string selected: "#FFFFFFFF"
        property var onPick: null

        property var palette: [
            { c: "#FFFFFFFF" },
            { c: "#FFFFE900" },
            { c: "#FF000000" },
            { c: "#FF4ea1ff" },
            { c: "#FFFF0000" },
            { c: "#FF00FF00" },
            { c: "#FF00FFFF" },
            { c: "#FFFF00FF" },
            { c: "#FF808080" }
        ]

        Repeater {
            model: sw.palette
            Rectangle {
                required property var modelData
                width: 22
                height: 22
                radius: width / 2
                color: modelData.c
                border.color: (sw.selected === modelData.c) ? Colors.accent : Colors.border
                border.width: (sw.selected === modelData.c) ? 3 : 1
                scale: swMous.containsMouse ? 1.15 : 1
                Behavior on scale { NumberAnimation { duration: 90 } }
                MouseArea {
                    id: swMous
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: if (sw.onPick) sw.onPick(modelData.c)
                }
            }
        }
    }

    // Draggable edges/corners for the floating window. Prefers the compositor
    // interaction (startSystemResize) and falls back to manual resizing.
    component ResizeGrip: MouseArea {
        id: grip
        property int edges: Qt.RightEdge
        property int minW: 380
        property int minH: 240
        property bool manual: false
        property int pressX
        property int pressY
        property int pressW
        property int pressH

        cursorShape: (edges & Qt.RightEdge)
                    ? ((edges & Qt.BottomEdge) ? Qt.SizeFDiagCursor : Qt.SizeHorCursor)
                    : ((edges & Qt.LeftEdge)
                        ? ((edges & Qt.BottomEdge) ? Qt.SizeBDiagCursor : Qt.SizeHorCursor)
                        : Qt.SizeVerCursor)

        onPressed: {
            pressX = mouse.x
            pressY = mouse.y
            pressW = root.width
            pressH = root.height
            manual = !root.startSystemResize(edges)
        }
        onPositionChanged: {
            if (!manual) return
            var dx = mouse.x - pressX
            var dy = mouse.y - pressY
            if (edges & Qt.RightEdge)
                root.width = Math.max(minW, pressW + dx)
            if (edges & Qt.BottomEdge)
                root.height = Math.max(minH, pressH + dy)
            if (edges & Qt.LeftEdge)
                root.width = Math.max(minW, pressW - dx)
        }
        onReleased: manual = false
    }

    // Resize handles on all edges of the floating window.
    ResizeGrip { z: 100; edges: Qt.RightEdge;   width: 8; anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.right: parent.right }
    ResizeGrip { z: 100; edges: Qt.BottomEdge;  height: 8; anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom }
    ResizeGrip { z: 100; edges: Qt.RightEdge | Qt.BottomEdge; width: 22; height: 22; anchors.right: parent.right; anchors.bottom: parent.bottom }
    ResizeGrip { z: 100; edges: Qt.LeftEdge | Qt.BottomEdge; width: 22; height: 22; anchors.left: parent.left; anchors.bottom: parent.bottom }
    ResizeGrip { z: 100; edges: Qt.LeftEdge;  width: 8; anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.left: parent.left }
    ResizeGrip { z: 100; edges: Qt.TopEdge;   height: 8; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top }
    ResizeGrip { z: 100; edges: Qt.TopEdge | Qt.LeftEdge; width: 22; height: 22; anchors.left: parent.left; anchors.top: parent.top }
    ResizeGrip { z: 100; edges: Qt.TopEdge | Qt.RightEdge; width: 22; height: 22; anchors.right: parent.right; anchors.top: parent.top }

    Item {
        id: contextMenu
        width: 210
        visible: false
        z: 50

        // Screen-space feel: body sits at the cursor, clamped inside the window.
        function openAt(x, y) {
            contextMenu.x = Math.min(Math.max(6, x), root.width - contextMenu.width - 6)
            contextMenu.y = Math.min(Math.max(6, y), root.height - contextMenu.height - 6)
            contextMenu.visible = true
        }
        function close() { visible = false }

        height: Math.min(ctxCol.implicitHeight + 12, root.height - 12)

        Rectangle {
            anchors.fill: parent
            radius: 12
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        Flickable {
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                bottom: parent.bottom
                margins: 6
            }
            clip: true
            contentWidth: ctxCol.implicitWidth
            contentHeight: ctxCol.implicitHeight

            Column {
                id: ctxCol
                width: contextMenu.width - 12
                spacing: 3

                MenuRow { rowText: qsTr("Média megnyitása…"); glyph: "\uF07C"; onActivate: () => openDialog.open() }
                MenuRow { rowText: qsTr("URL megnyitása…");   glyph: "\uF0AC"; onActivate: () => urlDialog.open() }
                MenuRow { rowText: qsTr("Képernyőkép");       glyph: "\uF030"; onActivate: () => mpv.takeScreenshot() }

                Rectangle {
                    height: 1
                    width: parent.width
                    color: Colors.border
                }

                MenuRow {
                    rowText: mpv.playing ? qsTr("Szünet") : qsTr("Lejátszás")
                    glyph: mpv.playing ? "\uF04C" : "\uF04B"
                    onActivate: () => mpv.togglePause()
                }
                MenuRow {
                    rowText: root.isFullScreen ? qsTr("Kilépés a teljes képernyőből") : qsTr("Teljes képernyő")
                    glyph: "\uF065"
                    onActivate: () => root.toggleFullscreen()
                }
                MenuRow {
                    rowText: qsTr("Elrejtés a tálcára")
                    glyph: "\uF2D1"
                    onActivate: () => mpv.hideToTray()
                }
                MenuRow {
                    rowText: qsTr("Frissítések keresése")
                    glyph: "\uF021"
                    onActivate: () => { updater.checkForUpdates(); updatePopup.open() }
                }
            }
        }
    }

    // --- fullscreen / window state --------------------------------------------
    Item {
        id: updatePopup
        width: 320
        visible: false
        z: 60

        readonly property real bodyH: updateCol.implicitHeight + 28

        function open() {
            visible = true
        }
        function close() { visible = false }

        x: (root.width - width) / 2
        y: 12
        height: bodyH

        Rectangle {
            anchors.fill: parent
            radius: 14
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        Column {
            id: updateCol
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
            spacing: 8

            Text {
                text: qsTr("Frissítések")
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Colors.overlayText
            }

            Text {
                text: updater.status
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: Colors.textDim
            }

            Text {
                visible: updater.updateAvailable
                font.pixelSize: 13
                color: Colors.overlayText
                text: updater.latestVersion.length > 0 ? qsTr("Új verzió: %1 (jelenlegi: %2)").arg(updater.latestVersion).arg(Qt.application.version) : ""
            }

            Row {
                visible: updater.updateAvailable && !updater.downloaded && !updater.busy
                spacing: 8

                Rectangle {
                    width: 110
                    height: 32
                    radius: 16
                    color: dlMouse.containsMouse || dlMouse.pressed ? Colors.accentGlow : Colors.accent
                    Behavior on color { ColorAnimation { duration: 110 } }
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Letöltés")
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        color: "#0b0b0e"
                    }
                    MouseArea {
                        id: dlMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: updater.downloadPackage()
                    }
                }

                Rectangle {
                    width: 88
                    height: 32
                    radius: 16
                    color: updCloseMouse.containsMouse ? Colors.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Bezárás")
                        font.pixelSize: 13
                        color: updCloseMouse.containsMouse ? Colors.overlayText : Colors.textDim
                    }
                    MouseArea {
                        id: updCloseMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: updatePopup.close()
                    }
                }
            }

            Row {
                visible: updater.updateAvailable && updater.downloaded && !updater.busy
                spacing: 8

                Rectangle {
                    width: 110
                    height: 32
                    radius: 16
                    color: instMouse.containsMouse || instMouse.pressed ? Colors.accentGlow : Colors.accent
                    Behavior on color { ColorAnimation { duration: 110 } }
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Telepítés")
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        color: "#0b0b0e"
                    }
                    MouseArea {
                        id: instMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: updater.installPackage()
                    }
                }

                Rectangle {
                    width: 88
                    height: 32
                    radius: 16
                    color: instCloseMouse2.containsMouse ? Colors.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Bezárás")
                        font.pixelSize: 13
                        color: instCloseMouse2.containsMouse ? Colors.overlayText : Colors.textDim
                    }
                    MouseArea {
                        id: instCloseMouse2
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: updatePopup.close()
                    }
                }
            }
        }
    }

    // Intro / recap / credits skip prompt (top center, streaming style).
    Item {
        id: skipBanner
        visible: mpv.skipPromptVisible
        z: 70
        height: 40
        width: skipRow.implicitWidth + 20
        anchors.horizontalCenter: parent.horizontalCenter
        y: 12

        Rectangle {
            anchors.fill: parent
            radius: 20
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        Row {
            id: skipRow
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 14; rightMargin: 6 }
            spacing: 12

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: mpv.skipPromptLabel
                font.pixelSize: 13
                color: Colors.overlayText
            }

            Rectangle {
                width: 96
                height: 30
                radius: 15
                anchors.verticalCenter: parent.verticalCenter
                color: skipMouse.containsMouse || skipMouse.pressed ? Colors.accentGlow : Colors.accent
                Behavior on color { ColorAnimation { duration: 110 } }
                Text {
                    anchors.centerIn: parent
                    text: qsTr("Kihagyás ▸")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: "#0b0b0e"
                }
                MouseArea {
                    id: skipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: mpv.skipCurrent()
                }
            }
        }
    }

    property bool isFullScreen: false
    // Both side drawers share one width so they swap size-for-size.
    readonly property real drawerWidth: Math.max(240, Math.min(380, Math.round(root.width * 0.62)))

    function toggleFullscreen() {
        isFullScreen = !isFullScreen
        mpv.windowFullscreen(isFullScreen)
    }

    // The bar's settings gear → video colours / subtitles / audio drawer.
    // Only one drawer may be open at a time: opening one dismisses the other.
    function openSettings() {
        playlistPanel.visible = false
        settingsMenu.open()
    }

    // The bar's playlist button (three lines) → refresh + open the list drawer.
    function openPlaylist() {
        settingsMenu.visible = false
        playlistPanel.refresh()
        playlistPanel.selectedIndex = -1
        playlistPanel.open()
    }

    // --- open media ------------------------------------------------------------
    FileDialog {
        id: openDialog
        title: qsTr("Média megnyitása")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("Médiafájlok (%1)").arg("*.mp4 *.mkv *.webm *.avi *.mov *.flv *.m4v *.mp3 *.flac *.opus *.ogg *.wav"),
            qsTr("Minden fájl (*)")
        ]
        onAccepted: mpv.openList(selectedFiles)
    }

    // --- add files to the playlist (does not disturb current playback) ------
    FileDialog {
        id: addFilesDialog
        title: qsTr("Fájlok hozzáadása a listához")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("Médiafájlok (%1)").arg("*.mp4 *.mkv *.webm *.avi *.mov *.flv *.m4v *.mp3 *.flac *.opus *.ogg *.wav"),
            qsTr("Minden fájl (*)")
        ]
        onAccepted: {
            mpv.appendToPlaylist(selectedFiles)
            playlistPanel.refresh()
        }
    }

    // --- external audio / subtitle pickers -------------------------------
    FileDialog {
        id: externalAudioDialog
        title: qsTr("Külső hanglejátszás megnyitása")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Hangfájlok (%1)").arg("*.mp3 *.flac *.opus *.ogg *.wav *.aac *.m4a *.ac3"),
            qsTr("Minden fájl (*)")
        ]
        onAccepted: mpv.loadExternalAudio(selectedFile)
    }

    FileDialog {
        id: externalSubtitleDialog
        title: qsTr("Külső felirat megnyitása")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Feliratok (%1)").arg("*.srt *.ass *.ssa *.vtt *.sub *.sup"),
            qsTr("Minden fájl (*)")
        ]
        onAccepted: {
            mpv.loadExternalSubtitle(selectedFile)
            settingsMenu.refreshSubTracks()
        }
    }

    // --- save playlist to an m3u file ----------------------------------------
    FileDialog {
        id: saveDialog
        title: qsTr("Lejátszási lista mentése")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("M3U lejátszási lista (*.m3u)")]
        onAccepted: mpv.savePlaylist(selectedFile)
    }

    // --- open URL (same frosted-glass design as the context menu) --------------
    Item {
        id: urlDialog
        visible: false
        z: 60
        width: 440
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        height: col.implicitHeight + 40

        function open() { visible = true }
        function close() { visible = false }

        Rectangle {
            anchors.fill: parent
            radius: 12
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        Column {
            id: col
            x: 20
            y: 20
            width: parent.width - 40
            spacing: 16

                Text {
                    text: "URL megnyitása"
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Colors.overlayText
                }

                TextField {
                    id: urlField
                    width: parent.width
                    clip: true
                    placeholderText: "https://…"
                    placeholderTextColor: Colors.textDim
                    color: Colors.overlayText
                    font.pixelSize: 14
                    topPadding: 11
                    bottomPadding: 11
                    leftPadding: 14
                    rightPadding: 14

                    background: Rectangle {
                        radius: 9
                        color: Colors.chrome
                        border.color: urlField.activeFocus ? Colors.accent : Colors.border
                        border.width: 1
                        Behavior on border.color { ColorAnimation { duration: 120 } }
                    }

                    onAccepted: {
                        mpv.open(urlField.text)
                        urlDialog.close()
                        urlField.clear()
                    }
                }

                Row {
                    spacing: 8
                    anchors.right: parent.right

                    Rectangle { // Mégse
                        id: cancelBtn
                        width: 88
                        height: 34
                        radius: 17
                        color: cancelMouse.containsMouse ? Colors.hover : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: "Mégse"
                            font.pixelSize: 13
                            color: cancelMouse.containsMouse ? Colors.overlayText : Colors.textDim
                        }
                        MouseArea {
                            id: cancelMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: urlDialog.close()
                        }
                    }

                    Rectangle { // Megnyitás
                        id: openBtn
                        width: 100
                        height: 34
                        radius: 17
                        color: openMouse.containsMouse || openMouse.pressed ? Colors.accentGlow : Colors.accent
                        Behavior on color { ColorAnimation { duration: 110 } }
                        Text {
                            anchors.centerIn: parent
                            text: "Megnyitás"
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            color: "#0b0b0e"
                        }
                        MouseArea {
                            id: openMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                mpv.open(urlField.text)
                                urlDialog.close()
                                urlField.clear()
                            }
                        }
                    }
                }
            }
    }

    // --- playlist panel (the bar's ≡ button) ---------------------------------
    // A right-edge drawer: it never leaves the window, so it stays usable on
    // the small floating player. It slides in/out along x and re-lays out the
    // list vertically to fill the drawer height.
    Item {
        id: playlistPanel
        visible: false
        z: 50

        x: root.width - width - 4
        y: 4
        width: root.drawerWidth
        // Stops above the control bar so the transport row stays reachable.
        height: root.height - bar.height - 16

        // A plain in-window panel instead of a Popup: an Overlay popup becomes
        // a native xdg-popup surface on Wayland and steals the keyboard focus
        // from the main window, so G/L/Esc would never work again. An Item
        // keeps the focus on the window and the shortcuts alive.

        // Row picked for Delete / Play — a playlist index (from the full mpv
        // list, not the filtered view), or -1 when nothing is selected.
        property int selectedIndex: -1
        property bool draggingItem: false

        function open() { visible = true }
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

            Row {
                Layout.fillWidth: true
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Lejátszási lista")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Colors.overlayText
                    topPadding: 2
                }
                Item { height: 1; width: 8 }

                // Search toggle.
                IconButton {
                    id: searchToggle
                    implicitWidth: 26
                    implicitHeight: 26
                    glyph: "\uF002"                               // FA magnifier
                    tip: qsTr("Keresés (Ctrl+F)")
                    onClicked: {
                        searchField.visible = !searchField.visible
                        if (searchField.visible)
                            searchField.forceActiveFocus()
                        else {
                            searchField.text = ""
                            playlistPanel.applyFilter()
                        }
                    }
                }
                IconButton {
                    id: saveList
                    implicitWidth: 26
                    implicitHeight: 26
                    glyph: "\uF0C7"                               // FA floppy: save
                    tip: qsTr("Lejátszási lista mentése")
                    onClicked: saveDialog.open()
                }
                IconButton {
                    id: addFiles
                    implicitWidth: 26
                    implicitHeight: 26
                    glyph: "\uF055"                               // FA circle-plus
                    tip: qsTr("Fájlok hozzáadása")
                    onClicked: addFilesDialog.open()
                }
                Item { height: 1; width: 8 }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("%1 tétel").arg(filteredModel.count)
                    font.pixelSize: 11
                    color: Colors.textDim
                }
                Item { Layout.fillWidth: true }

                // Close — the drawer also closes on outside click / Esc.
                IconButton {
                    id: plClose
                    implicitWidth: 26
                    implicitHeight: 26
                    glyph: "\uF00D"                               // FA xmark
                    tip: qsTr("Bezárás (Esc)")
                    onClicked: playlistPanel.close()
                }
            }

            TextField {
                id: searchField
                visible: false
                Layout.fillWidth: true
                placeholderText: qsTr("Keresés a listában…")
                placeholderTextColor: Colors.textDim
                color: Colors.overlayText
                font.pixelSize: 12
                topPadding: 7
                bottomPadding: 7
                leftPadding: 11
                rightPadding: 11
                onTextChanged: playlistPanel.applyFilter()

                background: Rectangle {
                    radius: 8
                    color: Colors.chrome
                    border.color: searchField.activeFocus ? Colors.accent : Colors.border
                    border.width: 1
                }
            }

            ListView {
                id: playlistList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: ListModel { id: filteredModel }

                delegate: Rectangle {
                    id: row
                    required property var model
                    width: playlistList.width
                    height: 30
                    radius: 7
                    color: (mouseArea.containsMouse || model.current
                            || playlistPanel.selectedIndex === model.realIndex)
                            ? Colors.hover : "transparent"
                    Behavior on color { ColorAnimation { duration: 90 } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 40
                        spacing: 8

                        Text {
                            text: model.current ? "\uF00C" : String(model.realIndex + 1)
                            color: model.current ? Colors.accent : Colors.textDim
                            font.pixelSize: 12
                            Layout.preferredWidth: 18
                        }
                        Text {
                            text: model.title
                            color: model.current || playlistPanel.selectedIndex === model.realIndex
                                  ? Colors.overlayText : Colors.textDim
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }

                    // Drag reel icon on the right — the whole row is draggable.
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: "\uF5D0"                            // FA grip-vertical
                        font.family: "Font Awesome 7 Free Solid"
                        font.pixelSize: 12
                        color: playlistPanel.draggingItem ? Colors.accent : Colors.textDim
                    }

                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent
                        hoverEnabled: true

                        // Drag reorder: vertical dragging lifts the row and
                        // computes the drop target from its offset. Single
                        // click selects (Delete removes); double click plays.
                        drag.target: row
                        drag.axis: Drag.YAxis
                        drag.threshold: 12

                        onClicked: {
                            dragTimer.restart()
                            playlistPanel.selectedIndex = model.realIndex
                        }
                        onDoubleClicked: {
                            mpv.playAt(model.realIndex)
                            playlistPanel.close()
                        }
                        onReleased: {
                            if (!playlistPanel.draggingItem)
                                return
                            dragTimer.stop()
                            const from = model.realIndex
                            const jumped = Math.round((row.y + row.height / 2) / row.height) - 1
                            const maxF = filteredModel.count - 1
                            const toF = Math.max(0, Math.min(maxF + 1, from + jumped))
                            row.y = 0
                            playlistPanel.draggingItem = false
                            if (toF !== from && toF !== from + 1)
                                playlistPanel.applyMove(from, toF)
                        }

                        onPressed: dragTimer.stop()
                        onPositionChanged: {
                            if (row.y !== 0 && mouseArea.drag.active)
                                playlistPanel.draggingItem = true
                        }
                    }
                }
            }

            Item {
                width: parent.width
                height: 22
                visible: filteredModel.count === 0
                Text {
                    anchors.centerIn: parent
                    text: playlistModel.count === 0 ? qsTr("Nincs média a listában")
                                                    : qsTr("Nincs találat")
                    font.pixelSize: 11
                    color: Colors.textDim
                }
            }

            // Bottom action row: play selected / remove selected.
            Row {
                width: parent.width
                spacing: 8
                visible: playlistPanel.selectedIndex >= 0

                Rectangle {
                    height: 28
                    radius: 14
                    color: playSel.containsMouse || playSel.pressed ? Colors.accentGlow : Colors.accent
                    Behavior on color { ColorAnimation { duration: 110 } }
                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        spacing: 8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "\uF04B"                        // FA play
                            font.family: "Font Awesome 7 Free Solid"
                            font.pixelSize: 12
                            color: "#0b0b0e"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Lejátszás")
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: "#0b0b0e"
                        }
                    }
                    MouseArea {
                        id: playSel
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            const it = playlistPanel.itemAt(playlistPanel.selectedIndex)
                            if (it && it.path)
                                mpv.open(it.path)
                        }
                    }
                }
                Rectangle {
                    height: 28
                    radius: 14
                    color: delSel.containsMouse ? Colors.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Törlés (Del)")
                        font.pixelSize: 12
                        color: delSel.containsMouse ? Colors.overlayText : Colors.textDim
                    }
                    MouseArea {
                        id: delSel
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: playlistPanel.removeSelected()
                    }
                }
                Item { height: 1; width: 1 }
            }
        }

        // Full playlist list (mirrors every mpv playlist entry), kept inside
        // the panel — a Popup can't reach models declared at window level.
        ListModel { id: playlistModel }

        // The short wait before a click counts as a selection (vs. the start
        // of a double-click that plays). Delayed so drag-holds don't select.
        Timer {
            id: dragTimer
            interval: 180
            onTriggered: { /* selection already set in onClicked */ }
        }

        function itemAt(realIndex) {
            for (var i = 0; i < filteredModel.count; i++) {
                const it = filteredModel.get(i)
                if (it.realIndex === realIndex)
                    return it
            }
            return null
        }

        function applyMove(from, toF) {
            const maxF = filteredModel.count - 1
            if (toF > maxF + 1)
                toF = maxF + 1
            if (toF < 0)
                toF = 0
            if (toF === from || toF === from + 1)
                return
            // mpv playlist-move swaps the entry before the target index, so
            // moving down needs one more (and -1 means "append at the end").
            const mpvTo = toF > from ? Math.min(toF + 1, maxF + 1) : toF
            const cmd = mpvTo > maxF ? -1 : mpvTo
            mpv.movePlaylistItem(from, cmd)
            playlistPanel.selectedIndex = cmd < 0 ? maxF : cmd
            refresh()
        }

        function refresh() {
            const items = mpv.playlistItems()
            playlistModel.clear()
            for (var i = 0; i < items.length; i++) {
                playlistModel.append({ "title": items[i].title,
                                       "path": items[i].path,
                                       "current": items[i].current })
            }
            applyFilter()
        }

        function applyFilter() {
            filteredModel.clear()
            const q = searchField.text.trim().toLowerCase()
            for (var i = 0; i < playlistModel.count; i++) {
                const it = playlistModel.get(i)
                if (!q || it.title.toLowerCase().indexOf(q) >= 0)
                    filteredModel.append({ "title": it.title,
                                           "path": it.path,
                                           "current": it.current,
                                           "realIndex": i })
            }
            if (playlistPanel.selectedIndex >= playlistModel.count)
                playlistPanel.selectedIndex = playlistModel.count > 0 ? playlistModel.count - 1 : -1
        }

        function removeSelected() {
            if (playlistPanel.selectedIndex < 0)
                return
            mpv.removePlaylistItem(playlistPanel.selectedIndex)
            playlistPanel.selectedIndex = -1
            refresh()
        }

        // While the drawer is open, follow the playing entry (N/P, auto-next,
        // playAt) live so the highlighted row always tracks the video.
        Connections {
            target: mpv
            function onCurrentIndexChanged(index) {
                if (playlistPanel.visible)
                    playlistPanel.refresh()
            }
        }
    }
    // --- settings panel (the bar's gear) -------------------------------
    // A right-edge drawer like the playlist panel: it stays inside the window
    // at any size (the subtitles block used to overflow out of the short
    // floating window), and the body scrolls when the content is taller.
    Item {
        id: settingsMenu
        visible: false
        z: 50

        x: root.width - width - 4
        y: 4
        width: root.drawerWidth
        // Stops above the control bar so the transport row stays reachable.
        height: root.height - bar.height - 16

        // Plain in-window panel — see playlistPanel: no native popup surface,
        // keyboard focus (and G/L/Esc) stay on the main window.

        property int tabIndex: 0
        property string cropAspect: ""
        property bool customCropOpen: false
        property var audioTracks: []
        property var subTracks: []
        property var eqFreqs: ["31","62","125","250","500","1k","2k","4k","8k","16k"]

        function open() {
            refreshAudioTracks()
            refreshSubTracks()
            visible = true
        }
        function close() { visible = false }

        function pickCrop(v) {
            if (v === "custom") {
                customCropOpen = !customCropOpen
                return
            }
            customCropOpen = false
            cropAspect = v
            if (v === "") {
                mpv.clearVideoCrop()
            } else {
                mpv.setVideoCropAspect(v)
            }
        }

        function refreshAudioTracks() { audioTracks = mpv.audioTracks() }
        function refreshSubTracks() { subTracks = mpv.subtitleTracks() }
        // Keep the track pickers in sync while the drawer is open.
        Connections {
            target: mpv
            function onCurrentAudioTrackChanged() { if (settingsMenu.visible) settingsMenu.refreshAudioTracks() }
            function onCurrentSubtitleTrackChanged() { if (settingsMenu.visible) settingsMenu.refreshSubTracks() }
        }

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

            Row {
                Layout.fillWidth: true
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Beállítások")
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Colors.overlayText
                }
                Item { Layout.fillWidth: true }

                // Close — the drawer also closes on outside click / Esc.
                IconButton {
                    id: setClose
                    implicitWidth: 26
                    implicitHeight: 26
                    glyph: "\uF00D"                               // FA xmark
                    tip: qsTr("Bezárás (Esc)")
                    onClicked: settingsMenu.close()
                }
            }

            // --- tab bar --------------------------------------------------
            Row {
                Layout.fillWidth: true
                spacing: 4

                Repeater {
                    model: [
                        qsTr("Videó"),
                        qsTr("Hang"),
                        qsTr("Felirat")
                    ]
                    Rectangle {
                        required property int index
                        required property string modelData
                        height: 26
                        radius: 6
                        width: settingsMenu.width / 3 - 4
                        color: (settingsMenu.tabIndex === index)
                               ? Colors.accent : (tabHover.containsMouse ? Colors.hover : "transparent")
                        Behavior on color { ColorAnimation { duration: 90 } }
                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: (settingsMenu.tabIndex === index) ? "#0b0b0e" : Colors.textDim
                        }
                        MouseArea {
                            id: tabHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: settingsMenu.tabIndex = index
                        }
                    }
                }
            }

            // --- video tab -----------------------------------------------
            Flickable {
                id: videoScroll
                visible: settingsMenu.tabIndex === 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: videoCol.width
                contentHeight: videoCol.implicitHeight

                ColumnLayout {
                    id: videoCol
                    width: videoScroll.width
                    spacing: 8

                    SectionLabel { text: qsTr("Videosáv") }
                    Text {
                        text: mpv.videoTrackLabel.length > 0
                              ? mpv.videoTrackLabel : qsTr("—")
                        color: Colors.textDim
                        font.pixelSize: 12
                        Layout.fillWidth: true
                    }

                    SectionLabel { text: qsTr("Képarány") }
                    SegmentRow {
                        Layout.fillWidth: true
                        segItems: [
                            { label: qsTr("Alap"), value: "no" },
                            { label: "4:3", value: "4:3" },
                            { label: "16:9", value: "16:9" },
                            { label: "16:10", value: "16:10" },
                            { label: "21:9", value: "21:9" },
                            { label: "5:4", value: "5:4" }
                        ]
                        segCurrent: mpv.videoAspect
                        onPick: v => mpv.setVideoAspect(v)
                    }

                    SectionLabel { text: qsTr("Körbevágás") }
                    SegmentRow {
                        Layout.fillWidth: true
                        segItems: [
                            { label: qsTr("Nincs"), value: "" },
                            { label: "4:3", value: "4:3" },
                            { label: "16:9", value: "16:9" },
                            { label: "16:10", value: "16:10" },
                            { label: "21:9", value: "21:9" },
                            { label: "5:4", value: "5:4" },
                            { label: qsTr("Egyéni"), value: "custom" }
                        ]
                        segCurrent: settingsMenu.cropAspect
                        onPick: v => settingsMenu.pickCrop(v)
                    }

                    Row {
                        visible: settingsMenu.customCropOpen
                        Layout.fillWidth: true
                        spacing: 6

                        TextField {
                            id: cropWField
                            Layout.preferredWidth: 90
                            placeholderText: qsTr("Szélesség")
                            placeholderTextColor: Colors.textDim
                            color: Colors.overlayText
                            font.pixelSize: 12
                            topPadding: 6
                            bottomPadding: 6
                            inputMask: "999999"
                            background: Rectangle {
                                radius: 7
                                color: Colors.chrome
                                border.color: cropWField.activeFocus ? Colors.accent : Colors.border
                            }
                        }
                        TextField {
                            id: cropHField
                            Layout.preferredWidth: 90
                            placeholderText: qsTr("Magasság")
                            placeholderTextColor: Colors.textDim
                            color: Colors.overlayText
                            font.pixelSize: 12
                            topPadding: 6
                            bottomPadding: 6
                            inputMask: "999999"
                            background: Rectangle {
                                radius: 7
                                color: Colors.chrome
                                border.color: cropHField.activeFocus ? Colors.accent : Colors.border
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            id: customCropBtn
                            Layout.preferredWidth: 76
                            Layout.preferredHeight: 28
                            radius: 14
                            color: customCropHover.containsMouse || customCropHover.pressed
                                   ? Colors.accent : Colors.accent
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Vágás")
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: "#0b0b0e"
                            }
                            MouseArea {
                                id: customCropHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    const w = parseInt(cropWField.text, 10)
                                    const h = parseInt(cropHField.text, 10)
                                    if (w > 0 && h > 0) {
                                        mpv.setCustomVideoCrop(w, h)
                                        settingsMenu.cropAspect = "custom"
                                        settingsMenu.customCropOpen = false
                                    }
                                }
                            }
                        }
                    }

                    SectionLabel { text: qsTr("Elforgatás") }
                    SegmentRow {
                        Layout.fillWidth: true
                        segItems: [
                            { label: "0°", value: 0 },
                            { label: "90°", value: 90 },
                            { label: "180°", value: 180 },
                            { label: "270°", value: 270 }
                        ]
                        segCurrent: mpv.videoRotate
                        onPick: v => mpv.videoRotate = v
                    }

                    SectionLabel { text: qsTr("Lejátszás") }
                    ValueSlider { vsLabel: qsTr("Sebesség"); vsMin: 25; vsMax: 400; vsStep: 5;
                                  vsInteger: true; vsValue: Math.round(mpv.speed * 100);
                                  onChanged: v => mpv.speed = v / 100 }

                    ToggleRow { trLabel: qsTr("Hardveres dekódolás"); trValue: mpv.hwdecEnabled;
                                onToggled: v => mpv.hwdecEnabled = v }
                    ToggleRow { trLabel: qsTr("Váltott soros szűrő"); trValue: mpv.deinterlaceEnabled;
                                onToggled: v => mpv.deinterlaceEnabled = v }
                    ToggleRow { trLabel: qsTr("HDR"); trValue: mpv.hdrEnabled;
                                onToggled: v => mpv.hdrEnabled = v }

                    SectionLabel { text: qsTr("Videó színek") }
                    ValueSlider { vsLabel: qsTr("Fényerő");     vsValue: mpv.brightness;
                                  onChanged: v => mpv.brightness = v }
                    ValueSlider { vsLabel: qsTr("Kontraszt");   vsValue: mpv.contrast;
                                  onChanged: v => mpv.contrast = v }
                    ValueSlider { vsLabel: qsTr("Telítettség"); vsValue: mpv.saturation;
                                  onChanged: v => mpv.saturation = v }
                    ValueSlider { vsLabel: qsTr("Gamma");       vsValue: mpv.gamma;
                                  onChanged: v => mpv.gamma = v }
                    ValueSlider { vsLabel: qsTr("Színárnyalat"); vsValue: mpv.hue;
                                  onChanged: v => mpv.hue = v }

                    RowLayout {
                        Layout.topMargin: 2
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            id: videoResetBtn
                            Layout.preferredWidth: 140
                            Layout.preferredHeight: 28
                            radius: 14
                            color: videoResetHover.containsMouse ? Colors.hover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Színek alaphelyzet")
                                font.pixelSize: 12
                                color: videoResetHover.containsMouse ? Colors.overlayText : Colors.textDim
                            }
                            MouseArea {
                                id: videoResetHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    mpv.speed = 1.0
                                    mpv.brightness = 0
                                    mpv.contrast = 0
                                    mpv.saturation = 0
                                    mpv.gamma = 0
                                    mpv.hue = 0
                                }
                            }
                        }
                    }

                    Item { height: 8 }
                }
            }

            // --- audio tab -----------------------------------------------
            Flickable {
                id: audioScroll
                visible: settingsMenu.tabIndex === 1
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: audioCol.width
                contentHeight: audioCol.implicitHeight

                ColumnLayout {
                    id: audioCol
                    width: audioScroll.width
                    spacing: 8

                    SectionLabel { text: qsTr("Hangsáv") }
                    TrackPicker {
                        id: audioTrackPicker
                        tpLabel: qsTr("Hangsáv")
                        tpModel: settingsMenu.audioTracks
                        tpCurrentId: mpv.currentAudioId
                        onPick: id => {
                            mpv.setAudioTrack(id)
                            settingsMenu.refreshAudioTracks()
                        }
                    }

                    RowLayout {
                        Text {
                            text: qsTr("Külső hang tallózó")
                            color: Colors.overlayText
                            font.pixelSize: 12
                            Layout.fillWidth: true
                        }
                        Rectangle {
                            id: pickAudioBtn
                            Layout.preferredWidth: 88
                            Layout.preferredHeight: 28
                            radius: 14
                            color: pickAudioHover.containsMouse ? Colors.hover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Tallózás")
                                font.pixelSize: 12
                                color: pickAudioHover.containsMouse ? Colors.overlayText : Colors.textDim
                            }
                            MouseArea {
                                id: pickAudioHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: externalAudioDialog.open()
                            }
                        }
                    }
                    Text {
                        visible: mpv.audioTrackLabel.length > 0
                        text: mpv.audioTrackLabel
                        color: Colors.textDim
                        font.pixelSize: 11
                    }

                    SectionLabel { text: qsTr("Hangkésleltetés") }
                    ValueSlider { vsLabel: qsTr("Késleltetés"); vsMin: -2000; vsMax: 2000; vsStep: 100;
                                  vsInteger: true; vsValue: Math.round(mpv.audioDelay * 1000);
                                  onChanged: v => mpv.audioDelay = v / 1000 }

                    SectionLabel { text: qsTr("Hangszínszabályzó") }
                    Repeater {
                        model: 10
                        Row {
                            required property int index
                            width: audioScroll.width - 24
                            height: 26
                            spacing: 8
                            Text {
                                text: settingsMenu.eqFreqs[index]
                                width: 34
                                color: Colors.textDim
                                font.pixelSize: 11
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Slider {
                                id: eqSlider
                                width: parent.width - 34 - 46
                                height: 24
                                from: -20
                                to: 20
                                stepSize: 1
                                value: mpv.audioEqGains.length > index ? mpv.audioEqGains[index] : 0
                                onMoved: mpv.setAudioEqBand(index, value)

                                background: Item {
                                    implicitHeight: 16
                                    Rectangle {
                                        anchors.fill: parent
                                        height: 4
                                        anchors.verticalCenter: parent.verticalCenter
                                        radius: 2
                                        color: Colors.track
                                    }
                                    Rectangle {
                                        width: eqSlider.visualPosition * parent.width
                                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                                        height: 4
                                        radius: 2
                                        color: Colors.accent
                                    }
                                }
                                handle: Rectangle {
                                    x: eqSlider.leftPadding + eqSlider.visualPosition * (eqSlider.availableWidth - width)
                                    y: eqSlider.topPadding + (eqSlider.availableHeight - height) / 2
                                    width: 12
                                    height: 12
                                    radius: 6
                                    color: eqSlider.hovered || eqSlider.dragging ? "#ffffff" : Colors.hover
                                    border.color: Colors.accent
                                    border.width: 2
                                }
                            }
                            Text {
                                text: (mpv.audioEqGains.length > index ? mpv.audioEqGains[index] : 0).toFixed(0) + " dB"
                                width: 32
                                color: Colors.textDim
                                font.pixelSize: 10
                                horizontalAlignment: Text.AlignRight
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }
                    RowLayout {
                        Layout.topMargin: 2
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            id: eqResetBtn
                            Layout.preferredWidth: 120
                            Layout.preferredHeight: 28
                            radius: 14
                            color: eqResetHover.containsMouse ? Colors.hover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("EQ alaphelyzet")
                                font.pixelSize: 12
                                color: eqResetHover.containsMouse ? Colors.overlayText : Colors.textDim
                            }
                            MouseArea {
                                id: eqResetHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: mpv.resetAudioEq()
                            }
                        }
                    }

                    Item { height: 8 }
                }
            }

            // --- subtitle tab --------------------------------------------
            Flickable {
                id: subScroll
                visible: settingsMenu.tabIndex === 2
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: subCol.width
                contentHeight: subCol.implicitHeight

                ColumnLayout {
                    id: subCol
                    width: subScroll.width
                    spacing: 8

                    ToggleRow { trLabel: qsTr("Felirat megjelenítése"); trValue: mpv.subtitlesVisible;
                                onToggled: v => mpv.toggleSubtitles() }

                    SectionLabel { text: qsTr("Felirat") }
                    TrackPicker {
                        id: subTrackPicker
                        tpLabel: qsTr("Felirat")
                        tpModel: settingsMenu.subTracks
                        tpCurrentId: mpv.currentSubtitleId
                        onPick: id => {
                            mpv.setSubtitleTrack(id)
                            settingsMenu.refreshSubTracks()
                        }
                    }

                    RowLayout {
                        Text {
                            text: qsTr("Külső felirat tallózó")
                            color: Colors.overlayText
                            font.pixelSize: 12
                            Layout.fillWidth: true
                        }
                        Rectangle {
                            id: pickSubBtn
                            Layout.preferredWidth: 88
                            Layout.preferredHeight: 28
                            radius: 14
                            color: pickSubHover.containsMouse ? Colors.hover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Tallózás")
                                font.pixelSize: 12
                                color: pickSubHover.containsMouse ? Colors.overlayText : Colors.textDim
                            }
                            MouseArea {
                                id: pickSubHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: externalSubtitleDialog.open()
                            }
                        }
                    }

                    SectionLabel { text: qsTr("Időzítés és elhelyezés") }
                    ValueSlider { vsLabel: qsTr("Késleltetés"); vsMin: -2000; vsMax: 2000; vsStep: 100;
                                  vsInteger: true; vsValue: Math.round(mpv.subDelay * 1000);
                                  onChanged: v => mpv.subDelay = v / 1000 }
                    ValueSlider { vsLabel: qsTr("Pozíció"); vsMin: 30; vsMax: 150; vsStep: 1;
                                  vsInteger: true; vsValue: Math.round(mpv.subPos);
                                  onChanged: v => mpv.subPos = v }
                    ValueSlider { vsLabel: qsTr("Nagyítás"); vsMin: 50; vsMax: 200; vsStep: 5;
                                  vsInteger: true; vsValue: Math.round(mpv.subScale * 100);
                                  onChanged: v => mpv.subScale = v / 100 }

                    SectionLabel { text: qsTr("Szöveg stílus") }
                    ValueSlider { vsLabel: qsTr("Betűméret"); vsMin: 25; vsMax: 200; vsStep: 1;
                                  vsInteger: true; vsValue: Math.round(mpv.subFontSize);
                                  onChanged: v => mpv.subFontSize = v }

                    RowLayout {
                        Text {
                            text: qsTr("Betűtípus")
                            color: Colors.overlayText
                            font.pixelSize: 12
                            Layout.fillWidth: true
                        }
                        TextField {
                            id: fontField
                            Layout.preferredWidth: 120
                            text: mpv.subFontFamily
                            color: Colors.overlayText
                            font.pixelSize: 12
                            topPadding: 5
                            bottomPadding: 5
                            leftPadding: 8
                            rightPadding: 8
                            background: Rectangle {
                                radius: 7
                                color: Colors.chrome
                                border.color: fontField.activeFocus ? Colors.accent : Colors.border
                            }
                            onEditingFinished: mpv.subFontFamily = text
                        }
                        Rectangle {
                            Layout.preferredWidth: 60
                            Layout.preferredHeight: 26
                            radius: 13
                            color: fontResetHover.containsMouse ? Colors.hover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Alap")
                                font.pixelSize: 11
                                color: fontResetHover.containsMouse ? Colors.overlayText : Colors.textDim
                            }
                            MouseArea {
                                id: fontResetHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: mpv.subFontFamily = "Sans"
                            }
                        }
                    }

                    SectionLabel { text: qsTr("Szín") }
                    ColorSwatches {
                        Layout.fillWidth: true
                        selected: mpv.subColor
                        onPick: c => mpv.subColor = c
                    }

                    SectionLabel { text: qsTr("Keret") }
                    ValueSlider { vsLabel: qsTr("Szélesség"); vsMin: 0; vsMax: 10; vsStep: 1;
                                  vsInteger: true; vsValue: Math.round(mpv.subBorderSize);
                                  onChanged: v => mpv.subBorderSize = v }
                    ColorSwatches {
                        Layout.fillWidth: true
                        selected: mpv.subBorderColor
                        onPick: c => mpv.subBorderColor = c
                    }

                    SectionLabel { text: qsTr("Háttér") }
                    ColorSwatches {
                        Layout.fillWidth: true
                        selected: mpv.subBackColor
                        onPick: c => mpv.subBackColor = c
                    }

                    RowLayout {
                        Layout.topMargin: 4
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            id: subResetBtn
                            Layout.preferredWidth: 120
                            Layout.preferredHeight: 30
                            radius: 15
                            color: subResetHover.containsMouse ? Colors.hover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Alaphelyzet")
                                font.pixelSize: 12
                                color: subResetHover.containsMouse ? Colors.overlayText : Colors.textDim
                            }
                            MouseArea {
                                id: subResetHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    mpv.subDelay = 0
                                    mpv.subPos = 100
                                    mpv.subScale = 1.0
                                    mpv.subFontSize = 55
                                    mpv.subFontFamily = "Sans"
                                    mpv.subColor = "#FFFFFFFF"
                                    mpv.subBorderColor = "#FF000000"
                                    mpv.subBorderSize = 3
                                    mpv.subBackColor = "#80000000"
                                }
                            }
                        }
                    }

                    Item { height: 8 }
                }
            }
        }
    }

    // --- drag & drop ------------------------------------------------------------
    DropArea {
        anchors.fill: parent
        onDropped: drop => {
            drop.acceptProposedAction()
            if (drop.urls.length > 0)
                mpv.open(drop.urls[0])
        }
    }

    // --- keyboard ----------------------------------------------------------------
    Shortcut { sequence: "Space"; onActivated: mpv.togglePause() }
    Shortcut { sequence: "Left"; onActivated: mpv.seekRelative(-5) }
    Shortcut { sequence: "Right"; onActivated: mpv.seekRelative(5) }
    Shortcut { sequence: "Up"; onActivated: mpv.setVolume(Math.min(mpv.volume + 10, 150)) }
    Shortcut { sequence: "Down"; onActivated: mpv.setVolume(Math.max(mpv.volume - 10, 0)) }
    Shortcut { sequence: "M"; onActivated: mpv.toggleMute() }
    Shortcut { sequence: "F"; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: "I"; onActivated: mpv.toggleMinimize() }
    Shortcut { sequence: "G"; onActivated: openSettings() }
    Shortcut { sequence: "L"; onActivated: openPlaylist() }
    // Playback speed (mpv default bindings: halve / double).
    Shortcut { sequence: "["; onActivated: mpv.speed = Math.max(0.25, mpv.speed / 2) }
    Shortcut { sequence: "]"; onActivated: mpv.speed = Math.min(4, mpv.speed * 2) }
    // Playlist navigation (mpv): [n]ext / [p]revious.
    Shortcut { sequence: "N"; onActivated: mpv.playlistNext() }
    Shortcut { sequence: "P"; onActivated: mpv.playlistPrevious() }
    Shortcut { sequence: "Delete"; onActivated: playlistPanel.removeSelected() }
    Shortcut { sequence: "Ctrl+F"; onActivated: searchToggle.clicked() }
    Shortcut { sequence: "Ctrl+O"; onActivated: openDialog.open() }
    Shortcut { sequence: "Ctrl+S"; onActivated: mpv.takeScreenshot() }
    Shortcut { sequence: "Esc"; onActivated: {
        if (settingsMenu.visible) { settingsMenu.close(); return }
        if (playlistPanel.visible) { playlistPanel.close(); return }
        if (urlDialog.visible) { urlDialog.close(); return }
        if (updatePopup.visible) { updatePopup.close(); return }
        if (root.isFullScreen) { root.isFullScreen = false; mpv.windowFullscreen(false) }
    } }
    Shortcut { sequence: "Ctrl+0"; onActivated: mpv.setVolume(100) }
}