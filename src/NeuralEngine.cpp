#include "NeuralEngine.h"
#include "Log.h"

// nvsdk_ngx_params.h declares parameter Set/Get overloads over ID3D11Resource*
// and ID3D12Resource* but includes neither, so both must be in scope first.
#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_params.h>

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

// --- NVIDIA NGX interface facts (not a public SDK API) ----------------------
// These strings are the names nvngx_dlssnr.dll answers to; unrecognised names are
// silently ignored by NGX, so only confirmed ones are used. The feature id and
// app id are likewise the runtime's own values.
constexpr uint32_t kFeatureNR = 18;
constexpr uint32_t kAppId     = 102100511u;

constexpr const char* P_Width     = "DLSSNR.Width";
constexpr const char* P_Height    = "DLSSNR.Height";
constexpr const char* P_Preset    = "DLSSNR.Hint.Render.Preset";
constexpr const char* P_Enabled   = "DLSSNR.Enabled";
constexpr const char* P_DepthInv  = "DLSSNR.DepthInverted";
constexpr const char* P_Color     = "DLSSNR.Color";
constexpr const char* P_Output    = "DLSSNR.Output";
constexpr const char* P_Reset     = "DLSSNR.Reset";
constexpr const char* P_UICorrect = "DLSSNR.UICorrection";
constexpr const char* P_Style     = "DLSSNR.Style";
constexpr const char* P_Intensity = "DLSSNR.Intensity";
constexpr const char* P_ToneStr   = "DLSSNR.LocalToneStrength";      // style blend weight
constexpr const char* P_LocalStr  = "DLSSNR.LocalStructureStrength";
constexpr const char* P_SkinStr   = "DLSSNR.SkinStructureStrength";
constexpr const char* P_AutoMask  = "DLSSNR.UseAutoMask";
constexpr const char* P_MVec      = "DLSSNR.MVec";
constexpr const char* P_MVecSclX  = "DLSSNR.MVecScaleX";
constexpr const char* P_MVecSclY  = "DLSSNR.MVecScaleY";
constexpr const char* P_MVecW     = "DLSSNR.MVecSubrectWidth";
constexpr const char* P_MVecH     = "DLSSNR.MVecSubrectHeight";
constexpr const char* P_Depth     = "DLSSNR.Depth";
constexpr const char* P_DepthW    = "DLSSNR.DepthSubrectWidth";
constexpr const char* P_DepthH    = "DLSSNR.DepthSubrectHeight";
constexpr const char* P_CtrlMask  = "DLSSNR.ControlMask";
constexpr const char* P_CtrlMaskW = "DLSSNR.ControlMaskSubrectWidth";
constexpr const char* P_CtrlMaskH = "DLSSNR.ControlMaskSubrectHeight";
constexpr const char* P_ColBaseX  = "DLSSNR.ColorSubrectBaseX";
constexpr const char* P_ColBaseY  = "DLSSNR.ColorSubrectBaseY";
constexpr const char* P_ColW      = "DLSSNR.ColorSubrectWidth";
constexpr const char* P_ColH      = "DLSSNR.ColorSubrectHeight";
constexpr const char* P_OutBaseX  = "DLSSNR.OutputSubrectBaseX";
constexpr const char* P_OutBaseY  = "DLSSNR.OutputSubrectBaseY";
constexpr const char* P_OutW      = "DLSSNR.OutputSubrectWidth";
constexpr const char* P_OutH      = "DLSSNR.OutputSubrectHeight";

// --- Caller-origin check bypass (IAT redirect) ------------------------------
// The runtime refuses every call unless it believes the caller module is the
// driver's own nvngx.dll: it resolves the caller by walking the return address
// to a module handle and asking GetModuleFileNameW for that module's name. We
// redirect the runtime's OWN import of GetModuleFileNameW so that, asked about
// our module, it answers "nvngx.dll", and passes every other query through.
// This is the standard import-table-redirect technique; the code is ours.
//
// A single host process loads the runtime once, so a plain install-on-load /
// restore-on-unload pair is correct here (no reference counting needed).

using PFN_GMFNW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

PFN_GMFNW g_realGMFNW = nullptr;   // saved original import target
HMODULE   g_ourModule = nullptr;   // the module the replacement answers "nvngx.dll" for
void**    g_hookSlot  = nullptr;   // the IAT slot we rewrote

DWORD WINAPI FakeGetModuleFileNameW(HMODULE mod, LPWSTR out, DWORD size) {
    if (out && size && mod && mod == g_ourModule) {
        constexpr DWORD kLen = 9;   // wcslen(L"nvngx.dll")
        if (size <= kLen) {         // same truncation contract as the real API
            out[0] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(out, L"nvngx.dll", (kLen + 1) * sizeof(wchar_t));
        return kLen;
    }
    return g_realGMFNW ? g_realGMFNW(mod, out, size) : 0;
}

// Range check for an RVA+size against the mapped image, written so a hostile RVA
// cannot wrap the addition.
bool RvaFits(uint32_t rva, size_t size, uint32_t imageSize) {
    return rva != 0 && size <= imageSize && rva <= imageSize - size;
}

// Finds and rewrites the runtime's IAT slot for GetModuleFileNameW. Returns the
// slot on success (so it can be restored), nullptr if the import is absent or the
// image looks malformed - in which case nothing was touched.
void** InstallCallerHook(HMODULE runtime) {
    auto* base = reinterpret_cast<uint8_t*>(runtime);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    // Headers live within the first mapped page; bound e_lfanew to it.
    constexpr LONG kPage = 0x1000;
    if (dos->e_lfanew < LONG(sizeof(IMAGE_DOS_HEADER)) ||
        dos->e_lfanew > kPage - LONG(sizeof(IMAGE_NT_HEADERS))) return nullptr;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return nullptr;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return nullptr;

    const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& imp =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!RvaFits(imp.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), imageSize)) return nullptr;

    for (uint32_t descRva = imp.VirtualAddress;
         RvaFits(descRva, sizeof(IMAGE_IMPORT_DESCRIPTOR), imageSize);
         descRva += uint32_t(sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + descRva);
        if (desc->FirstThunk == 0 && desc->OriginalFirstThunk == 0) break;   // terminator
        if (desc->OriginalFirstThunk == 0 || desc->FirstThunk == 0) continue; // name array gone

        // Names come from OriginalFirstThunk (untouched by the loader); the slot
        // to rewrite is the parallel FirstThunk entry the loader resolved.
        for (uint64_t off = 0;; off += sizeof(IMAGE_THUNK_DATA)) {
            const uint64_t nameThunkRva = desc->OriginalFirstThunk + off;
            if (nameThunkRva > imageSize ||
                !RvaFits(uint32_t(nameThunkRva), sizeof(IMAGE_THUNK_DATA), imageSize)) break;

            auto* nameThunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + nameThunkRva);
            if (nameThunk->u1.AddressOfData == 0) break;
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;   // ordinal import, no name

            static constexpr char kName[] = "GetModuleFileNameW";
            const uint64_t nameRva = nameThunk->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name);
            if (nameRva + sizeof(kName) > imageSize) continue;
            if (std::memcmp(base + nameRva, kName, sizeof(kName)) != 0) continue;

            const uint64_t slotRva = desc->FirstThunk + off;
            if (slotRva > imageSize || !RvaFits(uint32_t(slotRva), sizeof(void*), imageSize)) return nullptr;
            auto** slot = reinterpret_cast<void**>(base + slotRva);

            // The module to answer for is the one holding our replacement (the
            // return address the runtime walks back to lands in our code).
            HMODULE self = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(&FakeGetModuleFileNameW), &self) ||
                self == nullptr) return nullptr;
            if (*slot == reinterpret_cast<void*>(&FakeGetModuleFileNameW)) return nullptr; // already hooked

            DWORD prot = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) return nullptr;
            g_realGMFNW = reinterpret_cast<PFN_GMFNW>(*slot);
            g_ourModule = self;
            *slot = reinterpret_cast<void*>(&FakeGetModuleFileNameW);
            VirtualProtect(slot, sizeof(void*), prot, &prot);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return slot;
        }
    }
    return nullptr;
}

void RestoreCallerHook() {
    if (!g_hookSlot || !g_realGMFNW) return;
    DWORD prot = 0;
    if (VirtualProtect(g_hookSlot, sizeof(void*), PAGE_READWRITE, &prot)) {
        *g_hookSlot = reinterpret_cast<void*>(g_realGMFNW);
        VirtualProtect(g_hookSlot, sizeof(void*), prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), g_hookSlot, sizeof(void*));
    }
    g_hookSlot = nullptr;
    g_realGMFNW = nullptr;
    g_ourModule = nullptr;
}

// Directory of the module this code is compiled into, trailing separator kept, so
// the runtime is found next to the exe before falling back to a bare name.
std::wstring ThisModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ThisModuleDir), &self))
        return L"";
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) return L"";
    std::wstring p(path);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
}

std::wstring NgxLogDir() {
    wchar_t buf[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return (n == 0 || n > MAX_PATH + 1) ? L".\\" : std::wstring(buf, n);
}

} // namespace

NeuralEngine::~NeuralEngine() {
    ReleaseFeature();
    // The runtime's shutdown goes through the caller check too, so the hook must
    // outlive it and be undone before the module is unmapped (the slot it points
    // into vanishes with the library).
    if (m_runtime) {
        if (m_runtimeInit && m_shutdown) m_shutdown(m_device);
        if (m_hookInstalled) { RestoreCallerHook(); m_hookInstalled = false; }
        FreeLibrary(m_runtime);
        m_runtime = nullptr;
    }
    m_params = nullptr; // owned by the driver core, released with its shutdown
    if (m_coreInit && m_device) NVSDK_NGX_D3D12_Shutdown1(m_device);
    m_coreInit = false;
}

bool NeuralEngine::Load(ID3D12Device* device) {
    m_device = device;
    const std::wstring logDir = NgxLogDir();

    // The driver core is initialised only for its NGX_Parameter capability block;
    // the driver itself knows nothing of NR. That block is a generic bag the
    // runtime is happy to consume.
    NVSDK_NGX_Result r =
        NVSDK_NGX_D3D12_Init(kAppId, logDir.c_str(), device, nullptr, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        LOG("Neural engine: driver-core NGX init failed 0x" << std::hex << r);
        return false;
    }
    m_coreInit = true;

    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_params);
    if (NVSDK_NGX_FAILED(r) || !m_params) {
        LOG("Neural engine: GetCapabilityParameters failed 0x" << std::hex << r);
        return false;
    }

    const std::wstring dir = ThisModuleDir();
    const std::wstring beside = dir.empty() ? L"" : dir + L"nvngx_dlssnr.dll";
    if (!beside.empty()) m_runtime = LoadLibraryW(beside.c_str());
    if (!m_runtime) m_runtime = LoadLibraryW(L"nvngx_dlssnr.dll");
    if (!m_runtime) {
        LOG("Neural engine: nvngx_dlssnr.dll not found (place it beside the exe).");
        return false;
    }

    m_initExt  = reinterpret_cast<PFN_InitExt >(GetProcAddress(m_runtime, "NVSDK_NGX_D3D12_Init_Ext"));
    m_create   = reinterpret_cast<PFN_Create  >(GetProcAddress(m_runtime, "NVSDK_NGX_D3D12_CreateFeature"));
    m_evaluate = reinterpret_cast<PFN_Evaluate>(GetProcAddress(m_runtime, "NVSDK_NGX_D3D12_EvaluateFeature"));
    m_release  = reinterpret_cast<PFN_Release >(GetProcAddress(m_runtime, "NVSDK_NGX_D3D12_ReleaseFeature"));
    m_shutdown = reinterpret_cast<PFN_Shutdown>(GetProcAddress(m_runtime, "NVSDK_NGX_D3D12_Shutdown1"));
    if (!m_initExt || !m_create || !m_evaluate) {
        LOG("Neural engine: nvngx_dlssnr.dll is missing the D3D12 NGX entry points.");
        return false;
    }

    // Must be in place before the first runtime call, Init_Ext included.
    g_hookSlot = InstallCallerHook(m_runtime);
    m_hookInstalled = (g_hookSlot != nullptr);
    if (!m_hookInstalled)
        LOG("Neural engine: caller-check bypass could not be installed; the runtime may reject calls.");

    // The runtime keeps its own NGX state, so it needs its own init on our device.
    r = m_initExt(kAppId, logDir.c_str(), device, NVSDK_NGX_Version_API, nullptr);
    if (NVSDK_NGX_FAILED(r)) {
        LOG("Neural engine: runtime Init_Ext failed 0x" << std::hex << r);
        return false;
    }
    m_runtimeInit = true;
    LOG("Neural engine loaded (nvngx_dlssnr.dll, feature " << std::dec << kFeatureNR << ")");
    return true;
}

bool NeuralEngine::EnsureFeature(ID3D12GraphicsCommandList* cmd, uint32_t w, uint32_t h) {
    if (!m_runtimeInit || !cmd || !m_params) return false;
    if (m_feature && m_featureW == w && m_featureH == h) return true;
    ReleaseFeature();

    // Types matter: NGX stores a value under the type it was set with, and reads
    // it back as a specific type; a mismatch silently reads as the default.
    m_params->Set(P_Width, static_cast<unsigned int>(w));
    m_params->Set(P_Height, static_cast<unsigned int>(h));
    m_params->Set(P_Preset, int(m_preset));
    m_params->Set(P_DepthInv, 0);
    m_params->Set(P_Enabled, 1);
    m_params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    m_params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

    const NVSDK_NGX_Result r =
        m_create(cmd, static_cast<NVSDK_NGX_Feature>(kFeatureNR), m_params, &m_feature);
    if (NVSDK_NGX_FAILED(r) || !m_feature) {
        LOG("Neural engine: createFeature failed 0x" << std::hex << r);
        m_feature = nullptr;
        return false;
    }
    m_featureW = w; m_featureH = h;
    LOG("Neural engine: feature " << std::dec << kFeatureNR << " created at " << w << "x" << h);
    return true;
}

bool NeuralEngine::Evaluate(ID3D12GraphicsCommandList* cmd,
                            ID3D12Resource* colorSRGB, ID3D12Resource* outputSRGB,
                            uint32_t w, uint32_t h, const Settings& s) {
    if (!m_runtimeInit || !m_feature || !cmd || !colorSRGB || !outputSRGB) return false;

    m_params->Set(P_Color, colorSRGB);
    m_params->Set(P_Output, outputSRGB);

    const int iw = static_cast<int>(w), ih = static_cast<int>(h);
    m_params->Set(P_ColBaseX, 0); m_params->Set(P_ColBaseY, 0);
    m_params->Set(P_ColW, iw);    m_params->Set(P_ColH, ih);
    m_params->Set(P_OutBaseX, 0); m_params->Set(P_OutBaseY, 0);
    m_params->Set(P_OutW, iw);    m_params->Set(P_OutH, ih);

    // Optional guides. NGX parameter blocks are sticky, so an unbound guide is
    // explicitly nulled to keep an earlier binding from leaking into this frame.
    m_params->Set(P_MVec, s.mvec);
    if (s.mvec) {
        m_params->Set(P_MVecSclX, s.mvScaleX);
        m_params->Set(P_MVecSclY, s.mvScaleY);
        m_params->Set(P_MVecW, iw); m_params->Set(P_MVecH, ih);
    }
    m_params->Set(P_Depth, s.depth);
    if (s.depth) { m_params->Set(P_DepthW, iw); m_params->Set(P_DepthH, ih); }
    m_params->Set(P_CtrlMask, s.controlMask);
    if (s.controlMask) { m_params->Set(P_CtrlMaskW, iw); m_params->Set(P_CtrlMaskH, ih); }

    // Read every evaluation, not just at creation.
    m_params->Set(P_Enabled, 1);
    m_params->Set(P_DepthInv, s.depthInverted ? 1 : 0);
    m_params->Set(P_Reset, s.reset ? 1 : 0);
    m_params->Set(P_AutoMask, s.autoMask ? 1 : 0);
    m_params->Set(P_Style, s.style);              // read back as unsigned
    m_params->Set(P_Intensity, s.intensity);
    m_params->Set(P_ToneStr, 1.0f);               // full style blend
    m_params->Set(P_LocalStr, s.localStructure);
    m_params->Set(P_SkinStr, s.skinStructure);
    m_params->Set(P_UICorrect, 0);                // video has no separable UI layer

    const NVSDK_NGX_Result r = m_evaluate(cmd, m_feature, m_params, nullptr);
    if (NVSDK_NGX_FAILED(r)) {
        LOG("Neural engine: evaluate failed 0x" << std::hex << r);
        return false;
    }
    return true;
}

void NeuralEngine::ReleaseFeature() {
    if (m_feature && m_release) m_release(m_feature);
    m_feature = nullptr;
    m_featureW = m_featureH = 0;
}
