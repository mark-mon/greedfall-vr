// Value matching of UE3 camera constants (see cbmatch.h).
#include "cbmatch.h"

namespace ue3 {

static bool Near(float a, float b, float rel, float abs_) { return std::fabs(a - b) <= abs_ + rel * std::fabs(b); }

static bool PlausibleCamera(const Camera& c) {
    // UE3 perspective cameras: 5..170 degrees, positive near plane (P32 < 0 with P22 > 0, or reversed-Z P22 < 1 and
    // P32 > 0 as seen in Ethan Carter), not the identity-rotation UI camera
    if (c.ui) return false;
    {   // a projection matrix on its own (rotation identity or z-flip, no translation) decomposes as a camera at the
        // origin: GreedFall keeps View, Projection and ViewProjection side by side - only the full VP is the camera
        float dev = 0, t = std::fabs(c.V.m[3][0]) + std::fabs(c.V.m[3][1]) + std::fabs(c.V.m[3][2]);
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) dev = std::fmax(dev, std::fabs(std::fabs(c.V.m[i][j]) - (i == j ? 1.0f : 0.0f)));
        if (dev < 1e-4f && t < 1e-3f) return false;
    }
    if (!(c.fovH > 5 && c.fovH < 170 && c.fovV > 3 && c.fovV < 170)) return false;
    const float P00 = c.P.m[0][0], P11 = c.P.m[1][1], P20 = c.P.m[2][0], P21 = c.P.m[2][1], P22 = c.P.m[2][2], P32 = c.P.m[3][2];
    if (std::fabs(P20) > 0.3f || std::fabs(P21) > 0.3f) return false;             // (nearly) centred projection
    float aspect = P11 / P00;                                                         // width / height
    if (!(aspect > 0.4f && aspect < 4.f)) return false;
    bool standard = P22 > 0.5f && P22 < 1.0001f && P32 < 0 && -P32 / P22 > 0.01f && -P32 / P22 < 1000.f;
    bool reversed = P22 >= -0.0001f && P22 <= 0.5f && P32 > 0;                         // reversed / infinite-reversed Z
    if (!standard && !reversed) return false;
    return std::isfinite(c.pos[0]) && std::isfinite(c.pos[1]) && std::isfinite(c.pos[2]);
}

bool FindViewProjection(const float* f4, int n, CbLayout* L, Camera* cam) {
    for (int r = 0; r + 4 <= n; ++r)
        for (int t = 0; t < 2; ++t) {
            Camera c;
            if (!Decompose(LoadM4(f4, r, t == 1), &c) || !PlausibleCamera(c)) continue;
            L->vp = r; L->vpT = t == 1;
            if (cam) *cam = c;
            return true;
        }
    return false;
}

bool VerifyViewProjection(const float* f4, int n, const CbLayout& L, Camera* cam) {
    if (L.vp < 0 || L.vp + 4 > n) return false;
    Camera c;
    if (!Decompose(LoadM4(f4, L.vp, L.vpT), &c) || c.ui) return false;
    if (cam) *cam = c;
    return true;
}

void FindDerived(const float* f4, int n, const M4& VP, const Camera& cam, CbLayout* L, int skip) {
    auto skipped = [&](int r, int len) { return skip >= 0 && r < skip + 4 && r + len > skip; };
    // camera position: xyz within 0.1 % (+ 0.01 units)
    if (L->cam < 0)
        for (int r = 0; r < n; ++r) {
            if (skipped(r, 1)) continue;
            const float* v = f4 + r * 4;
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) ok = Near(v[i], cam.pos[i], 1e-3f, 1e-2f);
            float mag = std::fabs(cam.pos[0]) + std::fabs(cam.pos[1]) + std::fabs(cam.pos[2]);
            if (ok && mag > 1.f) { L->cam = r; break; }   // a camera at the origin would match zeros everywhere
        }
    if (L->cam >= 0 && L->cam2 < 0)
        for (int r = L->cam + 1; r < n; ++r) {
            if (skipped(r, 1)) continue;
            const float* v = f4 + r * 4;
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) ok = Near(v[i], cam.pos[i], 1e-3f, 1e-2f);
            if (ok && std::fabs(v[3] - 1.f) < 1e-3f) { L->cam2 = r; break; }
        }
    // ScreenToWorld = first 3 or 4 rows of Q * inv(VP)
    M4 iVP;
    if (!Inverse(VP, &iVP)) return;
    M4 s2w = Mul(Q(cam), iVP);
    auto rowsMatch = [&](int r, const M4& m, int rows, bool transposed) {
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < 4; ++j) {
                float want = transposed ? m.m[j][i] : m.m[i][j];
                if (!Near(f4[(r + i) * 4 + j], want, 2e-3f, 1e-6f)) return false;
            }
        return true;
    };
    auto nonTrivial = [&](const M4& m, int rows) {   // avoid matching runs of zeros against an almost-zero row
        float s = 0;
        for (int i = 0; i < rows; ++i) for (int j = 0; j < 4; ++j) s += std::fabs(m.m[i][j]);
        return s > 1e-6f;
    };
    if (L->s2w < 0 && nonTrivial(s2w, 3))
        for (int r = 0; r + 3 <= n; ++r) {
            if (skipped(r, 3) || !rowsMatch(r, s2w, 3, false)) continue;
            L->s2w = r;
            L->s2wN = (r + 4 <= n && rowsMatch(r, s2w, 4, false)) ? 4 : 3;
            break;
        }
    // separate View and Projection matrices (VP = V * P), left-handed as decomposed or right-handed (z flipped)
    if (L->v < 0 || L->p < 0) {
        M4 Vs = cam.V, Ps = cam.P;
        for (int i = 0; i < 4; ++i) { Vs.m[i][2] = -Vs.m[i][2]; Ps.m[2][i] = -Ps.m[2][i]; }
        for (int r = 0; r + 4 <= n; ++r) {
            if (skipped(r, 4)) continue;
            for (int t = 0; t < 2; ++t) {
                bool T = t == 1;
                if (L->v < 0 && rowsMatch(r, cam.V, 4, T)) { L->v = r; L->vT = T; L->vS = false; }
                else if (L->v < 0 && rowsMatch(r, Vs, 4, T)) { L->v = r; L->vT = T; L->vS = true; }
                else if (L->p < 0 && rowsMatch(r, cam.P, 4, T)) { L->p = r; L->pT = T; L->pS = false; }
                else if (L->p < 0 && rowsMatch(r, Ps, 4, T)) { L->p = r; L->pT = T; L->pS = true; }
            }
        }
    }
    // the VP itself (buffers without a camera of their own: pixel shaders of deferred passes)
    if (L->vp < 0 && skip < 0)
        for (int r = 0; r + 4 <= n && L->vp < 0; ++r)
            for (int t = 0; t < 2; ++t) if (rowsMatch(r, VP, 4, t == 1)) { L->vp = r; L->vpT = t == 1; break; }
    // inverse view (camera to world), left- or right-handed
    if (L->iv < 0) {
        M4 iV;
        if (Inverse(cam.V, &iV)) {
            M4 iVs = iV;
            for (int j = 0; j < 4; ++j) iVs.m[2][j] = -iVs.m[2][j];
            for (int r = 0; r + 4 <= n && L->iv < 0; ++r) {
                if (skipped(r, 4)) continue;
                for (int t = 0; t < 2; ++t) {
                    if (rowsMatch(r, iV, 4, t == 1)) { L->iv = r; L->ivT2 = t == 1; L->ivS = false; break; }
                    if (rowsMatch(r, iVs, 4, t == 1)) { L->iv = r; L->ivT2 = t == 1; L->ivS = true; break; }
                }
            }
        }
    }
    // previous frame's camera (same layout again: VP, and V / P if present) - a second plausible camera close to this one
    if (L->vp2 < 0 && L->vp >= 0) {
        auto used = [&](int r) {
            auto in = [&](int a, int len) { return a >= 0 && r < a + len && r + 4 > a; };
            return in(L->vp, 4) || in(L->v, 4) || in(L->p, 4) || in(L->iv, 4) || in(L->ivp, 4) || in(L->s2w, 4);
        };
        for (int r = 0; r + 4 <= n && L->vp2 < 0; ++r) {
            if (used(r)) continue;
            for (int t = 0; t < 2; ++t) {
                Camera c2;
                M4 m = LoadM4(f4, r, t == 1);
                if (!Decompose(m, &c2) || !PlausibleCamera(c2)) continue;
                float d = 0, dot = 0;
                for (int i = 0; i < 3; ++i) { d += (c2.pos[i] - cam.pos[i]) * (c2.pos[i] - cam.pos[i]); dot += c2.V.m[i][2] * cam.V.m[i][2]; }
                if (d > 1e6f || dot < 0.8f) continue;   // within 10 m (cm units) and nearly the same direction
                L->vp2 = r; L->vp2T = t == 1;
                for (int q = r + 4; q + 4 <= n && L->vp2b < 0; ++q) {   // an identical copy further on
                    if (used(q)) continue;
                    for (int u = 0; u < 2; ++u) if (rowsMatch(q, m, 4, u == 1)) { L->vp2b = q; L->vp2bT = u == 1; break; }
                }
                M4 Vs = c2.V, Ps = c2.P;
                for (int i = 0; i < 4; ++i) { Vs.m[i][2] = -Vs.m[i][2]; Ps.m[2][i] = -Ps.m[2][i]; }
                for (int q = 0; q + 4 <= n; ++q) {
                    if (used(q) || (q < r + 4 && q + 4 > r)) continue;
                    for (int u = 0; u < 2; ++u) {
                        bool T = u == 1;
                        if (L->v2 < 0 && rowsMatch(q, c2.V, 4, T)) { L->v2 = q; L->v2T = T; L->v2S = false; }
                        else if (L->v2 < 0 && rowsMatch(q, Vs, 4, T)) { L->v2 = q; L->v2T = T; L->v2S = true; }
                        else if (L->p2 < 0 && rowsMatch(q, c2.P, 4, T)) { L->p2 = q; L->p2T = T; L->p2S = false; }
                        else if (L->p2 < 0 && rowsMatch(q, Ps, 4, T)) { L->p2 = q; L->p2T = T; L->p2S = true; }
                    }
                }
                break;
            }
        }
    }
    if (L->ivp < 0 && nonTrivial(iVP, 4))
        for (int r = 0; r + 4 <= n; ++r)
            for (int t = 0; t < 2; ++t) {
                if (skipped(r, 4) || !rowsMatch(r, iVP, 4, t == 1)) continue;
                L->ivp = r; L->ivpT = t == 1;
                r = n;
                break;
            }
}

} // namespace ue3
