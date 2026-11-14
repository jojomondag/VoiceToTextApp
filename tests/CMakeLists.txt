cmake_minimum_required(VERSION 3.20)
project(VoiceToTextRegressionTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(VTT_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_path(VTT_JSON_INCLUDE nlohmann/json.hpp PATHS
    "${VTT_REPO}/artifacts/pianissimo-tooltip-build/_deps/nlohmann_json-src/single_include"
    "${VTT_REPO}/build/_deps/nlohmann_json-src/single_include"
    REQUIRED)

add_executable(ExternalLiveModes external_live_modes.cpp
    "${VTT_REPO}/src/live_dictation_buffer.cpp"
    "${VTT_REPO}/src/mode_switch_policy.cpp"
    "${VTT_REPO}/src/dictation_diagnostics.cpp")
target_include_directories(ExternalLiveModes PRIVATE "${VTT_REPO}/src" "${VTT_JSON_INCLUDE}")
target_compile_definitions(ExternalLiveModes PRIVATE NOMINMAX _WIN32_WINNT=0x0601)
target_compile_options(ExternalLiveModes PRIVATE /utf-8 /W4)
target_link_libraries(ExternalLiveModes PRIVATE user32 gdi32 ole32 oleaut32 uiautomationcore)
add_test(NAME ExternalLiveModes COMMAND ExternalLiveModes)

add_executable(ModeSwitchPolicyTests mode_switch_policy_tests.cpp
    "${VTT_REPO}/src/mode_switch_policy.cpp")
target_include_directories(ModeSwitchPolicyTests PRIVATE "${VTT_REPO}/src")
target_compile_options(ModeSwitchPolicyTests PRIVATE /utf-8 /W4)
add_test(NAME ModeSwitchPolicyTests COMMAND ModeSwitchPolicyTests)

add_executable(LlmRewriteTests llm_rewrite_tests.cpp
    "${VTT_REPO}/src/config_manager.cpp")
target_include_directories(LlmRewriteTests PRIVATE "${VTT_REPO}/src" "${VTT_JSON_INCLUDE}")
target_compile_definitions(LlmRewriteTests PRIVATE NOMINMAX)
target_compile_options(LlmRewriteTests PRIVATE /utf-8 /W4)
target_link_libraries(LlmRewriteTests PRIVATE winhttp user32)
add_test(NAME LlmRewriteTests COMMAND LlmRewriteTests)

add_executable(ClipboardTransactionTests clipboard_transaction_tests.cpp)
target_include_directories(ClipboardTransactionTests PRIVATE "${VTT_REPO}/src")
target_compile_definitions(ClipboardTransactionTests PRIVATE NOMINMAX)
target_compile_options(ClipboardTransactionTests PRIVATE /utf-8 /W4)
target_link_libraries(ClipboardTransactionTests PRIVATE user32 gdi32)
add_test(NAME ClipboardTransactionTests COMMAND ClipboardTransactionTests)
