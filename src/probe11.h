#pragma once
#include <d3d11.h>
#include <string>
namespace fvr {
// content probe (test aid): hashes a 64x64 patch at the center of a texture every frame (read back 3 frames later)
// and counts how often the content changed -> tells which stage of the pipeline is stale.
void ProbeTex(int id, const char* name, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src);
std::string ProbeLine();   // "content changes: name a/b ..." since the last call, then resets
}
