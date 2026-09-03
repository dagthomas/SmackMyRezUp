#include "SuperRes.h"
#include "Log.h"

// nvsdk_ngx_params.h declares parameter Set/Get overloads over ID3D11Resource*
// and ID3D12Resource* but includes neither, so both must be in scope first.
#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_params.h>

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace {

// The app id the driver core is registered under - the same one NeuralEngine
// uses, so both features live in the one NGX session when NR loaded first.
constexpr unsigned long long kNgxAppId = 102100511ull;

std::wstring NgxLogDir() {
    wchar_t buf[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return (n == 0 || n > MAX_PATH + 1) ? L".\\" : std::wstring(buf, n);
}

const char* QualityLabel(NVSDK_NGX_PerfQuality_Value q) {
    switch (q) {
    case NVSDK_NGX_PerfQuality_Value_DLAA:             return "DLAA";
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:       return "Quality";
    case NVSDK_NGX_PerfQuality_Value_Balanced:         return "Balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:          return "Performance";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "UltraPerformance";
    default:                                           return "?";
    }
}

} // namespace

SuperRes::~SuperRes() {
    ReleaseFeature();
    if (m_params) { NVSDK_NGX_D3D12_DestroyParameters(m_params); m_params = nullptr; }
    if (m_ownsCore && m_device) NVSDK_NGX_D3D12_Shutdown1(m_device);
    m_ownsCore = false;
}

bool SuperRes::Load(ID3D12Device* device, bool coreInitialised) {
    m_device = device;
    if (!coreInitialised) {
        const std::wstring logDir = NgxLogDir();
        const NVSDK_NGX_Result r =
            NVSDK_NGX_D3D12_Init(kNgxAppId, logDir.c_str(), device, nullptr, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) {
            LOG("DLSS SR: driver-core NGX init failed 0x" << std::hex << r);
            return false;
        }
        m_ownsCore = true;
    }

    // The capability block says whether the driver + nvngx_dlss.dll pair can
    // do SuperSampling at all, and it is where the optimal-settings callback
    // lives (the SDK helper reads it from the block it is handed). The feature
    // itself gets its own parameter block so nothing the neural pass sets can
    // leak into an SR evaluate.
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_caps);
    if (NVSDK_NGX_FAILED(r) || !m_caps) {
        LOG("DLSS SR: GetCapabilityParameters failed 0x" << std::hex << r);
        m_caps = nullptr;
        return false;
    }
    int available = 0, needsDriver = 0;
    m_caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    m_caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    if (!available) {
        LOG("DLSS SR: SuperSampling.Available=0 (nvngx_dlss.dll beside the exe? driver update needed="
            << needsDriver << ")");
        return false;
    }
    r = NVSDK_NGX_D3D12_AllocateParameters(&m_params);
    if (NVSDK_NGX_FAILED(r) || !m_params) {
        LOG("DLSS SR: AllocateParameters failed 0x" << std::hex << r);
        m_params = nullptr;
        return false;
    }
    m_available = true;
    LOG("DLSS SR available (nvngx_dlss.dll, SuperSampling)");
    return true;
}

bool SuperRes::EnsureFeature(ID3D12GraphicsCommandList* cmd,
                             uint32_t sourceW, uint32_t sourceH, uint32_t outputW, uint32_t outputH) {
    if (!m_available || !cmd || !sourceW || !sourceH || !outputW || !outputH) return false;
    if (m_feature && m_sourceW == sourceW && m_sourceH == sourceH &&
        m_outputW == outputW && m_outputH == outputH) return true;
    ReleaseFeature();

    // Every mode advertises a dynamic input range (measured: Quality, Balanced
    // and Performance share 50%..100% of the output, UltraPerformance is pinned
    // at 33%). A source outside every range is not a reason to give up: it is
    // moved as little as possible into the range that needs the smallest
    // resize - a mild bilinear step before the neural pass, then the real
    // reconstruction. Ties go to the mode whose optimal input is nearest.
    // 1:1 is DLAA.
    NVSDK_NGX_PerfQuality_Value pick = NVSDK_NGX_PerfQuality_Value_DLAA;
    uint32_t renderW = sourceW, renderH = sourceH;
    if (sourceW != outputW || sourceH != outputH) {
        static constexpr NVSDK_NGX_PerfQuality_Value kModes[] = {
            NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
            NVSDK_NGX_PerfQuality_Value_MaxPerf,    NVSDK_NGX_PerfQuality_Value_UltraPerformance };
        double bestCost = 1e9;
        bool any = false;
        for (auto q : kModes) {
            unsigned optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
            float sharpness = 0.0f;
            const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
                m_caps, outputW, outputH, q, &optW, &optH, &maxW, &maxH, &minW, &minH, &sharpness);
            if (NVSDK_NGX_FAILED(r) || !optW || !optH || !minW || !minH || maxW < minW || maxH < minH) {
                LOG("DLSS SR: optimal settings for " << QualityLabel(q) << " failed 0x" << std::hex << r);
                continue;
            }
            LOG("DLSS SR: " << QualityLabel(q) << " optimal " << optW << "x" << optH
                << " range " << minW << "x" << minH << ".." << maxW << "x" << maxH);
            const uint32_t cw = std::clamp(sourceW, uint32_t(minW), uint32_t(maxW));
            const uint32_t ch = std::clamp(sourceH, uint32_t(minH), uint32_t(maxH));
            // The resize the mode forces on the source, then a small tie-break
            // toward the mode tuned for that input.
            const double cost = std::fabs(std::log(double(cw) / double(sourceW)))
                              + std::fabs(std::log(double(ch) / double(sourceH)))
                              + 0.01 * std::fabs(std::log(double(optW) / double(cw)));
            if (cost < bestCost) { bestCost = cost; pick = q; renderW = cw; renderH = ch; any = true; }
        }
        if (!any) {
            LOG("DLSS SR: no usable quality mode for " << outputW << "x" << outputH);
            return false;
        }
        // Keep the DLAA path for a source that already matches the output.
        if (renderW == outputW && renderH == outputH) pick = NVSDK_NGX_PerfQuality_Value_DLAA;
    }
    m_quality = pick;
    m_sourceW = sourceW; m_sourceH = sourceH;

    // The transformer preset for every mode; a runtime that does not know the
    // hint ignores it.
    for (const char* hint : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance })
        m_params->Set(hint, int(NVSDK_NGX_DLSS_Hint_Render_Preset_K));

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = renderW;
    cp.Feature.InHeight = renderH;
    cp.Feature.InTargetWidth = outputW;
    cp.Feature.InTargetHeight = outputH;
    cp.Feature.InPerfQualityValue = m_quality;
    // Motion vectors are render-size (MVLowRes); the picture is LDR linear FP16
    // and auto-exposed. Depth is conventional (0 = near), the vectors carry no
    // camera jitter: neither DepthInverted nor MVJittered.
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    cp.InEnableOutputSubrects = false;
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &m_feature, m_params, &cp);
    if (NVSDK_NGX_FAILED(r) || !m_feature) {
        LOG("DLSS SR: CreateFeature failed 0x" << std::hex << r);
        m_feature = nullptr;
        return false;
    }
    m_renderW = renderW; m_renderH = renderH; m_outputW = outputW; m_outputH = outputH;
    m_evaluations = 0;
    LOG("DLSS SR: feature created " << renderW << "x" << renderH << " -> " << outputW << "x"
        << outputH << " (" << QualityLabel(m_quality) << ", preset K"
        << ((renderW != sourceW || renderH != sourceH) ? "; source resized into the mode's range" : "") << ")");
    return true;
}

bool SuperRes::Evaluate(ID3D12GraphicsCommandList* cmd, const Inputs& in) {
    if (!m_feature || !cmd || !in.color || !in.output || !in.depth || !in.motion) return false;

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = in.color;
    ep.Feature.pInOutput = in.output;
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = in.depth;
    ep.pInMotionVectors = in.motion;
    ep.InJitterOffsetX = in.jitterX;
    ep.InJitterOffsetY = in.jitterY;
    ep.InRenderSubrectDimensions = { m_renderW, m_renderH };
    ep.InReset = in.reset ? 1 : 0;
    // The field is current -> previous in render pixels already.
    ep.InMVScaleX = 1.0f;
    ep.InMVScaleY = 1.0f;
    ep.pInBiasCurrentColorMask = in.bias;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    ep.InFrameTimeDeltaInMsec = in.frameTimeMs;

    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, m_feature, m_params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        LOG("DLSS SR: evaluate failed 0x" << std::hex << r);
        return false;
    }
    ++m_evaluations;
    return true;
}

const char* SuperRes::QualityName() const { return QualityLabel(m_quality); }

void SuperRes::ReleaseFeature() {
    if (m_feature) NVSDK_NGX_D3D12_ReleaseFeature(m_feature);
    m_feature = nullptr;
    m_sourceW = m_sourceH = m_renderW = m_renderH = m_outputW = m_outputH = 0;
}
