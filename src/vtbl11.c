/* Computes D3D11 / DXGI COM vtable slot indices from the SDK's C interface declarations. */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "vtbl11.h"
#define D11DEV(m) const int VT_D11DEV_##m = (int)(offsetof(ID3D11DeviceVtbl, m) / sizeof(void*));
#define D11CTX(m) const int VT_D11CTX_##m = (int)(offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void*));
#define DXGISC(m) const int VT_DXGISC_##m = (int)(offsetof(IDXGISwapChainVtbl, m) / sizeof(void*));
#define DXGIF(m) const int VT_DXGIF_##m = (int)(offsetof(IDXGIFactoryVtbl, m) / sizeof(void*));
#define DXGIF2(m) const int VT_DXGIF2_##m = (int)(offsetof(IDXGIFactory2Vtbl, m) / sizeof(void*));
VT_D11DEV_LIST(D11DEV)
VT_D11CTX_LIST(D11CTX)
VT_DXGISC_LIST(DXGISC)
VT_DXGIF_LIST(DXGIF)
VT_DXGIF2_LIST(DXGIF2)
