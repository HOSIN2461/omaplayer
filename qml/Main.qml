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
        enabled: !contextMenu.visible && !urlDialog.visible

        onClicked: mouse => {
            bar.show()
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

        // Hover over the bottom strip reveals the bar; anything else auto-hides.
        onPositionChanged: mouse => {
            if (mouse.y > root.height - 84)
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

    Popup {
        id: contextMenu
        width: 210
        padding: 6
        z: 50

        // Screen-space feel: body sits at the cursor, clamped inside the window.
        function openAt(x, y) {
            contextMenu.x = Math.min(Math.max(6, x), root.width - contextMenu.width - 6)
            contextMenu.y = Math.min(Math.max(6, y), root.height - contextMenu.height - 6)
            contextMenu.open()
        }

        background: Rectangle {
            radius: 12
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        contentItem: Flickable {
            clip: true
            implicitHeight: Math.min(ctxCol.implicitHeight, root.height - 24)
            height: implicitHeight
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
                    rowText: qsTr("Elrejtés a tálcára (peek)")
                    glyph: "\uF2D1"
                    onActivate: () => mpv.toggleMinimize()
                }
            }
        }
    }

    // --- fullscreen / window state --------------------------------------------
    property bool isFullScreen: false

    function toggleFullscreen() {
        isFullScreen = !isFullScreen
        mpv.windowFullscreen(isFullScreen)
    }

    // The bar's settings gear → video colours / subtitles / audio drawer.
    function openSettings() {
        settingsMenu.open()
    }

    // The bar's playlist button (three lines) → refresh + open the list drawer.
    function openPlaylist() {
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

    // --- save playlist to an m3u file ----------------------------------------
    FileDialog {
        id: saveDialog
        title: qsTr("Lejátszási lista mentése")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("M3U lejátszási lista (*.m3u)")]
        onAccepted: mpv.savePlaylist(selectedFile)
    }

    // --- open URL (same frosted-glass design as the context menu) --------------
    Popup {
        id: urlDialog
        modal: true
        width: 440
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        padding: 0

        background: Rectangle {
            radius: 12
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        contentItem: Item {
            id: body
            width: urlDialog.width
            implicitHeight: col.implicitHeight + 40

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
    }

    // --- playlist panel (the bar's ≡ button) ---------------------------------
    // A right-edge drawer: it never leaves the window, so it stays usable on
    // the small floating player. It slides in/out along x and re-lays out the
    // list vertically to fill the drawer height.
    Popup {
        id: playlistPanel
        modal: true
        padding: 6
        z: 50

        x: root.width - width - 4
        y: 4
        width: Math.min(400, root.width - 8)
        height: root.height - 8

        enter: Transition {
            NumberAnimation { property: "x"; from: root.width; duration: 250; easing.type: Easing.OutCubic }
        }
        exit: Transition {
            NumberAnimation { property: "x"; to: root.width; duration: 210; easing.type: Easing.InCubic }
        }

        // Row picked for Delete / Play — a playlist index (from the full mpv
        // list, not the filtered view), or -1 when nothing is selected.
        property int selectedIndex: -1
        property bool draggingItem: false

        background: Rectangle {
            radius: 12
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        contentItem: ColumnLayout {
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
                            mpv.open(model.path)
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
    }
    // --- settings panel (the bar's gear) -------------------------------
    // A right-edge drawer like the playlist panel: it stays inside the window
    // at any size (the subtitles block used to overflow out of the short
    // floating window), and the body scrolls when the content is taller.
    Popup {
        id: settingsMenu
        modal: true
        padding: 6
        z: 50

        x: root.width - width - 4
        y: 4
        width: Math.min(380, root.width - 8)
        height: root.height - 8

        enter: Transition {
            NumberAnimation { property: "x"; from: root.width; duration: 250; easing.type: Easing.OutCubic }
        }
        exit: Transition {
            NumberAnimation { property: "x"; to: root.width; duration: 210; easing.type: Easing.InCubic }
        }

        background: Rectangle {
            radius: 12
            color: Colors.overlay
            border.color: Colors.border
            border.width: 1
        }

        contentItem: ColumnLayout {
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

            Flickable {
                id: settingsScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: settingsCol.implicitWidth
                contentHeight: settingsCol.implicitHeight

                ColumnLayout {
                    id: settingsCol
                    width: settingsScroll.width
                    spacing: 9

                    Text {
                        text: qsTr("Lejátszás")
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: Colors.accent
                    }
                    ValueSlider { vsLabel: qsTr("Sebesség"); vsMin: 25; vsMax: 400; vsStep: 5;
                                  vsInteger: true; vsValue: Math.round(mpv.speed * 100);
                                  onChanged: v => mpv.speed = v / 100 }

                    Text {
                        text: qsTr("Videó színek")
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: Colors.accent
                    }
                    ValueSlider { vsLabel: qsTr("Fényerő");     vsValue: mpv.brightness;
                                  onChanged: v => mpv.brightness = v }
                    ValueSlider { vsLabel: qsTr("Kontraszt");   vsValue: mpv.contrast;
                                  onChanged: v => mpv.contrast = v }
                    ValueSlider { vsLabel: qsTr("Telítettség"); vsValue: mpv.saturation;
                                  onChanged: v => mpv.saturation = v }
                    ValueSlider { vsLabel: qsTr("Gamma");       vsValue: mpv.gamma;
                                  onChanged: v => mpv.gamma = v }

                    Text {
                        text: qsTr("Feliratok")
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: Colors.accent
                    }
                    RowLayout {
                        Text {
                            text: qsTr("Megjelenítés")
                            color: Colors.overlayText
                            font.pixelSize: 12
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            id: subToggle
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 26
                            radius: 13
                            color: mpv.subtitlesVisible ? Colors.accent : Colors.hover
                            Behavior on color { ColorAnimation { duration: 100 } }
                            Text {
                                anchors.centerIn: parent
                                text: mpv.subtitlesVisible ? qsTr("Be") : qsTr("Ki")
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: mpv.subtitlesVisible ? "#0b0b0e" : Colors.textDim
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: mpv.toggleSubtitles()
                            }
                        }
                    }
                    ValueSlider { vsLabel: qsTr("Betűméret"); vsMin: 50; vsMax: 200; vsStep: 5;
                                  vsInteger: true; vsValue: Math.round(mpv.subScale * 100);
                                  onChanged: v => mpv.subScale = v / 100 }

                    Text {
                        text: qsTr("Hang")
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: Colors.accent
                    }
                    ValueSlider { vsLabel: qsTr("Késleltetés"); vsMin: -2000; vsMax: 2000; vsStep: 100;
                                  vsInteger: true; vsValue: Math.round(mpv.audioDelay * 1000);
                                  onChanged: v => mpv.audioDelay = v / 1000 }

                    RowLayout {
                        Layout.topMargin: 4
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            id: resetBtn
                            Layout.preferredWidth: 120
                            Layout.preferredHeight: 30
                            radius: 15
                            color: resetMouse.containsMouse ? Colors.hover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Alaphelyzet")
                                font.pixelSize: 12
                                color: resetMouse.containsMouse ? Colors.overlayText : Colors.textDim
                            }
                            MouseArea {
                                id: resetMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    mpv.speed = 1.0
                                    mpv.brightness = 0
                                    mpv.contrast = 0
                                    mpv.saturation = 0
                                    mpv.gamma = 0
                                    mpv.subScale = 1.0
                                    mpv.audioDelay = 0
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
        if (root.isFullScreen) { root.isFullScreen = false; mpv.windowFullscreen(false) }
    } }
    Shortcut { sequence: "Ctrl+0"; onActivated: mpv.setVolume(100) }
}