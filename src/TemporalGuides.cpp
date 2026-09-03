#include "TemporalGuides.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

void TemporalGuideGenerator::Reset() {
    m_prevLuma.clear();
    m_prevDepth.clear();
    m_gridW = m_gridH = 0;
    m_havePrev = false;
}

std::pair<uint32_t,uint32_t> TemporalGuideGenerator::AnalysisGrid(uint32_t sourceW, uint32_t sourceH, double targetFps,
                                                                  bool fine) {
    if (!sourceW || !sourceH) return {0,0};
    if (fine) {
        // Offline-export field: ~6 source pixels per grid cell (capped), so with
        // 1/8-grid subpixel refinement the motion field resolves well below one
        // render pixel instead of ~3.5-pixel steps.
        const uint32_t gw = std::clamp(sourceW / 6u, 128u, 384u);
        const uint32_t gh = std::max(72u, uint32_t((uint64_t(gw) * sourceH) / sourceW));
        return {gw, gh};
    }
    // Cell size is what decides how small a motion can be resolved at all: the
    // subpixel stage can only interpolate inside one cell, so a coarse grid turns
    // a slow pan into no motion. 24-30 fps content has the frame budget for a
    // dense field; a 50/60-fps movie does not, and keeps a compact one so guide
    // generation never becomes the reason it misses realtime. The GPU expands
    // either field to the exact DLSS input resolution.
    const bool highFps = std::isfinite(targetFps) && targetFps >= 45.0;
    const uint32_t maxGridW = highFps ? 144u : 240u;
    const uint32_t divisor = highFps ? 13u : 7u;
    const uint32_t gw = std::clamp(sourceW / divisor, 96u, maxGridW);
    const uint32_t minH = highFps ? 48u : 54u;
    const uint32_t gh = std::max(minH, uint32_t((uint64_t(gw) * sourceH) / sourceW));
    return {gw, gh};
}

float TemporalGuideGenerator::Luma(const uint8_t* p) {
    // BGRA -> Rec.709-ish luma in [0,1].
    return (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) * (1.0f / 255.0f);
}

void TemporalGuideGenerator::DownsampleLuma(const uint8_t* bgra, uint32_t w, uint32_t h,
                                             uint32_t gw, uint32_t gh, std::vector<float>& out) const {
    out.assign(size_t(gw) * gh, 0.0f);
    for (uint32_t gy = 0; gy < gh; ++gy) {
        const uint32_t y0 = uint32_t((uint64_t(gy) * h) / gh);
        const uint32_t y1 = std::max(y0 + 1, uint32_t((uint64_t(gy + 1) * h) / gh));
        for (uint32_t gx = 0; gx < gw; ++gx) {
            const uint32_t x0 = uint32_t((uint64_t(gx) * w) / gw);
            const uint32_t x1 = std::max(x0 + 1, uint32_t((uint64_t(gx + 1) * w) / gw));
            // Four stratified samples are much cheaper than averaging every source pixel.
            const uint32_t xs[2] = { x0, std::min(w - 1, (x0 + x1) / 2) };
            const uint32_t ys[2] = { y0, std::min(h - 1, (y0 + y1) / 2) };
            float s = 0.0f;
            for (uint32_t yy : ys) for (uint32_t xx : xs)
                s += Luma(bgra + (size_t(yy) * w + xx) * 4u);
            out[size_t(gy) * gw + gx] = s * 0.25f;
        }
    }
}

static float PatchSad(const std::vector<float>& cur, const std::vector<float>& prev,
                      int x, int y, int dx, int dy, int w, int h) {
    float sad = 0.0f;
    int count = 0;
    for (int py = -1; py <= 1; ++py) {
        int cy = y + py, oy = cy + dy;
        if (cy < 0 || cy >= h || oy < 0 || oy >= h) continue;
        for (int px = -1; px <= 1; ++px) {
            int cx = x + px, ox = cx + dx;
            if (cx < 0 || cx >= w || ox < 0 || ox >= w) continue;
            sad += std::abs(cur[size_t(cy) * w + cx] - prev[size_t(oy) * w + ox]);
            ++count;
        }
    }
    return count ? sad / float(count) : 10.0f;
}

static float SampleBilinear(const std::vector<float>& img, float x, float y, int w, int h) {
    if (x < 0.0f || y < 0.0f || x > float(w - 1) || y > float(h - 1))
        return std::numeric_limits<float>::quiet_NaN();
    const int x0 = std::clamp(int(std::floor(x)), 0, w - 1);
    const int y0 = std::clamp(int(std::floor(y)), 0, h - 1);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float tx = x - float(x0), ty = y - float(y0);
    const float a = img[size_t(y0) * w + x0] * (1.0f - tx) + img[size_t(y0) * w + x1] * tx;
    const float b = img[size_t(y1) * w + x0] * (1.0f - tx) + img[size_t(y1) * w + x1] * tx;
    return a * (1.0f - ty) + b * ty;
}

static float PatchSadSubpixel(const std::vector<float>& cur, const std::vector<float>& prev,
                              int x, int y, float dx, float dy, int w, int h) {
    float sad = 0.0f;
    int count = 0;
    for (int py = -1; py <= 1; ++py) {
        const int cy = y + py;
        if (cy < 0 || cy >= h) continue;
        for (int px = -1; px <= 1; ++px) {
            const int cx = x + px;
            if (cx < 0 || cx >= w) continue;
            const float pv = SampleBilinear(prev, float(cx) + dx, float(cy) + dy, w, h);
            if (!std::isfinite(pv)) continue;
            sad += std::abs(cur[size_t(cy) * w + cx] - pv);
            ++count;
        }
    }
    return count ? sad / float(count) : 10.0f;
}

void TemporalGuideGenerator::EstimateFlow(const std::vector<float>& cur, const std::vector<float>& prev,
                                           uint32_t gw, uint32_t gh,
                                           std::vector<float>& flowX, std::vector<float>& flowY,
                                           std::vector<float>& mismatch,
                                           float& globalX, float& globalY, float& globalCost) const {
    const int w = int(gw), h = int(gh);

    // Stage lighting - strobes, colour washes, a practical switching on - changes
    // the whole frame's level and contrast while its STRUCTURE stays put. A raw
    // SAD reads that as a total loss of correspondence, so globalCost crosses the
    // cut threshold and the caller resets the neural history. On a locked-off shot
    // under flashing lights that fires several times a second, and every reset
    // costs the temporal model its convergence.
    //
    // A lighting change is well modelled as an affine map of the previous frame,
    // cur ~= a*prev + b. Fit a and b by least squares and offer the matched frame
    // to the global search below, so the cost can measure structure rather than
    // exposure. A real cut has different structure, which no single gain/offset
    // can explain away, so it still scores high. The matched frame is a
    // CANDIDATE only: a colour wash is not a pure luma gain, and a fit that
    // misfires must never make a frame look less like its predecessor than the
    // raw comparison says it is.
    std::vector<float> matched;
    const std::vector<float>* pv = &prev;
    {
        double sp = 0.0, sc = 0.0, spp = 0.0, spc = 0.0;
        int n = 0;
        for (int y = 4; y < h - 4; y += 4) {
            for (int x = 4; x < w - 4; x += 4) {
                const double p = prev[size_t(y) * w + x], c = cur[size_t(y) * w + x];
                sp += p; sc += c; spp += p * p; spc += p * c; ++n;
            }
        }
        if (n > 8) {
            const double mp = sp / n, mc = sc / n;
            const double var = spp / n - mp * mp;
            const double cov = spc / n - mp * mc;
            // A flat previous frame carries no gain information; leave it alone.
            double a = var > 1e-6 ? cov / var : 1.0;
            a = std::clamp(a, 0.25, 4.0);
            const double b = mc - a * mp;
            // Only pay for the rewrite when the exposure actually moved.
            if (std::abs(a - 1.0) > 0.01 || std::abs(b) > 0.004) {
                matched.resize(prev.size());
                for (size_t i = 0; i < prev.size(); ++i)
                    matched[i] = float(std::clamp(a * prev[i] + b, 0.0, 1.0));
            }
        }
    }

    // First find a coarse whole-frame translation. This is especially valuable for camera pans.
    constexpr int globalRadius = 7;
    auto searchGlobal = [&](const std::vector<float>& ref, int& gx, int& gy) {
        float best = std::numeric_limits<float>::max();
        float zero = std::numeric_limits<float>::max();
        gx = gy = 0;
        for (int dy = -globalRadius; dy <= globalRadius; ++dy) {
            for (int dx = -globalRadius; dx <= globalRadius; ++dx) {
                float sad = 0.0f; int n = 0;
                for (int y = 4; y < h - 4; y += 4) {
                    const int oy = y + dy; if (oy < 0 || oy >= h) continue;
                    for (int x = 4; x < w - 4; x += 4) {
                        const int ox = x + dx; if (ox < 0 || ox >= w) continue;
                        sad += std::abs(cur[size_t(y) * w + x] - ref[size_t(oy) * w + ox]);
                        ++n;
                    }
                }
                if (n) sad /= float(n);
                // Mild penalty avoids jumping to large vectors in flat/noisy regions.
                sad += 0.0015f * float(dx * dx + dy * dy);
                if (dx == 0 && dy == 0) zero = sad;
                if (sad < best) { best = sad; gx = dx; gy = dy; }
            }
        }
        // A small independently moving object on an otherwise flat/static frame can make a
        // whole-frame translation look marginally better than zero. Do not smear that motion
        // over every pixel unless the global shift wins by a meaningful margin. Local block
        // matching below will still recover object motion around the zero/global seed.
        if ((gx != 0 || gy != 0) && std::isfinite(zero) && (zero - best) < 0.012f) {
            gx = gy = 0;
            best = zero;
        }
        return best;
    };
    int bestGX = 0, bestGY = 0;
    float bestGlobal = searchGlobal(prev, bestGX, bestGY);
    if (!matched.empty()) {
        int mx = 0, my = 0;
        const float mBest = searchGlobal(matched, mx, my);
        if (mBest < bestGlobal) { bestGlobal = mBest; bestGX = mx; bestGY = my; pv = &matched; }
    }
    // Whichever frame won the global search is the one the local stage refines.
    const std::vector<float>& pref = *pv;
    globalX = float(bestGX); globalY = float(bestGY); globalCost = bestGlobal;

    flowX.assign(size_t(gw) * gh, float(bestGX));
    flowY.assign(size_t(gw) * gh, float(bestGY));
    mismatch.assign(size_t(gw) * gh, bestGlobal);
    const bool fine = (m_quality == Quality::Fine);
    // The fine grid has smaller cells, so the same object motion spans more grid
    // pixels; widen the local search accordingly.
    const int localRadius = fine ? 5 : 3;
    // Solve local flow on a 2x2 lattice, then expand each result to the tiny block.
    // At a 160-wide analysis grid this retains useful object motion while making
    // 30/60 fps playback much less CPU-bound than matching every grid pixel.
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            float best = std::numeric_limits<float>::max();
            int bx = bestGX, by = bestGY;
            for (int oy = -localRadius; oy <= localRadius; ++oy) {
                for (int ox = -localRadius; ox <= localRadius; ++ox) {
                    const int dx = bestGX + ox, dy = bestGY + oy;
                    float cost = PatchSad(cur, pref, x, y, dx, dy, w, h);
                    cost += 0.002f * float(ox * ox + oy * oy);
                    if (cost < best) { best = cost; bx = dx; by = dy; }
                }
            }
            float fbx = float(bx), fby = float(by);
            // Integer block matching on a compact grid is far too quantized once the
            // field is scaled to 1440p/4K - one grid cell is several render pixels,
            // so a slow pan lands inside a single cell and reads as no motion at all.
            //
            // Two stages fix that. A short scan of bilinear-sampled offsets brackets
            // the minimum (unbiased, but only as fine as its step), then a parabola
            // through the winning sample and its two neighbours is solved for its
            // vertex on each axis. The second stage is what makes the output
            // CONTINUOUS: a one-pixel pan comes back as one pixel instead of being
            // rounded to the nearest rung of a ladder. It costs no extra patch
            // comparisons, because the scan already computed every cost it needs.
            if (best <= (fine ? 0.25f : 0.18f)) {
                static constexpr float subFast[] = {-0.50f, -0.25f, 0.0f, 0.25f, 0.50f};
                static constexpr float subFine[] = {-0.500f, -0.375f, -0.250f, -0.125f, 0.0f,
                                                     0.125f, 0.250f, 0.375f, 0.500f};
                const float* sub = fine ? subFine : subFast;
                const int subN = int(fine ? std::size(subFine) : std::size(subFast));
                const float step = fine ? 0.125f : 0.25f;
                float cost[std::size(subFine)][std::size(subFine)]{};
                int bix = subN / 2, biy = subN / 2;
                float refined = std::numeric_limits<float>::max();
                for (int syi = 0; syi < subN; ++syi) {
                    for (int sxi = 0; sxi < subN; ++sxi) {
                        const float sy = sub[syi], sx = sub[sxi];
                        float c = PatchSadSubpixel(cur, pref, x, y, float(bx) + sx, float(by) + sy, w, h);
                        c += 0.0015f * (sx * sx + sy * sy);
                        cost[syi][sxi] = c;
                        if (c < refined) { refined = c; bix = sxi; biy = syi; }
                    }
                }
                float ox = sub[bix], oy = sub[biy];
                // Only interpolate when the winner has a neighbour on both sides;
                // on the edge of the scan there is nothing to fit a parabola to.
                if (bix > 0 && bix + 1 < subN) {
                    const float cm = cost[biy][bix - 1], c0 = cost[biy][bix], cp = cost[biy][bix + 1];
                    const float curve = cm - 2.0f * c0 + cp;
                    if (curve > 1e-6f) ox += std::clamp(0.5f * step * (cm - cp) / curve, -step, step);
                }
                if (biy > 0 && biy + 1 < subN) {
                    const float cm = cost[biy - 1][bix], c0 = cost[biy][bix], cp = cost[biy + 1][bix];
                    const float curve = cm - 2.0f * c0 + cp;
                    if (curve > 1e-6f) oy += std::clamp(0.5f * step * (cm - cp) / curve, -step, step);
                }
                fbx = float(bx) + ox;
                fby = float(by) + oy;
                best = refined;
            }

            // High mismatch means a cut/disocclusion/no reliable correspondence.
            if (best > 0.18f) { fbx = 0.0f; fby = 0.0f; }
            for (int yy = y; yy < std::min(y + 2, h); ++yy) {
                for (int xx = x; xx < std::min(x + 2, w); ++xx) {
                    const size_t oi = size_t(yy) * gw + xx;
                    flowX[oi] = fbx;
                    flowY[oi] = fby;
                    mismatch[oi] = best;
                }
            }
        }
    }
}

void TemporalGuideGenerator::MedianFlow(std::vector<float>& x, std::vector<float>& y,
                                         uint32_t gw, uint32_t gh) const {
    std::vector<float> ox = x, oy = y;
    for (uint32_t py = 1; py + 1 < gh; ++py) {
        for (uint32_t px = 1; px + 1 < gw; ++px) {
            std::array<float, 9> xs{}, ys{}; size_t k = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
                const size_t idx = size_t(int(py) + j) * gw + size_t(int(px) + i);
                xs[k] = ox[idx]; ys[k] = oy[idx]; ++k;
            }
            std::nth_element(xs.begin(), xs.begin() + 4, xs.end());
            std::nth_element(ys.begin(), ys.begin() + 4, ys.end());
            const size_t idx = size_t(py) * gw + px;
            x[idx] = xs[4]; y[idx] = ys[4];
        }
    }
}

void TemporalGuideGenerator::BuildDepthProxy(const std::vector<float>& luma,
                                              const std::vector<float>& flowX, const std::vector<float>& flowY,
                                              uint32_t gw, uint32_t gh,
                                              std::vector<float>& depth) {
    depth.assign(size_t(gw) * gh, 0.75f);
    if (m_depthMode == DepthMode::Flat) return;

    float maxMotion = 1.0f;
    for (size_t i = 0; i < flowX.size(); ++i)
        maxMotion = std::max(maxMotion, std::sqrt(flowX[i] * flowX[i] + flowY[i] * flowY[i]));

    for (uint32_t y = 0; y < gh; ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            const size_t idx = size_t(y) * gw + x;
            const float yn = gh > 1 ? float(y) / float(gh - 1) : 0.5f;
            float grad = 0.0f;
            if (x > 0 && x + 1 < gw) grad += std::abs(luma[idx + 1] - luma[idx - 1]);
            if (y > 0 && y + 1 < gh) grad += std::abs(luma[idx + gw] - luma[idx - gw]);
            const float motion = std::sqrt(flowX[idx] * flowX[idx] + flowY[idx] * flowY[idx]) / maxMotion;
            // This is explicitly a VIDEO DEPTH PROXY, not geometric engine depth.
            // It provides stable segmentation/disocclusion hints when a movie has no Z buffer.
            float d = 0.92f - 0.42f * yn - 0.17f * std::clamp(motion, 0.0f, 1.0f)
                            - 0.10f * std::clamp(grad * 2.0f, 0.0f, 1.0f);
            d = std::clamp(d, 0.08f, 0.97f);
            if (m_prevDepth.size() == depth.size()) d = m_prevDepth[idx] * 0.80f + d * 0.20f;
            depth[idx] = d;
        }
    }
    m_prevDepth = depth;
}

bool TemporalGuideGenerator::Generate(const uint8_t* bgra, uint32_t sourceW, uint32_t sourceH,
                                       uint32_t renderW, uint32_t renderH, double targetFps, bool reset,
                                       GuideFrame& out) {
    if (!bgra || !sourceW || !sourceH || !renderW || !renderH) return false;
    if (reset) Reset();

    const auto [gw, gh] = AnalysisGrid(sourceW, sourceH, targetFps, m_quality == Quality::Fine);
    if (!gw || !gh) return false;
    if (gw != m_gridW || gh != m_gridH) Reset();
    m_gridW = gw; m_gridH = gh;

    std::vector<float> cur;
    DownsampleLuma(bgra, sourceW, sourceH, gw, gh, cur);

    std::vector<float> fx(size_t(gw) * gh, 0.0f), fy(size_t(gw) * gh, 0.0f), mismatch(size_t(gw) * gh, 1.0f);
    float globalX = 0.0f, globalY = 0.0f;
    bool history = m_havePrev && m_prevLuma.size() == cur.size();
    float globalCost = 0.0f;
    if (history) {
        EstimateFlow(cur, m_prevLuma, gw, gh, fx, fy, mismatch, globalX, globalY, globalCost);
        // Use correspondence quality, not raw frame difference, so fast camera pans are not mistaken for cuts.
        if (globalCost > 0.10f) {
            history = false;
            std::fill(fx.begin(), fx.end(), 0.0f);
            std::fill(fy.begin(), fy.end(), 0.0f);
            std::fill(mismatch.begin(), mismatch.end(), 1.0f);
            globalX = globalY = 0.0f;
            m_prevDepth.clear();
        } else {
            MedianFlow(fx, fy, gw, gh);
        }
    }

    std::vector<float> depthGrid;
    BuildDepthProxy(cur, fx, fy, gw, gh, depthGrid);

    // Keep CPU output compact. A D3D12 MRT pass bilinearly expands this grid to
    // full render-resolution R16G16 motion + R32 depth + R8 bias textures.
    out.gridW = gw;
    out.gridH = gh;
    out.guideGridRGBA32F.assign(size_t(gw) * gh * 4u, 0.0f);
    const float gridToRenderX = float(renderW) / float(gw);
    const float gridToRenderY = float(renderH) / float(gh);

    for (uint32_t y = 0; y < gh; ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            const size_t i = size_t(y) * gw + x;
            float mask = 0.0f;
            if (history) {
                const uint32_t xl = x ? x - 1 : x;
                const uint32_t xr = std::min(gw - 1, x + 1);
                const uint32_t yt = y ? y - 1 : y;
                const uint32_t yb = std::min(gh - 1, y + 1);
                const float dx = fx[size_t(y) * gw + xr] - fx[size_t(y) * gw + xl];
                const float dy = fy[size_t(yb) * gw + x] - fy[size_t(yt) * gw + x];
                if (mismatch[i] > 0.115f || std::abs(dx) + std::abs(dy) > 2.5f) mask = 1.0f;
            }
            const size_t o = i * 4u;
            out.guideGridRGBA32F[o + 0] = history ? fx[i] * gridToRenderX : 0.0f;
            out.guideGridRGBA32F[o + 1] = history ? fy[i] * gridToRenderY : 0.0f;
            out.guideGridRGBA32F[o + 2] = depthGrid[i];
            out.guideGridRGBA32F[o + 3] = mask;
        }
    }

    out.hasHistory = history;
    out.globalMotionX = globalX * gridToRenderX;
    out.globalMotionY = globalY * gridToRenderY;
    out.globalMatchCost = globalCost;
    m_prevLuma = std::move(cur);
    m_havePrev = true;
    return true;
}

