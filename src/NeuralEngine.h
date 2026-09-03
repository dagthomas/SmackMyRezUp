#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <cstdint>

// Host for NVIDIA's DLSS Neural Rendering runtime (nvngx_dlssnr.dll, NGX feature
// 18). It hand-loads the runtime, satisfies the runtime's caller-origin check,
// initialises a private NGX state against our device, and records feature-18
// create/evaluate onto a caller-supplied D3D12 command list.
//
// NR runs 1:1 at output resolution. Colour and output are sRGB-ENCODED LDR
// values in a plain float resource - an _SRGB-typed view would be linearised on
// read. The runtime also answers to guide inputs (a strings dump of the DLL
// lists DLSSNR.MVec/MVecScaleX/Y, DLSSNR.Depth, DLSSNR.ControlMask); Evaluate
// binds whichever of those the caller supplies so the temporal model can align
// its history instead of guessing.
//
// What is borrowed here are interface FACTS of NVIDIA's runtime, not anyone's
// code: the NGX parameter-name strings, the D3D12 NGX entry-point names, the
// feature id, and the well-known IAT-redirect method used to pass the runtime's
// module-origin check. This host is an original implementation around them; see
// THIRD_PARTY.md.
class NeuralEngine {
public:
    struct Settings {
        uint32_t style = 0;             // 0..2 style block (higher clamps to 2)
        float intensity = 1.0f;         // wet/dry blend against the original
        float localStructure = 1.0f;    // detail strength (only with autoMask on)
        float skinStructure = -1.0f;    // -1 = follow localStructure; 0 = off on skin
        bool autoMask = true;           // runtime-derived protection mask
        bool reset = false;             // clear temporal history this frame
        // Optional guide inputs (nullptr = leave unbound, the pre-guide
        // behaviour). MVec: R16G16_FLOAT, current -> previous, in input pixels
        // (scale 1). Depth: R32 resource, 0 = near unless depthInverted. Mask:
        // R8_UNORM temporal-uncertainty/bias mask.
        ID3D12Resource* mvec = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* controlMask = nullptr;
        float mvScaleX = 1.0f, mvScaleY = 1.0f;
        bool depthInverted = false;
    };

    NeuralEngine() = default;
    ~NeuralEngine();
    NeuralEngine(const NeuralEngine&) = delete;
    NeuralEngine& operator=(const NeuralEngine&) = delete;

    // Loads and initialises the runtime against `device`. Logs the failure reason
    // and returns false if the runtime is missing or rejects init.
    bool Load(ID3D12Device* device);

    // Render preset hint (DLSSNR.Hint.Render.Preset), applied at feature
    // creation. Set BEFORE the first EnsureFeature; other DLSS5 hosts default
    // to preset 3, this project's historical value is 0.
    void SetPreset(uint32_t p) { m_preset = p; }

    // Records feature creation into the OPEN command list (submit + wait before
    // the first Evaluate). Cheap no-op once created at (w,h).
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd, uint32_t w, uint32_t h);
    bool FeatureReady() const { return m_feature != nullptr; }
    // True once the driver-core NGX session exists for this device (Load did
    // it, and the destructor shuts it down): other features can share it.
    bool CoreInitialised() const { return m_coreInit; }

    // Records one evaluation. colour/output must be the feature size (w,h).
    bool Evaluate(ID3D12GraphicsCommandList* cmd,
                  ID3D12Resource* colorSRGB, ID3D12Resource* outputSRGB,
                  uint32_t w, uint32_t h, const Settings& s);

private:
    // Runtime entry points, resolved by name from nvngx_dlssnr.dll. The runtime
    // keeps NGX state separate from the driver core, hence its own Init_Ext.
    using PFN_InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
                                                      ID3D12Device*, NVSDK_NGX_Version,
                                                      const NVSDK_NGX_Parameter*);
    using PFN_Create = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                     NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using PFN_Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
                                                       const NVSDK_NGX_Handle*,
                                                       const NVSDK_NGX_Parameter*, void*);
    using PFN_Release = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
    using PFN_Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

    void ReleaseFeature();

    ID3D12Device* m_device = nullptr;
    HMODULE m_runtime = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;   // driver-core capability block
    NVSDK_NGX_Handle* m_feature = nullptr;
    uint32_t m_featureW = 0, m_featureH = 0;
    uint32_t m_preset = 0;
    bool m_coreInit = false;
    bool m_runtimeInit = false;
    bool m_hookInstalled = false;

    PFN_InitExt m_initExt = nullptr;
    PFN_Create m_create = nullptr;
    PFN_Evaluate m_evaluate = nullptr;
    PFN_Release m_release = nullptr;
    PFN_Shutdown m_shutdown = nullptr;
};
