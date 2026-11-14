#pragma once

#include <windows.h>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace clipboard_fixture {
std::map<UINT, HANDLE> formats;
DWORD sequence = 1;
bool opened = false, busy = false;
bool close_advances_sequence = false, clipboard_dirty = false, foreign_copy_on_close = false;
HWND clipboard_owner = nullptr, clipboard_opener = nullptr;
UINT fail_format = 0;
int failures = 0, empty_calls = 0;
constexpr UINT html = 0xc001, binary = 0xc002;
const HWND owner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
const HWND foreign_owner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(2));
const HANDLE invalid_memory = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1));

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void clear() {
    for (const auto& item : formats) {
        if (item.second != invalid_memory) GlobalFree(item.second);
    }
    formats.clear();
}
void reset() {
    clear();
    sequence = 1;
    opened = busy = false;
    close_advances_sequence = clipboard_dirty = foreign_copy_on_close = false;
    clipboard_owner = clipboard_opener = nullptr;
    fail_format = 0;
    failures = empty_calls = 0;
}
void seed(UINT format, const void* value, SIZE_T bytes) {
    const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    require(memory != nullptr, "fixture allocation failed");
    auto* target = GlobalLock(memory);
    require(target != nullptr, "fixture memory lock failed");
    std::memcpy(target, value, bytes);
    GlobalUnlock(memory);
    if (formats.count(format)) GlobalFree(formats[format]);
    formats[format] = memory;
    clipboard_owner = foreign_owner;
    ++sequence;
}
void seed_text(const wchar_t* text) {
    seed(CF_UNICODETEXT, text, (std::wcslen(text) + 1) * sizeof(wchar_t));
}
void seed_empty_marker(UINT format) {
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, 0);
    require(memory != nullptr && GlobalFlags(memory) != GMEM_INVALID_HANDLE,
        "fixture empty marker allocation failed");
    if (formats.count(format)) GlobalFree(formats[format]);
    formats[format] = memory;
    clipboard_owner = foreign_owner;
    ++sequence;
}
std::map<UINT, std::vector<unsigned char>> snapshot() {
    std::map<UINT, std::vector<unsigned char>> result;
    for (const auto& item : formats) {
        if (GlobalSize(item.second) == 0 && GlobalFlags(item.second) != GMEM_INVALID_HANDLE) {
            result[item.first] = {};
            continue;
        }
        auto* source = static_cast<const unsigned char*>(GlobalLock(item.second));
        require(source != nullptr, "fixture read failed");
        result[item.first] = {source, source + GlobalSize(item.second)};
        GlobalUnlock(item.second);
    }
    return result;
}
std::wstring text() {
    require(formats.count(CF_UNICODETEXT) != 0, "Unicode clipboard text is missing");
    const auto data = formats.at(CF_UNICODETEXT);
    const auto* value = static_cast<const wchar_t*>(GlobalLock(data));
    require(value != nullptr, "fixture Unicode clipboard read failed");
    const std::wstring result(value);
    GlobalUnlock(data);
    return result;
}
BOOL WINAPI open(HWND requested_owner) {
    if (busy || opened) return FALSE;
    opened = true;
    clipboard_opener = requested_owner;
    return TRUE;
}
BOOL WINAPI close() {
    opened = false;
    if (close_advances_sequence && clipboard_dirty) ++sequence;
    clipboard_dirty = false;
    if (foreign_copy_on_close) {
        foreign_copy_on_close = false;
        clear();
        seed_text(L"Concurrent new user copy");
    }
    return TRUE;
}
BOOL WINAPI empty() {
    require(opened, "production code emptied clipboard without ownership");
    ++empty_calls;
    clear();
    clipboard_owner = clipboard_opener;
    clipboard_dirty = true;
    ++sequence;
    return TRUE;
}
UINT WINAPI enumerate(UINT previous) {
    require(opened, "production code enumerated unopened clipboard");
    const auto next = formats.upper_bound(previous);
    SetLastError(ERROR_SUCCESS);
    return next == formats.end() ? 0 : next->first;
}
HANDLE WINAPI get(UINT format) {
    require(opened, "production code read unopened clipboard");
    const auto found = formats.find(format);
    const auto result = found == formats.end() ? nullptr : found->second;
    SetLastError(result ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
    return result;
}
HANDLE WINAPI set(UINT format, HANDLE value) {
    require(opened, "production code wrote unopened clipboard");
    if (failures > 0 && format == fail_format) { --failures; return nullptr; }
    if (formats.count(format)) GlobalFree(formats[format]);
    formats[format] = value;
    clipboard_dirty = true;
    ++sequence;
    return value;
}
DWORD WINAPI current_sequence() { return sequence; }
HWND WINAPI current_owner() { return clipboard_owner; }
SIZE_T WINAPI memory_size(HGLOBAL memory) {
    if (memory == invalid_memory) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return GlobalSize(memory);
}
UINT WINAPI memory_flags(HGLOBAL memory) {
    if (memory == invalid_memory) { SetLastError(ERROR_INVALID_HANDLE); return GMEM_INVALID_HANDLE; }
    return GlobalFlags(memory);
}
} // namespace clipboard_fixture
