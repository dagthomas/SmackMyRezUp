// Ground-truth accuracy check for TemporalGuideGenerator's motion field.
//
// Builds a textured frame, shifts it by a KNOWN amount (fractional shifts use a
// bilinear resample, because real pans are not whole pixels), and asks the
// generator what motion it saw. The DLSS/NGX contract is current -> previous,
// so content that moved by +s must come back as MVec = -s, already scaled to
// render pixels.
//
// Build (from a VS x64 developer prompt, or via tools\dev\mv_accuracy.bat):
//   cl /std:c++20 /O2 /EHsc /I ..\..\src mv_accuracy.cpp ..\..\src\TemporalGuides.cpp
#include "TemporalGuides.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <tuple>
#include <vector>

namespace {

// Several octaves of value noise: high frequency enough that block matching is
// well conditioned, but not the pure salt-and-pepper that would flatter it.
std::vector<uint8_t> MakeTexture(int w, int h, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<float> field(size_t(w) * h, 0.0f);
    for (int oct = 0; oct < 3; ++oct) {
        const int step = 4 << oct;
        const int cw = w / step + 2, ch = h / step + 2;
        std::vector<float> ctrl(size_t(cw) * ch);
        for (auto& v : ctrl) v = float(rng() % 1000) / 1000.0f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float fx = float(x) / step, fy = float(y) / step;
                const int x0 = int(fx), y0 = int(fy);
                const float tx = fx - x0, ty = fy - y0;
                const float a = ctrl[size_t(y0) * cw + x0] * (1 - tx) + ctrl[size_t(y0) * cw + x0 + 1] * tx;
                const float b = ctrl[size_t(y0 + 1) * cw + x0] * (1 - tx) + ctrl[size_t(y0 + 1) * cw + x0 + 1] * tx;
                field[size_t(y) * w + x] += (a * (1 - ty) + b * ty) / float(1 << oct);
            }
        }
    }
    std::vector<uint8_t> img(size_t(w) * h * 4, 255);
    for (int i = 0; i < w * h; ++i) {
        const uint8_t v = uint8_t(std::clamp(field[i] * 160.0f, 0.0f, 255.0f));
        img[size_t(i) * 4 + 0] = v; img[size_t(i) * 4 + 1] = v; img[size_t(i) * 4 + 2] = v;
    }
    return img;
}

// Move the picture by (sx, sy) source pixels; fractional amounts resample.
std::vector<uint8_t> Shift(const std::vector<uint8_t>& src, int w, int h, float sx, float sy) {
    auto tap = [&](int x, int y, int c) {
        x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1);
        return float(src[(size_t(y) * w + x) * 4 + c]);
    };
    std::vector<uint8_t> dst(src.size(), 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float fx = float(x) - sx, fy = float(y) - sy;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const float tx = fx - x0, ty = fy - y0;
            for (int c = 0; c < 3; ++c) {
                const float a = tap(x0, y0, c) * (1 - tx) + tap(x0 + 1, y0, c) * tx;
                const float b = tap(x0, y0 + 1, c) * (1 - tx) + tap(x0 + 1, y0 + 1, c) * tx;
                dst[(size_t(y) * w + x) * 4 + c] = uint8_t(std::clamp(a * (1 - ty) + b * ty, 0.0f, 255.0f));
            }
        }
    }
    return dst;
}

float Median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

struct Result { float mx, my, mean_err, p90_err, ms; bool history; };

Result Run(int w, int h, float sx, float sy, TemporalGuideGenerator::Quality q, double fps) {
    TemporalGuideGenerator gen;
    gen.SetQuality(q);
    const auto base = MakeTexture(w, h, 1234);
    const auto moved = Shift(base, w, h, sx, sy);
    GuideFrame g{};
    gen.Generate(base.data(), w, h, w, h, fps, true, g);          // prime the history
    const auto t0 = std::chrono::steady_clock::now();
    gen.Generate(moved.data(), w, h, w, h, fps, false, g);        // the measured call
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Skip a 4-cell border: content shifted in from outside has no correspondence.
    std::vector<float> xs, ys, err;
    for (uint32_t y = 4; y + 4 < g.gridH; ++y) {
        for (uint32_t x = 4; x + 4 < g.gridW; ++x) {
            const size_t o = (size_t(y) * g.gridW + x) * 4;
            xs.push_back(g.guideGridRGBA32F[o + 0]);
            ys.push_back(g.guideGridRGBA32F[o + 1]);
            err.push_back(std::hypot(g.guideGridRGBA32F[o + 0] + sx, g.guideGridRGBA32F[o + 1] + sy));
        }
    }
    std::sort(err.begin(), err.end());
    float mean = 0.0f;
    for (float e : err) mean += e;
    if (!err.empty()) mean /= float(err.size());
    return { Median(xs), Median(ys), mean, err.empty() ? 0.0f : err[size_t(err.size() * 0.90)], float(ms), g.hasHistory };
}

const char* Name(TemporalGuideGenerator::Quality q) {
    return q == TemporalGuideGenerator::Quality::Fine ? "fine" : "realtime";
}

} // namespace

int main() {
    const int W = 1280, H = 720;
    printf("source %dx%d, render 1:1 so grid MVs are already in render pixels\n", W, H);
    printf("contract: content shifted by +s must give MVec = -s\n\n");

    printf("%-9s %-11s %-15s %-15s %8s %8s %7s %s\n",
           "quality", "shift", "expected MV", "measured MV", "mean err", "p90 err", "ms", "history");
    const std::pair<float, float> cases[] = {
        {1.0f, 0.0f}, {2.0f, 0.0f}, {4.0f, 0.0f}, {-6.0f, 0.0f}, {0.0f, 5.0f},
        {3.0f, -2.0f}, {12.0f, 7.0f}, {1.5f, 0.0f}, {0.5f, 0.0f}, {2.7f, 1.3f},
    };
    for (auto q : {TemporalGuideGenerator::Quality::Realtime, TemporalGuideGenerator::Quality::Fine}) {
        for (auto [sx, sy] : cases) {
            const Result r = Run(W, H, sx, sy, q, 30.0);
            printf("%-9s %+5.1f,%+5.1f %+7.2f,%+6.2f %+7.2f,%+6.2f %8.2f %8.2f %7.1f %s\n",
                   Name(q), sx, sy, -sx, -sy, r.mx, r.my, r.mean_err, r.p90_err, r.ms,
                   r.history ? "yes" : "NO");
        }
    }

    printf("\nHorizontal sweep, measured MV x for a +s px pan (ideal = -s):\n");
    printf("%-9s", "shift");
    for (float s = 0.5f; s <= 6.01f; s += 0.5f) printf("%7.1f", s);
    printf("\n");
    for (auto q : {TemporalGuideGenerator::Quality::Realtime, TemporalGuideGenerator::Quality::Fine}) {
        printf("%-9s", Name(q));
        for (float s = 0.5f; s <= 6.01f; s += 0.5f) printf("%7.2f", Run(W, H, s, 0.0f, q, 30.0).mx);
        printf("\n");
    }

    printf("\nCost per Generate() call at %dx%d (grid size is capped, so this\n"
           "barely moves with source resolution):\n", W, H);
    for (auto [q, fps, label] : {std::tuple{TemporalGuideGenerator::Quality::Realtime, 30.0, "realtime 30fps"},
                                 std::tuple{TemporalGuideGenerator::Quality::Realtime, 60.0, "realtime 60fps"},
                                 std::tuple{TemporalGuideGenerator::Quality::Fine, 30.0, "fine (export)"}}) {
        double total = 0.0;
        for (int i = 0; i < 5; ++i) total += Run(W, H, 3.0f, 2.0f, q, fps).ms;
        printf("  %-16s %6.1f ms\n", label, total / 5.0);
    }
    printf("\nHigh-fps (>=45) path accuracy, measured MV x for a +s px pan:\n");
    printf("%-9s", "shift");
    for (float s = 0.5f; s <= 6.01f; s += 0.5f) printf("%7.1f", s);
    printf("\n%-9s", "60fps");
    for (float s = 0.5f; s <= 6.01f; s += 0.5f) printf("%7.2f", Run(W, H, s, 0.0f, TemporalGuideGenerator::Quality::Realtime, 60.0).mx);
    printf("\n");
    return 0;
}
