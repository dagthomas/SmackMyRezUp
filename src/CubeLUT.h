#pragma once
// Minimal .cube 3D LUT parser shared by the player and the exporter:
// LUT_3D_SIZE, optional DOMAIN_MIN/MAX, '#' comments, TITLE, then size^3
// "R G B" rows with red varying fastest. 1D LUTs are rejected.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "AppIdentity.h"

inline bool LoadCubeLUT(const std::wstring& path, std::vector<float>& rgb, uint32_t& size,
                        float domainMin[3], float domainMax[3]) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, SMRU_LOG_TAG " cannot open LUT file\n"); return false; }
    size = 0;
    domainMin[0] = domainMin[1] = domainMin[2] = 0.0f;
    domainMax[0] = domainMax[1] = domainMax[2] = 1.0f;
    rgb.clear();
    std::string line;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        if (line[b] == '#') continue;
        std::istringstream ss(line.substr(b));
        std::string tok; ss >> tok;
        if (tok == "TITLE") continue;
        if (tok == "LUT_1D_SIZE") { fprintf(stderr, SMRU_LOG_TAG " 1D LUTs are not supported (need LUT_3D_SIZE)\n"); return false; }
        if (tok == "LUT_3D_SIZE") { ss >> size; continue; }
        if (tok == "DOMAIN_MIN") { ss >> domainMin[0] >> domainMin[1] >> domainMin[2]; continue; }
        if (tok == "DOMAIN_MAX") { ss >> domainMax[0] >> domainMax[1] >> domainMax[2]; continue; }
        char* end = nullptr;
        const float r = std::strtof(tok.c_str(), &end);
        if (end == tok.c_str()) continue; // unknown keyword, skip
        float g = 0.0f, bl = 0.0f;
        if (!(ss >> g >> bl)) { fprintf(stderr, SMRU_LOG_TAG " malformed LUT data row\n"); return false; }
        rgb.push_back(r); rgb.push_back(g); rgb.push_back(bl);
    }
    if (size < 2 || rgb.size() != size_t(size) * size * size * 3) {
        fprintf(stderr, SMRU_LOG_TAG " LUT parse failed: size=%u rows=%zu (expected %zu)\n",
                size, rgb.size() / 3, size_t(size) * size * size);
        return false;
    }
    return true;
}
