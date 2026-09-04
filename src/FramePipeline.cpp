#include "FramePipeline.h"
#include <algorithm>

bool FramePipeline::Render(const uint8_t* frame, size_t bytes, bool reset, const Sidecars& sc) {
    if (m_policy.temporalOff) reset = true;

    // 16-bit frames arrive tightly packed as RGBA16LE; the CPU-side guide
    // analysis works on an 8-bit BGRA shadow of the same picture.
    const uint8_t* analysis = frame;
    if (m_policy.bits == 16) {
        const size_t px = size_t(m_policy.srcW) * m_policy.srcH;
        if (bytes < px * 8) return false;
        m_shadow8.resize(px * 4);
        const uint16_t* s = reinterpret_cast<const uint16_t*>(frame);
        for (size_t i = 0; i < px; ++i) {
            m_shadow8[i * 4 + 0] = uint8_t(s[i * 4 + 2] >> 8);
            m_shadow8[i * 4 + 1] = uint8_t(s[i * 4 + 1] >> 8);
            m_shadow8[i * 4 + 2] = uint8_t(s[i * 4 + 0] >> 8);
            m_shadow8[i * 4 + 3] = uint8_t(s[i * 4 + 3] >> 8);
        }
        analysis = m_shadow8.data();
    }

    GuideFrame g;
    if (!m_guides.Generate(analysis, m_policy.srcW, m_policy.srcH,
                           m_renderer.DLSSInputW(), m_renderer.DLSSInputH(),
                           m_policy.fps, reset, g))
        return false;

    // A scene cut (correspondence loss: globalCost over threshold in the guide
    // generator) already zeroes the flow field, but the accumulated neural
    // history would still smear the previous shot into the new one for several
    // frames unless the runtime's reset flag is raised too.
    const bool cut = m_policy.cutReset && !reset && !g.hasHistory;
    if (cut) ++m_cutCount;
    if (m_policy.cutReset && !g.hasHistory) reset = true;
    m_lastReset = reset;

    // The field as measured, captured before any override below.
    m_flowGW = g.gridW;
    m_flowGH = g.gridH;
    m_flowValid = g.hasHistory && !reset;
    m_lastFlow.resize(size_t(m_flowGW) * m_flowGH * 2);
    for (size_t i = 0, n = size_t(m_flowGW) * m_flowGH; i < n; ++i) {
        m_lastFlow[i * 2 + 0] = g.guideGridRGBA32F[i * 4 + 0];
        m_lastFlow[i * 2 + 1] = g.guideGridRGBA32F[i * 4 + 1];
    }

    // Global pan flattens the compact grid (R = mvX, G = mvY, B = depth,
    // A = bias) to one rigid vector: the per-axis median of the subpixel-
    // refined field, which keeps the camera motion but drops the per-block
    // noise that reads as swimming in the neural history.
    //
    // Zero mode deliberately leaves the grid alone. The renderer's
    // SetMVFieldScale(0) hands the runtime an all-zero motion texture, so the
    // grid stays honest and the motion-vector debug view keeps showing what
    // was actually measured.
    if (m_policy.motion == MotionMode::GlobalPan) {
        float* p = g.guideGridRGBA32F.data();
        const size_t cells = g.guideGridRGBA32F.size() / 4;
        float panX = 0.0f, panY = 0.0f;
        if (g.hasHistory && cells) {
            std::vector<float> xs(cells), ys(cells);
            for (size_t i = 0; i < cells; ++i) { xs[i] = p[i * 4 + 0]; ys[i] = p[i * 4 + 1]; }
            std::nth_element(xs.begin(), xs.begin() + cells / 2, xs.end());
            std::nth_element(ys.begin(), ys.begin() + cells / 2, ys.end());
            panX = xs[cells / 2];
            panY = ys[cells / 2];
        }
        for (size_t i = 0; i < cells; ++i, p += 4) { p[0] = panX; p[1] = panY; }
    }

    const float* grid = g.guideGridRGBA32F.data();
    const size_t gridBytes = g.guideGridRGBA32F.size() * sizeof(float);

    // No correspondence to the previous frame (a cut, a fresh history, and the
    // extra renders of the same frame below): the temporal stages get zero
    // vectors whatever the field says. The grid is already zero in that case;
    // an external flow frame is not - across a cut it is RAFT's guess between
    // two unrelated pictures, and it would warp the converging history of the
    // warmup renders, which are the SAME frame and have no motion at all.
    m_renderer.SetMotionInvalid(!g.hasHistory);

    // Micro-warmup: a bare reset trades ghosting for one or two visibly softer
    // frames (fresh history). Converge the new history on this frame with extra
    // discarded renders first, then let the kept render continue it.
    if (cut && m_policy.cutWarmup > 0) {
        for (int i = 0; i < m_policy.cutWarmup; ++i) {
            if (!m_renderer.RenderFrame(frame, bytes, grid, gridBytes, g.gridW, g.gridH,
                                        i == 0, sc.depth, sc.depthBytes,
                                        sc.flow, sc.flowBytes, sc.mask, sc.maskBytes))
                return false;
        }
        reset = false;
    }
    return m_renderer.RenderFrame(frame, bytes, grid, gridBytes, g.gridW, g.gridH,
                                  reset, sc.depth, sc.depthBytes,
                                  sc.flow, sc.flowBytes, sc.mask, sc.maskBytes);
}

void PackMaskLayers(const uint8_t* const* layersBGRA, uint32_t count,
                    uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    const size_t px = size_t(w) * h;
    out.assign(px * 4, 0);
    static constexpr int kByte[4] = {2, 1, 0, 3};   // BGRA byte order: R, G, B, A
    for (uint32_t k = 0; k < count && k < 4; ++k) {
        const uint8_t* src = layersBGRA ? layersBGRA[k] : nullptr;
        if (!src) continue;
        uint8_t* dst = out.data() + kByte[k];
        for (size_t i = 0; i < px; ++i) dst[i * 4] = src[i * 4 + 1];
    }
}

void DepthGrayToR16(const uint8_t* bgra, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    const size_t px = size_t(w) * h;
    out.resize(px * 2);
    uint16_t* dst = reinterpret_cast<uint16_t*>(out.data());
    for (size_t i = 0; i < px; ++i)
        dst[i] = uint16_t(65535u - uint32_t(bgra[i * 4 + 1]) * 257u);
}
