#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace d3d12 {

int32 nativeRasterOffset;

#ifdef RW_D3D12

enum { MAX_MIP_LEVELS = 16 };

struct D3D12Raster
{
	ID3D12Resource *resource;
	D3D12_RESOURCE_STATES state;
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE srvGpu;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv;
	uint8 *backingStore[MAX_MIP_LEVELS];
	uint32 levelSize[MAX_MIP_LEVELS];
	uint32 levelStride[MAX_MIP_LEVELS];
	uint32 numLevels;
	uint32 lockedLevel;
	bool32 hasAlpha;
};

#define GETD3D12RASTEREXT(raster) \
	PLUGINOFFSET(D3D12Raster, raster, nativeRasterOffset)

template<class T>
static void
releaseCom(T *&object)
{
	if(object){
		object->Release();
		object = nil;
	}
}

static D3D12_HEAP_PROPERTIES
heapProperties(D3D12_HEAP_TYPE type)
{
	D3D12_HEAP_PROPERTIES props;
	memset(&props, 0, sizeof(props));
	props.Type = type;
	props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;
	return props;
}

static D3D12_RESOURCE_DESC
textureDesc(uint32 width, uint32 height, uint16 levels, DXGI_FORMAT format,
	        D3D12_RESOURCE_FLAGS flags)
{
	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = levels;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	return desc;
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
createTextureResource(Raster *raster, D3D12Raster *nativeRaster)
{
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COPY_DEST;
	if(raster->type == Raster::CAMERATEXTURE){
		flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}

	D3D12_RESOURCE_DESC desc = textureDesc(
		raster->width, raster->height, (uint16)nativeRaster->numLevels,
		raster->type == Raster::CAMERATEXTURE ?
			DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM,
		flags);
	D3D12_HEAP_PROPERTIES props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if(FAILED(device->CreateCommittedResource(
	       &props, D3D12_HEAP_FLAG_NONE, &desc, initialState, nil,
	       IID_PPV_ARGS(&nativeRaster->resource))))
		return 0;
	nativeRaster->state = initialState;

	if(!allocateShaderResourceDescriptor(&nativeRaster->srvCpu,
	                                     &nativeRaster->srvGpu))
		return 0;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv;
	memset(&srv, 0, sizeof(srv));
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = nativeRaster->numLevels;
	device->CreateShaderResourceView(nativeRaster->resource, &srv,
	                                 nativeRaster->srvCpu);

	if(raster->type == Raster::CAMERATEXTURE){
		if(!allocateRenderTargetDescriptor(&nativeRaster->rtv))
			return 0;
		device->CreateRenderTargetView(nativeRaster->resource, nil,
		                               nativeRaster->rtv);
	}
	return 1;
}

static bool32
createDepthResource(Raster *raster, D3D12Raster *nativeRaster)
{
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_RESOURCE_DESC desc = textureDesc(
		raster->width, raster->height, 1, DXGI_FORMAT_D32_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	D3D12_CLEAR_VALUE clearValue;
	memset(&clearValue, 0, sizeof(clearValue));
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;
	D3D12_HEAP_PROPERTIES props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if(FAILED(device->CreateCommittedResource(
	       &props, D3D12_HEAP_FLAG_NONE, &desc,
	       D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
	       IID_PPV_ARGS(&nativeRaster->resource))))
		return 0;
	nativeRaster->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	if(!allocateDepthDescriptor(&nativeRaster->dsv))
		return 0;
	device->CreateDepthStencilView(nativeRaster->resource, nil,
	                               nativeRaster->dsv);
	raster->format = Raster::D32;
	raster->depth = 32;
	return 1;
}

static bool32
uploadLevel(Raster *raster, D3D12Raster *nativeRaster, uint32 level)
{
	ID3D12Device *device = getDevice();
	ID3D12CommandQueue *queue = getCommandQueue();
	if(device == nil || queue == nil || nativeRaster->resource == nil ||
	   level >= nativeRaster->numLevels)
		return 0;

	D3D12_RESOURCE_DESC texture = nativeRaster->resource->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT numRows = 0;
	UINT64 rowSize = 0;
	UINT64 uploadSize = 0;
	device->GetCopyableFootprints(&texture, level, 1, 0, &footprint,
	                             &numRows, &rowSize, &uploadSize);

	D3D12_RESOURCE_DESC buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = uploadSize;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource *upload = nil;
	D3D12_HEAP_PROPERTIES uploadProps = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
	if(FAILED(device->CreateCommittedResource(
	       &uploadProps, D3D12_HEAP_FLAG_NONE, &buffer,
	       D3D12_RESOURCE_STATE_GENERIC_READ, nil, IID_PPV_ARGS(&upload))))
		return 0;

	uint8 *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED(upload->Map(0, &readRange, (void**)&mapped))){
		releaseCom(upload);
		return 0;
	}
	for(UINT row = 0; row < numRows; row++)
		memcpy(mapped + footprint.Offset + row*footprint.Footprint.RowPitch,
		       raster->pixels + row*raster->stride,
		       nativeRaster->levelStride[level]);
	upload->Unmap(0, nil);

	ID3D12CommandAllocator *allocator = nil;
	ID3D12GraphicsCommandList *list = nil;
	bool32 ok = SUCCEEDED(device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(device->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
		IID_PPV_ARGS(&list)));
	if(!ok){
		releaseCom(list);
		releaseCom(allocator);
		releaseCom(upload);
		return 0;
	}

	if(nativeRaster->state != D3D12_RESOURCE_STATE_COPY_DEST){
		D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
			nativeRaster->resource, nativeRaster->state,
			D3D12_RESOURCE_STATE_COPY_DEST);
		list->ResourceBarrier(1, &barrier);
	}
	D3D12_TEXTURE_COPY_LOCATION dst;
	memset(&dst, 0, sizeof(dst));
	dst.pResource = nativeRaster->resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = level;
	D3D12_TEXTURE_COPY_LOCATION src;
	memset(&src, 0, sizeof(src));
	src.pResource = upload;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = footprint;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nil);
	D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
		nativeRaster->resource, D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	list->ResourceBarrier(1, &barrier);
	nativeRaster->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ok = SUCCEEDED(list->Close());
	if(ok){
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		ok = waitForGpu();
	}
	releaseCom(list);
	releaseCom(allocator);
	releaseCom(upload);
	return ok;
}

#endif

Raster*
rasterCreate(Raster *raster)
{
#ifdef RW_D3D12
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	nativeRaster->numLevels = 1;
	nativeRaster->hasAlpha = 0;

	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		goto done;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		goto done;

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		{
		int32 mipFlags = raster->format &
			(Raster::MIPMAP | Raster::AUTOMIPMAP);
		raster->format = Raster::C8888 | mipFlags;
		raster->depth = 32;
		raster->stride = raster->width*4;
		if((raster->format & Raster::MIPMAP) &&
		   !(raster->format & Raster::AUTOMIPMAP))
			nativeRaster->numLevels = Raster::calculateNumLevels(
				raster->width, raster->height);
		if(nativeRaster->numLevels > MAX_MIP_LEVELS)
			nativeRaster->numLevels = MAX_MIP_LEVELS;
		if(!createTextureResource(raster, nativeRaster))
			return nil;
		}
		break;
	case Raster::ZBUFFER:
		raster->stride = 0;
		if(!createDepthResource(raster, nativeRaster))
			return nil;
		break;
	case Raster::CAMERA:
		raster->format = Raster::C8888;
		raster->depth = 32;
		raster->stride = raster->width*4;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	if(raster->type == Raster::NORMAL || raster->type == Raster::TEXTURE ||
	   raster->type == Raster::CAMERATEXTURE){
		uint32 width = raster->width;
		uint32 height = raster->height;
		for(uint32 i = 0; i < nativeRaster->numLevels; i++){
			nativeRaster->levelStride[i] = width*4;
			nativeRaster->levelSize[i] = nativeRaster->levelStride[i]*height;
			nativeRaster->backingStore[i] = (uint8*)rwMalloc(
				nativeRaster->levelSize[i], MEMDUR_EVENT | ID_DRIVER);
			if(nativeRaster->backingStore[i] == nil)
				return nil;
			memset(nativeRaster->backingStore[i], 0,
			       nativeRaster->levelSize[i]);
			if(width > 1) width /= 2;
			if(height > 1) height /= 2;
		}
	}

done:
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	raster->originalPixels = raster->pixels;
	return raster;
#else
	return nil;
#endif
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
#ifdef RW_D3D12
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(raster->privateFlags != 0 || level < 0 ||
	   (uint32)level >= nativeRaster->numLevels)
		return nil;
	if(raster->type != Raster::NORMAL && raster->type != Raster::TEXTURE &&
	   raster->type != Raster::CAMERATEXTURE)
		return nil;

	uint32 width = raster->originalWidth;
	uint32 height = raster->originalHeight;
	for(int32 i = 0; i < level; i++){
		if(width > 1) width /= 2;
		if(height > 1) height /= 2;
	}
	raster->width = width;
	raster->height = height;
	raster->stride = nativeRaster->levelStride[level];
	raster->pixels = (uint8*)rwMalloc(nativeRaster->levelSize[level],
	                                  MEMDUR_EVENT | ID_DRIVER);
	if(raster->pixels == nil)
		return nil;
	if((lockMode & Raster::LOCKNOFETCH) == 0 ||
	   (lockMode & Raster::LOCKREAD))
		memcpy(raster->pixels, nativeRaster->backingStore[level],
		       nativeRaster->levelSize[level]);
	nativeRaster->lockedLevel = level;
	raster->privateFlags = lockMode;
	return raster->pixels;
#else
	return nil;
#endif
}

void
rasterUnlock(Raster *raster, int32 level)
{
#ifdef RW_D3D12
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(raster->pixels && (raster->privateFlags & Raster::LOCKWRITE) &&
	   level >= 0 && (uint32)level < nativeRaster->numLevels){
		memcpy(nativeRaster->backingStore[level], raster->pixels,
		       nativeRaster->levelSize[level]);
		if(!uploadLevel(raster, nativeRaster, level))
			fprintf(stderr, "librw D3D12: texture upload failed\n");
	}
	if(raster->pixels)
		rwFree(raster->pixels);
	raster->width = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->pixels = raster->originalPixels;
	raster->privateFlags = 0;
#endif
}

int32
rasterNumLevels(Raster *raster)
{
#ifdef RW_D3D12
	return GETD3D12RASTEREXT(raster)->numLevels;
#else
	return 1;
#endif
}

bool32
imageFindRasterFormat(Image *image, int32 type, int32 *width, int32 *height,
	                  int32 *depth, int32 *format)
{
	if((type & 0xF) != Raster::TEXTURE)
		return 0;
	*width = image->width;
	*height = image->height;
	*depth = 32;
	*format = Raster::C8888 | type;
	return 1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
#ifdef RW_D3D12
	if((raster->type & 0xF) != Raster::TEXTURE)
		return 0;
	Image *trueColor = nil;
	if(image->depth <= 8){
		trueColor = Image::create(image->width, image->height, image->depth);
		trueColor->pixels = image->pixels;
		trueColor->stride = image->stride;
		trueColor->palette = image->palette;
		trueColor->unpalettize();
		image = trueColor;
	}
	if(image->width != raster->width || image->height != raster->height ||
	   (image->depth != 24 && image->depth != 32 && image->depth != 16)){
		if(trueColor) trueColor->destroy();
		return 0;
	}

	bool32 wasLocked = raster->pixels != nil &&
		(raster->privateFlags & Raster::LOCKWRITE) != 0;
	uint8 *pixels = wasLocked ? raster->pixels :
		raster->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(pixels == nil){
		if(trueColor) trueColor->destroy();
		return 0;
	}
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	nativeRaster->hasAlpha = nativeRaster->hasAlpha || image->hasAlpha();
	for(int32 y = 0; y < image->height; y++){
		uint8 *src = image->pixels + y*image->stride;
		uint8 *dst = pixels + y*raster->stride;
		for(int32 x = 0; x < image->width; x++){
			if(image->depth == 32)
				conv_BGRA8888_from_RGBA8888(dst, src);
			else if(image->depth == 24)
				conv_BGRA8888_from_RGB888(dst, src);
			else{
				uint8 rgba[4];
				conv_RGBA8888_from_ARGB1555(rgba, src);
				conv_BGRA8888_from_RGBA8888(dst, rgba);
			}
			src += image->bpp;
			dst += 4;
		}
	}
	if(!wasLocked)
		raster->unlock(0);
	if(trueColor) trueColor->destroy();
	return 1;
#else
	return 0;
#endif
}

Image*
rasterToImage(Raster *raster)
{
#ifdef RW_D3D12
	if(raster->type != Raster::NORMAL && raster->type != Raster::TEXTURE &&
	   raster->type != Raster::CAMERATEXTURE)
		return nil;
	uint8 *pixels = raster->lock(0, Raster::LOCKREAD);
	if(pixels == nil)
		return nil;
	Image *image = Image::create(raster->width, raster->height, 32);
	image->allocate();
	for(int32 y = 0; y < raster->height; y++){
		uint8 *src = pixels + y*raster->stride;
		uint8 *dst = image->pixels + y*image->stride;
		for(int32 x = 0; x < raster->width; x++){
			conv_RGBA8888_from_BGRA8888(dst, src);
			src += 4;
			dst += 4;
		}
	}
	raster->unlock(0);
	return image;
#else
	return nil;
#endif
}

#ifdef RW_D3D12
bool32
getDepthTarget(Raster *raster, ID3D12Resource **resource,
	           D3D12_CPU_DESCRIPTOR_HANDLE *view)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type != Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil || nativeRaster->dsv.ptr == 0)
		return 0;
	if(resource) *resource = nativeRaster->resource;
	if(view) *view = nativeRaster->dsv;
	return 1;
}

bool32
getColorTarget(Raster *raster, ID3D12Resource **resource,
	           D3D12_CPU_DESCRIPTOR_HANDLE *view)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type != Raster::CAMERATEXTURE)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil || nativeRaster->rtv.ptr == 0)
		return 0;
	if(resource) *resource = nativeRaster->resource;
	if(view) *view = nativeRaster->rtv;
	return 1;
}

bool32
getRasterResource(Raster *raster, ID3D12Resource **resource)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type == Raster::CAMERA || raster->type == Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil)
		return 0;
	if(resource) *resource = nativeRaster->resource;
	return 1;
}

bool32
transitionRaster(Raster *raster, D3D12_RESOURCE_STATES state)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type == Raster::CAMERA || raster->type == Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil)
		return 0;
	if(nativeRaster->state == state)
		return 1;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return 0;
	D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
		nativeRaster->resource, nativeRaster->state, state);
	list->ResourceBarrier(1, &barrier);
	nativeRaster->state = state;
	return 1;
}

bool32
getTextureView(Raster *raster, D3D12_GPU_DESCRIPTOR_HANDLE *view,
	           bool32 *hasAlpha)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   (raster->type != Raster::NORMAL && raster->type != Raster::TEXTURE &&
	    raster->type != Raster::CAMERATEXTURE))
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil || nativeRaster->srvGpu.ptr == 0)
		return 0;
	if(nativeRaster->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
	   !transitionRaster(raster, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
		return 0;
	if(view) *view = nativeRaster->srvGpu;
	if(hasAlpha) *hasAlpha = nativeRaster->hasAlpha;
	return 1;
}
#endif

static void*
createNativeRaster(void *object, int32 offset, int32)
{
#ifdef RW_D3D12
	D3D12Raster *raster = PLUGINOFFSET(D3D12Raster, object, offset);
	memset(raster, 0, sizeof(*raster));
#endif
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
#ifdef RW_D3D12
	D3D12Raster *raster = PLUGINOFFSET(D3D12Raster, object, offset);
	deferRelease(raster->resource);
	raster->resource = nil;
	for(uint32 i = 0; i < MAX_MIP_LEVELS; i++){
		if(raster->backingStore[i]){
			rwFree(raster->backingStore[i]);
			raster->backingStore[i] = nil;
		}
	}
#endif
	return object;
}

static void*
copyNativeRaster(void *dst, void*, int32 offset, int32)
{
#ifdef RW_D3D12
	D3D12Raster *raster = PLUGINOFFSET(D3D12Raster, dst, offset);
	memset(raster, 0, sizeof(*raster));
#endif
	return dst;
}

void
registerNativeRaster(void)
{
#ifdef RW_D3D12
	nativeRasterOffset = Raster::registerPlugin(
		sizeof(D3D12Raster), ID_RASTERD3D12, createNativeRaster,
		destroyNativeRaster, copyNativeRaster);
#endif
}

}
}
