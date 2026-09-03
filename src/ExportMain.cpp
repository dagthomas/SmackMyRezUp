// SmackMyRezUpExport - headless DLSS / DLSS-NR video exporter (console).
//
// A focused, pipe-friendly replacement for `SmackMyRezUp.exe --export`:
//
//  * raw mode: BGRA frames in on stdin -> processed RGBA frames out on stdout.
//    No intermediate encodes, no temp files, exactly one output frame per input
//    frame (warm-up frames are duplicated internally and discarded).
//  * file mode: --input movie --export out.mp4 (decodes, processes, muxes the
//    original audio back), like the old path but with the corrected jitter
//    contract and a configurable encoder/crf.
//
// Jitter contract (the measured-dB feedback this tool exists to encode):
//  * Video frames were sampled at pixel centers; there is no camera jitter to
//    report, and resampling the decoded frame at jittered offsets adds no
//    information (bilinear taps of texels we already have). Default: --jitter zero.
//  * --jitter matched keeps the Halton resample but reports -j (DLSS wants the
//    negation of the renderer's sample offset). --jitter legacy reproduces the
//    old wrong-sign behavior for A/B comparison only.
//
// Engine: NeuralEngine (this project's own NGX host) drives feature 18 straight
// through the driver (feature created on the first frame; no arming wait).

#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <iomanip>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <memory>

#include "VideoDecoder.h"
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "CubeLUT.h"
#include "LabelStamp.h"
#include "Log.h"
#include "AppIdentity.h"
#include "AppPaths.h"
#include "TextEncoding.h"
#include "ChildProcess.h"
#include "FramePipeline.h"

namespace {

struct Options {
    bool rawMode = false;
    uint32_t inW = 0, inH = 0;        // raw mode input size
    uint32_t outW = 0, outH = 0;      // 0 -> same as input
    double fps = 24.0;
    uint64_t maxFrames = 0;           // 0 -> until EOF
    int warmup = 2;                   // duplicated first-frame renders (discarded)
    D3D12Renderer::JitterMode jitter = D3D12Renderer::JitterMode::Zero;
    // Tone handling of the NR output. preserve (default): keep only the neural
    // pass's high-frequency detail and restore the ORIGINAL's tone and colors -
    // the raw NR relight lifts gamma (faces especially) and drifts colors.
    // nr: raw neural output untouched.
    bool tonePreserve = true;
    float toneMix = 0.7f;             // 0 = raw NR .. 1 = fully preserved; 0.7 keeps
                                      // some NR relight character visible while
                                      // removing most of the gamma/color drift
    float nrSmooth = 0.0f;            // --nr-smooth: temporal EMA on the NR delta
                                      // (out - original). Helps boiling NR detail on
                                      // STATIC shots; on moving faces the un-warped
                                      // history trails the motion (measured slightly
                                      // worse), so it is opt-in. 0 = off (default).
    // Guide ablation / quality switches.
    // Motion vectors fed to DLSS: 0 zero (default - perceptually cleanest on real
    // footage; per-block flow errors read as swimming/flicker in the history),
    // 1 global (one robust median pan vector per frame), 2 estimated (full field).
    int mvMode = 0;
    uint32_t nrPreset = 0;            // --nr-preset N: DLSSNR.Hint.Render.Preset at
                                      // feature creation (other DLSS5 hosts default
                                      // to 3; this project's historical value is 0)
    uint32_t nrMaskMode = 0;          // --nr-mask-mode bias|white|inv: what the
                                      // ControlMask carries when bound (measured
                                      // semantics: "process where set")
    float nrMVScale = -1.0f;          // --nr-mvscale S: DLSSNR.MVecScaleX/Y. The
                                      // runtime reads MVec with the OPPOSITE sign
                                      // of our current->previous field (measured),
                                      // so -1 is the correct default.
    uint32_t nrGuides = 1;            // --nr-guides bitmask words: off | on(=mv) |
                                      // mv | depth | mask | all, comma-separable.
                                      // Binding ControlMask suppresses the neural
                                      // effect (zero mask = leave everything), so
                                      // the default binds MVec only.
    bool temporalOff = false;         // --temporal off: reset DLSS history every frame
    bool cutReset = true;             // --cut-reset on|off: when the guide generator
                                      // loses correspondence to the previous frame (a
                                      // scene cut), also reset the DLSS history and the
                                      // NR-delta smoother instead of smearing the old
                                      // shot into the new one
    int cutWarmup = 2;                // --cut-warmup N: extra discarded renders of the
                                      // first post-cut frame so the fresh history is
                                      // converged before the kept render (0 = the reset
                                      // trades ghosting for 1-2 visibly softer frames)
    int bits = 8;                     // --bits 8|16 (raw mode): 16 = frames arrive as
                                      // tightly packed RGBA16LE (W*H*8 bytes), reaching
                                      // DLSS without the 8-bit quantize that bands
                                      // float sources. Output stays RGBA8.
    float postSharpen = 0.0f;         // --post-sharpen 0..1: clamped unsharp on the
                                      // NEURAL OUTPUT in the present pass - the NR
                                      // models denoise while redrawing, and pre-sharpen
                                      // cannot restore acutance lost inside the pass
    bool lanczosScale = false;        // --scaler lanczos (file mode with --output-size):
                                      // CPU Lanczos3 upscale first, then a DLAA detail
                                      // pass at full output res - instead of DLSS SR
                                      // doing the scaling (which can read as soft)
    bool serveMode = false;           // --serve: persistent stdin/stdout batch server
                                      // (see RunServe) so hosts pay the NR arming wait
                                      // once per process instead of once per batch
    bool flowFine = true;             // --flow fine|fast (fine default for export)
    std::wstring lutPath;             // --lut file.cube (applied before DLSS/NR)
    float lutStrength = 1.0f;         // --lut-strength 0..1
    float sharpen = 0.0f;             // --sharpen 0..1: pre-DLSS unsharp mask
    bool depthIn = false;             // --depth-in (raw mode): each frame is followed by
                                      // a W*H*2 R16 depth plane (0 = near .. 1 = far)
    std::wstring depthVideo;          // --depth-video (file mode): grayscale depth movie
                                      // decoded alongside (bright = near, e.g. Depth Anything)
    std::wstring flowVideo;           // --flow-video (file mode): RAFT flow movie
    std::wstring maskVideo;           // --mask-video (file mode): segmentation mask movie
                                      // (white = process here) driving DLSSNR.ControlMask
                                      // (R=dx, G=dy in [-24..24] src px around 127.5)
    // Direct-engine NR knobs (set on the NGX parameter block per evaluation).
    uint32_t nrStyle = 0;             // --nr-style 0|1|2 (model A/B/C)
    float nrIntensity = 1.0f;         // --nr-intensity 0..1 wet/dry
    float nrStructure = 1.0f;         // --nr-structure 0..2 (needs automask on)
    float nrSkin = -1.0f;             // --nr-skin -1(=follow structure)|0..2
    bool nrAutoMask = true;           // --nr-automask on|off
    bool dlssOff = false;             // --dlss off: skip the neural pass entirely (the
                                      // player's DLSS OFF state) - resize + effects only
    D3D12Renderer::ColorSettings color; // --brightness/--contrast/--saturation/--gamma/
                                      // --temperature/--tint: the player's Color window
    std::wstring input;               // file mode
    std::wstring exportPath;          // file mode
    bool compare = false;             // file mode: stitch input | output side by side
    bool split = false;               // file mode: one frame, left half original, right half processed
    int crf = 16;
    int codec = 0;                    // --codec x264 (default) | hevc (hevc_nvenc
                                      // hardware encode - the sane choice for 4K/8K,
                                      // x264 crawls there) | av1 (av1_nvenc)
    bool showHelp = false;
    bool parseError = false;
    std::wstring parseErrorText;
};

void PrintUsage() {
    fwprintf(stderr,
L"SmackMyRezUpExport - headless DLSS / DLSS-NR video exporter\n"
L"\n"
L"Raw pipe mode (for hosts like ComfyUI):\n"
L"  SmackMyRezUpExport --raw --size WxH --fps N [options] < bgra_frames > rgba_frames\n"
L"    stdin : tightly packed BGRA8 frames, W*H*4 bytes each\n"
L"    stdout: tightly packed RGBA8 frames, outW*outH*4 bytes each\n"
L"    Exactly one output frame is written per input frame.\n"
L"\n"
L"File mode:\n"
L"  SmackMyRezUpExport --input movie.mp4 --export out.mp4 [options]\n"
L"    Original audio is muxed back; needs ffmpeg.exe beside this exe.\n"
L"\n"
L"Options:\n"
L"  --size WxH         raw input size (required in raw mode)\n"
L"  --output-size WxH  output size (default: same as input -> DLAA detail pass)\n"
L"  --fps N            frame rate (raw default 24; file mode reads the movie)\n"
L"  --frames N         stop after N frames (raw mode; default: until EOF)\n"
L"  --warmup N         discarded first-frame warmup renders (default 2)\n"
L"  --nr-style N       0|1|2: NR model A/B/C (default 0)\n"
L"  --nr-intensity X   0..1 wet/dry blend of the neural pass (default 1)\n"
L"  --nr-structure X   0..2 detail strength, consulted with automask on (default 1)\n"
L"  --nr-skin X        -1 = follow --nr-structure (default) | 0..2 skin strength\n"
L"  --nr-automask M    on (default) | off: engine-derived protection mask\n"
L"  --jitter MODE      zero (default) | matched | legacy\n"
L"  --quality Q        accepted, no effect: the neural pass runs 1:1 and has no mode\n"
L"  --crf N            encode quality for file mode (default 16; NVENC uses it as -cq)\n"
L"  --codec C          x264 (default) | hevc (NVENC hw) | av1 (NVENC hw)\n"
L"  --flow MODE        fine (default; dense subpixel flow) | fast (realtime grid)\n"
L"  --mv MODE          zero (default) | global (one pan vector/frame) | estimated\n"
L"  --nr-guides M      off | on (= mv, default) | all | mv,depth,mask combos\n"
L"  --nr-mask-mode M   bias (default) | white | inv: ControlMask content when bound\n"
L"  --nr-mvscale S     MVec scale reported to the runtime (default -1, measured sign)\n"
L"  --temporal MODE    on (default) | off (reset DLSS history every frame)\n"
L"  --cut-reset MODE   on (default) | off: reset the DLSS history on detected\n"
L"                     scene cuts (correspondence loss, robust to fast pans)\n"
L"  --cut-warmup N     extra discarded renders of the first post-cut frame so\n"
L"                     the fresh history converges before the kept render\n"
L"                     (default 2; 0 = accept 1-2 softer frames after each cut)\n"
L"  --bits N           8 (default) | 16: raw-mode input depth. 16 = RGBA16LE\n"
L"                     frames (W*H*8 bytes) - float sources reach DLSS without\n"
L"                     an 8-bit quantize. Output frames stay RGBA8.\n"
L"  --post-sharpen X   0..1 clamped unsharp on the NEURAL OUTPUT (the NR models\n"
L"                     denoise while redrawing; try 0.3-0.6 for a blurry result)\n"
L"  --scaler MODE      dlss (default) | lanczos: with --output-size in file\n"
L"                     mode, lanczos upscales on the CPU first and runs a DLAA\n"
L"                     detail pass at full res instead of DLSS SR scaling\n"
L"  --serve            persistent batch server on stdin/stdout (for hosts like\n"
L"                     ComfyUI): pays the NR arming wait once per process\n"
L"  --dlss on|off      off = skip the neural pass (the player's DLSS OFF look):\n"
L"                     resize + effects only (default on)\n"
L"  --brightness X     colour adjustments, applied exactly as the player's Color\n"
L"  --contrast X       window: brightness -2..2 stops (0), contrast 0..3 (1),\n"
L"  --saturation X     saturation 0..3 (1), gamma 0.25..3 (1),\n"
L"  --gamma X          temperature -1..1 (0), tint -1..1 (0)\n"
L"  --temperature X\n"
L"  --tint X\n"
L"  --lut FILE.cube    creative 3D LUT applied before the DLSS/NR pass\n"
L"  --lut-strength X   0..1 blend between original and graded (default 1)\n"
L"  --compare          file mode: export original | processed side by side\n"
L"  --split            file mode: single frame split down the middle\n"
L"                     (left half original, right half processed)\n"
L"  --tone MODE        preserve (default: NR detail on the ORIGINAL tone/colors,\n"
L"                     the player's tone-preserve pass)\n"
L"                     | nr (raw neural output, with its gamma/color shifts)\n"
L"  --tone-mix X       0..1 blend raw NR -> preserved (default 0.7)\n"
L"  --sharpen X        0..1 pre-DLSS unsharp mask so the neural pass sees more\n"
L"                     micro-contrast (default 0 = off; try 0.3-0.6)\n"
L"  --nr-smooth X      0..1 temporal smoothing of the NR contribution (default 0;\n"
L"                     the player's NR Smooth pass; helps boiling detail on\n"
L"                     static shots, can trail on motion)\n"
L"  --flow-video F     file mode: RAFT flow movie (tools\\make_flow_video.py);\n"
L"                     replaces the block-matcher motion vectors entirely\n"
L"  --mask-video F     file mode: segmentation mask movie (tools\\make_mask_video.py):\n"
L"                     white = process here. Feeds DLSSNR.ControlMask; pair it\n"
L"                     with --nr-guides mask (or all) to bind it\n"
L"\n"
L"4K / upscaling: pass --output-size (e.g. 3840x2194). The frame is resized to\n"
L"that size and the neural pass redraws every output pixel 1:1.\n");
}

Options ParseArgs() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { o.parseError = true; o.parseErrorText = L"CommandLineToArgvW failed"; return o; }
    auto fail = [&](const std::wstring& msg) { o.parseError = true; o.parseErrorText = msg; };
    auto parseSize = [&](const std::wstring& v, uint32_t& w, uint32_t& h) -> bool {
        auto x = v.find(L'x'); if (x == std::wstring::npos) x = v.find(L'X');
        if (x == std::wstring::npos) return false;
        const int pw = _wtoi(v.substr(0, x).c_str()), ph = _wtoi(v.substr(x + 1).c_str());
        if (pw < 16 || ph < 16 || pw > 16384 || ph > 16384) return false;
        w = uint32_t(pw); h = uint32_t(ph); return true;
    };
    for (int i = 1; i < argc && !o.parseError; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) { fail(a + L" needs a value"); return L""; }
            return argv[++i];
        };
        // Almost every flag below is one of four shapes: a lower-cased word, a
        // number that has to land in a range, an on/off switch, or a number that
        // is simply clamped into one. next() has already reported a MISSING
        // value by the time these run, so they only report a wrong one - and
        // they report it against `a`, the flag actually on the command line.
        auto badValue = [&](const wchar_t* usage) {
            if (!o.parseError) fail(L"bad " + a + L" (" + usage + L")");
        };
        auto lowerNext = [&] {
            std::wstring v = next();
            std::transform(v.begin(), v.end(), v.begin(), ::towlower);
            return v;
        };
        auto inRange = [&](float& dst, double lo, double hi, const wchar_t* usage) {
            const double v = _wtof(next().c_str());
            if (v >= lo && v <= hi) dst = float(v); else badValue(usage);
        };
        auto unit01 = [&](float& dst) { inRange(dst, 0.0, 1.0, L"0..1"); };
        auto onOff = [&](bool& dst) {
            const std::wstring v = lowerNext();
            if (v == L"on") dst = true;
            else if (v == L"off") dst = false;
            else badValue(L"on|off");
        };
        auto clampFloat = [&](float& dst, float lo, float hi) {
            dst = std::clamp(float(_wtof(next().c_str())), lo, hi);
        };
        auto clampInt = [&](int& dst, int lo, int hi) {
            dst = std::clamp(_wtoi(next().c_str()), lo, hi);
        };

        if (a == L"--raw") o.rawMode = true;
        else if (a == L"--serve") o.serveMode = true;
        else if (a == L"--compare") o.compare = true;
        else if (a == L"--split") o.split = true;
        else if (a == L"--depth-in") o.depthIn = true;
        else if (a == L"--input") o.input = next();
        else if (a == L"--export") o.exportPath = next();
        else if (a == L"--lut") o.lutPath = next();
        else if (a == L"--depth-video") { o.depthVideo = next(); o.depthIn = true; }
        else if (a == L"--flow-video") o.flowVideo = next();
        else if (a == L"--mask-video") o.maskVideo = next();
        else if (a == L"--size") { if (!parseSize(next(), o.inW, o.inH)) badValue(L"expected WxH"); }
        else if (a == L"--output-size") { if (!parseSize(next(), o.outW, o.outH)) badValue(L"expected WxH"); }
        else if (a == L"--fps") { const double f = _wtof(next().c_str()); if (f >= 1.0 && f <= 480.0) o.fps = f; else badValue(L"1..480"); }
        else if (a == L"--frames") o.maxFrames = uint64_t(_wtoi64(next().c_str()));
        else if (a == L"--warmup") clampInt(o.warmup, 0, 16);
        else if (a == L"--crf") clampInt(o.crf, 0, 51);
        else if (a == L"--cut-warmup") clampInt(o.cutWarmup, 0, 8);
        else if (a == L"--nr-preset") { int p = 0; clampInt(p, 0, 15); o.nrPreset = uint32_t(p); }
        else if (a == L"--nr-mvscale") clampFloat(o.nrMVScale, -8.0f, 8.0f);
        else if (a == L"--brightness") clampFloat(o.color.brightness, -2.0f, 2.0f);
        else if (a == L"--contrast") clampFloat(o.color.contrast, 0.0f, 3.0f);
        else if (a == L"--saturation") clampFloat(o.color.saturation, 0.0f, 3.0f);
        else if (a == L"--gamma") clampFloat(o.color.gamma, 0.25f, 3.0f);
        else if (a == L"--temperature") clampFloat(o.color.temperature, -1.0f, 1.0f);
        else if (a == L"--tint") clampFloat(o.color.tint, -1.0f, 1.0f);
        else if (a == L"--nr-intensity") unit01(o.nrIntensity);
        else if (a == L"--tone-mix") unit01(o.toneMix);
        else if (a == L"--nr-smooth") unit01(o.nrSmooth);
        else if (a == L"--sharpen") unit01(o.sharpen);
        else if (a == L"--post-sharpen") unit01(o.postSharpen);
        else if (a == L"--lut-strength") unit01(o.lutStrength);
        else if (a == L"--nr-structure") inRange(o.nrStructure, 0.0, 2.0, L"0..2");
        // -1 is the "follow the local structure strength" sentinel, not a value
        // on the scale, so this range has a hole in it.
        else if (a == L"--nr-skin") { const double v = _wtof(next().c_str()); if (v == -1.0 || (v >= 0.0 && v <= 2.0)) o.nrSkin = float(v); else badValue(L"-1|0..2"); }
        else if (a == L"--nr-style") { const int v = _wtoi(next().c_str()); if (v >= 0 && v <= 2) o.nrStyle = uint32_t(v); else badValue(L"0|1|2"); }
        else if (a == L"--bits") { const int b = _wtoi(next().c_str()); if (b == 8 || b == 16) o.bits = b; else badValue(L"8|16"); }
        else if (a == L"--nr-automask") onOff(o.nrAutoMask);
        else if (a == L"--cut-reset") onOff(o.cutReset);
        else if (a == L"--temporal") { bool on = !o.temporalOff; onOff(on); o.temporalOff = !on; }
        else if (a == L"--dlss") { bool on = !o.dlssOff; onOff(on); o.dlssOff = !on; }
        else if (a == L"--codec") {
            const std::wstring v = lowerNext();
            if (v == L"x264" || v == L"h264") o.codec = 0;
            else if (v == L"hevc" || v == L"h265") o.codec = 1;
            else if (v == L"av1") o.codec = 2;
            else badValue(L"x264|hevc|av1");
        }
        else if (a == L"--jitter") {
            const std::wstring v = lowerNext();
            if (v == L"zero") o.jitter = D3D12Renderer::JitterMode::Zero;
            else if (v == L"matched") o.jitter = D3D12Renderer::JitterMode::Matched;
            else if (v == L"legacy") o.jitter = D3D12Renderer::JitterMode::Legacy;
            else badValue(L"zero|matched|legacy");
        }
        else if (a == L"--flow") {
            const std::wstring v = lowerNext();
            if (v == L"fine") o.flowFine = true;
            else if (v == L"fast") o.flowFine = false;
            else badValue(L"fine|fast");
        }
        else if (a == L"--mv") {
            const std::wstring v = lowerNext();
            if (v == L"zero") o.mvMode = 0;
            else if (v == L"global") o.mvMode = 1;
            else if (v == L"estimated") o.mvMode = 2;
            else badValue(L"zero|global|estimated");
        }
        else if (a == L"--tone") {
            const std::wstring v = lowerNext();
            if (v == L"preserve") o.tonePreserve = true;
            else if (v == L"nr") o.tonePreserve = false;
            else badValue(L"preserve|nr");
        }
        else if (a == L"--scaler") {
            const std::wstring v = lowerNext();
            if (v == L"dlss") o.lanczosScale = false;
            else if (v == L"lanczos") o.lanczosScale = true;
            else badValue(L"dlss|lanczos");
        }
        else if (a == L"--nr-mask-mode") {
            const std::wstring v = lowerNext();
            if (v == L"bias") o.nrMaskMode = 0;
            else if (v == L"white") o.nrMaskMode = 1;
            else if (v == L"inv") o.nrMaskMode = 2;
            else badValue(L"bias|white|inv");
        }
        else if (a == L"--nr-guides") {
            const std::wstring v = lowerNext();
            if (v == L"off") o.nrGuides = 0;
            else if (v == L"on") o.nrGuides = 1;
            else if (v == L"all") o.nrGuides = 7;
            else {
                uint32_t m = 0; bool ok = !v.empty();
                size_t pos = 0;
                while (ok && pos <= v.size()) {
                    const size_t comma = v.find(L',', pos);
                    const std::wstring word = v.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
                    if (word == L"mv") m |= 1u;
                    else if (word == L"depth") m |= 2u;
                    else if (word == L"mask") m |= 4u;
                    else ok = false;
                    if (comma == std::wstring::npos) break;
                    pos = comma + 1;
                }
                if (ok) o.nrGuides = m; else badValue(L"off|on|all|mv,depth,mask");
            }
        }
        else if (a == L"--quality") {
            // Consumed and ignored. The neural pass runs 1:1 at output resolution
            // and takes no mode, so this only ever chose a value the renderer
            // discarded; the flag stays accepted so existing hosts keep working.
            next();
            fprintf(stderr, SMRU_LOG_TAG " note: --quality has no effect (the neural pass runs 1:1)\n");
        }
        else if (a == L"--help" || a == L"-h" || a == L"/?") o.showHelp = true;
        else fail(L"unknown option: " + a);
    }
    LocalFree(argv);
    return o;
}

const char* JitterName(D3D12Renderer::JitterMode m) {
    switch (m) {
    case D3D12Renderer::JitterMode::Matched: return "matched";
    case D3D12Renderer::JitterMode::Legacy: return "legacy";
    default: return "zero";
    }
}


// Bilinear resize + BGRA->RGBA swizzle of a decoded frame; the reference image
// for tone preservation and the left half of --compare.
void BuildRefRGBA(const uint8_t* bgra, uint32_t sw, uint32_t sh,
                  uint8_t* dst, uint32_t dw, uint32_t dh) {
    if (sw == dw && sh == dh) {
        const size_t n = size_t(sw) * sh;
        for (size_t i = 0; i < n; ++i) {
            dst[i*4+0] = bgra[i*4+2]; dst[i*4+1] = bgra[i*4+1];
            dst[i*4+2] = bgra[i*4+0]; dst[i*4+3] = 255;
        }
        return;
    }
    const float sx = float(sw) / dw, sy = float(sh) / dh;
    for (uint32_t y = 0; y < dh; ++y) {
        const float fy = std::min((y + 0.5f) * sy - 0.5f, float(sh - 1));
        const uint32_t y0 = uint32_t(std::max(0.0f, std::floor(fy)));
        const uint32_t y1 = std::min(y0 + 1, sh - 1);
        const float ty = std::clamp(fy - float(y0), 0.0f, 1.0f);
        uint8_t* row = dst + size_t(y) * dw * 4;
        for (uint32_t x = 0; x < dw; ++x) {
            const float fx = std::min((x + 0.5f) * sx - 0.5f, float(sw - 1));
            const uint32_t x0 = uint32_t(std::max(0.0f, std::floor(fx)));
            const uint32_t x1 = std::min(x0 + 1, sw - 1);
            const float tx = std::clamp(fx - float(x0), 0.0f, 1.0f);
            const uint8_t* p00 = bgra + (size_t(y0)*sw + x0)*4;
            const uint8_t* p01 = bgra + (size_t(y0)*sw + x1)*4;
            const uint8_t* p10 = bgra + (size_t(y1)*sw + x0)*4;
            const uint8_t* p11 = bgra + (size_t(y1)*sw + x1)*4;
            for (int c = 0; c < 3; ++c) {
                const int sc = 2 - c; // BGRA source channel for RGBA dst channel c
                const float a = p00[sc]*(1-tx) + p01[sc]*tx;
                const float b = p10[sc]*(1-tx) + p11[sc]*tx;
                row[x*4+c] = uint8_t(std::clamp(a*(1-ty) + b*ty + 0.5f, 0.0f, 255.0f));
            }
            row[x*4+3] = 255;
        }
    }
}

// Glyph5x7 / StampLabel now live in LabelStamp.h (shared with the player).

// Separable Lanczos3 upscale of a BGRA8 image (--scaler lanczos): a crisp
// spatial scaler feeds a DLAA detail pass at full output resolution, instead of
// DLSS SR doing the scaling (which can read as soft on video sources).
void LanczosResize(const uint8_t* src, uint32_t sw, uint32_t sh,
                   uint8_t* dst, uint32_t dw, uint32_t dh) {
    auto kern = [](float x) {
        x = std::fabs(x);
        if (x < 1e-6f) return 1.0f;
        if (x >= 3.0f) return 0.0f;
        const float px = 3.14159265f * x;
        return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
    };
    struct Taps { int start = 0; std::vector<float> w; };
    auto buildTaps = [&](uint32_t sN, uint32_t dN) {
        std::vector<Taps> taps(dN);
        const float scale = float(sN) / float(dN);
        const float support = 3.0f * std::max(scale, 1.0f);
        for (uint32_t o = 0; o < dN; ++o) {
            const float center = (o + 0.5f) * scale - 0.5f;
            const int lo = std::max(int(std::floor(center - support)), 0);
            const int hi = std::min(int(std::ceil(center + support)), int(sN) - 1);
            Taps t; t.start = lo; t.w.resize(size_t(hi - lo + 1));
            float sum = 0.0f;
            for (int k = lo; k <= hi; ++k) {
                const float v = kern((float(k) - center) / std::max(scale, 1.0f));
                t.w[size_t(k - lo)] = v; sum += v;
            }
            if (sum > 0.0f) for (auto& v : t.w) v /= sum;
            taps[o] = std::move(t);
        }
        return taps;
    };
    const auto tx = buildTaps(sw, dw), ty = buildTaps(sh, dh);
    static thread_local std::vector<float> mid;
    mid.resize(size_t(dw) * sh * 4);
    for (uint32_t y = 0; y < sh; ++y) {
        const uint8_t* row = src + size_t(y) * sw * 4;
        float* mrow = mid.data() + size_t(y) * dw * 4;
        for (uint32_t x = 0; x < dw; ++x) {
            const Taps& t = tx[x];
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.w.size(); ++k) {
                const uint8_t* p = row + (size_t(t.start) + k) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += t.w[k] * float(p[c]);
            }
            for (int c = 0; c < 4; ++c) mrow[x * 4 + c] = acc[c];
        }
    }
    for (uint32_t y = 0; y < dh; ++y) {
        const Taps& t = ty[y];
        uint8_t* drow = dst + size_t(y) * dw * 4;
        for (uint32_t x = 0; x < dw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.w.size(); ++k) {
                const float* m = mid.data() + ((size_t(t.start) + k) * dw + x) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += t.w[k] * m[c];
            }
            for (int c = 0; c < 4; ++c)
                drow[x * 4 + c] = uint8_t(std::clamp(acc[c] + 0.5f, 0.0f, 255.0f));
        }
    }
}

HWND CreateHiddenWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = smru::kWndClassExportHidden;
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, smru::kExporterNameW, WS_POPUP,
                           0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

bool ReadExact(HANDLE h, uint8_t* dst, size_t bytes, bool& eof) {
    eof = false;
    size_t got = 0;
    while (got < bytes) {
        DWORD chunk = 0;
        const DWORD want = DWORD(std::min<size_t>(bytes - got, 1u << 20));
        if (!ReadFile(h, dst + got, want, &chunk, nullptr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) { eof = (got == 0); return false; }
            fprintf(stderr, SMRU_LOG_TAG " stdin read failed winerr=%lu\n", err);
            return false;
        }
        if (chunk == 0) { eof = (got == 0); return false; }
        got += chunk;
    }
    return true;
}

bool WriteExact(HANDLE h, const uint8_t* src, size_t bytes) {
    size_t put = 0;
    while (put < bytes) {
        DWORD chunk = 0;
        const DWORD want = DWORD(std::min<size_t>(bytes - put, 1u << 20));
        if (!WriteFile(h, src + put, want, &chunk, nullptr) || chunk == 0) {
            fprintf(stderr, SMRU_LOG_TAG " stdout write failed winerr=%lu\n", GetLastError());
            return false;
        }
        put += chunk;
    }
    return true;
}

// --mv on the command line as the pipeline's motion mode.
FramePipeline::MotionMode MotionFor(int mvMode) {
    return mvMode == 0 ? FramePipeline::MotionMode::Zero
         : mvMode == 1 ? FramePipeline::MotionMode::GlobalPan
                       : FramePipeline::MotionMode::Estimated;
}

struct Pipeline {
    HWND hwnd = nullptr;
    D3D12Renderer renderer;
    FramePipeline frames{renderer};
    uint64_t frameIndex = 0;   // index of the frame being rendered, for diagnostics

    ~Pipeline() { if (hwnd) DestroyWindow(hwnd); }

    bool Init(Options& o, uint32_t srcWi, uint32_t srcHi, double fpsIn) {
        frames.SetPolicy({
            .srcW = srcWi, .srcH = srcHi, .fps = fpsIn,
            .motion = MotionFor(o.mvMode),
            .temporalOff = o.temporalOff, .cutReset = o.cutReset,
            .cutWarmup = o.cutWarmup, .bits = o.bits,
        });
        const uint32_t srcW = srcWi, srcH = srcHi;
        const double fps = fpsIn;
        if (o.bits == 16) renderer.EnableDeepInput();
        if (o.depthIn) renderer.EnableExternalDepth();
        if (!o.flowVideo.empty()) renderer.EnableExternalFlow();
        if (!o.maskVideo.empty()) renderer.EnableExternalMask();
        const uint32_t outW = o.outW ? o.outW : srcW;
        const uint32_t outH = o.outH ? o.outH : srcH;
        hwnd = CreateHiddenWindow();
        if (!hwnd) { fprintf(stderr, SMRU_LOG_TAG " hidden window failed\n"); return false; }
        frames.Guides().SetQuality(o.flowFine ? TemporalGuideGenerator::Quality::Fine
                                              : TemporalGuideGenerator::Quality::Realtime);
        if (!o.lutPath.empty()) {
            // A LUT that cannot be loaded is a warning, not a failed export: the
            // player treats it the same way in the preview, and an export that
            // dies on a moved .cube after the user waited for the dialog is
            // worse than one delivered without the grade. The warning is on
            // stderr, which the player shows in its failure/finished boxes.
            std::vector<float> lut; uint32_t lutSize = 0; float dmin[3], dmax[3];
            if (!LoadCubeLUT(o.lutPath, lut, lutSize, dmin, dmax)) {
                fprintf(stderr, SMRU_LOG_TAG " WARNING: LUT could not be loaded, exporting without it: %ls\n",
                        o.lutPath.c_str());
            } else if (!renderer.SetLUT(std::move(lut), lutSize, dmin, dmax)) {
                fprintf(stderr, SMRU_LOG_TAG " WARNING: LUT rejected by the renderer (%ux%ux%u), exporting without it: %ls\n",
                        lutSize, lutSize, lutSize, o.lutPath.c_str());
            } else {
                renderer.SetLUTStrength(o.lutStrength);
                fprintf(stderr, SMRU_LOG_TAG " LUT: %ls (%ux%ux%u, strength %.2f) applied before DLSS/NR\n",
                        o.lutPath.c_str(), lutSize, lutSize, lutSize, o.lutStrength);
            }
        }
        // Direct NR runs 1:1 at output resolution - an upscale is a bilinear (or
        // --scaler lanczos) resize first, then the neural pass over every output pixel.
        renderer.SetNRSettings({.style = o.nrStyle, .intensity = o.nrIntensity,
                                .localStructure = o.nrStructure, .skinStructure = o.nrSkin,
                                .autoMask = o.nrAutoMask});
        // The player's Color window and its DLSS toggle reach the exporter too,
        // so an export is what the preview showed - including "DLSS OFF".
        renderer.SetColorSettings(o.color);
        if (o.dlssOff) { renderer.SetDLSS(false); fprintf(stderr, SMRU_LOG_TAG " --dlss off: neural pass skipped, resize + effects only\n"); }
        const auto grid = TemporalGuideGenerator::AnalysisGrid(srcW, srcH, fps, o.flowFine);
        if (!renderer.Initialize(hwnd, srcW, srcH, outW, outH, grid.first, grid.second)) {
            fprintf(stderr, SMRU_LOG_TAG " renderer init failed (see %s)\n", smru::kExporterLogFile);
            return false;
        }
        renderer.SetDLSS(!o.dlssOff);
        renderer.SetJitterMode(o.jitter);
        renderer.SetNRGuideMask(o.nrGuides);
        renderer.SetNRMaskMode(o.nrMaskMode);
        renderer.SetNRMVScale(o.nrMVScale);
        // --mv zero must zero the FINAL MV field even when --flow-video is
        // attached (the ext-flow GPU path bypasses the CPU grid override).
        renderer.SetMVFieldScale(o.mvMode==0?0.0f:1.0f);
        renderer.SetNRPreset(o.nrPreset);
        renderer.EnableExport(true);
        renderer.SetPreSharpen(o.sharpen);
        renderer.SetPostSharpen(o.postSharpen);
        renderer.SetNRSmooth(o.nrSmooth);
        // Tone preserve is the renderer's pass (the one the preview shows): it
        // recombines before the colour adjustments, so an export is the preview.
        renderer.SetToneMix(o.tonePreserve ? o.toneMix : 0.0f);
        fprintf(stderr, SMRU_LOG_TAG " engine=direct style=%u intensity=%.2f structure=%.2f skin=%.2f automask=%s\n",
                o.nrStyle, o.nrIntensity, o.nrStructure, o.nrSkin, o.nrAutoMask ? "on" : "off");
        fprintf(stderr, SMRU_LOG_TAG " init %ux%u -> %ux%u @ %.3f fps, jitter=%s, dlss_input=%ux%u, guides=%ux%u\n",
                srcW, srcH, renderer.OutputW(), renderer.OutputH(), fps,
                JitterName(o.jitter), renderer.DLSSInputW(), renderer.DLSSInputH(), grid.first, grid.second);
        fprintf(stderr, SMRU_LOG_TAG " guides: nr-bind=%u flow=%s mv=%s temporal=%s cut-reset=%s(warmup %d) tone=%s(mix %.2f) sharpen=%.2f post-sharpen=%.2f bits=%d\n",
                o.nrGuides,
                o.flowFine ? "fine" : "fast",
                o.mvMode == 0 ? "zero" : (o.mvMode == 1 ? "global" : "estimated"),
                o.temporalOff ? "off" : "on", o.cutReset ? "on" : "off", o.cutWarmup,
                o.tonePreserve ? "preserve" : "nr", o.toneMix, o.sharpen, o.postSharpen, o.bits);
        return true;
    }

    // Renders one frame through guides + DLSS; the result (if kept) is in
    // renderer.ExportRGBA(). In 16-bit mode `frame` is tightly packed RGBA16LE.
    bool Render(const uint8_t* frame, size_t bytes, bool reset,
                const uint8_t* extDepth = nullptr, size_t extDepthBytes = 0,
                const uint8_t* extFlow = nullptr, size_t extFlowBytes = 0,
                const uint8_t* extMask = nullptr, size_t extMaskBytes = 0) {
        const uint64_t cutsBefore = frames.CutCount();
        const bool ok = frames.Render(frame, bytes, reset,
                                      {extDepth, extDepthBytes, extFlow, extFlowBytes,
                                       extMask, extMaskBytes});
        // Hosts read the exporter's stderr; a cut is worth reporting there and
        // not only in the log file.
        if (frames.CutCount() != cutsBefore)
            fprintf(stderr, SMRU_LOG_TAG " scene cut at frame %llu -> DLSS history reset\n",
                    (unsigned long long)frameIndex);
        PumpMessages();
        return ok;
    }

    void LogStats(uint64_t frameCount) const {
        fprintf(stderr, SMRU_LOG_TAG " done frames=%llu engine=direct feature=%d evals=%llu cuts=%llu\n",
                (unsigned long long)frameCount, renderer.DLSSFeatureCreated() ? 1 : 0,
                (unsigned long long)renderer.DLSSEvaluations(), (unsigned long long)frames.CutCount());
    }
};

int RunRaw(Options o) {
    if (!o.inW || !o.inH) { fprintf(stderr, SMRU_LOG_TAG " raw mode needs --size WxH\n"); return 2; }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

    Pipeline p;
    if (!p.Init(o, o.inW, o.inH, o.fps)) return 5;

    const size_t frameBytes = size_t(o.inW) * o.inH * (o.bits == 16 ? 8 : 4);
    const size_t depthBytes = o.depthIn ? size_t(o.inW) * o.inH * 2 : 0;
    std::vector<uint8_t> frame(frameBytes), depth(depthBytes);
    uint64_t n = 0;
    bool eof = false;

    while (!o.maxFrames || n < o.maxFrames) {
        if (!ReadExact(hin, frame.data(), frameBytes, eof)) {
            if (eof) break;                       // clean end of stream
            if (n == 0) return 4;                 // hard I/O error before any frame
            fprintf(stderr, SMRU_LOG_TAG " truncated input frame after %llu frames\n", (unsigned long long)n);
            break;
        }
        if (o.depthIn && !ReadExact(hin, depth.data(), depthBytes, eof)) {
            fprintf(stderr, SMRU_LOG_TAG " missing depth plane at frame %llu\n", (unsigned long long)n);
            return 4;
        }
        const uint8_t* dptr = o.depthIn ? depth.data() : nullptr;
        p.frameIndex = n;
        if (n == 0) {
            // Warm-up: duplicated first-frame renders create the feature (first-frame
            // flush) and converge fresh temporal history; outputs are discarded so
            // counting stays 1:1.
            for (int wpass = 0; wpass < o.warmup; ++wpass) {
                if (!p.Render(frame.data(), frameBytes, true, dptr, depthBytes)) {
                    fprintf(stderr, SMRU_LOG_TAG " warmup render failed\n");
                    return 5;
                }
            }
        }
        if (!p.Render(frame.data(), frameBytes, n == 0, dptr, depthBytes)) {
            fprintf(stderr, SMRU_LOG_TAG " render failed at frame %llu\n", (unsigned long long)n);
            return 5;
        }
        const auto& px = p.renderer.ExportRGBA();
        if (px.empty()) return 6;
        const uint32_t ow = p.renderer.OutputW(), oh = p.renderer.OutputH();
        const uint8_t* payload = px.data();
        if (!WriteExact(hout, payload, size_t(ow) * oh * 4)) return 6;
        ++n;
        if (n % 60 == 0) fprintf(stderr, SMRU_LOG_TAG " frame %llu\n", (unsigned long long)n);
    }

    p.renderer.WaitGPU();
    p.LogStats(n);
    return n > 0 ? 0 : 4;
}

int RunFile(Options o) {
    if (o.input.empty() || o.exportPath.empty()) {
        fprintf(stderr, SMRU_LOG_TAG " file mode needs --input and --export\n");
        return 2;
    }
    VideoDecoder decoder;
    if (!decoder.Open(o.input)) { fprintf(stderr, SMRU_LOG_TAG " cannot open input\n"); return 4; }
    const uint32_t W = decoder.Width(), H = decoder.Height();
    const double fps = decoder.FrameRate() > 1.0 ? decoder.FrameRate() : 30.0;

    // Optional model-depth movie decoded alongside (bright = near); converted to
    // the 0 = near R16 contract per frame. The last plane is reused if it is
    // shorter than the main movie.
    VideoDecoder depthDec;
    bool haveDepthVideo = false;
    if (!o.depthVideo.empty()) {
        if (depthDec.Open(o.depthVideo) && depthDec.Width() == W && depthDec.Height() == H) {
            haveDepthVideo = true;
            fprintf(stderr, SMRU_LOG_TAG " depth video attached: %ls\n", o.depthVideo.c_str());
        } else {
            fprintf(stderr, SMRU_LOG_TAG " depth video unusable (must match %ux%u); continuing without it\n", W, H);
            o.depthIn = false;
        }
    } else if (o.depthIn) {
        o.depthIn = false; // raw-mode flag has no meaning in file mode without a depth video
    }
    // Optional RAFT flow movie: replaces the block-matcher MV field per frame.
    VideoDecoder flowDec;
    bool haveFlowVideo = false;
    if (!o.flowVideo.empty()) {
        if (flowDec.Open(o.flowVideo)) {
            if (flowDec.Width() != W || flowDec.Height() != H) flowDec.SetDecodeSize(W, H);
            if (flowDec.Width() == W && flowDec.Height() == H) {
                haveFlowVideo = true;
                fprintf(stderr, SMRU_LOG_TAG " flow video attached: %ls\n", o.flowVideo.c_str());
            }
        }
        if (!haveFlowVideo) {
            fprintf(stderr, SMRU_LOG_TAG " flow video unusable; continuing with estimated/zero MVs\n");
            o.flowVideo.clear();
        }
    }
    // Optional segmentation mask movie (white = process here): replaces the
    // block-matcher uncertainty as the ControlMask source. The last plane is
    // reused if the mask movie is shorter than the main movie.
    VideoDecoder maskDec;
    bool haveMaskVideo = false;
    if (!o.maskVideo.empty()) {
        if (maskDec.Open(o.maskVideo)) {
            if (maskDec.Width() != W || maskDec.Height() != H) maskDec.SetDecodeSize(W, H);
            if (maskDec.Width() == W && maskDec.Height() == H) {
                haveMaskVideo = true;
                fprintf(stderr, SMRU_LOG_TAG " mask video attached: %ls\n", o.maskVideo.c_str());
            }
        }
        if (!haveMaskVideo) {
            fprintf(stderr, SMRU_LOG_TAG " mask video unusable; continuing with the block-matcher mask\n");
            o.maskVideo.clear();
        }
    }

    // --scaler lanczos: crisp CPU upscale first, then a DLAA detail pass at full
    // output resolution instead of DLSS SR doing the scaling. Sidecars are
    // dropped: their per-pixel data is calibrated to the source geometry.
    uint32_t procW = W, procH = H;
    std::vector<uint8_t> lz;
    const bool lanczos = o.lanczosScale && o.outW && o.outH && (o.outW != W || o.outH != H);
    if (lanczos) {
        procW = o.outW; procH = o.outH;
        o.outW = o.outH = 0;
        if (haveDepthVideo || haveFlowVideo || haveMaskVideo) {
            fprintf(stderr, SMRU_LOG_TAG " scaler=lanczos: depth/flow/mask sidecars are source-calibrated; ignoring them\n");
            haveDepthVideo = false; haveFlowVideo = false; haveMaskVideo = false; o.depthIn = false;
        }
        lz.resize(size_t(procW) * procH * 4);
        fprintf(stderr, SMRU_LOG_TAG " scaler=lanczos: %ux%u -> %ux%u CPU Lanczos3 + DLAA detail pass\n",
                W, H, procW, procH);
    }

    Pipeline p;
    if (!p.Init(o, procW, procH, fps)) return 5;
    const uint32_t outW = p.renderer.OutputW(), outH = p.renderer.OutputH();

    if (o.compare && o.split) { fprintf(stderr, SMRU_LOG_TAG " --compare and --split are mutually exclusive\n"); return 2; }
    // Side-by-side compare: original on the left (bilinear-upscaled when the
    // export upscales - the honest baseline), processed on the right, thin divider.
    // Split: one normal-size frame, left half original, right half processed,
    // divider line down the middle.
    constexpr uint32_t kDivider = 4;
    const uint32_t encW = o.compare ? outW * 2 + kDivider : outW;

    // ffmpeg is resolved through the shared locator so the exporter finds it in
    // the same places the player does, not only beside its own exe.
    const std::wstring ffmpeg = smru::paths::FfmpegExe().wstring();
    if (ffmpeg.empty()) { fprintf(stderr, SMRU_LOG_TAG " ffmpeg.exe not found\n"); return 3; }
    std::wstringstream cmd;
    // The source audio is capped to the probed video length as an INPUT option.
    // -shortest was measured to drop the LAST VIDEO FRAME at the mux boundary
    // (5053 frames in, 5052 out, every time, whatever the buffer), and an audio
    // track that outlives the picture must never decide the video length either.
    std::wstringstream audioCap;
    if (decoder.DurationSeconds() > 0.0)
        audioCap << L" -t " << std::fixed << std::setprecision(6) << decoder.DurationSeconds();
    // RGB -> YUV must use BT.709 coefficients AND tag the stream: swscale's
    // untagged default is BT.601, and players assume BT.709 for HD sizes, which
    // shifts the whole picture warm/red on playback.
    cmd << L"\"" << ffmpeg << L"\" -y -hide_banner -loglevel error"
        << L" -f rawvideo -pixel_format rgba -video_size " << encW << L"x" << outH
        << L" -framerate " << fps << L" -i -" << audioCap.str() << L" -i \"" << o.input << L"\""
        << L" -map 0:v:0 -map 1:a:0?"
        << L" -vf \"scale=out_color_matrix=bt709:out_range=tv,setparams=color_primaries=bt709:color_trc=iec61966-2-1\"";
    // Encoder: x264 for compatibility; NVENC HEVC/AV1 for 4K/8K where a software
    // encode would dominate the export time. NVENC's -cq tracks crf closely
    // enough at these bitrates to reuse the one quality knob.
    if (o.codec == 1)
        cmd << L" -c:v hevc_nvenc -preset p5 -rc vbr -cq " << o.crf << L" -b:v 0 -tag:v hvc1";
    else if (o.codec == 2)
        cmd << L" -c:v av1_nvenc -preset p5 -rc vbr -cq " << o.crf << L" -b:v 0";
    else
        cmd << L" -c:v libx264 -preset medium -crf " << o.crf;
    cmd << L" -pix_fmt yuv420p -c:a aac \"" << o.exportPath << L"\"";

    // Frames go down the pipe; ffmpeg keeps this console's stdout/stderr so its
    // own diagnostics land in whatever captured ours.
    smru::proc::Options spawn;
    spawn.in = smru::proc::Stdio::Pipe;
    spawn.out = smru::proc::Stdio::Console;
    spawn.err = smru::proc::Stdio::Console;
    spawn.pipeBytes = 1 << 22;
    smru::proc::Child encoder;
    if (!smru::proc::Spawn(cmd.str(), spawn, encoder)) {
        fprintf(stderr, SMRU_LOG_TAG " ffmpeg spawn failed\n");
        return 7;
    }
    const HANDLE wr = encoder.stdIn;

    uint64_t n = 0;
    int rc = 0;
    VideoFrame f, df, ff, mf;
    std::vector<uint8_t> stitched, ref;
    std::vector<uint8_t> depthPlane;
    // Sidecar movies can be shorter than the main one, and VideoDecoder::ReadNext
    // resizes and may PARTIALLY overwrite its target before reporting the end of
    // the stream - so the decoder's own frame is not a safe "last good plane".
    // Each sidecar keeps its complete frames here instead (swapped in, never
    // copied) and stops being read once it runs dry.
    std::vector<uint8_t> flowPlane, maskPlane;
    bool flowEnded = false, maskEnded = false, depthEnded = false;
    if (o.compare || o.split) stitched.assign(size_t(encW) * outH * 4, 0);
    while (decoder.ReadNext(f)) {
        const uint8_t* dptr = nullptr;
        size_t dbytes = 0;
        const uint8_t* fptr = nullptr;
        size_t fbytes = 0;
        if (haveFlowVideo && !flowEnded) {
            if (flowDec.ReadNext(ff) && ff.bgra.size() >= size_t(W) * H * 4) {
                flowPlane.swap(ff.bgra);
                fptr = flowPlane.data(); fbytes = flowPlane.size();
            } else {
                // Unlike depth and the mask, a motion field must not be frozen:
                // holding the last flow frame over moving video points the neural
                // pass at correspondences that no longer exist, which smears far
                // worse than having no field at all. Hand the rest of the export
                // back to the block matcher.
                flowEnded = true;
                p.renderer.SetExternalFlowEnabled(false);
                fprintf(stderr, SMRU_LOG_TAG " flow video ended at frame %llu; the block matcher"
                        " estimates the motion field for the rest of the export\n",
                        (unsigned long long)n);
            }
        }
        const uint8_t* mptr = nullptr;
        size_t mbytes = 0;
        if (haveMaskVideo) {
            if (!maskEnded) {
                if (maskDec.ReadNext(mf) && mf.bgra.size() >= size_t(W) * H * 4) {
                    maskPlane.swap(mf.bgra);
                } else {
                    maskEnded = true;
                    fprintf(stderr, SMRU_LOG_TAG " mask video ended at frame %llu; holding the last"
                            " mask plane for the rest of the export\n", (unsigned long long)n);
                }
            }
            if (!maskPlane.empty()) { mptr = maskPlane.data(); mbytes = maskPlane.size(); }
        }
        if (haveDepthVideo) {
            if (!depthEnded) {
                if (depthDec.ReadNext(df) && df.bgra.size() >= size_t(W) * H * 4) {
                    DepthGrayToR16(df.bgra.data(), W, H, depthPlane);
                } else {
                    depthEnded = true;
                    fprintf(stderr, SMRU_LOG_TAG " depth video ended at frame %llu; holding the last"
                            " depth plane for the rest of the export\n", (unsigned long long)n);
                }
            }
            if (!depthPlane.empty()) { dptr = depthPlane.data(); dbytes = depthPlane.size(); }
        }
        const uint8_t* fb = f.bgra.data();
        size_t fbBytes = f.bgra.size();
        if (lanczos) {
            LanczosResize(f.bgra.data(), W, H, lz.data(), procW, procH);
            fb = lz.data(); fbBytes = lz.size();
        }
        p.frameIndex = n;
        if (n == 0) {
            for (int wpass = 0; wpass < o.warmup; ++wpass) {
                if (!p.Render(fb, fbBytes, true, dptr, dbytes, fptr, fbytes, mptr, mbytes)) { rc = 5; break; }
            }
            if (rc) break;
        }
        if (!p.Render(fb, fbBytes, n == 0 || f.discontinuity, dptr, dbytes, fptr, fbytes, mptr, mbytes)) {
            fprintf(stderr, SMRU_LOG_TAG " render failed at frame %llu\n", (unsigned long long)n);
            rc = 5;
            break;
        }
        const auto& px = p.renderer.ExportRGBA();
        if (px.empty()) { fprintf(stderr, SMRU_LOG_TAG " empty readback at frame %llu\n", (unsigned long long)n); rc = 6; break; }
        if (o.compare || o.split) {
            ref.resize(size_t(outW) * outH * 4);
            // With the lanczos scaler the upscaled frame IS the true input, so it
            // is also the honest compare baseline.
            BuildRefRGBA(lanczos ? fb : f.bgra.data(), lanczos ? procW : W, lanczos ? procH : H,
                         ref.data(), outW, outH);
        }
        const uint8_t* right = px.data();
        const uint8_t* payload = right;
        size_t payloadBytes = size_t(outW) * outH * 4;
        if (o.compare) {
            for (uint32_t y = 0; y < outH; ++y) {
                uint8_t* row = stitched.data() + size_t(y) * encW * 4;
                memcpy(row, ref.data() + size_t(y) * outW * 4, size_t(outW) * 4);
                for (uint32_t x = outW; x < outW + kDivider; ++x) {
                    row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 56;
                    row[x * 4 + 3] = 255;
                }
                memcpy(row + size_t(outW + kDivider) * 4,
                       right + size_t(y) * outW * 4, size_t(outW) * 4);
            }
            const uint32_t ls = std::clamp(outH / 240u, 2u, 8u);
            StampLabel(stitched.data(), encW, outH, 16, 16, smru::kLabelOriginal, ls);
            StampLabel(stitched.data(), encW, outH, outW + kDivider + 16, 16, smru::kLabelProcessed, ls);
            payload = stitched.data();
            payloadBytes = stitched.size();
        } else if (o.split) {
            const uint32_t mid = outW / 2;
            const uint32_t divW = std::max(2u, outW / 960u);
            for (uint32_t y = 0; y < outH; ++y) {
                uint8_t* row = stitched.data() + size_t(y) * outW * 4;
                memcpy(row, ref.data() + size_t(y) * outW * 4, size_t(mid) * 4);
                memcpy(row + size_t(mid) * 4, right + (size_t(y) * outW + mid) * 4,
                       size_t(outW - mid) * 4);
                for (uint32_t x = mid - divW / 2; x < mid - divW / 2 + divW && x < outW; ++x) {
                    row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 235;
                    row[x * 4 + 3] = 255;
                }
            }
            const uint32_t ls = std::clamp(outH / 240u, 2u, 8u);
            StampLabel(stitched.data(), outW, outH, 16, 16, smru::kLabelOriginal, ls);
            StampLabel(stitched.data(), outW, outH, mid + divW + 16, 16, smru::kLabelProcessed, ls);
            payload = stitched.data();
            payloadBytes = stitched.size();
        }
        if (!WriteExact(wr, payload, payloadBytes)) { fprintf(stderr, SMRU_LOG_TAG " ffmpeg pipe write failed at frame %llu\n", (unsigned long long)n); rc = 6; break; }
        ++n;
        // Frequent enough for a host to render a live progress bar.
        if (n % 30 == 0) fprintf(stderr, SMRU_LOG_TAG " frame %llu\n", (unsigned long long)n);
    }

    p.renderer.WaitGPU();
    CloseHandle(encoder.stdIn); encoder.stdIn = nullptr; // EOF is what makes ffmpeg finalize the file
    WaitForSingleObject(encoder.process, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(encoder.process, &ec);
    smru::proc::Close(encoder);
    p.LogStats(n);
    if (rc) return rc;
    if (n == 0) return 4;
    return ec == 0 ? 0 : 9;
}

// ---- Persistent batch server (--serve) --------------------------------------
// Text-line framed batches over binary stdin/stdout, so hosts (like the ComfyUI
// node) pay the NR arming wait ONCE per process instead of once per call, and
// temporal history carries across batches of the same clip (the scene-cut reset
// is the safety net when consecutive batches are unrelated content).
//
//   host > BATCH w=.. h=.. ow=.. oh=.. fps=.. frames=N bits=8|16 jitter=..
//              mv=.. quality=.. (accepted, no effect) tone=.. tone_mix=.. sharpen=..
//              post_sharpen=.. cut_reset=0|1 cut_warmup=N reset=0|1
//              lut_strength=.. lut=<path, rest of line, may be empty>
//   host > N tightly packed frames (W*H*4 bytes each, or W*H*8 when bits=16)
//   serve< OK <outW> <outH>
//   serve< N processed RGBA8 frames (outW*outH*4 bytes each)
//   host > QUIT (or EOF)
//
// The pipeline is rebuilt only when geometry/bits/LUT change; all other
// options update live between batches. Fatal errors exit the process - the host
// detects the dead worker / short read and respawns.

bool ReadLineStdin(HANDLE h, std::string& out) {
    out.clear();
    for (;;) {
        char c = 0; DWORD got = 0;
        if (!ReadFile(h, &c, 1, &got, nullptr) || got == 0) return false;
        if (c == '\n') return true;
        if (c != '\r') out.push_back(c);
        if (out.size() > 65536) return false;
    }
}

int RunServe(Options base) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    auto sendLine = [&](const std::string& s) {
        const std::string t = s + "\n";
        return WriteExact(hout, reinterpret_cast<const uint8_t*>(t.data()), t.size());
    };
    if (!sendLine(SMRU_SERVE_BANNER)) return 6;
    fprintf(stderr, SMRU_LOG_TAG " serve: ready\n");

    std::unique_ptr<Pipeline> p;
    struct Sig { uint32_t w = 0, h = 0, ow = 0, oh = 0; int bits = 8; std::wstring lut; } cur;
    bool fresh = true;
    bool warnedQuality = false;
    std::vector<uint8_t> frame;
    uint64_t total = 0, batches = 0;

    std::string line;
    while (ReadLineStdin(hin, line)) {
        if (line == "QUIT" || line.empty()) break;
        if (line.rfind("BATCH", 0) != 0) { sendLine("ERR bad command"); return 2; }

        Options o = base;
        uint32_t w = 0, h = 0, ow = 0, oh = 0;
        uint64_t frames = 0;
        bool batchReset = false;
        std::string lutStr;
        std::string rest = line.size() > 5 ? line.substr(6) : "";
        size_t pos = 0;
        while (pos < rest.size()) {
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            if (pos >= rest.size()) break;
            const size_t eq = rest.find('=', pos);
            if (eq == std::string::npos) break;
            const std::string key = rest.substr(pos, eq - pos);
            std::string val;
            if (key == "lut") { val = rest.substr(eq + 1); pos = rest.size(); }
            else {
                const size_t sp = rest.find(' ', eq + 1);
                val = rest.substr(eq + 1, (sp == std::string::npos ? rest.size() : sp) - eq - 1);
                pos = (sp == std::string::npos) ? rest.size() : sp + 1;
            }
            if (key == "w") w = uint32_t(atoi(val.c_str()));
            else if (key == "h") h = uint32_t(atoi(val.c_str()));
            else if (key == "ow") ow = uint32_t(atoi(val.c_str()));
            else if (key == "oh") oh = uint32_t(atoi(val.c_str()));
            else if (key == "fps") { const double f = atof(val.c_str()); if (f >= 1.0 && f <= 480.0) o.fps = f; }
            else if (key == "frames") frames = uint64_t(_atoi64(val.c_str()));
            else if (key == "bits") { const int b = atoi(val.c_str()); if (b == 8 || b == 16) o.bits = b; }
            else if (key == "jitter") {
                if (val == "matched") o.jitter = D3D12Renderer::JitterMode::Matched;
                else if (val == "legacy") o.jitter = D3D12Renderer::JitterMode::Legacy;
                else o.jitter = D3D12Renderer::JitterMode::Zero;
            }
            else if (key == "mv") o.mvMode = (val == "global") ? 1 : (val == "estimated") ? 2 : 0;
            else if (key == "quality") {
                // Accepted for compatibility; the neural pass has no mode.
                if (val != "auto" && !warnedQuality) {
                    warnedQuality = true;
                    fprintf(stderr, SMRU_LOG_TAG " note: batch key quality= has no effect (the neural pass runs 1:1)\n");
                }
            }
            else if (key == "tone") o.tonePreserve = (val != "nr");
            else if (key == "tone_mix") o.toneMix = float(std::clamp(atof(val.c_str()), 0.0, 1.0));
            else if (key == "sharpen") o.sharpen = float(std::clamp(atof(val.c_str()), 0.0, 1.0));
            else if (key == "post_sharpen") o.postSharpen = float(std::clamp(atof(val.c_str()), 0.0, 1.0));
            else if (key == "cut_reset") o.cutReset = atoi(val.c_str()) != 0;
            else if (key == "cut_warmup") o.cutWarmup = std::clamp(atoi(val.c_str()), 0, 8);
            else if (key == "reset") batchReset = atoi(val.c_str()) != 0;
            else if (key == "lut_strength") o.lutStrength = float(std::clamp(atof(val.c_str()), 0.0, 1.0));
            else if (key == "lut") lutStr = val;
        }
        if (!w || !h || !frames) { sendLine("ERR bad batch geometry"); return 2; }

        o.inW = w; o.inH = h; o.outW = ow; o.outH = oh;
        o.lutPath = smru::text::Utf8ToWide(lutStr);

        const bool needInit = !p || cur.w != w || cur.h != h || cur.ow != ow || cur.oh != oh ||
                              cur.bits != o.bits || cur.lut != o.lutPath;
        if (needInit) {
            if (p) { p->renderer.WaitGPU(); p.reset(); }
            p = std::make_unique<Pipeline>();
            if (!p->Init(o, w, h, o.fps)) { sendLine("ERR pipeline init failed"); return 5; }
            cur = { w, h, ow, oh, o.bits, o.lutPath };
            fresh = true;
            if (batches) fprintf(stderr, SMRU_LOG_TAG " serve: pipeline rebuilt (%ux%u -> %ux%u, bits=%d)\n",
                                 w, h, p->renderer.OutputW(), p->renderer.OutputH(), o.bits);
        } else {
            auto pol = p->frames.GetPolicy();
            pol.fps = o.fps;
            pol.motion = MotionFor(o.mvMode);
            pol.cutReset = o.cutReset;
            pol.cutWarmup = o.cutWarmup;
            p->frames.SetPolicy(pol);
            p->renderer.SetJitterMode(o.jitter);
            p->renderer.SetPreSharpen(o.sharpen);
            p->renderer.SetPostSharpen(o.postSharpen);
            p->renderer.SetLUTStrength(o.lutStrength);
        }

        const uint32_t outWr = p->renderer.OutputW(), outHr = p->renderer.OutputH();
        {
            std::ostringstream ok; ok << "OK " << outWr << " " << outHr;
            if (!sendLine(ok.str())) return 6;
        }

        const size_t frameBytes = size_t(w) * h * (o.bits == 16 ? 8u : 4u);
        frame.resize(frameBytes);
        p->renderer.SetNRSmooth(o.nrSmooth);
        p->renderer.SetToneMix(o.tonePreserve ? o.toneMix : 0.0f);
        for (uint64_t i = 0; i < frames; ++i) {
            bool eof = false;
            if (!ReadExact(hin, frame.data(), frameBytes, eof)) {
                fprintf(stderr, SMRU_LOG_TAG " serve: truncated frame %llu\n", (unsigned long long)i);
                return 4;
            }
            if (fresh) {
                for (int wp = 0; wp < o.warmup; ++wp)
                    if (!p->Render(frame.data(), frameBytes, true)) return 5;
            }
            const bool reset = fresh || (i == 0 && batchReset);
            fresh = false;
            if (!p->Render(frame.data(), frameBytes, reset)) {
                fprintf(stderr, SMRU_LOG_TAG " serve: render failed at frame %llu\n", (unsigned long long)i);
                return 5;
            }
            const auto& px = p->renderer.ExportRGBA();
            if (px.empty()) return 6;
            const uint8_t* payload = px.data();
            if (!WriteExact(hout, payload, size_t(outWr) * outHr * 4)) return 6;
            ++total;
            if (total % 60 == 0) fprintf(stderr, SMRU_LOG_TAG " serve: frame %llu\n", (unsigned long long)total);
        }
        ++batches;
        fprintf(stderr, SMRU_LOG_TAG " serve: batch %llu done (%llu frames)\n",
                (unsigned long long)batches, (unsigned long long)frames);
    }

    if (p) { p->renderer.WaitGPU(); p->LogStats(total); }
    fprintf(stderr, SMRU_LOG_TAG " serve: exiting after %llu batches / %llu frames\n",
            (unsigned long long)batches, (unsigned long long)total);
    return 0;
}

} // namespace

int wmain() {
    Log::SetFileName(smru::kExporterLogFile);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    const bool mfOk = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    Options o = ParseArgs();
    int rc;
    if (o.showHelp) { PrintUsage(); rc = 0; }
    else if (o.parseError) { fwprintf(stderr, L"" SMRU_LOG_TAG " %ls\n\n", o.parseErrorText.c_str()); PrintUsage(); rc = 2; }
    else if (o.serveMode) rc = RunServe(o);
    else if (o.rawMode) rc = RunRaw(o);
    else rc = RunFile(o);
    if (mfOk) MFShutdown();
    CoUninitialize();
    return rc;
}
