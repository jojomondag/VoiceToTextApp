# The same native providers used by TeachersLittleHelper. No .NET runtime is
# needed: VoiceToText loads their public C APIs directly. Both package bytes
# and public headers are pinned, including when reused from NuGet's cache.
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Pianissimo desktop provider currently requires Windows x64.")
endif()
if(CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    message(FATAL_ERROR "Select the x64 platform for the pinned Pianissimo providers.")
endif()
set(VTT_SPEECH_HEADER_CACHE "${CMAKE_SOURCE_DIR}/artifacts/speech-headers" CACHE PATH
    "Optional offline cache of the pinned speech API headers")
set(VTT_SPEECH_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/speech-include")
file(MAKE_DIRECTORY "${VTT_SPEECH_INCLUDE_DIR}")

function(vtt_pinned_download destination url sha256 cache)
    if(EXISTS "${destination}")
        file(SHA256 "${destination}" existing_hash)
        if(existing_hash STREQUAL sha256)
            return()
        endif()
    endif()
    if(EXISTS "${cache}")
        file(SHA256 "${cache}" cache_hash)
        if(cache_hash STREQUAL sha256)
            configure_file("${cache}" "${destination}" COPYONLY)
            return()
        endif()
    endif()
    file(DOWNLOAD "${url}" "${destination}"
        EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
        message(FATAL_ERROR "Could not download pinned speech dependency ${url}: ${download_status}")
    endif()
endfunction()

set(transcribe_headers_root "https://raw.githubusercontent.com/handy-computer/transcribe.cpp/v0.2.3/include")
set(ort_headers_root "https://raw.githubusercontent.com/microsoft/onnxruntime/v1.28.0/include/onnxruntime/core/session")
vtt_pinned_download("${VTT_SPEECH_INCLUDE_DIR}/transcribe.h" "${transcribe_headers_root}/transcribe.h"
    "07fff3489a3c282ab7ee8835b010f1f9f9abd5b61426f8c0e80424dabaaf9a6b" "${VTT_SPEECH_HEADER_CACHE}/transcribe.h")
foreach(header_entry IN ITEMS
    "onnxruntime_c_api.h|b69a133143c0da61782b2b02cc8a620d21ac139231fd211719776d10385135c1"
    "onnxruntime_cxx_api.h|c6bf005e4063b7db911d0a6b030452bcb9d4fc5e87530a39d94c4a5fcd311b61"
    "onnxruntime_cxx_inline.h|97f960b1bc4a372918893ef4c77102dee725ccfce0d76607f4570b03e7e28325"
    "onnxruntime_ep_c_api.h|db86df0f846b8d3bdbf429a5c76c5805293196575ca3c475878612591cd18e51"
    "onnxruntime_error_code.h|5ce3b054e798eced8d14f5b86e98692fd33470463f96194ce0700a2d53dd8721"
    "onnxruntime_float16.h|88b242845d25981633a0bbd1c148e273cf8bfb016ea3f57c4af41a06530f72b0")
    string(REPLACE "|" ";" header_parts "${header_entry}")
    list(GET header_parts 0 header_name)
    list(GET header_parts 1 header_hash)
    vtt_pinned_download("${VTT_SPEECH_INCLUDE_DIR}/${header_name}" "${ort_headers_root}/${header_name}"
        "${header_hash}" "${VTT_SPEECH_HEADER_CACHE}/${header_name}")
endforeach()
vtt_pinned_download("${VTT_SPEECH_INCLUDE_DIR}/ONNX-Runtime-LICENSE.txt" "https://raw.githubusercontent.com/microsoft/onnxruntime/v1.28.0/LICENSE"
    "2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c" "${VTT_SPEECH_HEADER_CACHE}/ONNX-Runtime-LICENSE.txt")
vtt_pinned_download("${VTT_SPEECH_INCLUDE_DIR}/sherpa-onnx-LICENSE.txt" "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v1.13.8/LICENSE"
    "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30" "${VTT_SPEECH_HEADER_CACHE}/sherpa-onnx-LICENSE.txt")

if(DEFINED ENV{NUGET_PACKAGES} AND NOT "$ENV{NUGET_PACKAGES}" STREQUAL "")
    set(vtt_nuget_cache "$ENV{NUGET_PACKAGES}")
else()
    set(vtt_nuget_cache "$ENV{USERPROFILE}/.nuget/packages")
endif()
function(vtt_speech_package name version hash output_variable)
    set(package_file "${name}.${version}.nupkg")
    set(archive "${CMAKE_BINARY_DIR}/_deps/${package_file}")
    vtt_pinned_download("${archive}" "https://api.nuget.org/v3-flatcontainer/${name}/${version}/${package_file}" "${hash}"
        "${vtt_nuget_cache}/${name}/${version}/${package_file}")
    set(extracted "${CMAKE_BINARY_DIR}/_deps/${name}-${version}")
    if(NOT EXISTS "${extracted}/.vtt-extracted")
        file(MAKE_DIRECTORY "${extracted}")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${extracted}")
        file(WRITE "${extracted}/.vtt-extracted" "${hash}\n")
    endif()
    set(${output_variable} "${extracted}/runtimes/win-x64/native" PARENT_SCOPE)
endfunction()
vtt_speech_package("transcribecppsharp.native.win-x64" "0.2.3"
    "a1add967e102d78918938ca93d210a328dcfde4e18072d55fa97305a4d9bea95" VTT_TRANSCRIBE_RUNTIME_DIR)
vtt_speech_package("org.k2fsa.sherpa.onnx.runtime.win-x64" "1.13.8"
    "2257c06893d1d90792ea3157606de33a51122d935b6913bed5442d2ffb9e9bd7" VTT_ONNX_RUNTIME_DIR)
file(GLOB VTT_TRANSCRIBE_DLLS "${VTT_TRANSCRIBE_RUNTIME_DIR}/*.dll")
set(VTT_SPEECH_DLLS ${VTT_TRANSCRIBE_DLLS} "${VTT_ONNX_RUNTIME_DIR}/onnxruntime.dll")

function(vtt_copy_speech_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VTT_SPEECH_DLLS} "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/Pianissimo"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${VTT_TRANSCRIBE_RUNTIME_DIR}/licenses" "$<TARGET_FILE_DIR:${target}>/licenses/Pianissimo/transcribe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VTT_SPEECH_INCLUDE_DIR}/ONNX-Runtime-LICENSE.txt"
            "${VTT_SPEECH_INCLUDE_DIR}/sherpa-onnx-LICENSE.txt"
            "$<TARGET_FILE_DIR:${target}>/licenses/Pianissimo"
        VERBATIM)
endfunction()
