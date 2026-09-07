cmake_minimum_required(VERSION 3.28)
project(omaplayer VERSION 0.2.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 DBus Widgets Network)
find_package(PkgConfig REQUIRED)
# The libmpv client library (mpv render API + property/command API).
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

qt_add_executable(omaplayer
    src/main.cpp
    src/MpvCore.cpp
    src/MpvCore.h
    src/MpvVideoItem.cpp
    src/MpvVideoItem.h
    src/MprisPlayer.cpp
    src/MprisPlayer.h
    src/Updater.cpp
    src/Updater.h
    src/JellyfinClient.cpp
    src/JellyfinClient.h
    src/AudioIntroMatcher.cpp
    src/AudioIntroMatcher.h
    src/MetadataInfo.cpp
    src/MetadataInfo.h
)

set_source_files_properties(qml/Colors.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(omaplayer
    URI Omaplayer
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/ControlBar.qml
        qml/IconButton.qml
        qml/Colors.qml
        qml/JellyfinPanel.qml
        qml/MetaOverlay.qml
        qml/Toast.qml
)

target_include_directories(omaplayer PRIVATE src)

# Version macro: used by main.cpp for QCoreApplication::setApplicationVersion,
# keeping the header free of a hand-maintained number.
target_compile_definitions(omaplayer PRIVATE
    OMAPLAYER_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(omaplayer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Network
    Qt6::DBus
    PkgConfig::MPV
)

# --- Hungarian translation (pre-compiled .qm shipped in-tree) ---
qt_add_resources(omaplayer "translations"
    PREFIX "/translations"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/translations"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/translations/hu.qm"
)

# --- Vendored audio-intro-match helper (iina-skip-intro) ---
qt_add_resources(omaplayer "audio_intro"
    PREFIX "/audio-intro"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/res"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/res/audio-intro-helper.mjs"
)

install(TARGETS omaplayer RUNTIME DESTINATION bin)
install(FILES LICENSE DESTINATION share/licenses/omaplayer)
install(FILES packaging/omaplayer.desktop DESTINATION share/applications)
install(FILES packaging/omaplayer.svg DESTINATION share/icons/hicolor/scalable/apps)