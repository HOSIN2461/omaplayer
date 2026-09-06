import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omaplayer

// Jellyfin browser drawer (right edge), same in-window design as the playlist
// panel: a plain Item, never a native popup, so window focus and the G/L/Esc
// shortcuts stay alive. Drives the JellyfinClient context property.
Item {
    id: panel

    visible: false
    z: 50

    property bool hasBack: visible && navStack.length > 1
    property var navStack: []

    function open() {
        visible = true
        if (navStack.length === 0) {
            if (jellyfin.activeServerName.length > 0)
                showViews()
            else
                showServerList()
        }
    }
    function close() { visible = false }

    function push(mode, title, ctx) {
        navStack.push({ mode: mode, title: title, ctx: ctx || {} })
        if (navStack.length > 12)
            navStack.shift()
        navTitle.text = title
    }
    function goBack() {
        if (navStack.length > 1) {
            navStack.pop()
            const cur = navStack[navStack.length - 1]
            navTitle.text = cur.title
            switch (cur.mode) {
            case "views":    jellyfin.fetchViews();                          break
            case "resume":   jellyfin.fetchResume();                         break
            case "nextup":   jellyfin.fetchNextUp();                         break
            case "items":    jellyfin.fetchItems(cur.ctx.parentId);          break
            case "seasons":  jellyfin.fetchSeasons(cur.ctx.seriesId);        break
            case "episodes": jellyfin.fetchEpisodes(cur.ctx.seriesId,
                                                    cur.ctx.seasonId);       break
            default: break
            }
        } else
            showServerList()
    }

    function showServerList() {
        navStack = []
        navTitle.text = qsTr("Jellyfin")
    }
    function showViews() {
        push("views", jellyfin.activeServerName.length ? jellyfin.activeServerName : qsTr("Könyvtárak"))
        jellyfin.fetchViews()
    }
    function showResume() {
        push("resume", qsTr("Folytatás"))
        jellyfin.fetchResume()
    }
    function showNextUp() {
        push("nextup", qsTr("Következő epizód"))
        jellyfin.fetchNextUp()
    }
    function showItems(parentId, name) {
        push("items", name, { parentId: parentId })
        jellyfin.fetchItems(parentId)
    }
    function showSeasons(seriesId, name) {
        push("seasons", name, { seriesId: seriesId })
        jellyfin.fetchSeasons(seriesId)
    }
    function showEpisodes(seriesId, seasonId, seasonName) {
        push("episodes", seasonName, { seriesId: seriesId, seasonId: seasonId })
        jellyfin.fetchEpisodes(seriesId, seasonId)
    }

    // Click behaviour depends on the item type:
    //   folder/library → drill in; Series → seasons; Season → episodes;
    //   Movie/Episode/Audio → play.
    function itemActivated(item) {
        if (item.isFolder)
            showItems(item.id, item.name)
        else if (item.type === "Series")
            showSeasons(item.id, item.name)
        else if (item.type === "Season")
            showEpisodes(item.seriesId, item.id, item.name)
        else {
            jellyfin.playItem(item)
            close()
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: Colors.overlay
        border.color: Colors.border
        border.width: 1
    }

    ColumnLayout {
        id: panelCol
        anchors.fill: parent
        anchors.topMargin: 6
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        anchors.bottomMargin: 6
        spacing: 6

        // --- header: back / title / server nav / close -------------------
        Row {
            Layout.fillWidth: true
            spacing: 4

            IconButton {
                implicitWidth: 26
                implicitHeight: 26
                glyph: "\uF053"                       // FA chevron-left
                tip: qsTr("Vissza")
                enabled: panel.hasBack
                opacity: enabled ? 1 : 0.4
                onClicked: goBack()
            }
            Text {
                id: navTitle
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Jellyfin")
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Colors.overlayText
                elide: Text.ElideMiddle
            }
            Item { Layout.fillWidth: true }

            IconButton {
                implicitWidth: 26
                implicitHeight: 26
                glyph: "\uF015"                       // FA house: server list
                tip: qsTr("Szerverek")
                onClicked: showServerList()
            }
            IconButton {
                implicitWidth: 26
                implicitHeight: 26
                glyph: "\uF00D"                       // FA xmark
                tip: qsTr("Bezárás (Esc)")
                onClicked: close()
            }
        }

        // --- mode quick-tabs (only makes sense while connected) ----------
        Row {
            id: quickRow
            Layout.fillWidth: true
            visible: jellyfin.activeServerName.length > 0
            spacing: 5

            readonly property var tabs: [
                { label: qsTr("Könyvtárak"), act: () => showViews() },
                { label: qsTr("Folytatás"),  act: () => showResume() },
                { label: qsTr("Következő"),  act: () => showNextUp() }
            ]
            Repeater {
                model: quickRow.tabs
                delegate: Rectangle {
                    required property var modelData
                    width: (quickRow.width - 10) / 3
                    height: 26
                    radius: 7
                    color: tabMouse.containsMouse ? Colors.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        font.pixelSize: 11
                        color: Colors.textDim
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: modelData.act()
                    }
                }
            }
        }

        // --- server management -------------------------------------------
        Flickable {
            id: serverFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: jellyfin.activeServerName.length === 0
            clip: true
            contentHeight: serverCol.implicitHeight
            Column {
                id: serverCol
                width: serverFlick.width
                spacing: 6

                // Add-server form.
                Row {
                    width: parent.width
                    spacing: 6
                    TextField {
                        id: newServerField
                        width: parent.width - 70
                        placeholderText: qsTr("Szerver URL, pl. https://jf.local")
                        placeholderTextColor: Colors.textDim
                        color: Colors.overlayText
                        font.pixelSize: 12
                        topPadding: 7
                        bottomPadding: 7
                        leftPadding: 10
                        background: Rectangle {
                            radius: 8
                            color: Colors.chrome
                            border.color: activeFocus ? Colors.accent : Colors.border
                            border.width: 1
                        }
                        onAccepted: addServerBtn.clicked()
                    }
                    Rectangle {
                        id: addServerBtn
                        width: 60
                        height: 34
                        radius: 10
                        color: addServerBtnH.containsMouse ? Colors.accentGlow : Colors.accent
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Hozzáad")
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: "#0b0b0e"
                        }
                        MouseArea {
                            id: addServerBtnH
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                if (newServerField.text.trim().length > 0) {
                                    jellyfin.addServer(newServerField.text.trim())
                                    newServerField.text = ""
                                }
                            }
                        }
                    }
                }

                // Stored servers: one-click connect, or log in.
                Repeater {
                    model: jellyfin.servers
                    delegate: Rectangle {
                        id: srow
                        required property var modelData
                        required property int index
                        width: serverCol.width
                        height: scCol.implicitHeight + 12
                        radius: 10
                        color: Colors.chrome

                        Column {
                            id: scCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4

                            Row {
                                width: parent.width
                                spacing: 6
                                Text {
                                    text: modelData.name
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: Colors.overlayText
                                    width: parent.parent.width - 92
                                    elide: Text.ElideMiddle
                                }
                                Text {
                                    text: modelData.token.length > 0 ? qsTr("Kapcsolódva") : qsTr("Nincs munkamenet")
                                    font.pixelSize: 10
                                    color: modelData.token.length > 0 ? Colors.accent : Colors.textDim
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            Text {
                                text: modelData.url.length > 16 ? modelData.url : modelData.url
                                font.pixelSize: 10
                                color: Colors.textDim
                                elide: Text.ElideMiddle
                                width: parent.width - 70
                            }

                            // Token present → one-click connect. Otherwise login fields.
                            Column {
                                id: loginCol
                                width: parent.width
                                spacing: 5
                                visible: modelData.token.length === 0

                                TextField {
                                    id: userField
                                    width: parent.width
                                    placeholderText: qsTr("Felhasználónév")
                                    placeholderTextColor: Colors.textDim
                                    color: Colors.overlayText
                                    font.pixelSize: 11
                                    topPadding: 6
                                    bottomPadding: 6
                                    leftPadding: 8
                                    background: Rectangle {
                                        radius: 7
                                        color: "#0b0b0e"
                                        border.color: activeFocus ? Colors.accent : Colors.border
                                    }
                                    onAccepted: if (passField.text.length === 0) passField.forceActiveFocus()
                                                else loginBtn.clicked()
                                }
                                TextField {
                                    id: passField
                                    width: parent.width
                                    echoMode: TextInput.Password
                                    placeholderText: qsTr("Jelszó")
                                    placeholderTextColor: Colors.textDim
                                    color: Colors.overlayText
                                    font.pixelSize: 11
                                    topPadding: 6
                                    bottomPadding: 6
                                    leftPadding: 8
                                    background: Rectangle {
                                        radius: 7
                                        color: "#0b0b0e"
                                        border.color: activeFocus ? Colors.accent : Colors.border
                                    }
                                    onAccepted: loginBtn.clicked()
                                }
                                Rectangle {
                                    id: loginBtn
                                    width: 100
                                    height: 28
                                    radius: 8
                                    color: loginH.containsMouse ? Colors.accentGlow : Colors.accent
                                    Text {
                                        anchors.centerIn: parent
                                        text: qsTr("Belépés")
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        color: "#0b0b0e"
                                    }
                                    MouseArea {
                                        id: loginH
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: jellyfin.login(index, userField.text, passField.text)
                                    }
                                }
                            }

                            // Logged-in → connect button (re-select a saved token).
                            Rectangle {
                                id: connectBtn
                                width: 90
                                height: 26
                                radius: 8
                                visible: modelData.token.length > 0
                                color: connectH.containsMouse ? Colors.accentGlow : Colors.accent
                                Row {
                                    anchors.centerIn: parent
                                    spacing: 6
                                    Text {
                                        text: "\uF04B"
                                        font.family: "Font Awesome 7 Free Solid"
                                        font.pixelSize: 10
                                        color: "#0b0b0e"
                                    }
                                    Text {
                                        text: qsTr("Betöltés")
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        color: "#0b0b0e"
                                    }
                                }
                                MouseArea {
                                    id: connectH
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        jellyfin.selectServer(index)
                                        if (jellyfin.activeServerName.length > 0)
                                            showViews()
                                    }
                                }
                            }

                            // Remove this server.
                            Text {
                                anchors.right: parent.parent.right
                                anchors.top: parent.parent.top
                                text: "\uF057"                   // FA circle-xmark
                                font.family: "Font Awesome 7 Free Solid"
                                font.pixelSize: 12
                                color: Colors.textDim
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: jellyfin.removeServer(index)
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- search (only while connected) --------------------------------
        RowLayout {
            visible: jellyfin.activeServerName.length > 0
            Layout.fillWidth: true
            spacing: 5

            IconButton {
                implicitWidth: 28
                implicitHeight: 28
                glyph: "\uF002"                     // FA magnifying-glass
                tip: qsTr("Keresés")
                onClicked: searchField.forceActiveFocus()
            }
            TextField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: qsTr("Keresés a tárban…")
                placeholderTextColor: Colors.textDim
                color: Colors.overlayText
                font.pixelSize: 12
                topPadding: 7
                bottomPadding: 7
                leftPadding: 10
                background: Rectangle {
                    radius: 8
                    color: Colors.chrome
                    border.color: activeFocus ? Colors.accent : Colors.border
                }
                onAccepted: if (text.trim().length > 0)
                                jellyfin.fetchSearch(text.trim())
            }
        }

        // --- empty state (no results / nothing loaded yet) ----------------
        Text {
            visible: jellyfin.activeServerName.length > 0
                     && !jellyfin.busy && jellyfin.items.length === 0
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Nincs találat")
            font.pixelSize: 11
            color: Colors.textDim
        }

        // --- listings (views, items, seasons, episodes, resume…) ---------
        ListView {
            id: itemList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: jellyfin.activeServerName.length > 0
            clip: true
            model: jellyfin.items
            spacing: 3
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: irow
                required property var modelData
                required property int index
                width: itemList.width
                height: modelData.type === "Season" ? 40 : 44
                radius: 8
                color: rowhover.containsMouse ? Colors.hover : "transparent"
                Behavior on color { ColorAnimation { duration: 90 } }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    // Thumbnail (or a type glyph when there is no image).
                    Rectangle {
                        width: 62
                        height: 36
                        radius: 6
                        color: Colors.chrome
                        clip: true
                        Image {
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectCrop
                            source: modelData.imageTag ? jellyfin.imageUrl(modelData.id, "Primary", 160).toString() : ""
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: !modelData.imageTag
                            text: modelData.isFolder ? "\uF07B"
                                  : modelData.type === "Series" ? "\uF6A6"
                                  : modelData.type === "Audio" ? "\uF001"
                                  : "\uF03D"
                            font.family: "Font Awesome 7 Free Solid"
                            font.pixelSize: 16
                            color: Colors.textDim
                        }
                    }

                    Column {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 1

                        Text {
                            width: parent.width
                            text: modelData.name
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: Colors.overlayText
                            elide: Text.ElideMiddle
                        }
                        Text {
                            width: parent.width
                            text: modelData.type === "Episode"
                                  ? modelData.seriesName
                                  : (modelData.parentIndexNumber > 0
                                     ? (modelData.seriesName + " · ") : "")
                                 + (modelData.parentIndexNumber > 0
                                    ? ("S%1E%2").arg(modelData.parentIndexNumber)
                                                .arg(String(modelData.indexNumber).padStart(2, "0"))
                                    : (modelData.productionYear > 0 ? qsTr("%1 év").arg(modelData.productionYear) : ""))
                            font.pixelSize: 10
                            color: Colors.textDim
                            elide: Text.ElideMiddle
                        }
                    }

                    // Resume badge for partially watched items.
                    Text {
                        visible: modelData.playbackPositionTicks > 0 && !modelData.played
                        text: "\uF071"                       // FA triangle-exclamation: resumed
                        font.family: "Font Awesome 7 Free Solid"
                        font.pixelSize: 11
                        color: Colors.accent
                        Layout.alignment: Qt.AlignVCenter
                    }
                    // Episodes/movies get a play affordance on the right.
                    Text {
                        visible: !modelData.isFolder && modelData.type !== "Series"
                                && modelData.type !== "Season"
                        text: "\uF144"                       // FA circle-play
                        font.family: "Font Awesome 7 Free Solid"
                        font.pixelSize: 14
                        color: rowhover.containsMouse ? Colors.accent : Colors.textDim
                        Layout.alignment: Qt.AlignVCenter
                    }
                }

                MouseArea {
                    id: rowhover
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: itemActivated(modelData)
                }
            }
        }

        // --- status ------------------------------------------------------
        Text {
            Layout.fillWidth: true
            text: jellyfin.status
            font.pixelSize: 10
            color: Colors.textDim
            elide: Text.ElideMiddle
            visible: jellyfin.status.length > 0
        }
    }
}