#pragma once
// Finds UE3's camera constants in a shader's constant data by VALUE (D3D11 UE3 games ship shaders without reflection
// data, so there are no names as in D3D9's CTAB). Works on any float4 array: a D3D11 constant buffer's shadow copy or
// D3D9 constant registers.
//
//   ViewProjectionMatrix   any 4 consecutive float4 that decompose into a perspective camera (row or column order)
//   CameraPosition         a float4 whose xyz equals that camera's world position
//   ScreenToWorld          3-4 consecutive float4 equal to the first rows of Q * inverse(VP)
//   InvViewProjection      4 float4 equal to inverse(VP) (row or column order)
//
// A layout is found once per (shader, constant buffer slot) and then only verified (Decompose at the known offset).
#include "ue3math.h"
#include <cstddef>

namespace ue3 {

struct CbLayout {
    int vp = -1;  bool vpT = false;      // float4 index of ViewProjectionMatrix; stored transposed (column order)
    int cam = -1;                         // CameraPosition
    int s2w = -1, s2wN = 0;               // ScreenToWorld rows
    int ivp = -1; bool ivpT = false;      // inverse view-projection
    int v = -1; bool vT = false, vS = false;   // separate view matrix (vS: right-handed, z column negated)
    int p = -1; bool pT = false, pS = false;   // separate projection matrix (pS: right-handed, z row negated)
    int iv = -1; bool ivT2 = false, ivS = false;  // inverse view (camera-to-world; ivS: right-handed)
    int cam2 = -1;                        // a second copy of the camera position
    int vp2 = -1; bool vp2T = false;      // previous frame's view-projection (temporal AA / motion vectors)
    int vp2b = -1; bool vp2bT = false;    // a second copy of it (GreedFall cb0: rows 63 and 78; motion vectors read 78)
    int v2 = -1; bool v2T = false, v2S = false;
    int p2 = -1; bool p2T = false, p2S = false;
    bool any() const { return vp >= 0 || cam >= 0 || s2w >= 0 || ivp >= 0 || v >= 0 || p >= 0 || iv >= 0; }
};

// Reads the 4x4 matrix at float4 index `reg` (transposed: stored as columns).
inline M4 LoadM4(const float* f4, int reg, bool transposed) {
    M4 m;
    std::memcpy(m.m, f4 + reg * 4, sizeof(m.m));
    return transposed ? Transpose(m) : m;
}

// Searches n float4 for a perspective view-projection (not a UI camera). Returns true and fills layout.vp/vpT, cam.
bool FindViewProjection(const float* f4, int n, CbLayout* L, Camera* cam);
// With a known camera (from this buffer or the frame's main view): finds CameraPosition, ScreenToWorld, inverse VP.
// `skip`: float4 range [skip, skip+4) to leave out (the VP itself).
void FindDerived(const float* f4, int n, const M4& VP, const Camera& cam, CbLayout* L, int skip = -1);
// Quick check that the layout still holds a camera at the cached offset (game data may differ per draw).
bool VerifyViewProjection(const float* f4, int n, const CbLayout& L, Camera* cam);

} // namespace ue3
