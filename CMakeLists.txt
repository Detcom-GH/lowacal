cmake_minimum_required(VERSION 3.21)
project(lowacal VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Reverse-DNS application id. Flathub rules for GitHub-hosted apps:
# io.github.<user>.<repo>, with '-' in the user name written as '_'
# (Detcom-GH -> detcom_gh), all lowercase as AppStream recommends (GitHub
# itself is case-insensitive). Names the .desktop/metainfo/icon files, the
# D-Bus name used for single-instance, and the Flatpak app itself.
# Changing it after a Flathub release is a migration, not a rename.
set(LOWACAL_APP_ID "io.github.detcom_gh.lowacal")

# Google Calendar needs an OAuth client registered in Google Cloud (type
# "Desktop app"; see README, "Google"). Without one the app still builds
# and works, with the Google option shown as unavailable.
set(LOWACAL_GOOGLE_CLIENT_ID "" CACHE STRING "OAuth client ID for Google Calendar (Desktop app type)")
set(LOWACAL_GOOGLE_CLIENT_SECRET "" CACHE STRING "OAuth client secret that goes with LOWACAL_GOOGLE_CLIENT_ID")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Sql Widgets DBus LinguistTools)
find_package(Qt6Keychain REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW) # all our .qml files live flatly under qml/, no per-directory qmldir needed
endif()

qt_add_executable(lowacal
    src/main.cpp
    src/database.cpp
    src/icalparser.cpp
    src/caldavclient.cpp
    src/config.cpp
    src/calendarbridge.cpp
    src/traycontroller.cpp
    src/syncworker.cpp
    src/keychain.cpp
    src/appwiring.cpp
    src/platform.cpp
    src/googleauth.cpp
)
target_compile_definitions(lowacal PRIVATE
    LOWACAL_GOOGLE_CLIENT_ID="${LOWACAL_GOOGLE_CLIENT_ID}"
    LOWACAL_GOOGLE_CLIENT_SECRET="${LOWACAL_GOOGLE_CLIENT_SECRET}"
    LOWACAL_APP_ID="${LOWACAL_APP_ID}"
    LOWACAL_VERSION="${PROJECT_VERSION}")

# --- branding ---
# lowacal-<size>.png: the full logo on Flathub's icon grid (tile = 80.5% of
#   the canvas) - menus, window, notifications, app stores.
# lowacal-tray-<size>.png: simplified calendar glyph for the tray, rendered
#   from lowacal-tray.svg; the full logo is unreadable at 22 px.
set(LOWACAL_ICON_SIZES 32 48 64 128 256 512)
set(LOWACAL_ICON_FILES)
foreach(size ${LOWACAL_ICON_SIZES})
    list(APPEND LOWACAL_ICON_FILES data/icons/lowacal-${size}.png)
endforeach()
foreach(size 16 22 24 32 44 48 64)
    list(APPEND LOWACAL_ICON_FILES data/icons/lowacal-tray-${size}.png)
endforeach()
qt_add_resources(lowacal "branding"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/data/icons"
    FILES ${LOWACAL_ICON_FILES}
)

qt_add_qml_module(lowacal
    URI dev.lowa.calendar
    VERSION 1.0
    RESOURCE_PREFIX "/qt/qml"
    QML_FILES
        qml/main.qml
        qml/Sidebar.qml
        qml/MonthGrid.qml
        qml/WeekDayGrid.qml
        qml/EventChip.qml
        qml/EventCard.qml
        qml/EventEditor.qml
        qml/SettingsDialog.qml
        qml/AccountDialog.qml
        qml/AboutDialog.qml
        qml/AsciiDachshund.qml
)

# --- translations ---
# English is the source language (the strings in the code). Each
# i18n/lowacal_<lang>.ts holds one translation; lrelease compiles it to a
# .qm that is embedded at :/i18n/. To pick up new/changed strings after
# editing the code:  cmake --build build --target update_translations
qt_add_translations(lowacal
    TS_FILES i18n/lowacal_uk.ts
    QM_FILES_OUTPUT_VARIABLE LOWACAL_QM_FILES
)
qt_add_resources(lowacal "lowacal_translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${LOWACAL_QM_FILES}
)

target_link_libraries(lowacal PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
    Qt6::DBus
    Qt6Keychain::Qt6Keychain
)

# --- Optional standalone test target: exercises Database/IcalParser/Config
# directly, with zero QML/Kirigami/network involved. Useful for CI or for
# sanity-checking the parsing logic in isolation.
option(LOWACAL_BUILD_TESTS "Build the standalone logic smoke test" OFF)
if(LOWACAL_BUILD_TESTS)
    qt_add_executable(lowacal_logic_test
        tests/logic_test.cpp
        src/database.cpp
        src/icalparser.cpp
        src/config.cpp
        src/caldavclient.cpp
    )
    target_link_libraries(lowacal_logic_test PRIVATE Qt6::Core Qt6::Sql Qt6::Network)
    target_include_directories(lowacal_logic_test PRIVATE src)

    # End-to-end UI test: real QML + real backend wiring, against a fake
    # CalDAV server and a real keychain. Run with tests/run_ui_flow_test.sh.
    qt_add_executable(lowacal_ui_flow_test
        tests/ui_flow_test.cpp
        src/appwiring.cpp
        src/calendarbridge.cpp
        src/syncworker.cpp
        src/caldavclient.cpp
        src/database.cpp
        src/icalparser.cpp
        src/config.cpp
        src/keychain.cpp
        src/platform.cpp
        src/googleauth.cpp
    )
    target_include_directories(lowacal_ui_flow_test PRIVATE src)
    target_compile_definitions(lowacal_ui_flow_test PRIVATE LOWACAL_APP_ID="${LOWACAL_APP_ID}")
    qt_add_resources(lowacal_ui_flow_test "test_branding"
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/data/icons"
        FILES ${LOWACAL_ICON_FILES}
    )
    qt_add_resources(lowacal_ui_flow_test "test_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}"
        FILES ${LOWACAL_QM_FILES}
    )
    target_compile_definitions(lowacal_ui_flow_test PRIVATE LOWACAL_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(lowacal_ui_flow_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2
        Qt6::Network Qt6::Sql Qt6::DBus Qt6Keychain::Qt6Keychain)

    # Launch-at-login (file + Background portal) and single-instance.
    qt_add_executable(lowacal_platform_test tests/platform_test.cpp src/platform.cpp)
    target_include_directories(lowacal_platform_test PRIVATE src)
    target_compile_definitions(lowacal_platform_test PRIVATE LOWACAL_APP_ID="${LOWACAL_APP_ID}")
    target_link_libraries(lowacal_platform_test PRIVATE Qt6::Core Qt6::DBus)
endif()

# Default to a per-user install (~/.local/bin + ~/.local/share/applications)
# unless the caller explicitly chose a prefix - that's what makes the
# desktop file's `Exec=lowacal` actually resolve: ~/.local/bin is on PATH
# on typical desktop distros, and Plasma reads desktop files from
# ~/.local/share/applications. A system-wide install still works via an
# explicit -DCMAKE_INSTALL_PREFIX=/usr/local + sudo.
#
# The second condition handles build dirs configured before this default
# existed: their cache already holds CMake's stock /usr/local, so
# INITIALIZED_TO_DEFAULT is false there, and `cmake --install` would try
# (and fail without sudo) to write to /usr/local. Checked only once per
# build dir (LOWACAL_PREFIX_CHECKED); packagers pass an explicit prefix
# (/usr for AUR, /app for Flatpak), which is never touched.
if(NOT LOWACAL_PREFIX_CHECKED)
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
    endif()
    set(LOWACAL_PREFIX_CHECKED ON CACHE INTERNAL "")
endif()

install(TARGETS lowacal BUNDLE DESTINATION . RUNTIME DESTINATION bin)
configure_file(data/lowacal.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/${LOWACAL_APP_ID}.desktop" @ONLY)
configure_file(data/lowacal.metainfo.xml.in "${CMAKE_CURRENT_BINARY_DIR}/${LOWACAL_APP_ID}.metainfo.xml" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${LOWACAL_APP_ID}.desktop" DESTINATION share/applications)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${LOWACAL_APP_ID}.metainfo.xml" DESTINATION share/metainfo)
foreach(size ${LOWACAL_ICON_SIZES})
    install(FILES data/icons/lowacal-${size}.png
            DESTINATION share/icons/hicolor/${size}x${size}/apps
            RENAME "${LOWACAL_APP_ID}.png")
endforeach()
