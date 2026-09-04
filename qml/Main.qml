import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Omaplayer

ApplicationWindow {
    id: root

    visible: true
    width: 1024
    height: 600
    minimumWidth: 320
    minimumHeight: 200
    color: "black"
    property MpvCore mpv: MpvCore {}
    property bool autoPip: Qt.application.arguments.indexOf("--pip") >= 0

    Component.onCompleted: {
        if (autoPip)
            Qt.callLater(root.togglePip)
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

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }

    // Auto-hide: fade the bar out after idle, keep it while the pointer or a
    // seek drag is on it.
    Timer {
        id: barTimer
        interval: 2500
        running: mpv.playing
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
                contextMenu.popup()
            else
                mpv.togglePause()
        }
        onDoubleClicked: mouse => {
            if (mouse.button === Qt.LeftButton)
                root.toggleFullscreen()
        }
        onWheel: wheel => {
            if (wheel.modifiers & Qt.ControlModifier)
                mpv.setVolume(Math.max(0, Math.min(150, mpv.volume + wheel.angleDelta.y / 8)))
            else
                mpv.seekRelative(wheel.angleDelta.y > 0 ? 5 : -5)
        }

        // Hover over the bottom strip reveals the bar; anything else auto-hides.
        onPositionChanged: mouse => {
            if (mouse.y > bar.y)
                bar.show()
        }
    }

    Menu {
        id: contextMenu

        MenuItem {
            text: qsTr("Open media…")
            onTriggered: openDialog.open()
        }
        MenuItem {
            text: qsTr("Open URL…")
            onTriggered: urlDialog.open()
        }
        MenuItem {
            text: qsTr("Screenshot")
            onTriggered: mpv.takeScreenshot()
        }
        MenuItem {
            text: pipWindow.visible ? qsTr("Exit PiP") : qsTr("Picture-in-Picture")
            onTriggered: root.togglePip()
        }
        MenuSeparator {}
        MenuItem {
            text: mpv.playing ? qsTr("Pause") : qsTr("Play")
            onTriggered: mpv.togglePause()
        }
        MenuItem {
            text: root.visibility === Window.FullScreen ? qsTr("Exit fullscreen") : qsTr("Fullscreen")
            onTriggered: root.toggleFullscreen()
        }
    }

    // --- fullscreen / window state --------------------------------------------
    function toggleFullscreen() {
        root.visibility = root.visibility === Window.FullScreen
                              ? Window.Windowed
                              : Window.FullScreen
    }

    // --- picture-in-picture --------------------------------------------------
    // libmpv allows a single render context, so PiP re-parents the very same
    // video item into a small always-on-top window instead of rendering twice.
    function togglePip() {
        if (pipWindow.visible) {
            exitPip()
        } else {
            video.parent = pipWindow.contentItem
            pipWindow.show()
            pipWindow.requestActivate()
            root.hide()
        }
    }

    function exitPip() {
        pipWindow.hide()
        video.parent = root.contentItem
        root.show()
    }

    // --- auto-hide ------------------------------------------------------------
    Timer {
        id: autoHide
        interval: 2500
        running: mpv.playing && bar.state === "visible"
        onTriggered: bar.hide()
    }

    // --- open media ------------------------------------------------------------
    FileDialog {
        id: openDialog
        title: qsTr("Open media")
        nameFilters: [
            qsTr("Media files (%1)").arg("*.mp4 *.mkv *.webm *.avi *.mov *.flv *.m4v *.mp3 *.flac *.opus *.ogg *.wav"),
            qsTr("All files (*)")
        ]
        onAccepted: mpv.open(selectedFile)
    }

    Dialog {
        id: urlDialog
        title: qsTr("Open URL")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        implicitWidth: 460

        contentItem: TextField {
            id: urlField
            placeholderText: qsTr("https://…")
            onAccepted: urlDialog.accept()
        }

        onAccepted: mpv.open(urlField.text)
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
    Shortcut { sequence: "P"; onActivated: root.togglePip() }
    Shortcut { sequence: "Left"; onActivated: mpv.seekRelative(-5) }
    Shortcut { sequence: "Right"; onActivated: mpv.seekRelative(5) }
    Shortcut { sequence: "Up"; onActivated: mpv.setVolume(Math.min(mpv.volume + 10, 150)) }
    Shortcut { sequence: "Down"; onActivated: mpv.setVolume(Math.max(mpv.volume - 10, 0)) }
    Shortcut { sequence: "M"; onActivated: mpv.toggleMute() }
    Shortcut { sequence: "F"; onActivated: root.toggleFullscreen() }
    Shortcut { sequence: "Ctrl+O"; onActivated: openDialog.open() }
    Shortcut { sequence: "Ctrl+S"; onActivated: mpv.takeScreenshot() }
    Shortcut { sequence: "Esc"; onActivated: root.visibility = Window.Windowed }
    Shortcut { sequence: "Ctrl+0"; onActivated: mpv.setVolume(100) }

    // --- picture-in-picture window -------------------------------------------
    // Small always-on-top frame that hosts the re-parented video item.
    Window {
        id: pipWindow
        visible: false

        width: 360
        height: 203
        minimumWidth: 360
        maximumWidth: 360
        minimumHeight: 203
        maximumHeight: 203
        color: "black"
        title: mpv.mediaTitle.length > 0 ? mpv.mediaTitle : "Omaplayer"

        flags: Qt.Window | Qt.FramelessWindowHint
               | Qt.WindowStaysOnTopHint

        // The re-parented video item fills the contentItem; every chrome
        // element below uses explicit z so it floats above the video.
        Rectangle {
            anchors.fill: parent
            z: -1
            color: "transparent"
            border.color: Colors.border
            border.width: 1
        }

        // Title strip: drag to move, close button, double-click to exit.
        Rectangle {
            id: pipTitle
            height: 26
            z: 10
            color: Colors.chrome
            anchors { left: parent.left; right: parent.right; top: parent.top }

            MouseArea {
                anchors.fill: parent
                onPressed: mouse => pipWindow.startSystemMove()
                onDoubleClicked: mouse => root.exitPip()
                hoverEnabled: true
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Omaplayer — kis kép a képen")
                color: Colors.overlayText
                font.pixelSize: 11
            }

            IconButton {
                id: pipExit
                glyph: "\uF00D"          // FA xmark
                tip: qsTr("Bezárás (PiP kilépés)")
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 24
                implicitHeight: 24
                onClicked: root.exitPip()
            }
        }

        // Click the video to exit PiP; scroll to nudge volume.
        MouseArea {
            id: pipGestures
            anchors.fill: parent
            z: 5
            acceptedButtons: Qt.LeftButton
            onClicked: root.exitPip()
            onWheel: wheel => {
                if (wheel.modifiers & Qt.ControlModifier)
                    mpv.setVolume(Math.max(0, Math.min(150, mpv.volume + wheel.angleDelta.y / 8)))
                else
                    mpv.seekRelative(wheel.angleDelta.y > 0 ? 5 : -5)
            }
        }

        Shortcut { sequence: "Escape"; onActivated: root.exitPip() }
        Shortcut { sequence: "P"; onActivated: root.exitPip() }
        Shortcut { sequence: "Space"; onActivated: mpv.togglePause() }
    }
}