#include "VideoDecoder.h"
#include "AppPaths.h"
#include "ChildProcess.h"
#include "Log.h"
#include <propvarutil.h>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <cstring>

using Microsoft::WRL::ComPtr;

static bool ParseRate(const std::string& text, double& out) {
    const size_t slash = text.find('/');
    try {
        if (slash == std::string::npos) {
            const double v = std::stod(text);
            if (std::isfinite(v) && v > 0.0) { out = v; return true; }
            return false;
        }
        const double n = std::stod(text.substr(0, slash));
        const double d = std::stod(text.substr(slash + 1));
        if (d == 0.0) return false;
        const double v = n / d;
        if (std::isfinite(v) && v > 0.0) { out = v; return true; }
    } catch (const std::exception& e) {
        LOG("ParseRate: cannot parse '" << text << "': " << e.what());
    }
    return false;
}

VideoDecoder::~VideoDecoder() { Close(); }

const wchar_t* VideoDecoder::BackendName() const {
    switch (m_backend) {
    case Backend::FFmpeg: return L"FFmpeg";
    case Backend::MediaFoundation: return L"Media Foundation";
    default: return L"None";
    }
}

void VideoDecoder::Close() {
    StopFFmpeg();
    m_reader.Reset();
    m_backend = Backend::None;
}

bool VideoDecoder::Open(const std::wstring& path) {
    Close();
    m_path = path;
    m_width = m_height = 0;
    m_nativeWidth = m_nativeHeight = 0;
    m_stride = 0;
    m_fps = 30.0;
    m_durationSec = 0.0;
    m_displayAspect = 0.0;
    m_flowRange = 0.0;
    m_pixFmt.clear();

    LOG("Opening video. Decoder preference: FFmpeg -> Windows Media Foundation");

    // FFmpeg is intentionally preferred. It makes playback independent from
    // optional Microsoft Store codec packs and handles MKV/WebM/AV1/HEVC/etc.
    if (OpenFFmpeg(path)) {
        m_backend = Backend::FFmpeg;
        LOG("Video decoder selected: FFmpeg");
        return true;
    }

    if (m_deep) {
        LOG("FFmpeg backend unavailable or rejected the file; deep (16-bit) output has no Media Foundation path.");
        return false;
    }
    LOG("FFmpeg backend unavailable or rejected the file; trying Media Foundation.");
    if (OpenMediaFoundation(path)) {
        m_backend = Backend::MediaFoundation;
        LOG("Video decoder selected: Windows Media Foundation");
        return true;
    }

    LOG("All video decoder backends failed.");
    return false;
}

bool VideoDecoder::ProbeFFmpeg(const std::wstring& path) {
    std::wstring args =
        L"-v error -select_streams v:0 "
        L"-show_entries stream=width,height,display_aspect_ratio,sample_aspect_ratio,avg_frame_rate,r_frame_rate,pix_fmt:format=duration:format_tags=smru_flow_range "
        L"-of default=noprint_wrappers=1 " + smru::proc::Quote(path);

    // stderr stays on NUL: the key=value lines below are parsed as they come,
    // and a warning mixed into them is a line this parser cannot tell apart.
    std::string text;
    DWORD code = 0;
    if (m_ffprobeExe.empty() ||
        !smru::proc::RunCapture(smru::proc::Quote(m_ffprobeExe) + L" " + args, text, code,
                                {.captureStderr = false}) ||
        code != 0) {
        LOG("ffprobe failed, exitCode=" << code);
        return false;
    }

    uint32_t width = 0, height = 0;
    double avgRate = 0.0, rawRate = 0.0, duration = 0.0;
    double displayAspect = 0.0, sampleAspect = 1.0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "width") width = static_cast<uint32_t>(std::stoul(value));
            else if (key == "height") height = static_cast<uint32_t>(std::stoul(value));
            else if (key == "display_aspect_ratio" && value != "N/A") {
                const size_t c=value.find(':'); if(c!=std::string::npos){ double a=std::stod(value.substr(0,c)), b=std::stod(value.substr(c+1)); if(b>0) displayAspect=a/b; }
            }
            else if (key == "sample_aspect_ratio" && value != "N/A") {
                const size_t c=value.find(':'); if(c!=std::string::npos){ double a=std::stod(value.substr(0,c)), b=std::stod(value.substr(c+1)); if(b>0) sampleAspect=a/b; }
            }
            else if (key == "avg_frame_rate") ParseRate(value, avgRate);
            else if (key == "r_frame_rate") ParseRate(value, rawRate);
            else if (key == "duration" && value != "N/A") duration = std::stod(value);
            else if (key == "pix_fmt") m_pixFmt = value;
            else if (key == "TAG:smru_flow_range") m_flowRange = std::clamp(std::stod(value), 0.0, 65535.0);
        } catch (const std::exception& e) {
            LOG("ffprobe: ignoring unparsable " << key << "=" << value << " (" << e.what() << ")");
        }
    }

    if (!width || !height) {
        LOG("ffprobe returned no usable video dimensions.");
        return false;
    }

    m_nativeWidth = width;
    m_nativeHeight = height;
    m_width = width;
    m_height = height;
    m_stride = static_cast<int32_t>(m_width * BytesPerPixel());
    m_fps = avgRate > 0.0 ? avgRate : (rawRate > 0.0 ? rawRate : 30.0);
    m_durationSec = (std::isfinite(duration) && duration > 0.0) ? duration : 0.0;
    if (std::isfinite(displayAspect) && displayAspect > 0.1) m_displayAspect = displayAspect;
    else m_displayAspect = (double(m_width) * sampleAspect) / double(m_height);

    // Avoid pathological metadata causing gigantic pacing delays/CPU usage.
    m_fps = std::clamp(m_fps, 1.0, 240.0);

    LOG("ffprobe: " << m_width << "x" << m_height << " DAR=" << m_displayAspect << " @ " << m_fps
        << " fps, duration=" << m_durationSec);
    return true;
}

bool VideoDecoder::StartFFmpeg(double seekSeconds) {
    StopFFmpeg();
    seekSeconds = std::max(0.0, seekSeconds);

    std::wostringstream args;
    args << L"-hide_banner -loglevel error -nostdin -threads 0 ";
    if (seekSeconds > 0.0)
        args << L"-ss " << std::fixed << std::setprecision(6) << seekSeconds << L" ";
    args << L"-i " << smru::proc::Quote(m_path)
         << L" -map 0:v:0 -an -sn -dn ";
    if (m_nativeWidth && m_nativeHeight && (m_width != m_nativeWidth || m_height != m_nativeHeight))
        args << L"-vf scale=" << m_width << L":" << m_height << L":flags=bicubic ";
    args << (m_deep ? L"-pix_fmt rgba64le " : L"-pix_fmt bgra ") << L"-fps_mode cfr -r "
         << std::fixed << std::setprecision(6) << m_fps
         << L" -f rawvideo pipe:1";

    // A 4 MB pipe holds roughly a frame of 1080p BGRA, so ffmpeg keeps decoding
    // ahead while the player is busy with the frame it already has.
    smru::proc::Options opt;
    opt.out = smru::proc::Stdio::Pipe;
    opt.pipeBytes = 4 * 1024 * 1024;
    smru::proc::Child child;
    if (!smru::proc::Spawn(smru::proc::Quote(m_ffmpegExe) + L" " + args.str(), opt, child)) {
        LOG("CreateProcess(ffmpeg) failed winerr=" << GetLastError());
        return false;
    }

    m_ffmpegProcess = child.process;
    m_ffmpegStdout = child.stdOut;
    m_ffmpegFrameIndex = 0;
    m_ffmpegSeekBase100ns = static_cast<int64_t>(seekSeconds * 10000000.0);
    LOG("FFmpeg raw " << (m_deep ? "RGBA16" : "BGRA") << " decode process started.");
    return true;
}

void VideoDecoder::StopFFmpeg() {
    if (m_ffmpegStdout) {
        CloseHandle(m_ffmpegStdout);
        m_ffmpegStdout = nullptr;
    }
    if (m_ffmpegProcess) {
        DWORD code = 0;
        if (GetExitCodeProcess(m_ffmpegProcess, &code) && code == STILL_ACTIVE) {
            TerminateProcess(m_ffmpegProcess, 0);
            WaitForSingleObject(m_ffmpegProcess, 1000);
        }
        CloseHandle(m_ffmpegProcess);
        m_ffmpegProcess = nullptr;
    }
}

bool VideoDecoder::OpenFFmpeg(const std::wstring& path) {
    m_ffmpegExe = smru::paths::FfmpegExe().wstring();
    m_ffprobeExe = smru::paths::FfprobeExe().wstring();
    if (m_ffmpegExe.empty() || m_ffprobeExe.empty()) {
        LOG("Bundled/system FFmpeg tools not found. ffmpeg=" << (!m_ffmpegExe.empty())
            << " ffprobe=" << (!m_ffprobeExe.empty()));
        return false;
    }

    LOG("FFmpeg executable detected.");
    if (!ProbeFFmpeg(path)) return false;
    return StartFFmpeg(0.0);
}

bool VideoDecoder::ReadNextFFmpeg(VideoFrame& out) {
    if (!m_ffmpegStdout) return false;
    const size_t frameBytes = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * BytesPerPixel();
    if (!frameBytes) return false;

    out.bgra.resize(frameBytes);
    size_t total = 0;
    while (total < frameBytes) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(frameBytes - total, 4u << 20));
        DWORD got = 0;
        if (!ReadFile(m_ffmpegStdout, out.bgra.data() + total, want, &got, nullptr) || got == 0) {
            if (total != 0) LOG("FFmpeg ended in the middle of a raw video frame (" << total << "/" << frameBytes << ").");
            return false;
        }
        total += got;
    }

    out.timestamp100ns = m_ffmpegSeekBase100ns +
        static_cast<int64_t>((static_cast<double>(m_ffmpegFrameIndex) / m_fps) * 10000000.0);
    out.discontinuity = (m_ffmpegFrameIndex == 0 && m_ffmpegSeekBase100ns != 0);
    ++m_ffmpegFrameIndex;
    return true;
}

bool VideoDecoder::SetDecodeSize(uint32_t width, uint32_t height) {
    if (m_backend != Backend::FFmpeg || !m_nativeWidth || !m_nativeHeight) return false;
    width = std::max(2u, width & ~1u);
    height = std::max(2u, height & ~1u);
    width = std::min(width, m_nativeWidth & ~1u);
    height = std::min(height, m_nativeHeight & ~1u);
    if (!width || !height) return false;
    if (width == m_width && height == m_height) return true;

    const uint32_t oldW=m_width, oldH=m_height;
    m_width=width; m_height=height; m_stride=static_cast<int32_t>(m_width*4u);
    if (StartFFmpeg(0.0)) {
        LOG("FFmpeg realtime decode scale: " << m_nativeWidth << "x" << m_nativeHeight << " -> " << m_width << "x" << m_height);
        return true;
    }

    LOG("FFmpeg decode downscale failed; restoring native decode size.");
    m_width=oldW; m_height=oldH; m_stride=static_cast<int32_t>(m_width*4u);
    return StartFFmpeg(0.0);
}

bool VideoDecoder::OpenMediaFoundation(const std::wstring& path) {
    m_reader.Reset();

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 4))) return false;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &m_reader);
    if (FAILED(hr)) {
        LOG("MFCreateSourceReaderFromURL failed hr=0x" << std::hex << hr);
        return false;
    }

    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType.Get());
    if (FAILED(hr)) {
        // Some systems expose ARGB32 rather than RGB32 through the video processor.
        outType.Reset();
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType.Get());
    }
    if (FAILED(hr)) {
        LOG("SetCurrentMediaType(RGB32/ARGB32) failed hr=0x" << std::hex << hr);
        m_reader.Reset();
        return false;
    }

    ComPtr<IMFMediaType> current;
    if (FAILED(m_reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current))) return false;
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height);
    m_nativeWidth=m_width; m_nativeHeight=m_height;
    m_displayAspect = m_height ? double(m_width)/double(m_height) : 16.0/9.0;
    UINT32 frN = 0, frD = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &frN, &frD)) && frD)
        m_fps = double(frN) / double(frD);

    UINT32 strideU = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU)))
        m_stride = static_cast<int32_t>(strideU);
    else
        m_stride = static_cast<int32_t>(m_width * 4);

    PROPVARIANT var{};
    PropVariantInit(&var);
    if (SUCCEEDED(m_reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) {
        if (var.vt == VT_UI8 || var.vt == VT_I8)
            m_durationSec = static_cast<double>(var.vt == VT_I8 ? var.hVal.QuadPart : static_cast<LONGLONG>(var.uhVal.QuadPart)) / 10000000.0;
    }
    PropVariantClear(&var);

    LOG("Media Foundation: " << m_width << "x" << m_height << " @ " << m_fps << " fps, duration=" << m_durationSec);
    return m_width > 0 && m_height > 0;
}

bool VideoDecoder::ReadNextMediaFoundation(VideoFrame& out) {
    if (!m_reader) return false;

    for (;;) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                          &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            LOG("ReadSample failed hr=0x" << std::hex << hr);
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            LOG("Media Foundation video media type changed; continuing.");
            continue;
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) continue;

        const size_t dstStride = static_cast<size_t>(m_width) * 4u;
        out.bgra.resize(dstStride * m_height);

        int32_t stride = m_stride;
        size_t absStride = static_cast<size_t>(std::abs(stride));
        if (absStride * m_height > curLen) {
            stride = static_cast<int32_t>(dstStride);
            absStride = dstStride;
        }

        const BYTE* firstRow = data;
        if (stride < 0) firstRow = data + absStride * (m_height - 1);

        for (uint32_t y = 0; y < m_height; ++y) {
            const BYTE* src = stride >= 0 ? firstRow + absStride * y : firstRow - absStride * y;
            memcpy(out.bgra.data() + dstStride * y, src, std::min(dstStride, absStride));
        }
        buffer->Unlock();

        out.timestamp100ns = timestamp;
        out.discontinuity = (flags & MF_SOURCE_READERF_STREAMTICK) != 0;
        return true;
    }
}

bool VideoDecoder::ReadNext(VideoFrame& out) {
    if (m_backend == Backend::FFmpeg) return ReadNextFFmpeg(out);
    if (m_backend == Backend::MediaFoundation) return ReadNextMediaFoundation(out);
    return false;
}

bool VideoDecoder::SeekSeconds(double seconds) {
    seconds = std::clamp(seconds, 0.0, std::max(0.0, m_durationSec));
    if (m_backend == Backend::FFmpeg) return StartFFmpeg(seconds);
    if (m_backend != Backend::MediaFoundation || !m_reader) return false;

    PROPVARIANT pos{};
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10000000.0);
    HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    if (FAILED(hr)) {
        LOG("Media Foundation seek failed hr=0x" << std::hex << hr);
        return false;
    }
    return true;
}
