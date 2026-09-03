#pragma once
// The program's narrow<->wide text conversions.
//
// Every byte string SmackMyRezUp reads from the outside world - .lang files,
// ffprobe's JSON, a Python tool's stdout - is UTF-8 by contract, and every
// Win32 call downstream wants UTF-16. Text that violates the contract is
// decoded as the active ANSI code page rather than dropped, so a stray
// mis-encoded line degrades to readable characters instead of vanishing.
// The reverse direction exists for the log, which is UTF-8 narrow while the
// paths and command lines it records are wide.
#include <windows.h>
#include <string>
#include <string_view>

namespace smru::text {

inline std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int len = static_cast<int>(s.size());
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), len, nullptr, 0);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_ACP, 0, s.data(), len, nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), len, w.data(), n);
        return w;
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), len, w.data(), n);
    return w;
}

inline std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int len = static_cast<int>(w.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), len, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace smru::text
