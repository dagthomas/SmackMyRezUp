#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "NeuralEngine.h"

class D3D12Renderer {
public:
    enum class DebugView { Final, Input, MotionVectors, Depth, BiasMask };

    // How the per-frame subpixel jitter contract is fulfilled.
    //
    // Decoded video was sampled at pixel centers - there is no true camera jitter,
    // and resampling the frame at a jittered offset is only a bilinear interpolation
    // of texels we already have (measured: +0.16 dB over 32 samples - no new
    // information enters). DLSS expects to be told the offset the frame was
    // actually sampled at, and the sign convention matters a lot (measured on a
    // controlled scene: wrong sign -6.7 dB, zero-when-jittered -3.1 dB, both
    // presenting as under-converged rippling).
    //
    //  Zero    - no resample, report (0,0). The truthful contract for video; default.
    //  Matched - resample color+guides at +j, report -j (DLSS convention: the
    //            reported offset is the negation of where the renderer sampled).
    //  Legacy  - resample at +j and report +j: the historical behavior, which
    //            is the wrong-sign case. Kept only for A/B comparison.
    enum class JitterMode { Zero, Matched, Legacy };

    struct ColorSettings {
        float brightness = 0.0f;   // exposure-like brightness, in stops (-2..+2)
        float contrast = 1.0f;     // 0..3
        float saturation = 1.0f;   // 0..3
        float gamma = 1.0f;        // 0.25..3
        float temperature = 0.0f;  // -1..+1 (cool..warm)
        float tint = 0.0f;         // -1..+1 (green..magenta)
    };

    ~D3D12Renderer();
    bool Initialize(HWND hwnd, uint32_t sourceW, uint32_t sourceH,
                    uint32_t outputW, uint32_t outputH,
                    uint32_t gridW, uint32_t gridH);
    // extDepthR16: optional tightly packed R16_UNORM depth plane (sourceW x sourceH,
    // 0 = near .. 1 = far, matching the non-inverted NGX contract). When external
    // depth is enabled and provided, it replaces the heuristic depth proxy in the
    // exact depth resource DLSS/NR read. Enable BEFORE Initialize().
    bool RenderFrame(const uint8_t* bgra, size_t bytes,
                     const float* guideGridRGBA32F, size_t guideBytes,
                     uint32_t gridW, uint32_t gridH,
                     bool temporalReset,
                     const uint8_t* extDepthR16 = nullptr, size_t extDepthBytes = 0,
                     const uint8_t* extFlowBGRA = nullptr, size_t extFlowBytes = 0,
                     const uint8_t* extMaskBGRA = nullptr, size_t extMaskBytes = 0);
    void EnableExternalDepth() { m_useExtDepth = true; }
    // Runtime A/B gates: with external data armed, these switch between the
    // model-supplied guide and the built-in fallback instantly (no reload).
    void SetExternalDepthEnabled(bool on) { m_extDepthEnabled = on; }
    void SetExternalFlowEnabled(bool on) { m_extFlowEnabled = on; }
    // extFlowBGRA: optional decoded frame of a _flow.mp4 (R = dx, G = dy mapped
    // from [-24..24] source px to 0..255, 127.5 = zero). Replaces the CPU block
    // matcher's motion-vector field entirely. Enable BEFORE Initialize().
    void EnableExternalFlow() { m_useExtFlow = true; }
    // extMaskBGRA: optional decoded frame of a _mask.mp4 (a segmentation mask -
    // white = process here). Replaces the block-matcher uncertainty as the
    // ControlMask source, and unlike it keeps soft edges instead of a hard
    // threshold. SetNRMaskMode still selects as-is / white / inverted, so the
    // two sources A/B against each other. Enable BEFORE Initialize().
    void EnableExternalMask() { m_useExtMask = true; }

    void SetDLSS(bool enabled) { m_dlssEnabled = enabled; }
    // Direct DLSS-NR knobs (feature 18 through NeuralEngine). Live-settable;
    // read per evaluation. Only the style/intensity/structure/autoMask fields
    // are taken from `s`: reset, the three guide pointers and the MV scale are
    // per-frame state the renderer fills in itself on every evaluation.
    void SetNRSettings(const NeuralEngine::Settings& s) { m_nrSettings = s; }
    // Bind guide textures to the NR evaluation (the runtime answers to
    // DLSSNR.MVec/Depth/ControlMask). Bitmask: 1 = MVec, 2 = Depth, 4 =
    // ControlMask; 0 = the historical colour-only evaluate. Binding the
    // ControlMask was measured to SUPPRESS the neural effect (a zero mask
    // reads as "leave everything"), so the default is MVec only.
    void SetNRGuideMask(uint32_t m) { m_nrGuideMask = m; }
    // What the bias/ControlMask texture carries: 0 = raw uncertainty bias,
    // 1 = white (process everywhere), 2 = inverted (process where confident).
    // Applies to the GPU guide expansion, so the Mask debug view shows exactly
    // what would be fed to the runtime.
    void SetNRMaskMode(uint32_t m) { m_nrMaskMode = m; }
    // Scale reported for DLSSNR.MVecScaleX/Y. Default -1: the runtime reads
    // MVec with the opposite sign of our current->previous field (measured:
    // +1 turns real flow into 1.4px background lurches, -1 removes them).
    void SetNRMVScale(float s) { m_nrMVScale = s; }
    // Gate on the FINAL MV field the guide expansion writes (0 = force zero,
    // 1 = pass through). Zero motion mode must zero the field even when an
    // external flow texture bypasses the CPU-side grid override. Live-settable.
    // 0 = NR receives zero motion vectors (the MV debug view still shows the
    // estimated field); anything else = NR receives the estimated field.
    void SetMVFieldScale(float s) { m_mvFieldScale = s; }
    // Render preset hint (DLSSNR.Hint.Render.Preset) - applied when the NR
    // feature is created, so set it before the first RenderFrame.
    void SetNRPreset(uint32_t p) { NR().SetPreset(p); }
    bool DLSSAvailable() const { return m_nrLoaded; }
    bool DLSSEnabled() const { return m_dlssEnabled && m_nrLoaded; }
    uint32_t DLSSInputW() const { return m_renderW; }
    uint32_t DLSSInputH() const { return m_renderH; }
    uint32_t OutputW() const { return m_outputW; }
    uint32_t OutputH() const { return m_outputH; }
    void SetDebugView(DebugView v) { m_debugView = v; }
    DebugView GetDebugView() const { return m_debugView; }
    void SetJitterMode(JitterMode m) { m_jitterMode = m; }
    bool DLSSFeatureCreated() const { return m_nr && m_nr->FeatureReady(); }
    uint64_t DLSSEvaluations() const { return m_nrEvaluations; }
    void WaitGPU();
    bool PresentCurrent();
    // Inspection zoom on the presented image (and debug views): the present pass
    // samples uv*scale+offset, so the window shows a GPU-magnified crop of the
    // PROCESSED output. scale 1 = off; center is clamped inside the frame.
    // Export readback ignores it, so files never bake the zoom in.
    void SetZoom(float cx, float cy, float scale) {
        m_zoomScale = scale < 0.05f ? 0.05f : (scale > 1.0f ? 1.0f : scale);
        m_zoomCX = cx; m_zoomCY = cy;
    }
    // A/B compare separator over the final image: mode 0 = off, 1 = vertical
    // split (before | after left-to-right), 2 = horizontal split (before over
    // after). pos is the separator position in 0..1 screen space. The "before"
    // side is the NR/effects input; the "after" side is the full pipeline.
    // Present-stage only; exports ignore it.
    void SetCompare(int mode, float pos) { m_compareMode = mode; m_comparePos = pos; }
    // Drives the small FX ON / FX BYPASS pill under the DLSS ON compare label.
    void SetFxBypassIndicator(bool bypassed) { m_fxBypassIndicator = bypassed; }
    void SetColorSettings(const ColorSettings& settings) { m_colorSettings = settings; }

    // Headless export: when enabled, RenderFrame copies the final backbuffer to a
    // CPU-readable buffer and (after GPU sync) fills m_exportRGBA with tightly-packed
    // RGBA8 (outputW*outputH*4). Used by the --export path to write frames to ffmpeg.
    void EnableExport(bool on) { m_exportMode = on; }
    const std::vector<uint8_t>& ExportRGBA() const { return m_exportRGBA; }

    // Creative 3D LUT, applied to the decoded frame in gamma (display-referred sRGB)
    // space BEFORE the linear conversion that feeds DLSS - so the neural pass sees
    // and enhances the graded picture. Call before Initialize(). rgbTriples holds
    // size^3 * 3 floats in .cube order (red fastest). Strength lerps original->graded.
    bool SetLUT(std::vector<float> rgbTriples, uint32_t size,
                const float domainMin[3], const float domainMax[3]);
    void SetLUTStrength(float s) { m_lutStrength = s; }

    // Pre-DLSS unsharp mask on the decoded frame (0 = off). A small clamped
    // detail boost gives the neural pass more micro-contrast to bite into.
    void SetPreSharpen(float amount) { m_preSharpen = amount; }

    // Post-DLSS unsharp mask on the neural output (0 = off), applied in the
    // present/export pass. The NR models denoise while redrawing (measured
    // laplacian drop vs input); this restores acutance AFTER the neural pass,
    // where the pre-sharpen cannot reach. Live-settable.
    void SetPostSharpen(float amount) { m_postSharpen = amount; }

    // 16-bit input transport: the decoded texture becomes R16G16B16A16_UNORM and
    // RenderFrame expects tightly packed RGBA16 (little-endian, R first) frames of
    // sourceW*sourceH*8 bytes, so float sources reach DLSS without an 8-bit
    // quantize. Call BEFORE Initialize(). Output stays RGBA8.
    void EnableDeepInput() { m_deepInput = true; }

    // Live GPU approximation of the exporter's tone-preserve pass: keeps the
    // NR detail but pulls tone/colors back toward the original by `mix`
    // (0 = raw NR, 1 = fully preserved). Uses a downsampled low-frequency pair
    // instead of the exporter's exact box blur - a preview-grade approximation.
    void SetToneMix(float mix) { m_toneMix = mix; }
    // NR Smooth: motion-compensated EMA over the NR contribution (output -
    // input). One GPU pass, shared by the live preview and the exporter. 0 = off.
    void SetNRSmooth(float s) { m_nrSmooth = s; }

private:
    static constexpr uint32_t FrameCount = 3;
    // NVIDIA's D3D12 DLSS contract expects input resources in NON_PIXEL_SHADER_RESOURCE
    // at EvaluateFeature time. Debug/presentation passes temporarily transition selected
    // resources to PIXEL_SHADER_RESOURCE and restore them before the frame ends.
    static constexpr D3D12_RESOURCE_STATES GuideReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES DepthGuideReadState =
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool CreateDeviceAndSwapchain(HWND hwnd);
    bool CreateHeapsAndBackbuffers();
    bool CreatePipelines();
    bool CreateVideoResources();
    bool CreateOverlayLabels();   // bakes the "DLSS OFF"/"DLSS ON" atlas at heap slot 16
    bool InitializeDLSS();
    bool CreateUploadForTexture(const D3D12_RESOURCE_DESC& desc,
                                Microsoft::WRL::ComPtr<ID3D12Resource>& upload,
                                uint8_t*& mapped,
                                D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                uint32_t& rows,
                                uint64_t& rowBytes,
                                uint64_t& totalBytes,
                                const char* name);
    void CopyMappedRows(uint8_t* mapped, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                        const void* src, size_t tightRowBytes, uint32_t rows);
    bool WaitForFrameSlot(uint32_t slot);
    void SignalFrameSlot(uint32_t slot);
    // Records the NR Smooth pass over m_dlssOutput; drops its history (and does
    // nothing else) when NR did not run this frame or the strength is 0.
    void RecordNRSmooth(ID3D12GraphicsCommandList* cmd, bool used, bool temporalReset);
    void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    D3D12_CPU_DESCRIPTOR_HANDLE RTV(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE SRVCPU(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SRVGPU(uint32_t index) const;
    static float Halton(uint32_t index, uint32_t base);

    HWND m_hwnd = nullptr;
    uint32_t m_sourceW=0,m_sourceH=0,m_outputW=0,m_outputH=0,m_renderW=0,m_renderH=0,m_gridW=0,m_gridH=0;

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmds[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint64_t m_frameFence[FrameCount]{};
    uint32_t m_frameSlot = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    uint32_t m_rtvInc=0,m_srvInc=0,m_dsvInc=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[FrameCount];

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvert;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvertLUT;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresent;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMotionDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthWrite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthWriteExt;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoExpandGuides;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoExpandGuidesExt;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDownPair;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresentTone;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSmooth;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoToSRGB, m_psoFromSRGB;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_decodedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;      // R32_TYPELESS: D32 DSV + R32 SRV, same resource passed to NGX
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motion;
    // All-zero MV texture handed to NGX in Motion=Zero mode, so m_motion can keep
    // the estimated field for the MV debug view. Cleared once, then read-only.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motionZero;
    bool m_motionZeroCleared = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_biasCurrent;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideGrid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideUpload[FrameCount];

    uint8_t* m_uploadMapped[FrameCount]{};
    uint8_t* m_guideMapped[FrameCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_uploadFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_guideFootprint{};
    uint32_t m_numRows=0,m_guideRows=0;
    uint64_t m_rowSize=0,m_uploadBytes=0,m_guideRowSize=0,m_guideUploadBytes=0;

    bool m_sourceInCopyDest = true;
    bool m_gridInCopyDest = true;
    bool m_colorInRT = true;
    bool m_guidesInRT = true;
    bool m_depthInWrite = true;
    bool m_outputInUAV = true;
    bool m_dlssEnabled = true;
    bool m_allowTearing = false;
    uint64_t m_framesPresented = 0;
    bool m_exportMode = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_exportReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_exportFootprint{};
    std::vector<uint8_t> m_exportRGBA;
    DebugView m_debugView = DebugView::Final;
    JitterMode m_jitterMode = JitterMode::Zero;
    uint32_t m_nrGuideMask = 1;
    uint32_t m_nrMaskMode = 0;
    bool m_fxBypassIndicator = false;
    float m_nrMVScale = -1.0f;
    float m_mvFieldScale = 1.0f;
    float m_preSharpen = 0.0f;
    float m_postSharpen = 0.0f;
    bool m_deepInput = false;
    float m_toneMix = 0.0f;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lowRef;   // downsampled dlssColor (low-frequency estimate)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lowOut;   // downsampled dlssOutput
    uint32_t m_lowW = 0, m_lowH = 0;
    bool m_lowInRT = true;
    // NR Smooth: the smoothed picture (copied back over m_dlssOutput so every
    // later pass keeps reading the one texture) and a ping-pong pair holding
    // last frame's smoothed NR delta in sRGB units.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_smoothOut;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_smoothDelta[2];
    float m_nrSmooth = 0.0f;
    uint32_t m_smoothCur = 0;          // delta texture written this frame
    bool m_smoothHasHistory = false;   // false = the previous delta is stale
    std::vector<float> m_lutData;      // size^3 RGBA32F texels, red fastest
    uint32_t m_lutSize = 0;
    float m_lutStrength = 1.0f;
    float m_lutDomainScale[3]{1.0f,1.0f,1.0f};
    float m_lutDomainOffset[3]{0.0f,0.0f,0.0f};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lutTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lutUpload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_lutFootprint{};
    bool m_lutUploaded = false;
    // External (model-estimated) full-resolution depth, R16_UNORM at source size.
    bool m_useExtDepth = false;
    bool m_extDepthEnabled = true;
    bool m_extFlowEnabled = true;
    bool m_extDepthValid = false;
    bool m_extDepthInCopyDest = true;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_extDepthTex;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_extDepthUpload[FrameCount];
    uint8_t* m_extDepthMapped[FrameCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_extDepthFootprint{};
    uint32_t m_extDepthRows = 0;
    uint64_t m_extDepthRowSize = 0, m_extDepthUploadBytes = 0;
    // External (model-derived) segmentation mask, decoded _mask.mp4 frames (BGRA).
    bool m_useExtMask = false;
    bool m_extMaskValid = false;
    bool m_extMaskInCopyDest = true;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_extMaskTex;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_extMaskUpload[FrameCount];
    uint8_t* m_extMaskMapped[FrameCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_extMaskFootprint{};
    uint32_t m_extMaskRows = 0;
    uint64_t m_extMaskRowSize = 0, m_extMaskUploadBytes = 0;
    // External (model-estimated) optical flow, decoded _flow.mp4 frames (BGRA).
    bool m_useExtFlow = false;
    bool m_extFlowValid = false;
    bool m_extFlowInCopyDest = true;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_extFlowTex;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_extFlowUpload[FrameCount];
    uint8_t* m_extFlowMapped[FrameCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_extFlowFootprint{};
    uint32_t m_extFlowRows = 0;
    uint64_t m_extFlowRowSize = 0, m_extFlowUploadBytes = 0;
    ColorSettings m_colorSettings{};
    float m_zoomCX = 0.5f, m_zoomCY = 0.5f, m_zoomScale = 1.0f;
    int m_compareMode = 0; float m_comparePos = 0.5f;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_labelAtlas;   // RGBA8 512x64 overlay labels
    Microsoft::WRL::ComPtr<ID3D12Resource> m_labelUpload;
    bool m_lastDLSSUsed = false;
    // Direct DLSS-NR staging: FP16 textures carrying sRGB-ENCODED values. The NR
    // engine consumes sRGB-encoded LDR in a plain float resource - an _SRGB-typed
    // view would linearise on read (the double-transfer bug).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_nrColor;   // NR input, sRGB-encoded FP16
    Microsoft::WRL::ComPtr<ID3D12Resource> m_nrOut;     // NR output UAV, sRGB-encoded FP16
    // The NR runtime, created on first use so SetNRPreset can reach it before
    // Initialize. NR is 1:1, so there is no separate render size to track:
    // m_renderW/H are the output size once Load() succeeds.
    NeuralEngine& NR() { if (!m_nr) m_nr = std::make_unique<NeuralEngine>(); return *m_nr; }
    std::unique_ptr<NeuralEngine> m_nr;
    NeuralEngine::Settings m_nrSettings;
    uint64_t m_nrEvaluations = 0;
    bool m_nrLoaded = false;
};
