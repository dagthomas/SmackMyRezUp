#pragma once
// Tiny 5x7 bitmap font stamped directly into RGBA frames (dark backing box +
// white glyphs) - used for compare labels without any font dependency.
#include <cstdint>
#include <cstring>

inline const uint8_t* Glyph5x7(char c) {
    static const uint8_t O[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t R[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t I[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t G[7]={0x0E,0x11,0x10,0x17,0x11,0x11,0x0E};
    static const uint8_t N[7]={0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t A[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t L[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t D[7]={0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
    static const uint8_t S[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t F5[7]={0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
    static const uint8_t W[7]={0x11,0x11,0x11,0x15,0x15,0x15,0x0A};
    static const uint8_t T[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t H[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t U[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t F[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t X[7]={0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
    static const uint8_t M[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t C[7]={0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t K[7]={0x11,0x12,0x14,0x18,0x14,0x12,0x11};
    static const uint8_t Y[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
    static const uint8_t E[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t Z[7]={0x1F,0x01,0x02,0x04,0x08,0x10,0x1F};
    static const uint8_t P[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    switch (c) {
        case 'O': return O; case 'R': return R; case 'I': return I; case 'G': return G;
        case 'N': return N; case 'A': return A; case 'L': return L; case 'D': return D;
        case 'S': return S; case '5': return F5; case 'W': return W; case 'T': return T;
        case 'H': return H; case 'U': return U; case 'F': return F; case 'X': return X;
        case 'M': return M; case 'C': return C; case 'K': return K; case 'Y': return Y;
        case 'E': return E; case 'Z': return Z; case 'P': return P;
        default: return nullptr;
    }
}

inline void StampLabel(uint8_t* rgba, uint32_t imgW, uint32_t imgH,
                       uint32_t x0, uint32_t y0, const char* text, uint32_t scale) {
    const uint32_t len = uint32_t(strlen(text));
    const uint32_t gw = 6 * scale;
    const uint32_t boxW = len * gw + 3 * scale, boxH = 7 * scale + 4 * scale;
    for (uint32_t y = 0; y < boxH && y0 + y < imgH; ++y)
        for (uint32_t x = 0; x < boxW && x0 + x < imgW; ++x) {
            uint8_t* p = rgba + (size_t(y0 + y) * imgW + x0 + x) * 4;
            p[0] = uint8_t(p[0] / 4); p[1] = uint8_t(p[1] / 4); p[2] = uint8_t(p[2] / 4);
        }
    for (uint32_t i = 0; i < len; ++i) {
        const uint8_t* g = Glyph5x7(text[i]);
        if (!g) continue;
        const uint32_t gx0 = x0 + 2 * scale + i * gw, gy0 = y0 + 2 * scale;
        for (uint32_t ry = 0; ry < 7; ++ry)
            for (uint32_t rx = 0; rx < 5; ++rx) {
                if (!((g[ry] >> (4 - rx)) & 1)) continue;
                for (uint32_t sy = 0; sy < scale; ++sy)
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        const uint32_t px = gx0 + rx * scale + sx, py = gy0 + ry * scale + sy;
                        if (px >= imgW || py >= imgH) continue;
                        uint8_t* p = rgba + (size_t(py) * imgW + px) * 4;
                        p[0] = p[1] = p[2] = 245;
                    }
            }
    }
}
