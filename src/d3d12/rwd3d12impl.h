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
void deferRelease(IUnknown *object);

bool32 allocateShaderResourceDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE *gpu);
bool32 getSamplerView(uint32 filter, uint32 addressU, uint32 addressV,
                      D3D12_GPU_DESCRIPTOR_HANDLE *gpu);
bool32 allocateDepthDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu);
bool32 allocateRenderTargetDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu);

bool32 getDepthTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
bool32 getColorTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
bool32 getRasterResource(Raster *raster, ID3D12Resource **resource);
bool32 transitionRaster(Raster *raster, D3D12_RESOURCE_STATES state);
bool32 getTextureView(Raster *raster, D3D12_GPU_DESCRIPTOR_HANDLE *view,
                      bool32 *hasAlpha);

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
