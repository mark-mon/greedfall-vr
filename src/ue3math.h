#pragma once
// UE3 camera math shared by the D3D9 and D3D11 backends (header-only, no game or D3D dependencies).
// Conventions (see sharedtools\ue3\README.md): row vectors, clip = world * VP, left-handed view space (+z forward),
// P[2][3] = 1 (w = z_view), VP = V * P decomposes exactly.
#include <cmath>
#include <cstring>

namespace ue3 {

struct M4 { float m[4][4]; };

inline M4 Identity() { M4 r{}; r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1; return r; }
inline M4 Mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}
inline M4 Transpose(const M4& a) {
    M4 r;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
    return r;
}
inline bool Inverse(const M4& a, M4* out) {
    double m[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { m[i][j] = a.m[i][j]; m[i][j + 4] = i == j; }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; ++j) { double t = m[c][j]; m[c][j] = m[p][j]; m[p][j] = t; }
        double d = m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) { double f = m[r][c]; for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out->m[i][j] = (float)m[i][j + 4];
    return true;
}

// VP -> V (rotation + translation, LH) and P. Returns false for anything that is not a perspective camera.
struct Camera {
    M4 V, P;
    bool ui = false;          // exactly identity rotation (UE3 UI-scene camera)
    float pos[3];             // camera world position
    float fovH = 0, fovV = 0; // full angles in degrees (symmetric part)
};
inline bool Decompose(const M4& VP, Camera* c) {
    float vz[4] = {VP.m[0][3], VP.m[1][3], VP.m[2][3], VP.m[3][3]};
    float n = std::sqrt(vz[0] * vz[0] + vz[1] * vz[1] + vz[2] * vz[2]);
    if (!(n > 0.98f && n < 1.02f)) return false;   // orthographic or not a camera matrix (also rejects NaN)
    for (float& v : vz) v /= n;
    float c0[4], c1[4], c2[4];
    for (int i = 0; i < 4; ++i) { c0[i] = VP.m[i][0]; c1[i] = VP.m[i][1]; c2[i] = VP.m[i][2]; }
    float P20 = c0[0] * vz[0] + c0[1] * vz[1] + c0[2] * vz[2];
    float P21 = c1[0] * vz[0] + c1[1] * vz[1] + c1[2] * vz[2];
    float P22 = c2[0] * vz[0] + c2[1] * vz[1] + c2[2] * vz[2];
    float vx[4], vy[4];
    for (int i = 0; i < 4; ++i) { vx[i] = c0[i] - P20 * vz[i]; vy[i] = c1[i] - P21 * vz[i]; }
    float P00 = std::sqrt(vx[0] * vx[0] + vx[1] * vx[1] + vx[2] * vx[2]);
    float P11 = std::sqrt(vy[0] * vy[0] + vy[1] * vy[1] + vy[2] * vy[2]);
    if (!(P00 > 1e-4f && P11 > 1e-4f && P00 < 1e4f && P11 < 1e4f)) return false;
    for (int i = 0; i < 4; ++i) { vx[i] /= P00; vy[i] /= P11; }
    // the axes must be orthonormal (a real rotation): rejects matrices that only look like a camera
    float dxy = vx[0] * vy[0] + vx[1] * vy[1] + vx[2] * vy[2], dxz = vx[0] * vz[0] + vx[1] * vz[1] + vx[2] * vz[2];
    if (std::fabs(dxy) > 2e-3f || std::fabs(dxz) > 2e-3f) return false;
    float P32 = c2[3] - P22 * vz[3];
    M4 V = Identity(), P{};
    for (int i = 0; i < 4; ++i) { V.m[i][0] = vx[i]; V.m[i][1] = vy[i]; V.m[i][2] = vz[i]; V.m[i][3] = i == 3 ? 1.0f : 0.0f; }
    P.m[0][0] = P00; P.m[1][1] = P11; P.m[2][0] = P20; P.m[2][1] = P21; P.m[2][2] = P22; P.m[2][3] = n; P.m[3][2] = P32;
    c->V = V; c->P = P;
    float dev = 0;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) dev = std::fmax(dev, std::fabs(V.m[i][j] - (i == j ? 1.0f : 0.0f)));
    c->ui = dev < 1e-5f;
    M4 iV;
    if (!Inverse(V, &iV)) return false;
    for (int i = 0; i < 3; ++i) c->pos[i] = iV.m[3][i];
    c->fovH = 2 * std::atan(n / P00) * 57.2957795f;
    c->fovV = 2 * std::atan(n / P11) * 57.2957795f;
    return true;
}

// Q maps (x*w, y*w, w, 1) (screen position times scene depth) back to clip space: ScreenToWorld = rows of Q * inv(VP).
inline M4 Q(const Camera& c) {
    M4 q{};
    q.m[0][0] = 1; q.m[1][1] = 1; q.m[2][2] = c.P.m[2][2]; q.m[2][3] = 1; q.m[3][2] = c.P.m[3][2];
    return q;
}

} // namespace ue3
