#pragma once
// UEVR3 D3D11 backend (stage 1: recon - observes D3D11 UE3 games and logs where the camera constants are).
namespace fvr {
void InstallD3D11Hooks();
void D3D11LastFrame(bool* stereo, unsigned* patched);
bool D3D11ApplyRenderScale();
void D3D11RenderScaleChanged();
void D3D11RenderScaleTick();   // menu: apply render_scale ~1 s after the last change   // menu: render_scale live (resizes the game window -> the game resizes its targets)
void D3D11RenderSize(unsigned gameW, unsigned gameH, unsigned* w, unsigned* h);   // VR render size (0 = keep the game's)   // previous frame: stereo, patched draws (menu status line)
}
