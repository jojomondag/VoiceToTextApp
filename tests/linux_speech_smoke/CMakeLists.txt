# Standalone Linux smoke test for the Pianissimo speech engine and its model
# catalog. It builds without the rest of the application:
#
#   cmake -S tests/linux_speech_smoke -B build-smoke -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-smoke
#   build-smoke/linux_speech_smoke onnx-int8 speech-16k-mono.wav
cmake_minimum_required(VERSION 3.20)
project(VoiceToTextLinuxSpeechSmoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(VTT_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VTT_SPEECH_HEADER_CACHE "${VTT_REPO}/artifacts/speech-headers" CACHE PATH
    "Optional offline cache of the pinned speech API headers")
include("${VTT_REPO}/cmake/PianissimoDependencies.cmake")

add_executable(linux_speech_smoke
    "${VTT_REPO}/tests/linux_speech_smoke.cpp"
    "${VTT_REPO}/src/pianissimo_engine.cpp"
    "${VTT_REPO}/src/speech_model_catalog.cpp")
target_include_directories(linux_speech_smoke PRIVATE "${VTT_REPO}/src" "${VTT_SPEECH_INCLUDE_DIR}")
target_compile_options(linux_speech_smoke PRIVATE -Wall -Wextra)
target_link_libraries(linux_speech_smoke PRIVATE ${VTT_SPEECH_LINK_LIBRARIES})
vtt_copy_speech_runtime(linux_speech_smoke)
