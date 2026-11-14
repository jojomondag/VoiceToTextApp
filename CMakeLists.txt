cmake_minimum_required(VERSION 3.20)
project(VoiceToText LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Single-config generators (Ninja, Makefiles) default to an unoptimised build.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Pianissimo speech providers (the same versions as TeachersLittleHelper)
# ---------------------------------------------------------------------------
include(cmake/PianissimoDependencies.cmake)

# ---------------------------------------------------------------------------
# miniaudio  (single-header library -- only need the header, not its build)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(miniaudio)

add_library(miniaudio_header INTERFACE)
target_include_directories(miniaudio_header INTERFACE ${miniaudio_SOURCE_DIR})

# ---------------------------------------------------------------------------
# nlohmann/json
# ---------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# Application sources
# ---------------------------------------------------------------------------
# Shared between the platforms.
set(SHARED_SOURCES
    src/app.cpp
    src/pianissimo_engine.cpp
    src/speech_model_catalog.cpp
    src/live_dictation_buffer.cpp
    src/live_mode_processor.cpp
    src/mode_switch_policy.cpp
    src/dictation_stream.cpp
    src/dictation_diagnostics.cpp
    src/audio_capture.cpp
    src/llm_cleaner.cpp
    src/gemma_runtime.cpp
    src/correction_store.cpp
    src/transcript_history.cpp
    src/config_manager.cpp
)

if(WIN32)
    # Win32 UI, UI Automation, WinHTTP, RegisterHotKey, SendInput
    set(SOURCES
        ${SHARED_SOURCES}
        src/main.cpp
        src/dictation_window.cpp
        src/external_dictation_target.cpp
        src/clipboard_text_transaction.cpp
        src/app_icon.cpp
        src/hotkey_manager.cpp
        src/text_injector.cpp
        src/history_window.cpp
        src/overlay_window.cpp
        src/tray_icon.cpp
    )

    add_executable(${PROJECT_NAME} WIN32 ${SOURCES} resources/app.rc)

    target_link_libraries(${PROJECT_NAME} PRIVATE
        miniaudio_header
        nlohmann_json::nlohmann_json
        user32
        winhttp
        shell32
        dwmapi
        gdiplus
        comctl32
        urlmon
        bcrypt
        ole32
        oleaut32
        uiautomationcore
    )
    target_include_directories(${PROJECT_NAME} PRIVATE "${VTT_SPEECH_INCLUDE_DIR}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    endif()
    vtt_copy_speech_runtime(${PROJECT_NAME})
else()
    # Linux: Qt 6 UI (tray, overlay, windows), libcurl, Hyprland key binds,
    # wtype / wl-clipboard for text. See src/linux.
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    # Optional: places the status overlay as a wlr-layer-shell surface.
    find_package(LayerShellQt QUIET)

    # The app's own virtual keyboard speaks zwp_virtual_keyboard_v1.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(VTT_PROTOCOL_XML "${CMAKE_SOURCE_DIR}/protocols/virtual-keyboard-unstable-v1.xml")
    set(VTT_PROTOCOL_DIR "${CMAKE_BINARY_DIR}/protocols")
    file(MAKE_DIRECTORY "${VTT_PROTOCOL_DIR}")
    add_custom_command(
        OUTPUT "${VTT_PROTOCOL_DIR}/virtual-keyboard-unstable-v1-client-protocol.h"
               "${VTT_PROTOCOL_DIR}/virtual-keyboard-unstable-v1-protocol.c"
        COMMAND ${WAYLAND_SCANNER} client-header "${VTT_PROTOCOL_XML}"
                "${VTT_PROTOCOL_DIR}/virtual-keyboard-unstable-v1-client-protocol.h"
        COMMAND ${WAYLAND_SCANNER} private-code "${VTT_PROTOCOL_XML}"
                "${VTT_PROTOCOL_DIR}/virtual-keyboard-unstable-v1-protocol.c"
        DEPENDS "${VTT_PROTOCOL_XML}"
        VERBATIM)

    set(SOURCES
        ${SHARED_SOURCES}
        src/log.cpp
        src/linux/main.cpp
        src/linux/win_compat.cpp
        src/linux/platform_linux.cpp
        src/linux/hyprland_ipc.cpp
        src/linux/wayland_input.cpp
        src/linux/virtual_keyboard.cpp
        "${VTT_PROTOCOL_DIR}/virtual-keyboard-unstable-v1-protocol.c"
        src/linux/app_icon.cpp
        src/linux/hotkey_manager.cpp
        src/linux/text_injector.cpp
        src/linux/dictation_window.cpp
        src/linux/external_dictation_target.cpp
        src/linux/history_window.cpp
        src/linux/overlay_window.cpp
        src/linux/tray_icon.cpp
    )

    add_executable(${PROJECT_NAME} ${SOURCES})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        AUTOMOC ON
        # The speech runtime libraries are loaded from next to the executable.
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )

    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${VTT_SPEECH_INCLUDE_DIR}"
        "${VTT_PROTOCOL_DIR}"
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE
        miniaudio_header
        nlohmann_json::nlohmann_json
        Qt6::Widgets
        PkgConfig::WAYLAND_CLIENT
        CURL::libcurl
        Threads::Threads
        ${VTT_SPEECH_LINK_LIBRARIES}
        ${CMAKE_DL_LIBS}
        m
    )
    if(LayerShellQt_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE LayerShellQt::Interface)
        target_compile_definitions(${PROJECT_NAME} PRIVATE VTT_HAVE_LAYER_SHELL)
    else()
        message(STATUS "LayerShellQt not found: the overlay will be a normal window")
    endif()
    vtt_copy_speech_runtime(${PROJECT_NAME})
endif()

# Set working directory for debugging inside Visual Studio / IDE
set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY      "${CMAKE_BINARY_DIR}/bin"
)

set(VTT_RUNTIME_RESOURCES
    "${CMAKE_SOURCE_DIR}/resources/overlay_voice_mode.png"
    "${CMAKE_SOURCE_DIR}/resources/overlay_ai_language_mode.png"
    "${CMAKE_SOURCE_DIR}/resources/overlay_cleanup_mode.png"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${VTT_RUNTIME_RESOURCES}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
)

if(NOT WIN32)
    # `cmake --install build --component app --prefix ~/.local` puts the
    # program with its runtime libraries and icons in lib/voicetotext, links
    # it into bin, and adds a launcher entry.
    include(GNUInstallDirs)
    set(VTT_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/voicetotext")
    install(TARGETS ${PROJECT_NAME}
            RUNTIME DESTINATION "${VTT_INSTALL_DIR}" COMPONENT app)
    install(FILES ${VTT_SPEECH_RUNTIME_LIBS}
            DESTINATION "${VTT_INSTALL_DIR}" COMPONENT app)
    install(FILES ${VTT_RUNTIME_RESOURCES}
            DESTINATION "${VTT_INSTALL_DIR}/resources" COMPONENT app)
    install(DIRECTORY "$<TARGET_FILE_DIR:${PROJECT_NAME}>/licenses"
            DESTINATION "${VTT_INSTALL_DIR}" COMPONENT app)
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
        file(CREATE_LINK \"../${VTT_INSTALL_DIR}/${PROJECT_NAME}\"
             \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/${PROJECT_NAME}\"
             SYMBOLIC)
        message(STATUS \"Linked ${CMAKE_INSTALL_BINDIR}/${PROJECT_NAME}\")
    " COMPONENT app)
    install(FILES resources/linux/voicetotext.desktop
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT app)
    install(FILES resources/linux/voicetotext.svg
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
            COMPONENT app)
endif()

if(WIN32)
add_custom_target(package-standalone
    COMMAND powershell -ExecutionPolicy Bypass -File
            "${CMAKE_SOURCE_DIR}/scripts/package_standalone.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}"
            -Configuration "$<CONFIG>"
            -OutputDir "${CMAKE_SOURCE_DIR}/dist/VoiceToText"
    DEPENDS ${PROJECT_NAME}
    COMMENT "Creating standalone VoiceToText package"
)
endif()
