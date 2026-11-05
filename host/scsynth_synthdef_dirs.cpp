// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// See scsynth_synthdef_dirs.h.
#include "scsynth_synthdef_dirs.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
constexpr char kPathDelimiter = ';';
#else
constexpr char kPathDelimiter = ':';
#endif

// The user's application-support directory, as scsynth's SC_Filesystem found
// it: XDG_DATA_HOME when set, else the platform's own place. Empty when the
// platform cannot say.
std::filesystem::path userAppSupportDirectory() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return std::filesystem::path(xdg);
#ifdef _WIN32
    PWSTR wide = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wide))) return {};
    std::filesystem::path local(wide);
    CoTaskMemFree(wide);
    return local;
#else
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
#ifdef __APPLE__
    return std::filesystem::path(home) / "Library" / "Application Support";
#else
    return std::filesystem::path(home) / ".local" / "share";
#endif
#endif
}

} // namespace

std::vector<std::string> scsynth_synthdef_directories() {
    std::vector<std::string> dirs;
    if (const char* path = std::getenv("SC_SYNTHDEF_PATH")) {
        std::string list(path);
        size_t start = 0;
        while (start <= list.size()) {
            size_t end = list.find(kPathDelimiter, start);
            if (end == std::string::npos) end = list.size();
            if (end > start) dirs.push_back(list.substr(start, end - start));
            start = end + 1;
        }
    } else {
        const std::filesystem::path support = userAppSupportDirectory();
        if (!support.empty()) dirs.push_back((support / "SuperCollider" / "synthdefs").string());
    }
    return dirs;
}
