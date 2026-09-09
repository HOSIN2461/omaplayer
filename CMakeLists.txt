cmake_minimum_required(VERSION 3.28)
project(omaplayer VERSION 0.2.9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Widgets Network)
# D-Bus (MPRIS) exists only on Unix desktops — never on Windows/macOS.
if(UNIX AND NOT APPLE)
    find_package(Qt6 6.5 REQUIRED COMPONENTS DBus)
endif()
find_package(PkgConfig)
if(NOT WIN32 AND NOT PkgConfig_FOUND)
    message(FATAL_ERROR "pkg-config is required on this platform")
endif()
find_package(OpenSSL REQUIRED)
# The libmpv client library (mpv render API + property/command API).
# Linux: system pkg-config. macOS (Homebrew): pkg-config if the .pc ships
# with the formula, otherwise the brew prefix layout below. Windows
# (CI/vcpkg or mpv-winbuild): point MPV_ROOT at a tree with
# include/mpv/client.h and the import library.
if(WIN32)
    find_path(MPV_INCLUDE_DIR NAMES mpv/client.h PATHS ENV MPV_ROOT
              PATH_SUFFIXES include NO_DEFAULT_PATH)
    find_library(MPV_LIBRARY NAMES mpv libmpv mpv-2
                 PATHS ENV MPV_ROOT PATH_SUFFIXES lib NO_DEFAULT_PATH)
    if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
        message(FATAL_ERROR "libmpv not found: set MPV_ROOT (include/mpv/client.h + lib)")
    endif()
elseif(APPLE)
    pkg_check_modules(MPV QUIET IMPORTED_TARGET mpv)
    if(NOT MPV_FOUND)
        # Homebrew layout (/opt/homebrew, /usr/local, or $HOMEBREW_PREFIX).
        find_path(MPV_INCLUDE_DIR NAMES mpv/client.h
                  PATHS ENV HOMEBREW_PREFIX
                  PATHS /opt/homebrew /usr/local
                  PATH_SUFFIXES include NO_DEFAULT_PATH)
        find_library(MPV_LIBRARY NAMES mpv
                     PATHS ENV HOMEBREW_PREFIX
                     PATHS /opt/homebrew /usr/local
                     PATH_SUFFIXES lib NO_DEFAULT_PATH)
        if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
            message(FATAL_ERROR "libmpv not found: brew install mpv (or set MPV_ROOT)")
        endif()
        set(MPV_USE_MANUAL TRUE)
    endif()
else()
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
endif()
# libsodium (ed25519/X25519/ChaCha20-Poly1305 for AirPlay pairing).
if(WIN32)
    # vcpkg supplies an unofficial config package.
    find_package(unofficial-sodium CONFIG REQUIRED)
    set(SODIUM_TARGET unofficial::sodium::sodium)
else()
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
    set(SODIUM_TARGET PkgConfig::SODIUM)
endif()

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

qt_add_executable(omaplayer
    src/main.cpp
    src/MpvCore.cpp
    src/MpvCore.h
    src/MpvVideoItem.cpp
    src/MpvVideoItem.h
    src/Updater.cpp
    src/Updater.h
    src/JellyfinClient.cpp
    src/JellyfinClient.h
    src/CastManager.cpp
    src/CastManager.h
    src/DirModel.cpp
    src/DirModel.h
    src/GoogleCastClient.cpp
    src/GoogleCastClient.h
    src/AirPlayPairing.cpp
    src/AirPlayPairing.h
    src/AirPlaySession.cpp
    src/AirPlaySession.h
    src/Bplist.cpp
    src/Bplist.h
    src/HlsSession.cpp
    src/HlsSession.h
    src/SubtitleClient.cpp
    src/SubtitleClient.h
    src/KeyManager.cpp
    src/KeyManager.h
    src/AudioIntroMatcher.cpp
    src/AudioIntroMatcher.h
    src/MetadataInfo.cpp
    src/MetadataInfo.h
    src/SeekThumbnails.cpp
    src/SeekThumbnails.h
)

# MPRIS lives on Linux desktops only (needs Qt D-Bus).
if(UNIX AND NOT APPLE)
    target_sources(omaplayer PRIVATE
        src/MprisPlayer.cpp
        src/MprisPlayer.h
    )
endif()

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
        qml/CastPanel.qml
        qml/FileBrowser.qml
        qml/MetaOverlay.qml
        qml/Toast.qml
        qml/StatsOverlay.qml
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
    ${SODIUM_TARGET}
    OpenSSL::Crypto
)
if(UNIX AND NOT APPLE)
    target_link_libraries(omaplayer PRIVATE Qt6::DBus)
endif()
if(WIN32)
    target_include_directories(omaplayer PRIVATE ${MPV_INCLUDE_DIR})
    target_link_libraries(omaplayer PRIVATE ${MPV_LIBRARY})
    # GUI subsystem (no console window); version/product metadata below.
    set_target_properties(omaplayer PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    if(MPV_USE_MANUAL)
        target_include_directories(omaplayer PRIVATE ${MPV_INCLUDE_DIR})
        target_link_libraries(omaplayer PRIVATE ${MPV_LIBRARY})
    else()
        target_link_libraries(omaplayer PRIVATE PkgConfig::MPV)
    endif()
    # App bundle (macdeployqt runs in CI on top of this).
    set_target_properties(omaplayer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.omarchy.omaplayer"
        MACOSX_BUNDLE_BUNDLE_NAME "omaplayer"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
else()
    target_link_libraries(omaplayer PRIVATE PkgConfig::MPV)
endif()

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

if(APPLE)
    # App bundle (macdeployqt runs in CI on top of the build tree).
    install(TARGETS omaplayer BUNDLE DESTINATION .)
elseif(UNIX)
    install(TARGETS omaplayer RUNTIME DESTINATION bin)
    install(FILES LICENSE DESTINATION share/licenses/omaplayer)
    install(FILES packaging/omaplayer.desktop DESTINATION share/applications)
    install(FILES packaging/omaplayer.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()