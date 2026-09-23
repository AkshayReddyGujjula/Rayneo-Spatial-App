#pragma once

// UTF-8 filesystem path conversion (Windows).
//
// The engine and the controller pass paths across process boundaries as UTF-8
// (command lines are quoted UTF-8, the layout/calibration loaders take
// std::string). Constructing a std::filesystem::path from a narrow string on
// Windows decodes with the ANSI code page, so any non-ASCII directory silently
// breaks file IO. Route every narrow path through path_from_utf8 and every
// discovered path back through utf8_from_path; both are strict UTF-8 and fall
// back to the input on conversion failure so behaviour never gets worse.
//
// Header-only: every target already has src/ on its include path.

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gt {

inline std::filesystem::path path_from_utf8(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) {
        return std::filesystem::path();
    }
    if (utf8.size() > static_cast<size_t>(INT_MAX)) {
        return std::filesystem::path(utf8);
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return std::filesystem::path(utf8);
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                            size) <= 0) {
        return std::filesystem::path(utf8);
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8);
#endif
}

inline std::string utf8_from_path(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return std::string();
    }
    if (wide.size() > static_cast<size_t>(INT_MAX)) {
        return path.string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return path.string();
    }
    std::string out(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(),
                            size, nullptr, nullptr) <= 0) {
        return path.string();
    }
    return out;
#else
    return path.string();
#endif
}

inline std::string utf8_from_wide_text(const std::wstring& wide) {
#ifdef _WIN32
    if (wide.empty()) {
        return std::string();
    }
    if (wide.size() > static_cast<size_t>(INT_MAX)) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(),
                            size, nullptr, nullptr) <= 0) {
        return std::string();
    }
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

}  // namespace gt
