# Third-party code

| Component | Where | License |
|---|---|---|
| OpenXR SDK (headers + static loader) | `third_party/openxr` | Apache-2.0 (see `third_party/openxr/LICENSE`) |
| Dear ImGui (Omar Cornut) | `shared/vrkit/third_party/imgui` | MIT |
| MinHook (Tsuda Kageyu) | `shared/xrbridge/third_party/minhook` | BSD 2-Clause |
| LumaSharpen (CeeJay.dk, ReShade shader, ported to an HLSL pass in `src/d3d11hooks.cpp`) | - | MIT |

NVIDIA variable rate shading is used through `nvapi64.dll` from the installed driver (no NVAPI code is shipped).
