#pragma once

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

namespace rw {
struct Raster;

namespace d3d12 {

ID3D12Device *getDevice(void);
ID3D12CommandQueue *getCommandQueue(void);
ID3D12GraphicsCommandList *getCommandList(void);
ID3D12DescriptorHeap *getShaderResourceHeap(void);
ID3D12DescriptorHeap *getSamplerHeap(void);
uint32 getFrameIndex(void);
void getPresentSize(int32 *width, int32 *height);
void setPresentInterval(uint32 interval);
// Size of the camera raster currently bound by beginUpdate.  Im2D vertices
// are expressed in pixels of that raster, which can differ from the desktop
// swapchain for VR eyes, HUD layers and other camera textures.
void getCurrentRenderTargetSize(int32 *width, int32 *height);
bool32 readPresentedFrame(uint8 *pixels, uint32 stride,
                          int32 width, int32 height);
bool32 prepareForReadback(void);
// Submit the active command list to the graphics queue without stalling the
// CPU. OpenXR owns completion synchronization after xrReleaseSwapchainImage.
bool32 submitForExternal(void);
// Submit the currently open graphics list and wait until resources shared with
// an external compositor (OpenXR) are safe to release.
bool32 submitAndWaitForExternal(void);
bool32 copyCurrentBackBufferToExternal(ID3D12Resource *destination);
bool32 uploadRgbaToExternal(ID3D12Resource *destination, const uint8 *pixels,
                            uint32 stride, int32 width, int32 height);
void deferRelease(IUnknown *object);
// Upload command allocators, lists and buffers are submitted before the next
// presented frame. Keep them alive until that frame's fence has completed.
void deferReleaseAfterNextSubmit(IUnknown *object);

bool32 allocateShaderResourceDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE *gpu,
                                        uint32 *index);
bool32 getSamplerView(uint32 filter, uint32 addressU, uint32 addressV,
                      D3D12_GPU_DESCRIPTOR_HANDLE *gpu);
bool32 allocateDepthDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                               uint32 *index);
bool32 allocateRenderTargetDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                      uint32 *index);
void deferDescriptorRelease(uint32 srvIndex, uint32 rtvIndex,
                            uint32 dsvIndex);

bool32 getDepthTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
bool32 getColorTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
bool32 getRasterResource(Raster *raster, ID3D12Resource **resource);
bool32 transitionRaster(Raster *raster, D3D12_RESOURCE_STATES state);
bool32 getTextureView(Raster *raster, D3D12_GPU_DESCRIPTOR_HANDLE *view,
                      bool32 *hasAlpha);
// Draw a camera texture into an external typeless RGBA8 target while remapping
// its UV rectangle. Used by OpenXR to convert the stable symmetric game view
// into each runtime-provided asymmetric eye frustum.
bool32 resolveRasterToExternal(Raster *source, ID3D12Resource *destination,
                               int32 width, int32 height,
                               float32 uvScaleX, float32 uvScaleY,
                               float32 uvOffsetX, float32 uvOffsetY,
                               bool32 fxaaEnabled, uint32 colorMode,
                               const float32 blurColor[4],
                               const float32 contrastMult[3],
                               const float32 contrastAdd[3]);

bool32 initializeImmediate(void);
void shutdownImmediate(void);
void setRenderState(int32 state, void *value);
void *getRenderState(int32 state);
void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1,
                       int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType type, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType type, void *vertices,
                               int32 numVertices, void *indices,
                               int32 numIndices);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world,
                   uint32 flags);
void im3DRenderPrimitive(PrimitiveType type);
void im3DRenderIndexedPrimitive(PrimitiveType type, void *indices,
                               int32 numIndices);
void im3DEnd(void);

}
}
#endif
