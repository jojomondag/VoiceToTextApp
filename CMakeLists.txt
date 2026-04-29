cmake_minimum_required(VERSION 3.20)
project(VoiceToText LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# whisper.cpp  (with optional CUDA / GPU support)
# ---------------------------------------------------------------------------
option(VTT_CUDA "Enable CUDA GPU acceleration" OFF)
if(VTT_CUDA)
    set(GGML_CUDA ON CACHE BOOL "Enable CUDA support for whisper.cpp" FORCE)
else()
    set(GGML_CUDA OFF CACHE BOOL "Enable CUDA support for whisper.cpp" FORCE)
endif()
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(whisper)

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
    src/whisper_engine.cpp
    src/audio_capture.cpp
    src/hotkey_manager.cpp
    src/text_injector.cpp
    src/llm_cleaner.cpp
    src/correction_store.cpp
    src/transcript_history.cpp
    src/history_window.cpp
    src/overlay_window.cpp
    src/config_manager.cpp
    src/tray_icon.cpp
    src/vad.cpp
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES} resources/app.rc)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${whisper_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    whisper
    miniaudio_header
    nlohmann_json::nlohmann_json
    user32
    winhttp
    shell32
    ole32
    winmm
    dwmapi
)

# Set working directory for debugging inside Visual Studio / IDE
set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY      "${CMAKE_BINARY_DIR}/bin"
)
