#pragma once

// Shared by the executable's version resource and the recording overlay.
#define VTT_VERSION_MAJOR 1
#define VTT_VERSION_MINOR 0
#define VTT_VERSION_PATCH 8

// Windows numeric version resources use four fields; display only major.minor.patch.
#define VTT_VERSION_NUMBERS \
    VTT_VERSION_MAJOR, VTT_VERSION_MINOR, VTT_VERSION_PATCH, 0

#define VTT_VERSION_STRINGIFY_IMPL(value) #value
#define VTT_VERSION_STRINGIFY(value) VTT_VERSION_STRINGIFY_IMPL(value)
#define VTT_VERSION_STRING \
    VTT_VERSION_STRINGIFY(VTT_VERSION_MAJOR) "." \
    VTT_VERSION_STRINGIFY(VTT_VERSION_MINOR) "." \
    VTT_VERSION_STRINGIFY(VTT_VERSION_PATCH)
