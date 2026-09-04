#pragma once
// The one "guides -> motion override -> reset policy -> RenderFrame" sequence.
//
// The preview and the exported file are meant to be the same picture, so the
// orchestration between the temporal guide generator and the renderer lives
// here rather than once per executable: what a scene cut does to the neural
// history, how a motion mode reaches the runtime, and how a 16-bit frame is
// analysed are single decisions. A difference between the player and the
// exporter is then a bug in one caller, not two policies that drifted.
//
// The renderer stays the caller's: this owns the guide generator and the
// per-frame policy, and holds a reference to the renderer it drives.
#include <cstdint>
#include <vector>
#include "D3D12Renderer.h"
#include "TemporalGuides.h"

class FramePipeline {
public:
    // What the neural pass is told about motion.
    enum class MotionMode {
        Zero,       // no motion at all (the renderer hands NGX a zero MV texture)
        GlobalPan,  // one rigid vector per frame, the per-axis median of the field
        Estimated,  // the per-block field as measured
    };

    struct Policy {
        uint32_t srcW = 0, srcH = 0;  // size of the frames handed to Render()
        double fps = 24.0;            // nominal rate, the fallback frame time
        MotionMode motion = MotionMode::Estimated;
        bool temporalOff = false;     // every frame starts from fresh history
        bool cutReset = true;         // a detected scene cut clears the history
        int cutWarmup = 0;            // discarded renders that converge new history
        int bits = 8;                 // 8 = BGRA8 frames, 16 = packed RGBA16LE
    };

    // Optional guide planes decoded from the sidecar files for THIS frame.
    struct Sidecars {
        const uint8_t* depth = nullptr; size_t depthBytes = 0;
        const uint8_t* flow  = nullptr; size_t flowBytes  = 0;
        const uint8_t* mask  = nullptr; size_t maskBytes  = 0;
    };

    explicit FramePipeline(D3D12Renderer& renderer) : m_renderer(renderer) {}

    void SetPolicy(const Policy& p) { m_policy = p; }
    const Policy& GetPolicy() const { return m_policy; }
    // The one policy field the player flips live from its menu.
    void SetMotionMode(MotionMode m) { m_policy.motion = m; }

    // The guide generator, for the settings that belong to it (depth mode,
    // analysis quality) and for dropping its history on a seek.
    TemporalGuideGenerator& Guides() { return m_guides; }
    void ResetHistory() { m_guides.Reset(); }

    // Renders one frame; the result is in D3D12Renderer::ExportRGBA() when the
    // renderer is in export mode. `reset` forces fresh neural history for this
    // frame (a seek, a discontinuity); a detected cut can raise it too.
    bool Render(const uint8_t* frame, size_t bytes, bool reset, const Sidecars& sidecars = {});

    // State of the last Render, for callers that post-process the result.
    bool LastReset() const { return m_lastReset; }
    uint64_t CutCount() const { return m_cutCount; }
    // The measured flow of the last frame (current -> previous, guide-grid
    // cells, x then y), captured BEFORE any motion override so a caller can
    // motion-compensate even when the runtime was fed zero or global vectors.
    // Null when the last frame had no usable history.
    const float* LastFlow() const { return m_flowValid ? m_lastFlow.data() : nullptr; }
    uint32_t FlowGridW() const { return m_flowGW; }
    uint32_t FlowGridH() const { return m_flowGH; }
    // The BGRA8 view built from the last 16-bit frame (empty in 8-bit mode).
    // Callers that want a low-frequency reference of the input reuse it
    // instead of converting the same frame a second time.
    const std::vector<uint8_t>& Shadow8() const { return m_shadow8; }

private:
    D3D12Renderer& m_renderer;
    TemporalGuideGenerator m_guides;
    Policy m_policy;
    std::vector<uint8_t> m_shadow8;   // BGRA8 view of a 16-bit frame, for analysis
    std::vector<float> m_lastFlow;
    uint32_t m_flowGW = 0, m_flowGH = 0;
    bool m_flowValid = false;
    bool m_lastReset = false;
    uint64_t m_cutCount = 0;
};

// A decoded _depth.mp4 frame (gray, bright = near) as the R16 plane the
// renderer's external-depth input wants (0 = near).
void DepthGrayToR16(const uint8_t* bgra, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

// Up to four decoded segmentation-layer frames (gray BGRA, white = the object)
// packed into the one BGRA8 plane the renderer's mask input takes: layer 0 in
// R, 1 in G, 2 in B, 3 in A. A null or missing layer is transparent. The
// renderer composites them with per-layer weights (D3D12Renderer::SetMaskLayers).
void PackMaskLayers(const uint8_t* const* layersBGRA, uint32_t count,
                    uint32_t w, uint32_t h, std::vector<uint8_t>& out);
