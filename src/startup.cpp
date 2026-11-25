#include "startup.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace todofloat {

namespace {

std::wstring trimWide(std::wstring_view s) {
    std::size_t start = 0;
    while (start < s.size() && std::iswspace(static_cast<wint_t>(s[start]))) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::iswspace(static_cast<wint_t>(s[end - 1]))) {
        --end;
    }
    return std::wstring(s.substr(start, end - start));
}

std::wstring normalizePathForCompare(const std::filesystem::path& p) {
    std::wstring raw = p.lexically_normal().wstring();
    std::replace(raw.begin(), raw.end(), L'/', L'\\');
    for (wchar_t& ch : raw) {
        ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    }
    return raw;
}

std::wstring extractExecutableFromCommand(std::wstring_view cmd) {
    std::wstring trimmed = trimWide(cmd);
    if (trimmed.empty()) {
        return {};
    }
    if (trimmed.front() == L'"') {
        std::size_t closeQuote = trimmed.find(L'"', 1);
        if (closeQuote == std::wstring::npos) {
            return trimmed.substr(1);
        }
        return trimmed.substr(1, closeQuote - 1);
    }
    return trimmed;
}

} // namespace

std::filesystem::path currentExecutablePath() {
    std::vector<wchar_t> buf(MAX_PATH + 1, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1) {
            return std::filesystem::path(std::wstring_view(buf.data(), len));
        }
        buf.resize(buf.size() * 2, L'\0');
    }
}

std::wstring formatStartupCommand(const std::filesystem::path& exePath) {
    std::wstring native = exePath.lexically_normal().wstring();
    if (native.empty()) {
        return {};
    }
    return L"\"" + native + L"\"";
}

bool startupCommandMatches(std::wstring_view registeredValue, const std::filesystem::path& exePath) {
    if (exePath.empty()) {
        return false;
    }
    std::wstring registeredExe = extractExecutableFromCommand(registeredValue);
    if (registeredExe.empty()) {
        return false;
    }
    return normalizePathForCompare(std::filesystem::path(registeredExe)) == normalizePathForCompare(exePath);
}

bool isAutoStartEnabled(
    const std::filesystem::path& exePath,
    const wchar_t* subKey,
    const wchar_t* valueName) {
    if (exePath.empty() || !subKey || !valueName) {
        return false;
    }

    HKEY hKey = nullptr;
    LONG openRes = RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_QUERY_VALUE, &hKey);
    if (openRes != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD byteSize = 0;
    LONG queryRes = RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &byteSize);
    if (queryRes != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || byteSize < sizeof(wchar_t)) {
        RegCloseKey(hKey);
        return false;
    }

    std::vector<wchar_t> buffer((byteSize / sizeof(wchar_t)) + 1, L'\0');
    DWORD readBytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    queryRes = RegQueryValueExW(
        hKey,
        valueName,
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(buffer.data()),
        &readBytes);
    RegCloseKey(hKey);
    if (queryRes != ERROR_SUCCESS) {
        return false;
    }

    return startupCommandMatches(std::wstring_view(buffer.data()), exePath);
}

bool setAutoStartEnabled(
    bool enable,
    const std::filesystem::path& exePath,
    const wchar_t* subKey,
    const wchar_t* valueName) {
    if (!subKey || !valueName) {
        return false;
    }

    if (enable) {
        std::wstring cmd = formatStartupCommand(exePath);
        if (cmd.empty()) {
            return false;
        }
        HKEY hKey = nullptr;
        LONG createRes = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            subKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &hKey,
            nullptr);
        if (createRes != ERROR_SUCCESS) {
            return false;
        }
        DWORD byteSize = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        LONG setRes = RegSetValueExW(
            hKey,
            valueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            byteSize);
        RegCloseKey(hKey);
        return setRes == ERROR_SUCCESS;
    }

    HKEY hKey = nullptr;
    LONG openRes = RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &hKey);
    if (openRes == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openRes != ERROR_SUCCESS) {
        return false;
    }
    LONG delRes = RegDeleteValueW(hKey, valueName);
    RegCloseKey(hKey);
    return delRes == ERROR_SUCCESS || delRes == ERROR_FILE_NOT_FOUND;
}

} // namespace todofloat
