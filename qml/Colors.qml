pragma Singleton
import QtQuick

// A video player surface is almost all video — chrome-color themes matter
// little, but the controls need to stay readable on any content, so they get
// a translucent dark backing instead of a light/dark flip.
QtObject {
    readonly property color chrome: "#15151a"
    readonly property color overlay: "#e00d0d12"      // floating pill backing
    readonly property color overlayText: "#f2f3f5"
    readonly property color accent: "#4ea1ff"
    readonly property color accentGlow: "#6fb7ff"
    readonly property color textDim: "#9aa0aa"
    readonly property color border: "#2c2c33"
    readonly property color borderGlow: "#3a76bd"      // hover highlight
    readonly property color hover: "#232327"
    readonly property color selection: "#3a4a6a"
    readonly property color track: "#3a3a42"
    readonly property real radius: 13                   // pill radius
    readonly property real controlHeight: 42            // bar row height
    readonly property real thumbSize: 14                // seekbar thumb
}