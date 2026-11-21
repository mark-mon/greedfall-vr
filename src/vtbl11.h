#pragma once
/* D3D11 / DXGI vtable slot indices, computed in vtbl11.c from the SDK's C declarations. */
#define VT_D11DEV_LIST(X) X(CreateBuffer) X(CreateTexture2D) X(CreateVertexShader) X(CreatePixelShader) \
    X(CreateRenderTargetView) X(CreateShaderResourceView) X(CreateDepthStencilView) X(CreateUnorderedAccessView) X(CreateDeferredContext)
#define VT_D11CTX_LIST(X) X(VSSetConstantBuffers) X(PSSetShaderResources) X(PSSetShader) X(VSSetShader) X(DrawIndexed) X(Draw) \
    X(Map) X(Unmap) X(PSSetConstantBuffers) X(DrawIndexedInstanced) X(DrawInstanced) X(OMSetRenderTargets) \
    X(UpdateSubresource) X(ClearRenderTargetView) X(ClearDepthStencilView) X(CopyResource) \
    X(CopySubresourceRegion) X(ResolveSubresource) X(OMSetRenderTargetsAndUnorderedAccessViews) \
    X(Dispatch) X(DispatchIndirect) X(DrawAuto) X(DrawInstancedIndirect) X(DrawIndexedInstancedIndirect) X(VSSetShaderResources) \
    X(CSSetShaderResources) X(CSSetUnorderedAccessViews) X(ExecuteCommandList) X(FinishCommandList) X(RSSetViewports) \
    X(HSSetConstantBuffers) X(DSSetConstantBuffers) X(GSSetConstantBuffers)
#define VT_DXGISC_LIST(X) X(Present) X(ResizeBuffers)
#define VT_DXGIF_LIST(X) X(CreateSwapChain)
#define VT_DXGIF2_LIST(X) X(CreateSwapChainForHwnd)
#ifdef __cplusplus
extern "C" {
#endif
#define VT11_DECL(p) extern const int VT_##p;
#define D11DEV_D(m) VT11_DECL(D11DEV_##m)
#define D11CTX_D(m) VT11_DECL(D11CTX_##m)
#define DXGISC_D(m) VT11_DECL(DXGISC_##m)
#define DXGIF_D(m) VT11_DECL(DXGIF_##m)
#define DXGIF2_D(m) VT11_DECL(DXGIF2_##m)
VT_D11DEV_LIST(D11DEV_D)
VT_D11CTX_LIST(D11CTX_D)
VT_DXGISC_LIST(DXGISC_D)
VT_DXGIF_LIST(DXGIF_D)
VT_DXGIF2_LIST(DXGIF2_D)
#ifdef __cplusplus
}
#endif
