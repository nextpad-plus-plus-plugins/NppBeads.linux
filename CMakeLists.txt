cmake_minimum_required(VERSION 3.20)
project(NppBeads VERSION 0.9.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0 json-glib-1.0)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET gtk4 libadwaita-1 webkitgtk-6.0)

set(NPP_LINUX_DIR "${CMAKE_SOURCE_DIR}/../../nextpad-plus-plus-gtk4")

# Non-UI native layer as a static lib (shared by the plugin, the standalone
# app, and the self-test).
add_library(beads_core STATIC
    src/BdCommandRunner.cpp
    src/JsonlDataSource.cpp
    src/BdDataSource.cpp
    src/BeadsProjectScanner.cpp
    src/BeadsWatcher.cpp
    src/BeadsPoll.cpp)
target_include_directories(beads_core PUBLIC src "${NPP_LINUX_DIR}/src"
    "${NPP_LINUX_DIR}/scintilla/include")
target_link_libraries(beads_core PUBLIC PkgConfig::GLIB)
set_property(TARGET beads_core PROPERTY POSITION_INDEPENDENT_CODE ON)

# View layer (needs WebKitGTK/GTK4): scheme handler + editor indicator.
# The full panel + plugin entry land here as they're ported.
add_library(beads_view STATIC
    src/BeadsSchemeHandler.cpp
    src/BeadIdIndicator.cpp
    src/BeadsPanel.cpp)
target_include_directories(beads_view PUBLIC src "${NPP_LINUX_DIR}/src")
target_link_libraries(beads_view PUBLIC beads_core PkgConfig::WEBKIT)
set_property(TARGET beads_view PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────
#  Target — NppBeads.so (the Nextpad++ plugin)
# ─────────────────────────────────────────────────────────────────────────
add_library(NppBeads SHARED src/NppBeads.cpp)
set_target_properties(NppBeads PROPERTIES PREFIX "" OUTPUT_NAME "NppBeads")
target_include_directories(NppBeads PRIVATE src "${NPP_LINUX_DIR}/src")
target_link_libraries(NppBeads PRIVATE beads_view)
# scintilla_view_send_message resolves at dlopen from the host's libscintilla.so
# global scope — leave it undefined at link time.
target_link_options(NppBeads PRIVATE -Wl,--allow-shlib-undefined)

# Headless self-test for the native layer.
add_executable(beads_selftest src/beads_selftest.cpp)
target_link_libraries(beads_selftest PRIVATE beads_core)

# ─────────────────────────────────────────────────────────────────────────
#  Install + package (folder layout: NppBeads.so + resources/ + toolbar.png)
# ─────────────────────────────────────────────────────────────────────────
set(NPPBEADS_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/NppBeads"
    CACHE PATH "Where to install the plugin folder")
install(TARGETS NppBeads DESTINATION "${NPPBEADS_INSTALL_DIR}")
install(FILES resources/toolbar.png DESTINATION "${NPPBEADS_INSTALL_DIR}/resources")
install(DIRECTORY resources/viewer DESTINATION "${NPPBEADS_INSTALL_DIR}/resources"
    PATTERN ".git" EXCLUDE PATTERN ".DS_Store" EXCLUDE PATTERN ".gitignore" EXCLUDE)

# Distributable zip: NppBeads/NppBeads.so + NppBeads/resources/...
set(NPPBEADS_STAGE "${CMAKE_BINARY_DIR}/pkg/NppBeads")
file(MAKE_DIRECTORY "${NPPBEADS_STAGE}/resources")
add_custom_target(package_zip
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:NppBeads> "${NPPBEADS_STAGE}/NppBeads.so"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/resources/toolbar.png" "${NPPBEADS_STAGE}/resources/toolbar.png"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources/viewer" "${NPPBEADS_STAGE}/resources/viewer"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/pkg"
            ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/NppBeads.linux.zip" --format=zip NppBeads
    DEPENDS NppBeads
    COMMENT "Packaging NppBeads.linux.zip")

# Headless WebView harness (exercises panel + scheme handler + JS bridge).
add_executable(beads_panel_harness src/beads_panel_harness.cpp)
target_link_libraries(beads_panel_harness PRIVATE beads_view)

# ─────────────────────────────────────────────────────────────────────────
#  Target — BeadsViewer (standalone app; reuses beads_core + beads_view)
# ─────────────────────────────────────────────────────────────────────────
add_executable(BeadsViewer shell-app/beads_app.cpp)
set_target_properties(BeadsViewer PROPERTIES OUTPUT_NAME "beadsviewer")
target_include_directories(BeadsViewer PRIVATE src shell-app)
target_link_libraries(BeadsViewer PRIVATE beads_view)

# System install (binary → bin/, viewer → share/beadsviewer/resources — the
# resolveResourcesDir() fallback path — plus a .desktop launcher).
install(TARGETS BeadsViewer RUNTIME DESTINATION bin)
install(FILES resources/toolbar.png DESTINATION share/beadsviewer/resources)
install(DIRECTORY resources/viewer DESTINATION share/beadsviewer/resources
    PATTERN ".git" EXCLUDE PATTERN ".DS_Store" EXCLUDE PATTERN ".gitignore" EXCLUDE)
install(FILES shell-app/beadsviewer.desktop DESTINATION share/applications)
