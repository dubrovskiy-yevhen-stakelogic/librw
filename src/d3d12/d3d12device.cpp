#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <vector>

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwd3d12.h"
#include "rwd3d12impl.h"

namespace rw {
namespace d3d12 {

static void*
driverOpen(void *object, int32, int32)
{
	Driver *driver = engine->driver[PLATFORM_D3D12];
	driver->rasterNativeOffset = nativeRasterOffset;
	driver->rasterCreate = rasterCreate;
	driver->rasterLock = rasterLock;
	driver->rasterUnlock = rasterUnlock;
	driver->rasterLockPalette = null::rasterLockPalette;
	driver->rasterUnlockPalette = null::rasterUnlockPalette;
	driver->rasterNumLevels = rasterNumLevels;
	driver->imageFindRasterFormat = imageFindRasterFormat;
	driver->rasterFromImage = rasterFromImage;
	driver->rasterToImage = rasterToImage;
	driver->defaultPipeline = makeDefaultPipeline();
#ifdef RW_D3D12
	initializeImmediate();
#endif
	return object;
}

static void*
driverClose(void *object, int32, int32)
{
#ifdef RW_D3D12
	shutdownImmediate();
#endif
	shutdownDefaultPipeline();
	Driver *driver = engine->driver[PLATFORM_D3D12];
	if(driver->defaultPipeline &&
	   driver->defaultPipeline != engine->dummyDefaultPipeline){
		driver->defaultPipeline->destroy();
		driver->defaultPipeline = engine->dummyDefaultPipeline;
	}
	return object;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_D3D12, 0, PLATFORM_D3D12,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

#ifdef RW_D3D12

enum {
	FRAME_COUNT = 3,
	MAX_RENDER_TARGET_DESCRIPTORS = 256,
	MAX_DEPTH_DESCRIPTORS = 128,
	MAX_SHADER_RESOURCE_DESCRIPTORS = 16384,
	MAX_SAMPLER_DESCRIPTORS = 128,
	SAMPLER_FILTER_COUNT = 7,
	SAMPLER_ADDRESS_COUNT = 5
};

struct D3D12Context
{
	HWND window;
	IDXGIFactory4 *factory;
	IDXGIFactory6 *factory6;
	IDXGIAdapter1 *adapter;
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	ID3D12Fence *fence;
	IDXGISwapChain3 *swapChain;
	ID3D12DescriptorHeap *rtvHeap;
	ID3D12DescriptorHeap *dsvHeap;
	ID3D12DescriptorHeap *srvHeap;
	ID3D12DescriptorHeap *samplerHeap;
	ID3D12Resource *backBuffers[FRAME_COUNT];
	ID3D12CommandAllocator *commandAllocators[FRAME_COUNT];
	ID3D12GraphicsCommandList *commandList;
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[FRAME_COUNT];
	HANDLE fenceEvent;
	UINT64 fenceValue;
	UINT64 frameFenceValues[FRAME_COUNT];
	UINT rtvDescriptorSize;
	UINT dsvDescriptorSize;
	UINT srvDescriptorSize;
	UINT samplerDescriptorSize;
	UINT nextRtvDescriptor;
	UINT nextDsvDescriptor;
	UINT nextSrvDescriptor;
	UINT nextSamplerDescriptor;
	UINT frameIndex;
	UINT lastPresentedFrame;
	UINT presentInterval;
	int32 width;
	int32 height;
	int32 currentTargetWidth;
	int32 currentTargetHeight;
	int32 desktopWidth;
	int32 desktopHeight;
	int32 currentVideoMode;
	char adapterName[128];
	bool32 initialized;
	bool32 presentationReady;
	bool32 frameOpen;
	bool32 backBufferRendering;
	bool32 hasPresentedFrame;
	D3D12_GPU_DESCRIPTOR_HANDLE samplerCache[SAMPLER_FILTER_COUNT][SAMPLER_ADDRESS_COUNT][SAMPLER_ADDRESS_COUNT];
	Raster *currentColorRaster;
	ID3D12Resource *currentColorResource;
	D3D12_CPU_DESCRIPTOR_HANDLE currentColorView;
	D3D12_CPU_DESCRIPTOR_HANDLE currentDepthView;
	std::vector<IUnknown*> deferredReleases[FRAME_COUNT];
	std::vector<IUnknown*> pendingSubmitReleases;
	std::vector<uint32> deferredSrvDescriptors[FRAME_COUNT];
	std::vector<uint32> deferredRtvDescriptors[FRAME_COUNT];
	std::vector<uint32> deferredDsvDescriptors[FRAME_COUNT];
	std::vector<uint32> freeSrvDescriptors;
	std::vector<uint32> freeRtvDescriptors;
	std::vector<uint32> freeDsvDescriptors;
};

static D3D12Context context;
static uint32 externalCopyLogCount;

static void
logExternalCopy(const char *message, const D3D12_RESOURCE_DESC *source = nil,
	            const D3D12_RESOURCE_DESC *destination = nil, HRESULT result = S_OK)
{
	if(externalCopyLogCount++ >= 32)
		return;
	FILE *file = fopen("d3d12_external_copy.log", externalCopyLogCount == 1 ? "w" : "a");
	if(file == nil)
		return;
	fprintf(file,
	        "%s hr=%08lX frameOpen=%d hasPresented=%d frame=%u last=%u ready=%d",
	        message, (unsigned long)result, (int)context.frameOpen,
	        (int)context.hasPresentedFrame, (unsigned)context.frameIndex,
	        (unsigned)context.lastPresentedFrame, (int)context.presentationReady);
	if(source)
		fprintf(file, " src=%llux%u fmt=%u samples=%u state=%s",
		        (unsigned long long)source->Width, (unsigned)source->Height,
		        (unsigned)source->Format, (unsigned)source->SampleDesc.Count,
		        context.frameOpen ? "render-target" : "present");
	if(destination)
		fprintf(file, " dst=%llux%u fmt=%u samples=%u",
		        (unsigned long long)destination->Width, (unsigned)destination->Height,
		        (unsigned)destination->Format,
		        (unsigned)destination->SampleDesc.Count);
	fputc('\n', file);
	fclose(file);
}

static D3D12_RESOURCE_BARRIER transitionBarrier(
	ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after);
static void finishFrame(void);

static bool32
areCopyCompatibleFormats(DXGI_FORMAT source, DXGI_FORMAT destination)
{
	if(source == destination)
		return 1;
	// OpenXR runtimes commonly expose an UNORM swapchain through a typeless
	// ID3D12Resource. D3D12 explicitly permits copies inside the same typeless
	// format family, so do not reject these resources before CopyResource.
	if((source == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	    source == DXGI_FORMAT_R8G8B8A8_UNORM ||
	    source == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
	   (destination == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	    destination == DXGI_FORMAT_R8G8B8A8_UNORM ||
	    destination == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
		return 1;
	return 0;
}

template<class T>
static void
releaseCom(T *&object)
{
	if(object){
		object->Release();
		object = nil;
	}
}

ID3D12Device*
getDevice(void)
{
	return context.device;
}

ID3D12CommandQueue*
getCommandQueue(void)
{
	return context.queue;
}

ID3D12GraphicsCommandList*
getCommandList(void)
{
	return context.frameOpen ? context.commandList : nil;
}

ID3D12DescriptorHeap*
getShaderResourceHeap(void)
{
	return context.srvHeap;
}

ID3D12DescriptorHeap*
getSamplerHeap(void)
{
	return context.samplerHeap;
}

uint32
getFrameIndex(void)
{
	return context.frameIndex;
}

void
getPresentSize(int32 *width, int32 *height)
{
	if(width)
		*width = context.width;
	if(height)
		*height = context.height;
}

void
setPresentInterval(uint32 interval)
{
	// OpenXR already provides the frame pacing for VR.  Waiting for the
	// desktop swapchain as well can serialize two unrelated refresh rates and
	// make scripted camera sequences run below real time.
	context.presentInterval = interval ? 1u : 0u;
}

void
getCurrentRenderTargetSize(int32 *width, int32 *height)
{
	const int32 targetWidth = context.currentTargetWidth > 0 ?
		context.currentTargetWidth : context.width;
	const int32 targetHeight = context.currentTargetHeight > 0 ?
		context.currentTargetHeight : context.height;
	if(width)
		*width = targetWidth;
	if(height)
		*height = targetHeight;
}

bool32
readPresentedFrame(uint8 *pixels, uint32 stride, int32 width, int32 height)
{
	if(!context.presentationReady || !context.hasPresentedFrame ||
	   pixels == nil || width <= 0 || height <= 0 ||
	   context.lastPresentedFrame >= FRAME_COUNT)
		return 0;
	// Screenshot capture runs after showRaster. Keep this path defensive for
	// callers that request a capture between EndUpdate and presentation.
	if(context.frameOpen)
		finishFrame();
	if(!waitForGpu())
		return 0;

	ID3D12Resource *source = context.backBuffers[context.lastPresentedFrame];
	if(source == nil)
		return 0;
	D3D12_RESOURCE_DESC texture = source->GetDesc();
	UINT copyWidth = (UINT)(width < (int32)texture.Width ? width : texture.Width);
	UINT copyHeight = (UINT)(height < (int32)texture.Height ? height : texture.Height);
	if(copyWidth == 0 || copyHeight == 0 || stride < copyWidth*4)
		return 0;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT rows = 0;
	UINT64 rowSize = 0;
	UINT64 bufferSize = 0;
	context.device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint,
	                                      &rows, &rowSize, &bufferSize);
	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = bufferSize;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *readback = nil;
	ID3D12CommandAllocator *allocator = nil;
	ID3D12GraphicsCommandList *list = nil;
	bool32 ok = SUCCEEDED(context.device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
		nil, IID_PPV_ARGS(&readback))) &&
		SUCCEEDED(context.device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(context.device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
			IID_PPV_ARGS(&list)));
	if(ok){
		D3D12_RESOURCE_BARRIER toCopy = transitionBarrier(
			source, D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->ResourceBarrier(1, &toCopy);
		D3D12_TEXTURE_COPY_LOCATION dst;
		memset(&dst, 0, sizeof(dst));
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = footprint;
		D3D12_TEXTURE_COPY_LOCATION src;
		memset(&src, 0, sizeof(src));
		src.pResource = source;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_BOX box = { 0, 0, 0, copyWidth, copyHeight, 1 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
		D3D12_RESOURCE_BARRIER toPresent = transitionBarrier(
			source, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_PRESENT);
		list->ResourceBarrier(1, &toPresent);
		ok = SUCCEEDED(list->Close());
	}
	if(ok){
		ID3D12CommandList *lists[] = { list };
		context.queue->ExecuteCommandLists(1, lists);
		ok = waitForGpu();
	}
	if(ok){
		uint8 *mapped = nil;
		D3D12_RANGE readRange = {
			(SIZE_T)footprint.Offset,
			(SIZE_T)(footprint.Offset + footprint.Footprint.RowPitch*copyHeight)
		};
		ok = SUCCEEDED(readback->Map(0, &readRange, (void**)&mapped));
		if(ok){
			for(UINT row = 0; row < copyHeight; row++)
				memcpy(pixels + row*stride,
				       mapped + footprint.Offset + row*footprint.Footprint.RowPitch,
				       copyWidth*4);
			D3D12_RANGE writtenRange = { 0, 0 };
			readback->Unmap(0, &writtenRange);
		}
	}
	releaseCom(list);
	releaseCom(allocator);
	releaseCom(readback);
	return ok;
}

bool32
prepareForReadback(void)
{
	if(context.device == nil || context.queue == nil)
		return 0;
	if(context.frameOpen)
		finishFrame();
	return waitForGpu();
}

bool32
submitForExternal(void)
{
	if(context.device == nil || context.queue == nil)
		return 0;
	if(context.frameOpen)
		finishFrame();
	return 1;
}

bool32
submitAndWaitForExternal(void)
{
	if(!submitForExternal())
		return 0;
	return waitForGpu();
}

bool32
copyCurrentBackBufferToExternal(ID3D12Resource *destination)
{
	if(destination == nil || context.device == nil || context.queue == nil){
		logExternalCopy("missing destination/device/queue");
		return 0;
	}
	const bool32 useOpenFrame = context.frameOpen && context.commandList != nil &&
		context.backBuffers[context.frameIndex] != nil;
	if(!useOpenFrame && (!context.hasPresentedFrame ||
	   context.lastPresentedFrame >= FRAME_COUNT ||
	   context.backBuffers[context.lastPresentedFrame] == nil)){
		logExternalCopy("no open or presented source");
		return 0;
	}
	ID3D12Resource *source = useOpenFrame ?
		context.backBuffers[context.frameIndex] :
		context.backBuffers[context.lastPresentedFrame];
	D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
	D3D12_RESOURCE_DESC destinationDesc = destination->GetDesc();
	if(sourceDesc.Dimension != destinationDesc.Dimension ||
	   sourceDesc.Width != destinationDesc.Width ||
	   sourceDesc.Height != destinationDesc.Height ||
	   !areCopyCompatibleFormats(sourceDesc.Format, destinationDesc.Format) ||
	   sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count){
		logExternalCopy("resource mismatch", &sourceDesc, &destinationDesc);
		return 0;
	}
	logExternalCopy(useOpenFrame ? "copy from open frame" : "copy from presented frame",
	                &sourceDesc, &destinationDesc);

	if(!useOpenFrame){
		// Menus and startup movies can close/present their regular frame before the
		// OpenXR cinema layer asks for it. Copy the last presented image through a
		// short independent list; this intentionally costs one frame of latency only
		// in the mono theatre path.
		if(!waitForGpu()){
			logExternalCopy("pre-copy wait failed", &sourceDesc, &destinationDesc);
			return 0;
		}
		ID3D12CommandAllocator *allocator = nil;
		ID3D12GraphicsCommandList *list = nil;
		bool32 ok = SUCCEEDED(context.device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
			SUCCEEDED(context.device->CreateCommandList(
				0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
				IID_PPV_ARGS(&list)));
		if(ok){
			D3D12_RESOURCE_BARRIER barriers[2] = {
				transitionBarrier(source, D3D12_RESOURCE_STATE_PRESENT,
				                  D3D12_RESOURCE_STATE_COPY_SOURCE),
				transitionBarrier(destination, D3D12_RESOURCE_STATE_COMMON,
				                  D3D12_RESOURCE_STATE_COPY_DEST)
			};
			list->ResourceBarrier(2, barriers);
			list->CopyResource(destination, source);
			barriers[0] = transitionBarrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
			                                D3D12_RESOURCE_STATE_PRESENT);
			barriers[1] = transitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
			                                D3D12_RESOURCE_STATE_COMMON);
			list->ResourceBarrier(2, barriers);
			ok = SUCCEEDED(list->Close());
		}
		if(ok){
			ID3D12CommandList *lists[] = { list };
			context.queue->ExecuteCommandLists(1, lists);
			ok = waitForGpu();
		}
		if(!ok)
			logExternalCopy("independent copy failed", &sourceDesc, &destinationDesc);
		releaseCom(list);
		releaseCom(allocator);
		return ok;
	}

	D3D12_RESOURCE_BARRIER barriers[2] = {
		transitionBarrier(source, D3D12_RESOURCE_STATE_RENDER_TARGET,
		                  D3D12_RESOURCE_STATE_COPY_SOURCE),
		transitionBarrier(destination, D3D12_RESOURCE_STATE_COMMON,
		                  D3D12_RESOURCE_STATE_COPY_DEST)
	};
	context.commandList->ResourceBarrier(2, barriers);
	context.commandList->CopyResource(destination, source);
	barriers[0] = transitionBarrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
	                                D3D12_RESOURCE_STATE_RENDER_TARGET);
	barriers[1] = transitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
	                                D3D12_RESOURCE_STATE_COMMON);
	context.commandList->ResourceBarrier(2, barriers);
	return 1;
}

bool32
uploadRgbaToExternal(ID3D12Resource *destination, const uint8 *pixels,
	                 uint32 stride, int32 width, int32 height)
{
	if(destination == nil || pixels == nil || context.device == nil ||
	   context.commandList == nil || !context.frameOpen ||
	   width <= 0 || height <= 0 || stride < (uint32)width*4)
		return 0;
	D3D12_RESOURCE_DESC texture = destination->GetDesc();
	if(texture.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
	   texture.Width != (UINT64)width || texture.Height != (UINT)height ||
	   !areCopyCompatibleFormats(DXGI_FORMAT_R8G8B8A8_UNORM, texture.Format))
		return 0;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT rows = 0;
	UINT64 rowSize = 0;
	UINT64 uploadSize = 0;
	context.device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint,
	                                     &rows, &rowSize, &uploadSize);
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC buffer = {};
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = uploadSize;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *upload = nil;
	if(FAILED(context.device->CreateCommittedResource(
	   &heap, D3D12_HEAP_FLAG_NONE, &buffer,
	   D3D12_RESOURCE_STATE_GENERIC_READ, nil, IID_PPV_ARGS(&upload))))
		return 0;
	uint8 *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED(upload->Map(0, &readRange, (void**)&mapped))){
		releaseCom(upload);
		return 0;
	}
	const uint32 copyBytes = (uint32)width*4;
	for(UINT row = 0; row < rows && row < (UINT)height; row++)
		memcpy(mapped + footprint.Offset + row*footprint.Footprint.RowPitch,
		       pixels + row*stride, copyBytes);
	upload->Unmap(0, nil);

	D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
		destination, D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COPY_DEST);
	context.commandList->ResourceBarrier(1, &barrier);
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = upload;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint = footprint;
	D3D12_TEXTURE_COPY_LOCATION target = {};
	target.pResource = destination;
	target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	target.SubresourceIndex = 0;
	context.commandList->CopyTextureRegion(&target, 0, 0, 0, &source, nil);
	barrier = transitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
	                           D3D12_RESOURCE_STATE_COMMON);
	context.commandList->ResourceBarrier(1, &barrier);
	deferRelease(upload);
	return 1;
}

static void
releaseDeferredFrame(uint32 frame)
{
	if(frame >= FRAME_COUNT)
		return;
	for(size_t i = 0; i < context.deferredReleases[frame].size(); i++)
		context.deferredReleases[frame][i]->Release();
	context.deferredReleases[frame].clear();
	context.freeSrvDescriptors.insert(context.freeSrvDescriptors.end(),
		context.deferredSrvDescriptors[frame].begin(),
		context.deferredSrvDescriptors[frame].end());
	context.freeRtvDescriptors.insert(context.freeRtvDescriptors.end(),
		context.deferredRtvDescriptors[frame].begin(),
		context.deferredRtvDescriptors[frame].end());
	context.freeDsvDescriptors.insert(context.freeDsvDescriptors.end(),
		context.deferredDsvDescriptors[frame].begin(),
		context.deferredDsvDescriptors[frame].end());
	context.deferredSrvDescriptors[frame].clear();
	context.deferredRtvDescriptors[frame].clear();
	context.deferredDsvDescriptors[frame].clear();
}

static void
releasePendingSubmitObjects(void)
{
	for(size_t i = 0; i < context.pendingSubmitReleases.size(); i++)
		context.pendingSubmitReleases[i]->Release();
	context.pendingSubmitReleases.clear();
}

void
deferRelease(IUnknown *object)
{
	if(object == nil)
		return;
	if(context.device == nil){
		object->Release();
		return;
	}
	context.deferredReleases[context.frameIndex % FRAME_COUNT].push_back(object);
}

void
deferReleaseAfterNextSubmit(IUnknown *object)
{
	if(object == nil)
		return;
	if(context.device == nil){
		object->Release();
		return;
	}
	context.pendingSubmitReleases.push_back(object);
}

void
deferDescriptorRelease(uint32 srvIndex, uint32 rtvIndex, uint32 dsvIndex)
{
	if(context.device == nil)
		return;
	uint32 frame = context.frameIndex % FRAME_COUNT;
	if(srvIndex != UINT32_MAX)
		context.deferredSrvDescriptors[frame].push_back(srvIndex);
	if(rtvIndex != UINT32_MAX && rtvIndex >= FRAME_COUNT)
		context.deferredRtvDescriptors[frame].push_back(rtvIndex);
	if(dsvIndex != UINT32_MAX)
		context.deferredDsvDescriptors[frame].push_back(dsvIndex);
}

bool32
allocateShaderResourceDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
	                             D3D12_GPU_DESCRIPTOR_HANDLE *gpu,
	                             uint32 *allocatedIndex)
{
	if(context.srvHeap == nil || cpu == nil || gpu == nil)
		return 0;
	uint32 index;
	if(!context.freeSrvDescriptors.empty()){
		index = context.freeSrvDescriptors.back();
		context.freeSrvDescriptors.pop_back();
	}else{
		if(context.nextSrvDescriptor >= MAX_SHADER_RESOURCE_DESCRIPTORS)
			return 0;
		index = context.nextSrvDescriptor++;
	}
	*cpu = context.srvHeap->GetCPUDescriptorHandleForHeapStart();
	*gpu = context.srvHeap->GetGPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)index*context.srvDescriptorSize;
	gpu->ptr += (UINT64)index*context.srvDescriptorSize;
	if(allocatedIndex)
		*allocatedIndex = index;
	return 1;
}

bool32
getSamplerView(uint32 filter, uint32 addressU, uint32 addressV,
               D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
	if(context.samplerHeap == nil || context.device == nil || gpu == nil)
		return 0;
	if(filter < 1 || filter >= SAMPLER_FILTER_COUNT)
		filter = 2;
	if(addressU < 1 || addressU >= SAMPLER_ADDRESS_COUNT)
		addressU = 1;
	if(addressV < 1 || addressV >= SAMPLER_ADDRESS_COUNT)
		addressV = 1;
	D3D12_GPU_DESCRIPTOR_HANDLE &cached =
		context.samplerCache[filter][addressU][addressV];
	if(cached.ptr != 0){
		*gpu = cached;
		return 1;
	}
	if(context.nextSamplerDescriptor >= MAX_SAMPLER_DESCRIPTORS)
		return 0;

	static const D3D12_FILTER filters[SAMPLER_FILTER_COUNT] = {
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_FILTER_MIN_MAG_MIP_POINT,
		D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		D3D12_FILTER_MIN_MAG_MIP_POINT,
		D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
		D3D12_FILTER_MIN_MAG_MIP_LINEAR
	};
	static const D3D12_TEXTURE_ADDRESS_MODE addresses[SAMPLER_ADDRESS_COUNT] = {
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_BORDER
	};
	D3D12_SAMPLER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Filter = filters[filter];
	desc.AddressU = addresses[addressU];
	desc.AddressV = addresses[addressV];
	desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	desc.MaxAnisotropy = 1;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.MinLOD = 0.0f;
	desc.MaxLOD = D3D12_FLOAT32_MAX;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu =
		context.samplerHeap->GetCPUDescriptorHandleForHeapStart();
	cached = context.samplerHeap->GetGPUDescriptorHandleForHeapStart();
	cpu.ptr += (SIZE_T)context.nextSamplerDescriptor*context.samplerDescriptorSize;
	cached.ptr += (UINT64)context.nextSamplerDescriptor*context.samplerDescriptorSize;
	context.nextSamplerDescriptor++;
	context.device->CreateSampler(&desc, cpu);
	*gpu = cached;
	return 1;
}

bool32
allocateDepthDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
	                    uint32 *allocatedIndex)
{
	if(context.dsvHeap == nil || cpu == nil)
		return 0;
	uint32 index;
	if(!context.freeDsvDescriptors.empty()){
		index = context.freeDsvDescriptors.back();
		context.freeDsvDescriptors.pop_back();
	}else{
		if(context.nextDsvDescriptor >= MAX_DEPTH_DESCRIPTORS)
			return 0;
		index = context.nextDsvDescriptor++;
	}
	*cpu = context.dsvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)index*context.dsvDescriptorSize;
	if(allocatedIndex)
		*allocatedIndex = index;
	return 1;
}

bool32
allocateRenderTargetDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
	                           uint32 *allocatedIndex)
{
	if(context.rtvHeap == nil || cpu == nil)
		return 0;
	uint32 index;
	if(!context.freeRtvDescriptors.empty()){
		index = context.freeRtvDescriptors.back();
		context.freeRtvDescriptors.pop_back();
	}else{
		if(context.nextRtvDescriptor >= MAX_RENDER_TARGET_DESCRIPTORS)
			return 0;
		index = context.nextRtvDescriptor++;
	}
	*cpu = context.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)index*context.rtvDescriptorSize;
	if(allocatedIndex)
		*allocatedIndex = index;
	return 1;
}

static void
setAdapterName(IDXGIAdapter1 *adapter)
{
	DXGI_ADAPTER_DESC1 desc;
	memset(&desc, 0, sizeof(desc));
	if(adapter == nil || FAILED(adapter->GetDesc1(&desc))){
		strncpy(context.adapterName, "Unknown D3D12 adapter", sizeof(context.adapterName));
		context.adapterName[sizeof(context.adapterName)-1] = '\0';
		return;
	}

	int written = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
	                                  context.adapterName,
	                                  (int)sizeof(context.adapterName),
	                                  nil, nil);
	if(written == 0){
		strncpy(context.adapterName, "D3D12 adapter", sizeof(context.adapterName));
		context.adapterName[sizeof(context.adapterName)-1] = '\0';
	}
}

static bool32
tryCreateDevice(IDXGIAdapter1 *adapter)
{
	ID3D12Device *device = nil;
	if(FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
	                            IID_PPV_ARGS(&device))))
		return 0;

	context.adapter = adapter;
	context.adapter->AddRef();
	context.device = device;
	setAdapterName(adapter);
	return 1;
}

static bool32
selectAdapter(void)
{
	IDXGIAdapter1 *candidate = nil;

	if(context.factory6){
		for(UINT i = 0;
		    context.factory6->EnumAdapterByGpuPreference(
		        i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
		        IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
		    i++){
			DXGI_ADAPTER_DESC1 desc;
			candidate->GetDesc1(&desc);
			if((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
			   tryCreateDevice(candidate)){
				releaseCom(candidate);
				return 1;
			}
			releaseCom(candidate);
		}
	}else{
		for(UINT i = 0;
		    context.factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND;
		    i++){
			DXGI_ADAPTER_DESC1 desc;
			candidate->GetDesc1(&desc);
			if((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
			   tryCreateDevice(candidate)){
				releaseCom(candidate);
				return 1;
			}
			releaseCom(candidate);
		}
	}

	// WARP keeps the backend testable on machines without a hardware D3D12
	// adapter. Normal gameplay always reaches the high-performance path first.
	if(SUCCEEDED(context.factory->EnumWarpAdapter(IID_PPV_ARGS(&candidate)))){
		bool32 created = tryCreateDevice(candidate);
		releaseCom(candidate);
		return created;
	}
	return 0;
}

bool32
waitForGpu(void)
{
	if(context.queue == nil || context.fence == nil || context.fenceEvent == nil)
		return 0;

	const UINT64 value = ++context.fenceValue;
	if(FAILED(context.queue->Signal(context.fence, value)))
		return 0;
	if(context.fence->GetCompletedValue() < value){
		if(FAILED(context.fence->SetEventOnCompletion(value, context.fenceEvent)))
			return 0;
		WaitForSingleObject(context.fenceEvent, INFINITE);
	}
	// This signal is ordered after standalone texture-copy command lists.
	// Their temporary upload resources can now be released safely.
	releasePendingSubmitObjects();
	return 1;
}

static bool32
waitForFrame(UINT frameIndex)
{
	const UINT64 value = context.frameFenceValues[frameIndex];
	if(value == 0 || context.fence->GetCompletedValue() >= value)
		return 1;
	if(FAILED(context.fence->SetEventOnCompletion(value, context.fenceEvent)))
		return 0;
	WaitForSingleObject(context.fenceEvent, INFINITE);
	return 1;
}

static void
destroyFrameResources(void)
{
	context.frameOpen = 0;
	context.backBufferRendering = 0;
	context.currentColorRaster = nil;
	context.currentColorResource = nil;
	context.currentColorView.ptr = 0;
	context.currentDepthView.ptr = 0;
	context.currentTargetWidth = 0;
	context.currentTargetHeight = 0;
	releaseCom(context.commandList);
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		releaseCom(context.commandAllocators[i]);
		releaseCom(context.backBuffers[i]);
		context.frameFenceValues[i] = 0;
		context.rtvHandles[i].ptr = 0;
	}
	releaseCom(context.rtvHeap);
	releaseCom(context.swapChain);
	context.rtvDescriptorSize = 0;
	context.nextRtvDescriptor = FRAME_COUNT;
	context.frameIndex = 0;
	context.lastPresentedFrame = 0;
	context.hasPresentedFrame = 0;
	context.presentationReady = 0;
}

static bool32
createFrameResources(void)
{
	if(context.window == nil)
		return 1;

	DXGI_SWAP_CHAIN_DESC1 swapDesc;
	memset(&swapDesc, 0, sizeof(swapDesc));
	swapDesc.Width = context.width > 0 ? context.width : 1;
	swapDesc.Height = context.height > 0 ? context.height : 1;
	swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapDesc.SampleDesc.Count = 1;
	swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapDesc.BufferCount = FRAME_COUNT;
	swapDesc.Scaling = DXGI_SCALING_STRETCH;
	swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	IDXGISwapChain1 *swapChain1 = nil;
	HRESULT hr = context.factory->CreateSwapChainForHwnd(
		context.queue, context.window, &swapDesc, nil, nil, &swapChain1);
	if(FAILED(hr))
		return 0;
	context.factory->MakeWindowAssociation(context.window, DXGI_MWA_NO_ALT_ENTER);
	hr = swapChain1->QueryInterface(IID_PPV_ARGS(&context.swapChain));
	releaseCom(swapChain1);
	if(FAILED(hr))
		return 0;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc;
	memset(&heapDesc, 0, sizeof(heapDesc));
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heapDesc.NumDescriptors = MAX_RENDER_TARGET_DESCRIPTORS;
	if(FAILED(context.device->CreateDescriptorHeap(
	        &heapDesc, IID_PPV_ARGS(&context.rtvHeap))))
		return 0;

	context.rtvDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	context.nextRtvDescriptor = FRAME_COUNT;
	D3D12_CPU_DESCRIPTOR_HANDLE handle =
		context.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		if(FAILED(context.swapChain->GetBuffer(
		        i, IID_PPV_ARGS(&context.backBuffers[i]))) ||
		   FAILED(context.device->CreateCommandAllocator(
		        D3D12_COMMAND_LIST_TYPE_DIRECT,
		        IID_PPV_ARGS(&context.commandAllocators[i]))))
			return 0;
		context.rtvHandles[i] = handle;
		context.device->CreateRenderTargetView(context.backBuffers[i], nil, handle);
		handle.ptr += context.rtvDescriptorSize;
	}

	if(FAILED(context.device->CreateCommandList(
	        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
	        context.commandAllocators[0], nil,
	        IID_PPV_ARGS(&context.commandList))))
		return 0;
	if(FAILED(context.commandList->Close()))
		return 0;

	context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
	context.presentationReady = 1;
	return 1;
}

static bool32
resizeFrameResources(int32 width, int32 height)
{
	if(!context.presentationReady || context.swapChain == nil ||
	   width <= 0 || height <= 0)
		return 0;
	if(width == context.width && height == context.height)
		return 1;
	if(context.frameOpen)
		finishFrame();
	if(!waitForGpu())
		return 0;
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		releaseDeferredFrame(i);
		releaseCom(context.backBuffers[i]);
		context.frameFenceValues[i] = 0;
	}
	context.currentColorRaster = nil;
	context.currentColorResource = nil;
	context.currentColorView.ptr = 0;
	context.currentDepthView.ptr = 0;
	context.backBufferRendering = 0;
	context.hasPresentedFrame = 0;
	HRESULT hr = context.swapChain->ResizeBuffers(
		FRAME_COUNT, (UINT)width, (UINT)height,
		DXGI_FORMAT_R8G8B8A8_UNORM, 0);
	if(FAILED(hr)){
		context.presentationReady = 0;
		return 0;
	}
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		if(FAILED(context.swapChain->GetBuffer(
		       i, IID_PPV_ARGS(&context.backBuffers[i])))){
			context.presentationReady = 0;
			return 0;
		}
		context.device->CreateRenderTargetView(
			context.backBuffers[i], nil, context.rtvHandles[i]);
	}
	context.width = width;
	context.height = height;
	context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
	context.lastPresentedFrame = context.frameIndex;
	return 1;
}

static bool32
refreshFrameSize(void)
{
	if(context.window == nil)
		return 1;
	RECT rect;
	if(!GetClientRect(context.window, &rect))
		return 1;
	int32 width = rect.right - rect.left;
	int32 height = rect.bottom - rect.top;
	if(width <= 0 || height <= 0 ||
	   (width == context.width && height == context.height))
		return 1;
	return resizeFrameResources(width, height);
}

static D3D12_RESOURCE_BARRIER
transitionBarrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
	              D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

static bool32
beginFrame(Camera *camera)
{
	if(!context.presentationReady || !refreshFrameSize())
		return 0;
	if(!context.frameOpen){
		context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
		if(!waitForFrame(context.frameIndex))
			return 0;
		releaseDeferredFrame(context.frameIndex);
		if(FAILED(context.commandAllocators[context.frameIndex]->Reset()) ||
		   FAILED(context.commandList->Reset(
		       context.commandAllocators[context.frameIndex], nil)))
			return 0;

		D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
			context.backBuffers[context.frameIndex],
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.commandList->ResourceBarrier(1, &barrier);
		context.backBufferRendering = 1;
		if(context.srvHeap && context.samplerHeap){
			ID3D12DescriptorHeap *heaps[] = {
				context.srvHeap, context.samplerHeap
			};
			context.commandList->SetDescriptorHeaps(2, heaps);
		}
		context.frameOpen = 1;
	}

	Raster *frameBuffer = camera ? camera->frameBuffer : nil;
	Raster *parent = frameBuffer ? frameBuffer->parent : nil;
	Raster *colorRaster = parent && parent->type == Raster::CAMERATEXTURE ?
		parent : nil;
	if(context.currentColorRaster &&
	   context.currentColorRaster != colorRaster)
		transitionRaster(context.currentColorRaster,
		                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	context.currentColorRaster = colorRaster;
	context.currentColorResource = context.backBuffers[context.frameIndex];
	context.currentColorView = context.rtvHandles[context.frameIndex];
	if(colorRaster){
		if(!transitionRaster(colorRaster, D3D12_RESOURCE_STATE_RENDER_TARGET) ||
		   !getColorTarget(colorRaster, &context.currentColorResource,
		                   &context.currentColorView))
			return 0;
	}
	context.currentDepthView.ptr = 0;
	if(camera && camera->zBuffer)
		getDepthTarget(camera->zBuffer, nil, &context.currentDepthView);
	context.commandList->OMSetRenderTargets(
		1, &context.currentColorView, FALSE,
		context.currentDepthView.ptr ? &context.currentDepthView : nil);

	D3D12_VIEWPORT viewport;
	viewport.TopLeftX = frameBuffer && frameBuffer != parent ?
		(float)frameBuffer->offsetX : 0.0f;
	viewport.TopLeftY = frameBuffer && frameBuffer != parent ?
		(float)frameBuffer->offsetY : 0.0f;
	viewport.Width = (float)(frameBuffer ? frameBuffer->width : context.width);
	viewport.Height = (float)(frameBuffer ? frameBuffer->height : context.height);
	context.currentTargetWidth = frameBuffer ? frameBuffer->width : context.width;
	context.currentTargetHeight = frameBuffer ? frameBuffer->height : context.height;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor = {
		(LONG)viewport.TopLeftX, (LONG)viewport.TopLeftY,
		(LONG)(viewport.TopLeftX + viewport.Width),
		(LONG)(viewport.TopLeftY + viewport.Height)
	};
	context.commandList->RSSetViewports(1, &viewport);
	context.commandList->RSSetScissorRects(1, &scissor);
	return 1;
}

static void
beginUpdate(Camera *camera)
{
	if(camera && camera->getFrame()){
		float view[16], projection[16];
		Matrix inverse;
		Matrix::invert(&inverse, camera->getFrame()->getLTM());
		view[0] = -inverse.right.x; view[1] = inverse.right.y;
		view[2] = inverse.right.z; view[3] = 0.0f;
		view[4] = -inverse.up.x; view[5] = inverse.up.y;
		view[6] = inverse.up.z; view[7] = 0.0f;
		view[8] = -inverse.at.x; view[9] = inverse.at.y;
		view[10] = inverse.at.z; view[11] = 0.0f;
		view[12] = -inverse.pos.x; view[13] = inverse.pos.y;
		view[14] = inverse.pos.z; view[15] = 1.0f;
		memcpy(&camera->devView, view, sizeof(view));

		float32 inverseWidth = 1.0f/camera->viewWindow.x;
		float32 inverseHeight = 1.0f/camera->viewWindow.y;
		float32 inverseDepth = 1.0f/(camera->farPlane-camera->nearPlane);
		memset(projection, 0, sizeof(projection));
		projection[0] = inverseWidth;
		projection[5] = inverseHeight;
		projection[8] = camera->viewOffset.x*inverseWidth;
		projection[9] = camera->viewOffset.y*inverseHeight;
		projection[12] = -projection[8];
		projection[13] = -projection[9];
		if(camera->projection == Camera::PERSPECTIVE){
			projection[10] = camera->farPlane*inverseDepth;
			projection[11] = 1.0f;
			projection[15] = 0.0f;
		}else{
			projection[10] = inverseDepth;
			projection[11] = 0.0f;
			projection[15] = 1.0f;
		}
		projection[14] = -camera->nearPlane*projection[10];
		memcpy(&camera->devProj, projection, sizeof(projection));
	}
	beginFrame(camera);
}

static void
clearCamera(Camera *camera, RGBA *color, uint32 mode)
{
	if(!beginFrame(camera))
		return;
	if((mode & Camera::CLEARIMAGE) && color){
		const float clearColor[4] = {
			color->red / 255.0f,
			color->green / 255.0f,
			color->blue / 255.0f,
			color->alpha / 255.0f
		};
		context.commandList->ClearRenderTargetView(
			context.currentColorView, clearColor, 0, nil);
	}
	if((mode & Camera::CLEARZ) && context.currentDepthView.ptr)
		context.commandList->ClearDepthStencilView(
			context.currentDepthView,
			D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
			1.0f, 0, 0, nil);
}

static void
endUpdate(Camera*)
{
	// RenderWare can end several auxiliary cameras during one presented frame.
	// The D3D12 list therefore remains open until showRaster submits it.
}

static void
finishFrame(void)
{
	if(!context.frameOpen)
		return;
	if(context.currentColorRaster)
		transitionRaster(context.currentColorRaster,
		                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	context.currentColorRaster = nil;
	context.currentColorResource = nil;
	context.currentColorView.ptr = 0;
	context.currentTargetWidth = 0;
	context.currentTargetHeight = 0;

	if(context.backBufferRendering){
		D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
			context.backBuffers[context.frameIndex],
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PRESENT);
		context.commandList->ResourceBarrier(1, &barrier);
		context.backBufferRendering = 0;
	}
	if(FAILED(context.commandList->Close())){
		context.frameOpen = 0;
		return;
	}
	ID3D12CommandList *lists[] = { context.commandList };
	context.queue->ExecuteCommandLists(1, lists);
	const UINT64 value = ++context.fenceValue;
	if(SUCCEEDED(context.queue->Signal(context.fence, value))){
		context.frameFenceValues[context.frameIndex] = value;
		// Standalone uploads were submitted before this frame list. Associate
		// their temporary resources with the fence we have just signalled, not
		// with the stale fence that beginFrame already waited for.
		context.deferredReleases[context.frameIndex].insert(
			context.deferredReleases[context.frameIndex].end(),
			context.pendingSubmitReleases.begin(),
			context.pendingSubmitReleases.end());
		context.pendingSubmitReleases.clear();
	}
	context.frameOpen = 0;
}

static void
showRaster(Raster*, uint32)
{
	if(!context.presentationReady)
		return;
	if(context.frameOpen)
		finishFrame();
	UINT presentedFrame = context.frameIndex;
	if(FAILED(context.swapChain->Present(context.presentInterval, 0)))
		return;
	context.lastPresentedFrame = presentedFrame;
	context.hasPresentedFrame = 1;
	context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
}

static bool32
rasterRenderFast(Raster *source, int32 x, int32 y)
{
	if(!context.frameOpen || source == nil || source->parent == nil ||
	   source->parent->type != Raster::CAMERA)
		return 0;
	Raster *destination = Raster::getCurrentContext();
	if(destination)
		destination = destination->parent;
	ID3D12Resource *destinationResource = nil;
	if(destination == nil ||
	   !getRasterResource(destination, &destinationResource))
		return 0;
	if(x < 0 || y < 0 || x >= destination->width ||
	   y >= destination->height)
		return 0;
	if(!transitionRaster(destination, D3D12_RESOURCE_STATE_COPY_DEST))
		return 0;

	D3D12_RESOURCE_BARRIER toCopy = transitionBarrier(
		context.backBuffers[context.frameIndex],
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	context.commandList->ResourceBarrier(1, &toCopy);
	D3D12_TEXTURE_COPY_LOCATION dst;
	memset(&dst, 0, sizeof(dst));
	dst.pResource = destinationResource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION src;
	memset(&src, 0, sizeof(src));
	src.pResource = context.backBuffers[context.frameIndex];
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_BOX box;
	box.left = 0;
	box.top = 0;
	box.front = 0;
	box.right = (UINT)((source->width < destination->width-x) ?
		source->width : destination->width-x);
	box.bottom = (UINT)((source->height < destination->height-y) ?
		source->height : destination->height-y);
	box.back = 1;
	context.commandList->CopyTextureRegion(
		&dst, (UINT)x, (UINT)y, 0, &src, &box);
	D3D12_RESOURCE_BARRIER toRender = transitionBarrier(
		context.backBuffers[context.frameIndex],
		D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	context.commandList->ResourceBarrier(1, &toRender);
	transitionRaster(destination,
	                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return 1;
}

static void
destroyCoreDevice(void)
{
	if(context.queue && context.fence && context.fenceEvent)
		waitForGpu();
	releasePendingSubmitObjects();
	for(uint32 i = 0; i < FRAME_COUNT; i++)
		releaseDeferredFrame(i);
	destroyFrameResources();

	if(context.fenceEvent){
		CloseHandle(context.fenceEvent);
		context.fenceEvent = nil;
	}
	releaseCom(context.fence);
	releaseCom(context.srvHeap);
	releaseCom(context.samplerHeap);
	releaseCom(context.dsvHeap);
	releaseCom(context.queue);
	releaseCom(context.device);
	releaseCom(context.adapter);
	releaseCom(context.factory6);
	releaseCom(context.factory);
	context.fenceValue = 0;
	context.srvDescriptorSize = 0;
	context.samplerDescriptorSize = 0;
	context.dsvDescriptorSize = 0;
	context.nextSrvDescriptor = 0;
	context.nextSamplerDescriptor = 0;
	context.nextDsvDescriptor = 0;
	context.freeSrvDescriptors.clear();
	context.freeRtvDescriptors.clear();
	context.freeDsvDescriptors.clear();
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		context.deferredSrvDescriptors[i].clear();
		context.deferredRtvDescriptors[i].clear();
		context.deferredDsvDescriptors[i].clear();
	}
	memset(context.samplerCache, 0, sizeof(context.samplerCache));
	context.initialized = 0;
}

static bool32
createCoreDevice(void)
{
	if(context.initialized)
		return 1;

	UINT factoryFlags = 0;
#ifdef DEBUG
	ID3D12Debug *debug = nil;
	if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))){
		debug->EnableDebugLayer();
		factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		releaseCom(debug);
	}
#endif

	if(FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&context.factory))))
		return 0;
	context.factory->QueryInterface(IID_PPV_ARGS(&context.factory6));

	if(!selectAdapter()){
		destroyCoreDevice();
		return 0;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc;
	memset(&queueDesc, 0, sizeof(queueDesc));
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	if(FAILED(context.device->CreateCommandQueue(&queueDesc,
	                                            IID_PPV_ARGS(&context.queue))) ||
	   FAILED(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
	                                     IID_PPV_ARGS(&context.fence)))){
		destroyCoreDevice();
		return 0;
	}

	context.fenceEvent = CreateEventW(nil, FALSE, FALSE, nil);
	if(context.fenceEvent == nil){
		destroyCoreDevice();
		return 0;
	}
	D3D12_DESCRIPTOR_HEAP_DESC srvDesc;
	memset(&srvDesc, 0, sizeof(srvDesc));
	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.NumDescriptors = MAX_SHADER_RESOURCE_DESCRIPTORS;
	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12_DESCRIPTOR_HEAP_DESC dsvDesc;
	memset(&dsvDesc, 0, sizeof(dsvDesc));
	dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvDesc.NumDescriptors = MAX_DEPTH_DESCRIPTORS;
	D3D12_DESCRIPTOR_HEAP_DESC samplerDesc;
	memset(&samplerDesc, 0, sizeof(samplerDesc));
	samplerDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerDesc.NumDescriptors = MAX_SAMPLER_DESCRIPTORS;
	samplerDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if(FAILED(context.device->CreateDescriptorHeap(
	       &srvDesc, IID_PPV_ARGS(&context.srvHeap))) ||
	   FAILED(context.device->CreateDescriptorHeap(
	       &samplerDesc, IID_PPV_ARGS(&context.samplerHeap))) ||
	   FAILED(context.device->CreateDescriptorHeap(
	       &dsvDesc, IID_PPV_ARGS(&context.dsvHeap)))){
		destroyCoreDevice();
		return 0;
	}
	context.srvDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	context.samplerDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	context.dsvDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	if(!createFrameResources()){
		destroyCoreDevice();
		return 0;
	}
	context.initialized = 1;
	return 1;
}

bool32
isInitialized(void)
{
	return context.initialized;
}

bool32
isPresentationReady(void)
{
	return context.presentationReady;
}

const char*
getAdapterName(void)
{
	return context.adapterName[0] ? context.adapterName : "D3D12 not initialized";
}

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	VideoMode *mode;
	switch(req){
	case DEVICEOPEN:
		context.window = (HWND)((EngineOpenParams*)arg)->window;
		context.width = 1280;
		context.height = 720;
		context.desktopWidth = GetSystemMetrics(SM_CXSCREEN);
		context.desktopHeight = GetSystemMetrics(SM_CYSCREEN);
		DEVMODE displayMode;
		memset(&displayMode, 0, sizeof(displayMode));
		displayMode.dmSize = sizeof(displayMode);
		if(EnumDisplaySettings(nil, ENUM_CURRENT_SETTINGS, &displayMode)){
			context.desktopWidth = (int32)displayMode.dmPelsWidth;
			context.desktopHeight = (int32)displayMode.dmPelsHeight;
		}
		if(context.desktopWidth <= 0)
			context.desktopWidth = context.width;
		if(context.desktopHeight <= 0)
			context.desktopHeight = context.height;
		context.currentVideoMode = 0;
		context.presentInterval = 1;
		if(context.window){
			RECT rect;
			if(GetClientRect(context.window, &rect)){
				context.width = rect.right - rect.left;
				context.height = rect.bottom - rect.top;
			}
		}
		return 1;
	case DEVICECLOSE:
		context.window = nil;
		return 1;
	case DEVICEINIT:
		// psSelectDevice changes the HWND after DEVICEOPEN. Read the final client
		// area here so the swap chain matches the RenderWare camera pixel-for-pixel.
		if(context.window){
			RECT rect;
			if(GetClientRect(context.window, &rect) &&
			   rect.right > rect.left && rect.bottom > rect.top){
				context.width = rect.right - rect.left;
				context.height = rect.bottom - rect.top;
			}
		}
		return createCoreDevice();
	case DEVICETERM:
		destroyCoreDevice();
		return 1;
	case DEVICEFINALIZE:
		return context.initialized;
	case DEVICEGETNUMSUBSYSTEMS:
		// RenderWare selects a subsystem before DEVICEINIT. D3D12 performs
		// the real high-performance adapter selection during initialization,
		// but must expose that selectable logical subsystem beforehand.
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return n == 0;
	case DEVICEGETSUBSSYSTEMINFO:
		if(arg == nil || n != 0)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, getAdapterName(),
		        sizeof(SubSystemInfo::name));
		((SubSystemInfo*)arg)->name[sizeof(SubSystemInfo::name)-1] = '\0';
		return 1;
	case DEVICEGETNUMVIDEOMODES:
		// reVC's device selector requires both a windowed mode and at least one
		// exclusive-marked mode. The latter is implemented by the Win32 skeleton
		// as a borderless desktop window; DXGI exclusive fullscreen is deliberately
		// not used because flip-model swap chains work more reliably without it.
		return 2;
	case DEVICEGETCURRENTVIDEOMODE:
		return context.currentVideoMode;
	case DEVICESETVIDEOMODE:
		if(n < 0 || n >= 2)
			return 0;
		context.currentVideoMode = n;
		return 1;
	case DEVICEGETVIDEOMODEINFO:
		if(arg == nil || n < 0 || n >= 2)
			return 0;
		mode = (VideoMode*)arg;
		mode->width = n == 0 ? context.width : context.desktopWidth;
		mode->height = n == 0 ? context.height : context.desktopHeight;
		mode->depth = 32;
		mode->flags = n == 1 ? VIDEOMODEEXCLUSIVE : 0;
		return 1;
	case DEVICEGETMAXMULTISAMPLINGLEVELS:
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		return n <= 1;
	}
	return 0;
}

#else

bool32 isInitialized(void) { return 0; }
bool32 isPresentationReady(void) { return 0; }
const char *getAdapterName(void) { return "D3D12 backend not compiled"; }
bool32 waitForGpu(void) { return 0; }
static int deviceSystem(DeviceReq req, void *arg, int32 n)
{
	return null::deviceSystem(req, arg, n);
}

#endif

// Camera presentation is live. Raster allocation, depth and draw pipelines are
// introduced in the next slices; their device entries remain explicitly null.
Device renderdevice = {
	0.0f, 1.0f,
#ifdef RW_D3D12
	d3d12::beginUpdate,
	d3d12::endUpdate,
	d3d12::clearCamera,
	d3d12::showRaster,
#else
	null::beginUpdate,
	null::endUpdate,
	null::clearCamera,
	null::showRaster,
#endif
#ifdef RW_D3D12
	d3d12::rasterRenderFast,
#else
	null::rasterRenderFast,
#endif
#ifdef RW_D3D12
	d3d12::setRenderState,
	d3d12::getRenderState,
	d3d12::im2DRenderLine,
	d3d12::im2DRenderTriangle,
	d3d12::im2DRenderPrimitive,
	d3d12::im2DRenderIndexedPrimitive,
#else
	null::setRenderState,
	null::getRenderState,
	null::im2DRenderLine,
	null::im2DRenderTriangle,
	null::im2DRenderPrimitive,
	null::im2DRenderIndexedPrimitive,
#endif
#ifdef RW_D3D12
	d3d12::im3DTransform,
	d3d12::im3DRenderPrimitive,
	d3d12::im3DRenderIndexedPrimitive,
	d3d12::im3DEnd,
#else
	null::im3DTransform,
	null::im3DRenderPrimitive,
	null::im3DRenderIndexedPrimitive,
	null::im3DEnd,
#endif
	d3d12::deviceSystem
};

}
}
