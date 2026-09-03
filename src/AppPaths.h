#pragma once
// Where SmackMyRezUp finds its own pieces at run time.
//
// Everything the program needs to find is resolved from the executable's folder,
// from an environment override, or from the settings file, so a checkout can
// live anywhere and a distributed exe runs from any folder. The one exception is
// PythonExecutable(), which ends with a short list of well-known ComfyUI install
// paths as a last resort after SMRU_PYTHON, the settings file and SMRU_COMFYUI_DIR.
#include <windows.h>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <string>
#include "AppIdentity.h"

namespace smru::paths {

inline std::filesystem::path ExeDirectory() {
    wchar_t p[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, p, static_cast<DWORD>(std::size(p)));
    if (!n || n >= std::size(p)) return std::filesystem::current_path();
    return std::filesystem::path(p).parent_path();
}

inline std::filesystem::path SettingsFile() { return ExeDirectory() / kSettingsFile; }

inline std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[32768]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return (n && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
}

inline std::wstring SettingsString(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[32768]{};
    GetPrivateProfileStringW(section, key, L"", buf, static_cast<DWORD>(std::size(buf)),
                             SettingsFile().c_str());
    return buf;
}

// A project folder such as "tools" or "luts": an environment override first,
// then beside the exe (a shipped layout), then walking up from the build output
// folder to the checkout root. Empty when nothing matches.
inline std::filesystem::path FindProjectDir(const wchar_t* name, const wchar_t* envOverride = nullptr) {
    std::error_code ec;
    if (envOverride) {
        const std::wstring v = EnvVar(envOverride);
        if (!v.empty() && std::filesystem::is_directory(v, ec)) return v;
    }
    std::filesystem::path dir = ExeDirectory();
    for (int up = 0; up < 4; ++up) {
        const auto candidate = dir / name;
        if (std::filesystem::is_directory(candidate, ec)) return candidate;
        const auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

inline std::filesystem::path ToolsDirectory() { return FindProjectDir(L"tools", kEnvToolsDir); }
inline std::filesystem::path LutDirectory()   { return FindProjectDir(L"luts"); }

// A bundled command-line tool: beside the exe (the shipped layout, where the
// player self-extracts its payload), then the staged ffmpeg folders a checkout
// build leaves behind, then whatever is on PATH. Empty when nothing matches -
// callers report that as a missing tool rather than spawning a bare name.
inline std::filesystem::path FindBundledTool(const wchar_t* exeName) {
    std::error_code ec;
    const std::filesystem::path base = ExeDirectory();
    for (const auto& candidate : {base / exeName,
                                  base / L"ffmpeg" / exeName,
                                  base / L"ffmpeg" / L"bin" / exeName,
                                  base.parent_path() / L"ffmpeg" / L"bin" / exeName}) {
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    wchar_t found[32768]{};
    const DWORD n = SearchPathW(nullptr, exeName, nullptr,
                                static_cast<DWORD>(std::size(found)), found, nullptr);
    if (n && n < std::size(found)) return std::filesystem::path(found);
    return {};
}

inline std::filesystem::path FfmpegExe()  { return FindBundledTool(L"ffmpeg.exe"); }
inline std::filesystem::path FfprobeExe() { return FindBundledTool(L"ffprobe.exe"); }

// Python interpreter for the sidecar generators (needs torch, torchvision and
// transformers; ComfyUI's embedded python.exe is the reference setup).
// Order: SMRU_PYTHON, [Tools] Python= in the settings file, SMRU_COMFYUI_DIR,
// then the ComfyUI portable layouts we know about. Empty when none exists.
inline std::filesystem::path PythonExecutable() {
    std::error_code ec;
    auto usable = [&](const std::filesystem::path& p) {
        return !p.empty() && std::filesystem::is_regular_file(p, ec);
    };
    if (const auto p = std::filesystem::path(EnvVar(kEnvPython)); usable(p)) return p;
    if (const auto p = std::filesystem::path(SettingsString(kToolsSection, kToolsPythonKey)); usable(p)) return p;
    if (const auto root = EnvVar(kEnvComfyDir); !root.empty()) {
        if (const auto p = std::filesystem::path(root) / L"python_embeded" / L"python.exe"; usable(p)) return p;
    }
    for (const wchar_t* root : {L"X:\\comfyui\\comfyui\\ComfyUI_windows_portable",
                                L"C:\\ComfyUI_windows_portable",
                                L"D:\\ComfyUI_windows_portable"}) {
        if (const auto p = std::filesystem::path(root) / L"python_embeded" / L"python.exe"; usable(p)) return p;
    }
    return {};
}

// Where the Python sidecars cache their downloaded model weights: beside the
// exe, so copying an install carries its models instead of re-downloading them
// into the next machine's user profile. SMRU_MODELS_DIR moves it.
inline std::filesystem::path ModelCacheDirectory() {
    const std::wstring custom = EnvVar(kEnvModelsDir);
    return custom.empty() ? ExeDirectory() / kModelsDir : std::filesystem::path(custom);
}

// Everything a child process inherits, published once at start:
//   SMRU_BIN_DIR  - where ffmpeg/ffprobe and the exporter live
//   HF_HOME       - Hugging Face cache (Depth Anything, SAM 3)
//   TORCH_HOME    - torch hub cache (RAFT)
// The two cache variables are only set when the user has not chosen one
// already, and only when the folder is genuinely writable - an install under
// Program Files must not point gigabyte downloads at a read-only path, so it
// falls back to the per-user default. Returns the cache actually published,
// empty when the children keep their own default.
inline std::filesystem::path PublishChildEnvironment() {
    SetEnvironmentVariableW(kEnvBinDir, ExeDirectory().c_str());
    if (!EnvVar(L"HF_HOME").empty() || !EnvVar(L"TORCH_HOME").empty()) return {};

    const std::filesystem::path root = ModelCacheDirectory();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (!std::filesystem::is_directory(root, ec)) return {};
    const HANDLE probe = CreateFileW((root / L".writable").c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return {};
    CloseHandle(probe);

    SetEnvironmentVariableW(L"HF_HOME", (root / L"huggingface").c_str());
    SetEnvironmentVariableW(L"TORCH_HOME", (root / L"torch").c_str());
    // Hugging Face warns when it cannot symlink into its cache and falls back
    // to real copies. That fallback is what makes the folder copyable to
    // another machine at all, so the warning is noise here.
    SetEnvironmentVariableW(L"HF_HUB_DISABLE_SYMLINKS_WARNING", L"1");
    return root;
}

} // namespace smru::paths
