// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// See scsynth_synthdef_dirs.h.
#include "scsynth_synthdef_dirs.h"

#include "SC_Codecvt.hpp"
#include "SC_Filesystem.hpp"
#include "SC_StringParser.h"

#include <cstdlib>

std::vector<std::string> scsynth_synthdef_directories() {
    std::vector<std::string> dirs;
    if (const char* path = std::getenv("SC_SYNTHDEF_PATH")) {
        SC_StringParser sp(path, SC_STRPARSE_PATHDELIMITER);
        while (!sp.AtEnd()) {
            const char* dir = sp.NextToken();
            if (dir && *dir) dirs.emplace_back(dir);
        }
    } else {
        using DirName = SC_Filesystem::DirName;
        dirs.push_back(SC_Codecvt::path_to_utf8_str(
            SC_Filesystem::instance().getDirectory(DirName::UserAppSupport) / "synthdefs"));
    }
    return dirs;
}
