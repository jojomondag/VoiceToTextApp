cmake_minimum_required(VERSION 3.20)
project(VoiceToText LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
set(SOURCES
    src/main.cpp
    src/app.cpp
    src/pianissimo_engine.cpp
    src/speech_model_catalog.cpp
    src/live_dictation_buffer.cpp
    src/live_mode_processor.cpp
    src/mode_switch_policy.cpp
    src/dictation_stream.cpp
    src/dictation_window.cpp
    src/external_dictation_target.cpp
    src/clipboard_text_transaction.cpp
    src/dictation_diagnostics.cpp
    src/audio_capture.cpp
    src/app_icon.cpp
    src/hotkey_manager.cpp
    src/text_injector.cpp
    src/llm_cleaner.cpp
    src/gemma_runtime.cpp
    src/correction_store.cpp
    src/transcript_history.cpp
    src/history_window.cpp
    src/overlay_window.cpp
    src/config_manager.cpp
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

add_custom_target(package-standalone
    COMMAND powershell -ExecutionPolicy Bypass -File
            "${CMAKE_SOURCE_DIR}/scripts/package_standalone.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}"
            -Configuration "$<CONFIG>"
            -OutputDir "${CMAKE_SOURCE_DIR}/dist/VoiceToText"
    DEPENDS ${PROJECT_NAME}
    COMMENT "Creating standalone VoiceToText package"
)
