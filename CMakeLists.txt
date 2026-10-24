cmake_minimum_required(VERSION 3.21)

include(cmake/LpmMetadata.cmake)
project(${LILYGO_UI_CMAKE_PROJECT_NAME} LANGUAGES C CXX)

# The LVGL target is created by LilyGoUI during find_package(). Define this
# before importing AppKit so LVGL's optional QR-code sources are compiled too.
add_compile_definitions(LV_USE_QRCODE=1)

include(GNUInstallDirs)
include(CTest)
find_package(LilyGoUI CONFIG REQUIRED)
find_package(Threads REQUIRED)

configure_file(
    cmake/app_identity.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/app_identity.h
    @ONLY)

add_library(lilygo_ui_picoclaw_core STATIC
    src/domain/picoclaw_client.cpp
    src/domain/picoclaw_protocol.cpp
    src/domain/picoclaw_websocket.cpp
    src/pages/picoclaw/picoclaw_model.cpp)
target_include_directories(lilygo_ui_picoclaw_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_picoclaw_core PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_picoclaw_core PUBLIC cxx_std_17)
target_compile_definitions(lilygo_ui_picoclaw_core PRIVATE
    LILYGO_PICOCLAW_CONTROL_SCRIPT="${CMAKE_INSTALL_FULL_LIBDIR}/${LILYGO_UI_PACKAGE_NAME}/picoclaw-control"
    LILYGO_PICOCLAW_SOURCE_CONTROL_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/scripts/picoclaw-control.sh")
target_link_libraries(lilygo_ui_picoclaw_core PUBLIC Threads::Threads)

add_library(lilygo_ui_picoclaw_ui STATIC
    src/app.cpp
    src/components/components.cpp
    src/components/host_text_input.cpp
    src/pages/picoclaw/picoclaw_view.cpp
    src/pages/picoclaw/picoclaw_view_model.cpp)
target_include_directories(lilygo_ui_picoclaw_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_picoclaw_ui PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_picoclaw_ui PUBLIC
    lilygo_ui_picoclaw_core
    LilyGoUI::Runtime)

add_executable(lilygo_ui_picoclaw src/main.cpp)
target_compile_options(lilygo_ui_picoclaw PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_picoclaw PRIVATE lilygo_ui_picoclaw_ui)
set_target_properties(lilygo_ui_picoclaw PROPERTIES
    OUTPUT_NAME "${LILYGO_UI_EXECUTABLE_NAME}")

configure_file(data/launcher.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json @ONLY)
configure_file(data/application.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop @ONLY)
configure_file(data/application.metainfo.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml @ONLY)

install(TARGETS lilygo_ui_picoclaw
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT app)
install(PROGRAMS scripts/picoclaw-control.sh
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${LILYGO_UI_PACKAGE_NAME}
    RENAME picoclaw-control
    COMPONENT app)
install(PROGRAMS scripts/picoclaw-releases.py scripts/picoclaw-provider.py scripts/picoclaw-channels.py
                 scripts/picoclaw-weixin.py
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${LILYGO_UI_PACKAGE_NAME}
    COMPONENT app)
include(cmake/PicoClawRuntime.cmake)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/launcher/apps
    COMPONENT app)
lilygo_ui_install_launcher_icon(
    "${LILYGO_UI_APP_ID}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${LILYGO_UI_APP_ICON}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    COMPONENT app)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}"
    app)
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}
    COMPONENT app)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME picoclaw-control COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_control.py)
    add_test(NAME picoclaw-provider COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_provider.py)
    add_test(NAME picoclaw-channels COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_channels.py)
    add_test(NAME picoclaw-weixin COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_weixin.py)
    add_test(NAME picoclaw-releases COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_releases.py)

    add_executable(picoclaw-core-tests tests/test_core.cpp)
    target_compile_options(picoclaw-core-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(picoclaw-core-tests PRIVATE
        lilygo_ui_picoclaw_core)
    add_test(NAME picoclaw-core COMMAND picoclaw-core-tests)

    add_executable(picoclaw-protocol-tests tests/test_protocol.cpp)
    target_compile_options(picoclaw-protocol-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(picoclaw-protocol-tests PRIVATE
        lilygo_ui_picoclaw_ui)
    add_test(NAME picoclaw-protocol COMMAND picoclaw-protocol-tests)

    add_executable(picoclaw-websocket-tests tests/test_websocket.cpp)
    target_compile_options(picoclaw-websocket-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(picoclaw-websocket-tests PRIVATE
        lilygo_ui_picoclaw_core Threads::Threads)
    add_test(NAME picoclaw-websocket COMMAND picoclaw-websocket-tests)

    add_executable(picoclaw-view-model-tests tests/test_view_model.cpp)
    target_compile_options(picoclaw-view-model-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(picoclaw-view-model-tests PRIVATE
        lilygo_ui_picoclaw_ui)
    add_test(NAME picoclaw-view-model COMMAND picoclaw-view-model-tests)

    add_executable(picoclaw-render-ui tests/render_ui.cpp)
    target_compile_options(picoclaw-render-ui PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(picoclaw-render-ui PRIVATE
        lilygo_ui_picoclaw_ui)
    foreach(viewport IN ITEMS compact-portrait portrait landscape desktop)
        if(viewport STREQUAL "compact-portrait")
            set(_width 320)
            set(_height 568)
        elseif(viewport STREQUAL "portrait")
            set(_width 568)
            set(_height 1232)
        elseif(viewport STREQUAL "landscape")
            set(_width 1232)
            set(_height 568)
        else()
            set(_width 1024)
            set(_height 768)
        endif()
        add_test(NAME picoclaw-render-${viewport} COMMAND picoclaw-render-ui
            ${_width} ${_height}
            ${CMAKE_CURRENT_BINARY_DIR}/picoclaw-${viewport}.ppm)
    endforeach()
endif()

# New applications have no legacy CM0 package to replace. Add Debian migration
# fields only when this project supersedes an actually published package.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${LILYGO_UI_VERSION}")
if(LILYGO_UI_VERSION MATCHES
   "^([0-9]+:)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_4}")
else()
    set(CPACK_PACKAGE_VERSION_MAJOR 0)
    set(CPACK_PACKAGE_VERSION_MINOR 0)
    set(CPACK_PACKAGE_VERSION_PATCH 0)
endif()
set(CPACK_PACKAGE_CONTACT "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_VENDOR "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${LILYGO_UI_APP_HOMEPAGE}")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_SET_DESTDIR ON)
set(CPACK_COMPONENTS_ALL app)
set(CPACK_DEBIAN_APP_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_DEBIAN_APP_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_APP_PACKAGE_DEPENDS
    "libc6, libstdc++6, libdrm2, libfreetype6, ca-certificates, curl, tar, python3 (>= 3.9), util-linux, lilygo-ui-appkit-dev (>= ${LILYGO_UI_MIN_APPKIT_VERSION})")
set(CPACK_DEBIAN_APP_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        "${LILYGO_UI_PACKAGE_ARCHITECTURE}")
endif()
include(CPack)
