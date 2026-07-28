#include "AsioDriverCheck.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <vector>

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// Read a REG_SZ / REG_EXPAND_SZ value. `name == nullptr` reads the key's
// default value. REG_EXPAND_SZ is expanded — InprocServer32 paths are
// routinely stored with %ProgramFiles% and friends unexpanded.
bool regString(HKEY key, const wchar_t* name, std::wstring& out) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
        return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return false;

    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)buf.data(), &bytes) != ERROR_SUCCESS)
        return false;
    buf.resize(wcslen(buf.c_str()));   // registry strings needn't be NUL-terminated

    if (type == REG_EXPAND_SZ) {
        const DWORD need = ExpandEnvironmentStringsW(buf.c_str(), nullptr, 0);
        if (need > 0) {
            std::wstring expanded(need, L'\0');
            ExpandEnvironmentStringsW(buf.c_str(), expanded.data(), need);
            expanded.resize(wcslen(expanded.c_str()));
            buf = std::move(expanded);
        }
    }

    out = std::move(buf);
    return true;
}

bool inprocServerPath(const std::wstring& clsid, std::wstring& path) {
    const std::wstring sub = L"CLSID\\" + clsid + L"\\InprocServer32";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const bool ok = regString(key, nullptr, path);
    RegCloseKey(key);
    return ok && !path.empty();
}

// One line per driver per process — this runs on every device rescan.
void logRejectionOnce(const std::string& name, const std::string& dll) {
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lock(mutex);
    if (!logged.insert(name).second)
        return;
    fprintf(stderr,
            "[asio] hiding '%s': driver DLL is built for another architecture "
            "and cannot load in this process (%s)\n",
            name.c_str(), dll.c_str());
    fflush(stderr);
}

} // namespace

namespace sonicpi::device {

std::set<std::string> unloadableAsioDrivers() {
    std::set<std::string> unloadable;

    HKEY asioKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"software\\asio", 0, KEY_READ, &asioKey)
        != ERROR_SUCCESS)
        return unloadable;

    for (DWORD i = 0;; ++i) {
        wchar_t keyName[256] = {};
        DWORD keyLen = (DWORD)std::size(keyName);
        if (RegEnumKeyExW(asioKey, i, keyName, &keyLen,
                          nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(asioKey, keyName, 0, KEY_READ, &driverKey) != ERROR_SUCCESS)
            continue;

        std::wstring clsid, description;
        const bool haveClsid = regString(driverKey, L"clsid", clsid);
        // Mirrors JUCE's addDriverInfo: the listed name is `description`
        // when present, otherwise the registry key name.
        if (!regString(driverKey, L"description", description) || description.empty())
            description = keyName;
        RegCloseKey(driverKey);

        std::wstring dll;
        if (!haveClsid || !inprocServerPath(clsid, dll))
            continue;

        // DONT_RESOLVE_DLL_REFERENCES maps the image without running
        // DllMain or resolving imports, so nothing in the driver executes
        // and no hardware is touched — but the architecture check still
        // happens, which is the whole point. Strictly less invasive than
        // the CoCreateInstance the probe path already performs.
        if (HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES)) {
            FreeLibrary(mod);
            continue;
        }
        if (GetLastError() != ERROR_BAD_EXE_FORMAT)
            continue;   // absent, locked, or otherwise unclear — leave it listed

        const std::string name = narrow(description);
        unloadable.insert(name);
        logRejectionOnce(name, narrow(dll));
    }

    RegCloseKey(asioKey);
    return unloadable;
}

} // namespace sonicpi::device

#else // !_WIN32

namespace sonicpi::device {

std::set<std::string> unloadableAsioDrivers() { return {}; }

} // namespace sonicpi::device

#endif
