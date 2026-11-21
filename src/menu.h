#pragma once
// In-game settings menu (vrkit, D3D9 backend): hold L3+R3 or press Insert. See claude/settings-menu.md.
#include <d3d9.h>
#include <d3d11.h>

namespace fvr {

void MenuInit(IDirect3DDevice9* dev);   // once the device exists (BeforePresent)
bool MenuUpdate();                       // each frame: input, open/close; true while open
// Draw the open menu onto `surface` inside `rect` (the HUD region of the image going to the headset).
void MenuRender(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const RECT& rect);
bool MenuBlocksGameInput();              // the game should see no pad / keyboard input now
void MenuOnLostDevice();
// D3D11 games (same menu, D3D11 backend): init once the device and the game image size are known; Update each frame
// (input; redraws the menu texture while open); the texture is R8G8B8A8_TYPELESS, premultiplied, opaque panel.
bool MenuInit11(ID3D11Device* dev, ID3D11DeviceContext* imm, UINT screenW, UINT screenH);
bool MenuUpdate11();
ID3D11Texture2D* MenuTexture11();
void MenuShutdown11();   // the game's device is going away

} // namespace fvr
