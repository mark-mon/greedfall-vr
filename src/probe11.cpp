#include "probe11.h"
#include <cstdio>
#include <cstdint>
namespace fvr {
namespace {
struct Probe {
    const char* name = nullptr; ID3D11Texture2D* st[4] = {}; bool used[4] = {}; UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    int i = 0; uint64_t last = 0; int changes = 0, n = 0, notReady = 0;
};
Probe g_p[4];
UINT Bpp(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}
}
void ProbeTex(int id, const char* name, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    if (id < 0 || id >= 4 || !src || !dev || !ctx) return;
    Probe& p = g_p[id]; p.name = name;
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
    if (d.SampleDesc.Count > 1 || d.Width < 64 || d.Height < 64) return;
    const UINT S = 64;
    if (p.fmt != d.Format || !p.st[0]) {
        for (auto& t : p.st) if (t) { t->Release(); t = nullptr; }
        D3D11_TEXTURE2D_DESC s{}; s.Width = S; s.Height = S; s.MipLevels = 1; s.ArraySize = 1; s.Format = d.Format; s.SampleDesc = {1, 0};
        s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& t : p.st) if (FAILED(dev->CreateTexture2D(&s, nullptr, &t))) return;
        p.fmt = d.Format; p.i = 0; for (auto& u : p.used) u = false;
    }
    // read back the oldest copy (3 frames ago)
    int r = (p.i + 1) % 4;
    if (p.used[r]) {
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = ctx->Map(p.st[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { p.notReady++; hr = ctx->Map(p.st[r], 0, D3D11_MAP_READ, 0, &m); }
        if (SUCCEEDED(hr)) {
            uint64_t h = 1469598103934665603ull; UINT rowB = S * Bpp(p.fmt);
            for (UINT y = 0; y < S; ++y) { const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch; for (UINT x = 0; x < rowB; ++x) { h ^= row[x]; h *= 1099511628211ull; } }
            ctx->Unmap(p.st[r], 0);
            p.n++; if (h != p.last) { p.changes++; p.last = h; }
        }
        p.used[r] = false;
    }
    D3D11_BOX b{d.Width / 2 - S / 2, d.Height / 2 - S / 2, 0, d.Width / 2 + S / 2, d.Height / 2 + S / 2, 1};
    ctx->CopySubresourceRegion(p.st[p.i], 0, 0, 0, 0, src, 0, &b);
    p.used[p.i] = true;
    p.i = (p.i + 1) % 4;
}
std::string ProbeLine() {
    std::string s = "content changes (center patch, frames that differ from the previous frame):";
    char b[160]; bool any = false;
    for (auto& p : g_p) if (p.name && p.n) { snprintf(b, sizeof b, " %s %d of %d%s;", p.name, p.changes, p.n, p.notReady ? " (some waited for GPU)" : ""); s += b; any = true; p.changes = p.n = p.notReady = 0; }
    return any ? s : std::string();
}
}
