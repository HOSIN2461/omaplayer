import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.settings
import Omaplayer

// Saját fájltallózó a natív (GTK) dialógus helyett: nem kell hozzá sem
// portal, sem platformtéma, azonnal nyílik és az app stílusát követi.
// A címtár-listát a C++ DirModel adja (determinisztikus szerepek, rendezés,
// szűrés: mappák mindig látszanak, csak a fájlok szűrődnek).
//
// Módok (fileMode): "openFiles" (többszörös kijelölés), "openFile" (egyszeres),
// "saveFile" (mappa + fájlnév-mező). Sikeres záráskor accepted(paths) jön,
// ahol paths a kiválasztott helyi elérési utak tömbje (mentésnél 1 elemű).
//
// Szándékosan sima ablakon belüli Item (nem Popup): az Overlay-popup Wayland
// alatt natív xdg-popup felületet kapna és ellopná a billentyűzetet.
Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 70

    property string titleText: qsTr("Fájl kiválasztása")
    property string fileMode: "openFiles"
    property var nameFilters: []
    property string acceptText: qsTr("Megnyitás")
    property string defaultFileName: ""

    signal accepted(var paths)

    readonly property bool multi: fileMode === "openFiles"
    readonly property bool save: fileMode === "saveFile"

    function open(firstPath) {
        if (firstPath !== undefined && firstPath !== "")
            dirModel.setFolder(firstPath)
        else if (dirModel.folderPath === "")
            dirModel.setFolder(remembered.lastDir !== "" ? remembered.lastDir : "/home")
        else
            dirModel.refresh()
        selection = {}
        selectedSingle = ""
        selectedCount = 0
        fileList.currentIndex = -1
        if (save)
            fileNameField.text = root.defaultFileName
        visible = true
        fileList.forceActiveFocus()
    }
    function close() {
        visible = false
    }

    // Kijelölés: útvonal -> true (multi), illetve egy útvonal (single).
    property var selection: ({})
    property string selectedSingle: ""
    property int selectedCount: 0
    // Tartomány-kijelölés horgonya (Shift+kattintás, multi módban).
    property int anchorIndex: -1

    Settings {
        id: remembered
        category: "FileBrowser"
        property string lastDir: ""
    }

    DirModel {
        id: dirModel
        nameFilters: root.nameFilters
        showHidden: hiddenToggle.on
        onFolderChanged: {
            root.selection = {}
            root.selectedSingle = ""
            root.selectedCount = 0
            root.anchorIndex = -1
            fileList.currentIndex = -1
            if (dirModel.folderPath !== "")
                remembered.lastDir = dirModel.folderPath
        }
    }

    function isSelected(path) {
        return root.selection[path] === true
    }
    function toggleSelect(path) {
        var s = root.selection
        if (s[path] === true) {
            delete s[path]
            root.selectedCount--
        } else {
            s[path] = true
            root.selectedCount++
        }
        root.selection = s
    }
    function selectRange(a, b) {
        var rows = dirModel.entries()
        var lo = Math.max(0, Math.min(a, b))
        var hi = Math.min(rows.length - 1, Math.max(a, b))
        var s = root.selection
        var n = root.selectedCount
        for (var i = lo; i <= hi; i++) {
            if (!rows[i].isDir && s[rows[i].path] !== true) {
                s[rows[i].path] = true
                n++
            }
        }
        root.selection = s
        root.selectedCount = n
    }
    function selectAllFiles() {
        var rows = dirModel.entries()
        var s = {}
        var n = 0
        for (var i = 0; i < rows.length; i++) {
            if (!rows[i].isDir) {
                s[rows[i].path] = true
                n++
            }
        }
        root.selection = s
        root.selectedCount = n
    }
    function activateRow(isDir, path, fileName) {
        if (isDir) {
            dirModel.setFolder(path)
            return
        }
        if (root.save) {
            fileNameField.text = fileName
            return
        }
        if (root.multi)
            root.toggleSelect(path)
        else
            root.selectedSingle = path
    }
    function acceptRow(isDir, path, fileName) {
        if (isDir) {
            dirModel.setFolder(path)
            return
        }
        if (root.save)
            return
        if (root.multi) {
            if (!root.isSelected(path))
                root.toggleSelect(path)
        } else {
            root.selectedSingle = path
        }
        root.accept()
    }
    function acceptBtnReady() {
        if (root.save)
            return fileNameField.text.trim() !== ""
        if (root.multi)
            return root.selectedCount > 0
        return root.selectedSingle !== ""
    }
    function accept() {
        if (root.save) {
            var name = fileNameField.text.trim()
            if (name === "")
                return
            if (name.indexOf(".") < 0 && root.defaultFileName.indexOf(".") >= 0)
                name += root.defaultFileName.slice(root.defaultFileName.lastIndexOf("."))
            root.accepted([dirModel.folderPath + "/" + name])
        } else if (root.multi) {
            var out = []
            for (var p in root.selection) {
                if (root.selection[p] === true)
                    out.push(p)
            }
            if (out.length === 0)
                return
            out.sort()
            root.accepted(out)
        } else {
            if (root.selectedSingle === "")
                return
            root.accepted([root.selectedSingle])
        }
        root.close()
    }

    // Háttér: kattintásra zár.
    MouseArea {
        anchors.fill: parent
        onClicked: root.close()
    }

    Rectangle {
        id: card
        width: Math.min(760, parent.width - 60)
        height: Math.min(560, parent.height - 60)
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        radius: 12
        color: Colors.overlay
        border.color: Colors.border
        border.width: 1

        MouseArea {
            anchors.fill: parent
            onClicked: {} // a kártyán belüli kattintás ne zárjon
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: root.titleText
                    color: Colors.overlayText
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Text {
                    text: "\uF00D"
                    font.family: "Font Awesome 7 Free Solid"
                    font.pixelSize: 14
                    color: closeMouse.containsMouse ? Colors.overlayText : Colors.textDim
                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.close()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 17
                    color: upMouse.containsMouse ? Colors.hover : "transparent"
                    opacity: dirModel.canGoUp ? 1.0 : 0.35
                    Text {
                        anchors.centerIn: parent
                        text: "\uF062"
                        font.family: "Font Awesome 7 Free Solid"
                        font.pixelSize: 13
                        color: Colors.textDim
                    }
                    MouseArea {
                        id: upMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: dirModel.cdUp()
                    }
                }

                Text {
                    text: dirModel.folderPath
                    color: Colors.textDim
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 17
                    color: hiddenMouse.containsMouse ? Colors.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "\uF06E"
                        font.family: "Font Awesome 7 Free Solid"
                        font.pixelSize: 13
                        color: hiddenToggle.on ? Colors.accent : Colors.textDim
                    }
                    MouseArea {
                        id: hiddenMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: hiddenToggle.on = !hiddenToggle.on
                    }
                }
                QtObject {
                    id: hiddenToggle
                    property bool on: false
                }
            }

            ListView {
                id: fileList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: dirModel
                highlightMoveDuration: 0

                Keys.onUpPressed: decrementCurrentIndex()
                Keys.onDownPressed: incrementCurrentIndex()
                Keys.onLeftPressed: dirModel.cdUp()
                Keys.onRightPressed: activateCurrent()
                Keys.onReturnPressed: acceptCurrent()
                Keys.onEnterPressed: acceptCurrent()

                function activateCurrent() {
                    if (currentIndex < 0 || currentItem === null)
                        return
                    root.activateRow(currentItem.isDir, currentItem.path,
                                     currentItem.name)
                }
                function acceptCurrent() {
                    if (currentIndex < 0 || currentItem === null) {
                        root.accept()
                        return
                    }
                    root.acceptRow(currentItem.isDir, currentItem.path,
                                   currentItem.name)
                }

                delegate: Rectangle {
                    required property string name
                    required property string path
                    required property bool isDir
                    required property double size
                    required property int index
                    id: rowRect
                    width: fileList.width
                    height: 32
                    radius: 7
                    color: {
                        if (index === fileList.currentIndex)
                            return Colors.selection
                        return rowMouse.containsMouse ? Colors.hover : "transparent"
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Text {
                            text: {
                                if (isDir)
                                    return "\uF07B"
                                if (root.multi && root.isSelected(path))
                                    return "\uF14A"
                                if (!root.multi && !root.save && root.selectedSingle === path)
                                    return "\uF00C"
                                return "\uF016"
                            }
                            font.family: "Font Awesome 7 Free Solid"
                            font.pixelSize: 13
                            color: {
                                if (isDir)
                                    return Colors.accent
                                if (root.multi && root.isSelected(path))
                                    return Colors.accent
                                return Colors.textDim
                            }
                            Layout.preferredWidth: 20
                        }
                        Text {
                            text: name
                            color: Colors.overlayText
                            font.pixelSize: 13
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                        Text {
                            text: {
                                if (isDir)
                                    return ""
                                var kb = Math.round(size / 1024)
                                return kb < 1024 ? kb + " KB" : (kb / 1024).toFixed(1) + " MB"
                            }
                            color: Colors.textDim
                            font.pixelSize: 11
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            fileList.currentIndex = index
                            if (isDir) {
                                root.anchorIndex = -1
                                root.activateRow(isDir, path, name)
                                return
                            }
                            if (root.save) {
                                root.activateRow(isDir, path, name)
                                return
                            }
                            if (root.multi) {
                                if ((rowMouse.modifiers & Qt.ShiftModifier) && root.anchorIndex >= 0)
                                    root.selectRange(root.anchorIndex, index)
                                else {
                                    root.toggleSelect(path)
                                    root.anchorIndex = index
                                }
                            } else {
                                root.selectedSingle = path
                            }
                        }
                        onDoubleClicked: {
                            fileList.currentIndex = index
                            root.anchorIndex = index
                            root.acceptRow(isDir, path, name)
                        }
                    }
                }
            }

            TextField {
                id: fileNameField
                visible: root.save
                Layout.fillWidth: true
                placeholderText: qsTr("Fájlnév")
                font.pixelSize: 13
                color: Colors.overlayText
                background: Rectangle {
                    radius: 9
                    color: Colors.chrome
                    border.color: fileNameField.activeFocus ? Colors.accent : Colors.border
                    border.width: 1
                }
                onAccepted: root.accept()
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    visible: root.multi
                    text: root.selectedCount > 0 ? qsTr("%n kijelölve", "", root.selectedCount) : ""
                    color: Colors.textDim
                    font.pixelSize: 12
                    Layout.fillWidth: true
                }
                Item {
                    visible: !root.multi
                    Layout.fillWidth: true
                }

                Rectangle {
                    visible: root.multi
                    Layout.preferredWidth: 76
                    Layout.preferredHeight: 34
                    radius: 17
                    color: allMouse.containsMouse ? Colors.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Mind")
                        font.pixelSize: 13
                        color: allMouse.containsMouse ? Colors.overlayText : Colors.textDim
                    }
                    MouseArea {
                        id: allMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.selectAllFiles()
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 88
                    Layout.preferredHeight: 34
                    radius: 17
                    color: cancelMouse.containsMouse ? Colors.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Mégse")
                        font.pixelSize: 13
                        color: cancelMouse.containsMouse ? Colors.overlayText : Colors.textDim
                    }
                    MouseArea {
                        id: cancelMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.close()
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 110
                    Layout.preferredHeight: 34
                    radius: 17
                    opacity: root.acceptBtnReady() ? 1.0 : 0.45
                    color: acceptMouse.containsMouse && root.acceptBtnReady() ? Colors.accentGlow : Colors.accent
                    Text {
                        anchors.centerIn: parent
                        text: root.acceptText
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        color: "#0b0b0e"
                    }
                    MouseArea {
                        id: acceptMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (root.acceptBtnReady())
                                root.accept()
                        }
                    }
                }
            }
        }

        Shortcut {
            sequence: "Escape"
            enabled: root.visible
            onActivated: root.close()
        }
        Shortcut {
            sequence: "Ctrl+A"
            enabled: root.visible && root.multi
            onActivated: root.selectAllFiles()
        }
    }
}
