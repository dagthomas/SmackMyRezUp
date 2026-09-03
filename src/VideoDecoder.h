#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdint>
#include <string>
#include <vector>

struct VideoFrame {
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns = 0;
    bool discontinuity = false;
};

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    bool Open(const std::wstring& path);
    void Close();
    bool ReadNext(VideoFrame& out);
    bool SeekSeconds(double seconds);
    bool SetDecodeSize(uint32_t width, uint32_t height);
    // Deep output: frames arrive as RGBA 16-bit little-endian (8 bytes/px,
    // channel order R, G, B, A) instead of BGRA 8-bit, so 10-bit sources keep
    // their precision. Set before Open(); needs the FFmpeg backend (Media
    // Foundation is not tried). Used for _flow.mp4 sidecars.
    void SetDeepOutput(bool on) { m_deep = on; }
    bool DeepOutput() const { return m_deep; }
    uint32_t BytesPerPixel() const { return m_deep ? 8u : 4u; }
    // The smru_flow_range metadata tag of a flow sidecar: the source-pixel
    // displacement (+/-) mapped across the full code range. 0 when the file
    // carries none (the legacy contract, +/-24 px over 8 bits).
    double FlowRangePx() const { return m_flowRange; }
    const std::string& PixelFormat() const { return m_pixFmt; }

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t NativeWidth() const { return m_nativeWidth ? m_nativeWidth : m_width; }
    uint32_t NativeHeight() const { return m_nativeHeight ? m_nativeHeight : m_height; }
    double FrameRate() const { return m_fps; }
    double DurationSeconds() const { return m_durationSec; }
    double DisplayAspectRatio() const { return m_displayAspect > 0.0 ? m_displayAspect : (m_height ? double(m_width)/double(m_height) : 16.0/9.0); }
    const wchar_t* BackendName() const;

private:
    enum class Backend { None, FFmpeg, MediaFoundation };

    bool OpenFFmpeg(const std::wstring& path);
    bool ProbeFFmpeg(const std::wstring& path);
    bool StartFFmpeg(double seekSeconds);
    bool ReadNextFFmpeg(VideoFrame& out);
    void StopFFmpeg();

    bool OpenMediaFoundation(const std::wstring& path);
    bool ReadNextMediaFoundation(VideoFrame& out);

    Backend m_backend = Backend::None;
    Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;
    std::wstring m_path;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_nativeWidth = 0;
    uint32_t m_nativeHeight = 0;
    int32_t m_stride = 0;
    double m_fps = 30.0;
    double m_durationSec = 0.0;
    double m_displayAspect = 0.0;
    bool m_deep = false;
    double m_flowRange = 0.0;
    std::string m_pixFmt;

    std::wstring m_ffmpegExe;
    std::wstring m_ffprobeExe;
    HANDLE m_ffmpegProcess = nullptr;
    HANDLE m_ffmpegStdout = nullptr;
    uint64_t m_ffmpegFrameIndex = 0;
    int64_t m_ffmpegSeekBase100ns = 0;
};
