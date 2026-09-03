#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <cstdint>

// Host for DLSS Super Resolution - the public NGX SuperSampling feature, run
// through the driver core with nvngx_dlss.dll beside the exe. It is a temporal
// upscaler: fed the same motion vectors, depth and bias mask the neural pass
// receives, it reconstructs the output size from the frame history instead of
// resampling one frame. It runs AFTER the neural pass, which then works at
// source resolution (cheaper, and on real pixels rather than a resize).
class SuperRes {
public:
    ~SuperRes();
    SuperRes() = default;
    SuperRes(const SuperRes&) = delete;
    SuperRes& operator=(const SuperRes&) = delete;

    // `coreInitialised`: NVSDK_NGX_D3D12_Init has already been done for this
    // device (NeuralEngine does it, and owns the matching shutdown); otherwise
    // this object initialises the core and shuts it down itself.
    bool Load(ID3D12Device* device, bool coreInitialised);
    bool Available() const { return m_available; }

    // Chooses the render size and quality mode for source -> output: every mode
    // advertises a dynamic input range, and the source is moved as little as
    // possible into the range that needs the smallest resize (unchanged when a
    // range admits it as it is). Records feature creation on `cmd`: submit and
    // wait before the first Evaluate. Cheap no-op once created for these sizes.
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd,
                       uint32_t sourceW, uint32_t sourceH, uint32_t outputW, uint32_t outputH);
    bool FeatureReady() const { return m_feature != nullptr; }
    // The input size the feature was created for: what the pipeline renders at.
    uint32_t RenderW() const { return m_renderW; }
    uint32_t RenderH() const { return m_renderH; }
    const char* QualityName() const;

    struct Inputs {
        ID3D12Resource* color = nullptr;   // linear FP16, render size, NON_PIXEL_SHADER_RESOURCE
        ID3D12Resource* output = nullptr;  // FP16 UAV, output size
        ID3D12Resource* depth = nullptr;   // R32 (D32) render size, 0 = near
        ID3D12Resource* motion = nullptr;  // RG16F render size, current -> previous, render pixels
        ID3D12Resource* bias = nullptr;    // R8 render size, 1 = distrust the history here
        float jitterX = 0.0f, jitterY = 0.0f;   // render pixels, the sample offset DLSS must undo
        float frameTimeMs = 1000.0f / 30.0f;
        bool reset = false;
    };
    // Records one evaluation. Leaves the command list with NGX's own descriptor
    // heaps and root signature, like the neural pass: the caller restores them.
    bool Evaluate(ID3D12GraphicsCommandList* cmd, const Inputs& in);
    uint64_t Evaluations() const { return m_evaluations; }

private:
    void ReleaseFeature();

    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_caps = nullptr;     // the driver's capability block: optimal-settings queries live there
    NVSDK_NGX_Parameter* m_params = nullptr;   // this feature's own block (create + evaluate)
    NVSDK_NGX_Handle* m_feature = nullptr;
    NVSDK_NGX_PerfQuality_Value m_quality = NVSDK_NGX_PerfQuality_Value_Balanced;
    uint32_t m_sourceW = 0, m_sourceH = 0;   // what the feature was chosen for
    uint32_t m_renderW = 0, m_renderH = 0, m_outputW = 0, m_outputH = 0;
    bool m_ownsCore = false;
    bool m_available = false;
    uint64_t m_evaluations = 0;
};
