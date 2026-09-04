pragma Singleton
import QtQuick

// A video player surface is almost all video — chrome-color themes matter
// little, but the controls need to stay readable on any content, so they get
// a translucent black backing instead of a light/dark flip.
QtObject {
    readonly property color chrome: "#1a1a1e"
    readonly property color overlay: "#cc1a1a1e"      // control bar backing
    readonly property color overlayText: "#ffffff"
    readonly property color accent: "#4ea1ff"
    readonly property color textDim: "#a0a0a8"
    readonly property color border: "#3c3c42"
    readonly property color hover: "#2e2e34"
    readonly property color selection: "#3a4a6a"
    readonly property real radius: 8
    readonly property real controlHeight: 40        // bar row height
    readonly property real thumbHeight: 26          // seekbar thumb
}