#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#endif

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwd3d12.h"
#include "rwd3d12impl.h"

namespace rw {
namespace d3d12 {

static void
tracePipeline(const char *message)
{
	FILE *file = fopen("d3d12_stage5_trace.log", "a");
	if(file){
		fprintf(file, "%s\n", message);
		fclose(file);
	}
}

#ifdef RW_D3D12

struct Vertex
{
	V3d position;
	V3d normal;
	RGBA color;
	TexCoords texCoords;
	float32 weights[4];
	uint8 boneIndices[4];
};

struct MeshDraw
{
	uint32 numIndices;
	uint32 startIndex;
	Material *material;
	bool32 vertexAlpha;
};

struct D3D12InstanceDataHeader : InstanceDataHeader
{
	uint32 serialNumber;
	uint32 numMeshes;
	uint32 numVertices;
	uint32 numIndices;
	D3D12_PRIMITIVE_TOPOLOGY topology;
	ID3D12Resource *vertexBuffer;
	ID3D12Resource *indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	D3D12_INDEX_BUFFER_VIEW indexView;
	MeshDraw *meshes;
};

static ID3D12RootSignature *rootSignature;
static ID3D12PipelineState *opaquePipelineState;
static ID3D12PipelineState *alphaPipelineState;
static Raster *whiteRaster;
static bool32 pipelineReady;

enum {
	BONE_FRAME_COUNT = 3,
	MAX_SKIN_BONES = 64,
	BONE_UPLOAD_SIZE = 4*1024*1024
};

struct BoneArena
{
	ID3D12Resource *resource;
	uint8 *mapped;
	uint32 offset;
};

struct LightingConstants
{
	float ambient[4];
	float direction[4];
	float directionColor[4];
	float surface[4];
};

static BoneArena boneArenas[BONE_FRAME_COUNT];
static uint32 activeBoneArena = UINT32_MAX;

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
uploadHeapProperties(void)
{
	D3D12_HEAP_PROPERTIES props;
	memset(&props, 0, sizeof(props));
	props.Type = D3D12_HEAP_TYPE_UPLOAD;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;
	return props;
}

static D3D12_RESOURCE_DESC
bufferDesc(uint64 size)
{
	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return desc;
}

static bool32
createUploadBuffer(const void *data, uint64 size, ID3D12Resource **resource)
{
	ID3D12Device *device = getDevice();
	if(device == nil || data == nil || size == 0 || resource == nil)
		return 0;
	D3D12_HEAP_PROPERTIES props = uploadHeapProperties();
	D3D12_RESOURCE_DESC desc = bufferDesc(size);
	if(FAILED(device->CreateCommittedResource(
	       &props, D3D12_HEAP_FLAG_NONE, &desc,
	       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
	       IID_PPV_ARGS(resource))))
		return 0;
	void *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED((*resource)->Map(0, &readRange, &mapped))){
		releaseCom(*resource);
		return 0;
	}
	memcpy(mapped, data, (size_t)size);
	(*resource)->Unmap(0, nil);
	return 1;
}

static bool32
compileShader(const char *source, const char *entry, const char *target,
	          ID3DBlob **shader)
{
	ID3DBlob *errors = nil;
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
	HRESULT hr = D3DCompile(source, strlen(source), "librw_d3d12_default",
	                        nil, nil, entry, target, flags, 0, shader, &errors);
	if(FAILED(hr) && errors)
		fprintf(stderr, "librw D3D12 shader: %s\n",
		        (const char*)errors->GetBufferPointer());
	releaseCom(errors);
	return SUCCEEDED(hr);
}

static bool32
createPipelineResources(void)
{
	tracePipeline("pipeline resources begin");
	if(pipelineReady)
		return 1;
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_DESCRIPTOR_RANGE range;
	memset(&range, 0, sizeof(range));
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER params[4];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 56;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor.ShaderRegister = 1;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[3].Descriptor.ShaderRegister = 2;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

	D3D12_STATIC_SAMPLER_DESC sampler;
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 4;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;
	tracePipeline("pipeline root signature ready");

	static const char *shaderSource =
		"cbuffer DrawConstants : register(b0) {"
		" row_major float4x4 world; row_major float4x4 view;"
		" row_major float4x4 projection; float4 materialColor; float4 drawFlags; };"
		"cbuffer SkinConstants : register(b1) { row_major float4x4 bones[64]; };"
		"cbuffer LightingConstants : register(b2) { float4 ambientLight;"
		" float4 lightDirection; float4 lightColor; float4 surfaceProps; };"
		"Texture2D diffuseTexture : register(t0);"
		"SamplerState diffuseSampler : register(s0);"
		"struct VSIn { float3 position : POSITION; float3 normal : NORMAL;"
		" float4 color : COLOR0; float2 uv : TEXCOORD0;"
		" float4 weights : BLENDWEIGHT0; uint4 indices : BLENDINDICES0; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" float4 localPosition = float4(input.position, 1.0);"
		" float3 localNormal = input.normal;"
		" if(drawFlags.y > 0.5) { localPosition = float4(0.0, 0.0, 0.0, 0.0);"
		" localNormal = float3(0.0, 0.0, 0.0);"
		" [unroll] for(int i = 0; i < 4; i++)"
		" { localPosition += mul(float4(input.position, 1.0), bones[input.indices[i]]) * input.weights[i];"
		" localNormal += mul(float4(input.normal, 0.0), bones[input.indices[i]]).xyz * input.weights[i]; } }"
		" float4 p = mul(localPosition, world);"
		" p = mul(p, view); output.position = mul(p, projection);"
		" float3 normal = normalize(mul(float4(localNormal, 0.0), world).xyz);"
		" float3 litColor = input.color.rgb + ambientLight.rgb * surfaceProps.x;"
		" litColor += max(0.0, dot(normal, -lightDirection.xyz)) * lightColor.rgb * surfaceProps.z;"
		" output.color = float4(saturate(litColor), input.color.a) * materialColor; output.uv = input.uv;"
		" return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float4 color = input.color;"
		" if(drawFlags.x > 0.5) color *= diffuseTexture.Sample(diffuseSampler, input.uv);"
		" clip(color.a - 0.02);"
		" return color; }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		return 0;
	}
	tracePipeline("pipeline shaders ready");

	D3D12_INPUT_ELEMENT_DESC input[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
		  (UINT)offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
		  (UINT)offsetof(Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
		  (UINT)offsetof(Vertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
		  (UINT)offsetof(Vertex, texCoords), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
		  (UINT)offsetof(Vertex, weights), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0,
		  (UINT)offsetof(Vertex, boneIndices), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso;
	memset(&pso, 0, sizeof(pso));
	pso.pRootSignature = rootSignature;
	pso.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	pso.VS.BytecodeLength = vertexShader->GetBufferSize();
	pso.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	pso.PS.BytecodeLength = pixelShader->GetBufferSize();
	pso.InputLayout.pInputElementDescs = input;
	pso.InputLayout.NumElements = (UINT)nelem(input);
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
	pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pso.SampleMask = UINT_MAX;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pso.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(&pso,
	                                        IID_PPV_ARGS(&opaquePipelineState));
	if(SUCCEEDED(hr)){
		pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
		hr = device->CreateGraphicsPipelineState(&pso,
		                                        IID_PPV_ARGS(&alphaPipelineState));
	}
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	if(FAILED(hr)){
		releaseCom(opaquePipelineState);
		return 0;
	}
	tracePipeline("pipeline PSO ready");

	D3D12_HEAP_PROPERTIES boneProps = uploadHeapProperties();
	D3D12_RESOURCE_DESC boneBuffer = bufferDesc(BONE_UPLOAD_SIZE);
	for(uint32 i = 0; i < BONE_FRAME_COUNT; i++){
		if(FAILED(device->CreateCommittedResource(
		       &boneProps, D3D12_HEAP_FLAG_NONE, &boneBuffer,
		       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
		       IID_PPV_ARGS(&boneArenas[i].resource))))
			return 0;
		D3D12_RANGE readRange = { 0, 0 };
		if(FAILED(boneArenas[i].resource->Map(
		       0, &readRange, (void**)&boneArenas[i].mapped)))
			return 0;
	}
	activeBoneArena = UINT32_MAX;

	Image *white = Image::create(1, 1, 32);
	white->allocate();
	white->pixels[0] = white->pixels[1] = white->pixels[2] =
		white->pixels[3] = 0xFF;
	whiteRaster = Raster::createFromImage(white, PLATFORM_D3D12);
	white->destroy();
	if(whiteRaster == nil)
		return 0;
	tracePipeline("pipeline white raster ready");
	pipelineReady = 1;
	printf("librw D3D12: default world pipeline ready\n");
	return 1;
}

static void
freeInstanceData(Geometry *geometry)
{
	if(geometry == nil || geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D12)
		return;
	D3D12InstanceDataHeader *header =
		(D3D12InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	deferRelease(header->vertexBuffer);
	deferRelease(header->indexBuffer);
	header->vertexBuffer = nil;
	header->indexBuffer = nil;
	rwFree(header->meshes);
	rwFree(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static bool32
instanceGeometry(Geometry *geometry)
{
	if(geometry == nil || geometry->meshHeader == nil ||
	   geometry->numVertices <= 0 || geometry->morphTargets == nil ||
	   geometry->morphTargets[0].vertices == nil)
		return 0;
	if(geometry->flags & Geometry::NATIVE)
		return 0;

	if(geometry->instData){
		D3D12InstanceDataHeader *existing =
			(D3D12InstanceDataHeader*)geometry->instData;
		if(existing->platform == PLATFORM_D3D12 &&
		   existing->serialNumber == geometry->meshHeader->serialNum &&
		   geometry->lockedSinceInst == 0)
			return 1;
		freeInstanceData(geometry);
	}

	D3D12InstanceDataHeader *header = rwNewT(
		D3D12InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	memset(header, 0, sizeof(*header));
	header->platform = PLATFORM_D3D12;
	header->serialNumber = geometry->meshHeader->serialNum;
	header->numMeshes = geometry->meshHeader->numMeshes;
	header->numVertices = geometry->numVertices;
	header->numIndices = geometry->meshHeader->totalIndices;
	header->topology = geometry->meshHeader->flags == MeshHeader::TRISTRIP ?
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP :
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	header->meshes = rwNewT(MeshDraw, header->numMeshes,
	                            MEMDUR_EVENT | ID_GEOMETRY);

	Vertex *vertices = rwNewT(Vertex, header->numVertices,
	                          MEMDUR_EVENT | ID_GEOMETRY);
	Skin *skin = Skin::get(geometry);
	for(uint32 i = 0; i < header->numVertices; i++){
		vertices[i].position = geometry->morphTargets[0].vertices[i];
		if((geometry->flags & Geometry::NORMALS) &&
		   geometry->morphTargets[0].normals)
			vertices[i].normal = geometry->morphTargets[0].normals[i];
		else
			vertices[i].normal.set(0.0f, 0.0f, 1.0f);
		if((geometry->flags & Geometry::PRELIT) && geometry->colors)
			vertices[i].color = geometry->colors[i];
		else
			vertices[i].color = makeRGBA(255, 255, 255, 255);
		if(geometry->numTexCoordSets > 0 && geometry->texCoords[0])
			vertices[i].texCoords = geometry->texCoords[0][i];
		else{
			vertices[i].texCoords.u = 0.0f;
			vertices[i].texCoords.v = 0.0f;
		}
		vertices[i].weights[0] = 1.0f;
		vertices[i].weights[1] = vertices[i].weights[2] =
			vertices[i].weights[3] = 0.0f;
		memset(vertices[i].boneIndices, 0,
		       sizeof(vertices[i].boneIndices));
		if(skin && skin->weights && skin->indices){
			memcpy(vertices[i].weights, skin->weights + i*4,
			       sizeof(vertices[i].weights));
			memcpy(vertices[i].boneIndices, skin->indices + i*4,
			       sizeof(vertices[i].boneIndices));
		}
	}
	uint16 *indices = rwNewT(uint16, header->numIndices,
	                         MEMDUR_EVENT | ID_GEOMETRY);
	Mesh *mesh = geometry->meshHeader->getMeshes();
	uint32 indexOffset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		header->meshes[i].numIndices = mesh[i].numIndices;
		header->meshes[i].startIndex = indexOffset;
		header->meshes[i].material = mesh[i].material;
		header->meshes[i].vertexAlpha = 0;
		for(uint32 j = 0; j < mesh[i].numIndices; j++){
			indices[indexOffset + j] = mesh[i].indices[j];
			if(geometry->colors &&
			   geometry->colors[mesh[i].indices[j]].alpha != 0xFF)
				header->meshes[i].vertexAlpha = 1;
		}
		indexOffset += mesh[i].numIndices;
	}

	bool32 ok = createUploadBuffer(vertices,
		header->numVertices*sizeof(Vertex), &header->vertexBuffer) &&
		createUploadBuffer(indices,
		header->numIndices*sizeof(uint16), &header->indexBuffer);
	rwFree(vertices);
	rwFree(indices);
	if(!ok){
		releaseCom(header->vertexBuffer);
		releaseCom(header->indexBuffer);
		rwFree(header->meshes);
		rwFree(header);
		return 0;
	}
	header->vertexView.BufferLocation =
		header->vertexBuffer->GetGPUVirtualAddress();
	header->vertexView.SizeInBytes = header->numVertices*sizeof(Vertex);
	header->vertexView.StrideInBytes = sizeof(Vertex);
	header->indexView.BufferLocation =
		header->indexBuffer->GetGPUVirtualAddress();
	header->indexView.SizeInBytes = header->numIndices*sizeof(uint16);
	header->indexView.Format = DXGI_FORMAT_R16_UINT;
	geometry->instData = header;
	geometry->lockedSinceInst = 0;
	return 1;
}

static void
fillMatrix(float *dst, const Matrix *matrix)
{
	dst[0] = matrix->right.x; dst[1] = matrix->right.y;
	dst[2] = matrix->right.z; dst[3] = 0.0f;
	dst[4] = matrix->up.x; dst[5] = matrix->up.y;
	dst[6] = matrix->up.z; dst[7] = 0.0f;
	dst[8] = matrix->at.x; dst[9] = matrix->at.y;
	dst[10] = matrix->at.z; dst[11] = 0.0f;
	dst[12] = matrix->pos.x; dst[13] = matrix->pos.y;
	dst[14] = matrix->pos.z; dst[15] = 1.0f;
}

static bool32
allocateBoneConstants(const float *matrices,
	                  D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	if(activeBoneArena != frame){
		activeBoneArena = frame;
		boneArenas[frame].offset = 0;
	}
	BoneArena &arena = boneArenas[frame];
	uint32 offset = (arena.offset + 255u) & ~255u;
	uint32 size = MAX_SKIN_BONES*16*sizeof(float);
	if(arena.mapped == nil || offset + size > BONE_UPLOAD_SIZE)
		return 0;
	memcpy(arena.mapped + offset, matrices, size);
	*address = arena.resource->GetGPUVirtualAddress() + offset;
	arena.offset = offset + size;
	return 1;
}

static bool32
allocateLightingConstants(const LightingConstants *constants,
	                     D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	if(activeBoneArena != frame){
		activeBoneArena = frame;
		boneArenas[frame].offset = 0;
	}
	BoneArena &arena = boneArenas[frame];
	uint32 offset = (arena.offset + 255u) & ~255u;
	const uint32 size = 256;
	if(arena.mapped == nil || offset + size > BONE_UPLOAD_SIZE)
		return 0;
	memset(arena.mapped + offset, 0, size);
	memcpy(arena.mapped + offset, constants, sizeof(*constants));
	*address = arena.resource->GetGPUVirtualAddress() + offset;
	arena.offset = offset + size;
	return 1;
}

static void
collectLighting(Atomic *atomic, LightingConstants *constants)
{
	memset(constants, 0, sizeof(*constants));
	constants->ambient[3] = 1.0f;
	constants->direction[3] = 0.0f;
	constants->directionColor[3] = 1.0f;
	if(atomic == nil || atomic->geometry == nil ||
	   (atomic->geometry->flags & Geometry::LIGHT) == 0 ||
	   engine->currentWorld == nil)
		return;

	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	memset(&lightData, 0, sizeof(lightData));
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;
	((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	constants->ambient[0] = lightData.ambient.red;
	constants->ambient[1] = lightData.ambient.green;
	constants->ambient[2] = lightData.ambient.blue;
	if((atomic->geometry->flags & Geometry::NORMALS) &&
	   lightData.numDirectionals > 0 && lightData.directionals[0] &&
	   lightData.directionals[0]->getFrame()){
		Light *light = lightData.directionals[0];
		V3d direction = light->getFrame()->getLTM()->at;
		constants->direction[0] = direction.x;
		constants->direction[1] = direction.y;
		constants->direction[2] = direction.z;
		constants->directionColor[0] = light->color.red;
		constants->directionColor[1] = light->color.green;
		constants->directionColor[2] = light->color.blue;
	}
}

static bool32
uploadSkinMatrices(Atomic *atomic, D3D12_GPU_VIRTUAL_ADDRESS *address,
	               bool32 *isSkinned)
{
	float matrices[MAX_SKIN_BONES*16];
	memset(matrices, 0, sizeof(matrices));
	for(int32 i = 0; i < MAX_SKIN_BONES; i++){
		matrices[i*16] = 1.0f;
		matrices[i*16+5] = 1.0f;
		matrices[i*16+10] = 1.0f;
		matrices[i*16+15] = 1.0f;
	}
	Skin *skin = atomic && atomic->geometry ? Skin::get(atomic->geometry) : nil;
	*isSkinned = skin != nil && skin->numBones > 0 &&
		skin->inverseMatrices != nil;
	static bool32 tracedFirstSkin;
	if(*isSkinned && !tracedFirstSkin){
		tracePipeline("pipeline first skinned draw");
		tracedFirstSkin = 1;
	}
	if(!*isSkinned){
		uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
		if(boneArenas[frame].resource == nil)
			return 0;
		*address = boneArenas[frame].resource->GetGPUVirtualAddress();
		return 1;
	}

	HAnimHierarchy *hierarchy = Skin::getHierarchy(atomic);
	int32 count = skin->numBones < MAX_SKIN_BONES ?
		skin->numBones : MAX_SKIN_BONES;
	if(hierarchy && hierarchy->matrices && atomic->getFrame()){
		if(hierarchy->numNodes < count)
			count = hierarchy->numNodes;
		Matrix *inverseMatrices = (Matrix*)skin->inverseMatrices;
		if(hierarchy->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(int32 i = 0; i < count; i++){
				Matrix result;
				Matrix::mult(&result, &inverseMatrices[i],
				             &hierarchy->matrices[i]);
				fillMatrix(matrices + i*16, &result);
			}
		}else{
			Matrix inverseAtomic;
			Matrix::invert(&inverseAtomic, atomic->getFrame()->getLTM());
			for(int32 i = 0; i < count; i++){
				Matrix local, result;
				Matrix::mult(&local, &hierarchy->matrices[i],
				             &inverseAtomic);
				Matrix::mult(&result, &inverseMatrices[i], &local);
				fillMatrix(matrices + i*16, &result);
			}
		}
	}
	return allocateBoneConstants(matrices, address);
}

static void
renderGeometry(Atomic *atomic)
{
	static bool32 tracedFirstDraw;
	if(!tracedFirstDraw){
		tracePipeline("pipeline first render request");
		tracedFirstDraw = 1;
	}
	if(!pipelineReady || atomic == nil || atomic->geometry == nil ||
	   !instanceGeometry(atomic->geometry))
		return;
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(list == nil || camera == nil || atomic->getFrame() == nil)
		return;
	D3D12InstanceDataHeader *header =
		(D3D12InstanceDataHeader*)atomic->geometry->instData;
	list->SetGraphicsRootSignature(rootSignature);
	list->SetPipelineState(opaquePipelineState);
	list->IASetPrimitiveTopology(header->topology);
	list->IASetVertexBuffers(0, 1, &header->vertexView);
	list->IASetIndexBuffer(&header->indexView);

	float constants[56];
	memset(constants, 0, sizeof(constants));
	fillMatrix(constants, atomic->getFrame()->getLTM());
	memcpy(constants + 16, &camera->devView, 16*sizeof(float));
	memcpy(constants + 32, &camera->devProj, 16*sizeof(float));
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	bool32 isSkinned;
	if(!uploadSkinMatrices(atomic, &boneAddress, &isSkinned))
		return;
	constants[53] = isSkinned ? 1.0f : 0.0f;
	list->SetGraphicsRootConstantBufferView(2, boneAddress);
	LightingConstants lighting;
	collectLighting(atomic, &lighting);
	D3D12_GPU_DESCRIPTOR_HANDLE fallback;
	getTextureView(whiteRaster, &fallback, nil);
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *material = header->meshes[i].material;
		RGBA color = material ? material->color : makeRGBA(255, 255, 255, 255);
		constants[48] = color.red/255.0f;
		constants[49] = color.green/255.0f;
		constants[50] = color.blue/255.0f;
		constants[51] = color.alpha/255.0f;
		if(material){
			lighting.surface[0] = material->surfaceProps.ambient;
			lighting.surface[1] = material->surfaceProps.specular;
			lighting.surface[2] = material->surfaceProps.diffuse;
			lighting.surface[3] = 0.0f;
		}else{
			lighting.surface[0] = 1.0f;
			lighting.surface[1] = 0.0f;
			lighting.surface[2] = 1.0f;
			lighting.surface[3] = 0.0f;
		}
		D3D12_GPU_VIRTUAL_ADDRESS lightingAddress;
		if(!allocateLightingConstants(&lighting, &lightingAddress))
			return;
		list->SetGraphicsRootConstantBufferView(3, lightingAddress);
		D3D12_GPU_DESCRIPTOR_HANDLE texture = fallback;
		bool32 textured = 0;
		bool32 textureAlpha = 0;
		if(material && material->texture && material->texture->raster)
			textured = getTextureView(material->texture->raster,
			                          &texture, &textureAlpha);
		bool32 transparent = header->meshes[i].vertexAlpha ||
			color.alpha != 0xFF || (textured && textureAlpha);
		list->SetPipelineState(transparent ?
			alphaPipelineState : opaquePipelineState);
		constants[52] = textured ? 1.0f : 0.0f;
		list->SetGraphicsRoot32BitConstants(0, 56, constants, 0);
		list->SetGraphicsRootDescriptorTable(1, texture);
		list->DrawIndexedInstanced(header->meshes[i].numIndices, 1,
		                           header->meshes[i].startIndex, 0, 0);
	}
}

static void
pipelineInstance(ObjPipeline*, Atomic *atomic)
{
	if(atomic && atomic->geometry)
		instanceGeometry(atomic->geometry);
}

static void
pipelineUninstance(ObjPipeline*, Atomic *atomic)
{
	if(atomic)
		freeInstanceData(atomic->geometry);
}

static void
pipelineRender(ObjPipeline*, Atomic *atomic)
{
	renderGeometry(atomic);
}

#else

void *destroyNativeData(void *object, int32, int32) { return object; }

#endif

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipeline = ObjPipeline::create();
#ifdef RW_D3D12
	pipeline->init(PLATFORM_D3D12);
	pipeline->impl.instance = pipelineInstance;
	pipeline->impl.uninstance = pipelineUninstance;
	pipeline->impl.render = pipelineRender;
	createPipelineResources();
#endif
	return pipeline;
}

void
shutdownDefaultPipeline(void)
{
#ifdef RW_D3D12
	pipelineReady = 0;
	if(whiteRaster){
		whiteRaster->destroy();
		whiteRaster = nil;
	}
	for(uint32 i = 0; i < BONE_FRAME_COUNT; i++){
		if(boneArenas[i].resource && boneArenas[i].mapped)
			boneArenas[i].resource->Unmap(0, nil);
		boneArenas[i].mapped = nil;
		boneArenas[i].offset = 0;
		releaseCom(boneArenas[i].resource);
	}
	activeBoneArena = UINT32_MAX;
	releaseCom(alphaPipelineState);
	releaseCom(opaquePipelineState);
	releaseCom(rootSignature);
#endif
}

}
}
