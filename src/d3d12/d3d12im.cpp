#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#include "rwd3d12.h"
#include "rwd3d12impl.h"

namespace rw {
namespace d3d12 {

#ifdef RW_D3D12

enum {
	IMMEDIATE_FRAME_COUNT = 3,
	IMMEDIATE_UPLOAD_SIZE = 8*1024*1024,
	RENDER_STATE_COUNT = GSALPHATESTREF + 1
};

struct ImmediateArena
{
	ID3D12Resource *resource;
	uint8 *mapped;
	uint32 offset;
};

static ID3D12RootSignature *immediateRootSignature;
enum {
	PIPELINE_TRIANGLE,
	PIPELINE_LINE,
	PIPELINE_POINT,
	PIPELINE_TOPOLOGY_COUNT
};
enum {
	BLEND_ALPHA,
	BLEND_ADD_ONE,
	BLEND_ADD_ALPHA,
	BLEND_SHADOW,
	BLEND_INVERSE_DEST,
	BLEND_REPLACE,
	BLEND_KEEP_DESTINATION,
	BLEND_MODULATE_DESTINATION,
	BLEND_MODE_COUNT
};
enum {
	DEPTH_DISABLED,
	DEPTH_TEST_ONLY,
	DEPTH_TEST_WRITE,
	DEPTH_WRITE_ONLY,
	DEPTH_MODE_COUNT
};
enum {
	DEPTH_COMPARE_LESS_EQUAL,
	DEPTH_COMPARE_LESS,
	DEPTH_COMPARE_COUNT
};

static ID3D12PipelineState *im2DPipelines[BLEND_MODE_COUNT][DEPTH_COMPARE_COUNT][DEPTH_MODE_COUNT][PIPELINE_TOPOLOGY_COUNT];
static ID3D12RootSignature *screenDropletRootSignature;
static ID3D12PipelineState *screenDropletPipeline;
static ID3D12RootSignature *im3DRootSignature;
static ID3D12PipelineState *im3DPipelines[BLEND_MODE_COUNT][DEPTH_MODE_COUNT][PIPELINE_TOPOLOGY_COUNT];
static Raster *immediateWhiteRaster;
static ImmediateArena arenas[IMMEDIATE_FRAME_COUNT];
static uint32 activeArena = UINT32_MAX;
static void *renderStates[RENDER_STATE_COUNT];
static bool32 immediateReady;
static D3D12_VERTEX_BUFFER_VIEW im3DVertexView;
static int32 num3DVertices;
static uint32 im3DFlags;
static float im3DWorld[16];
static bool32 immediate2DStrictDepth;

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
	HRESULT hr = D3DCompile(source, strlen(source), "librw_d3d12_im2d",
	                        nil, nil, entry, target, flags, 0, shader, &errors);
	if(FAILED(hr) && errors)
		fprintf(stderr, "librw D3D12 Im2D shader: %s\n",
		        (const char*)errors->GetBufferPointer());
	releaseCom(errors);
	return SUCCEEDED(hr);
}

static bool32
getPipelineBlend(uint32 mode, D3D12_BLEND *source, D3D12_BLEND *destination)
{
	switch(mode){
	case BLEND_ADD_ONE:
		*source = D3D12_BLEND_ONE; *destination = D3D12_BLEND_ONE; break;
	case BLEND_ADD_ALPHA:
		*source = D3D12_BLEND_SRC_ALPHA; *destination = D3D12_BLEND_ONE; break;
	case BLEND_SHADOW:
		*source = D3D12_BLEND_ZERO; *destination = D3D12_BLEND_INV_SRC_COLOR; break;
	case BLEND_INVERSE_DEST:
		*source = D3D12_BLEND_INV_DEST_COLOR; *destination = D3D12_BLEND_ZERO; break;
	case BLEND_REPLACE:
		*source = D3D12_BLEND_ONE; *destination = D3D12_BLEND_ZERO; break;
	case BLEND_KEEP_DESTINATION:
		*source = D3D12_BLEND_ZERO; *destination = D3D12_BLEND_ONE; break;
	case BLEND_MODULATE_DESTINATION:
		*source = D3D12_BLEND_ZERO; *destination = D3D12_BLEND_SRC_COLOR; break;
	default:
		*source = D3D12_BLEND_SRC_ALPHA; *destination = D3D12_BLEND_INV_SRC_ALPHA; break;
	}
	return 1;
}

static uint32
getActiveBlendMode(void)
{
	void *source = renderStates[SRCBLEND];
	void *destination = renderStates[DESTBLEND];
	if(source == (void*)BLENDONE && destination == (void*)BLENDONE)
		return BLEND_ADD_ONE;
	if(source == (void*)BLENDSRCALPHA && destination == (void*)BLENDONE)
		return BLEND_ADD_ALPHA;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDINVSRCCOLOR)
		return BLEND_SHADOW;
	if(source == (void*)BLENDINVDESTCOLOR && destination == (void*)BLENDZERO)
		return BLEND_INVERSE_DEST;
	if(source == (void*)BLENDONE && destination == (void*)BLENDZERO)
		return BLEND_REPLACE;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDONE)
		return BLEND_KEEP_DESTINATION;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDSRCCOLOR)
		return BLEND_MODULATE_DESTINATION;
	return BLEND_ALPHA;
}

static bool32
createPipeline(ID3D12Device *device, D3D12_PRIMITIVE_TOPOLOGY_TYPE type,
	           const D3D12_SHADER_BYTECODE &vs,
	           const D3D12_SHADER_BYTECODE &ps,
	           const D3D12_INPUT_LAYOUT_DESC &input,
	           uint32 blendMode, bool32 depthTest, bool32 depthWrite,
	           bool32 strictDepth,
	           ID3D12PipelineState **pipeline)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = immediateRootSignature;
	desc.VS = vs;
	desc.PS = ps;
	desc.InputLayout = input;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	getPipelineBlend(blendMode, &desc.BlendState.RenderTarget[0].SrcBlend,
	                 &desc.BlendState.RenderTarget[0].DestBlend);
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = depthTest || depthWrite;
	desc.DepthStencilState.DepthWriteMask = depthWrite ?
		D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = depthTest ?
		(strictDepth ? D3D12_COMPARISON_FUNC_LESS :
		 D3D12_COMPARISON_FUNC_LESS_EQUAL) : D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = type;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	return SUCCEEDED(device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(pipeline)));
}

static bool32
createIm3DPipeline(ID3D12Device *device,
	               D3D12_PRIMITIVE_TOPOLOGY_TYPE type,
	               const D3D12_SHADER_BYTECODE &vs,
	               const D3D12_SHADER_BYTECODE &ps,
	               const D3D12_INPUT_LAYOUT_DESC &input,
	               uint32 blendMode, bool32 depthTest, bool32 depthWrite,
	               ID3D12PipelineState **pipeline)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = im3DRootSignature;
	desc.VS = vs;
	desc.PS = ps;
	desc.InputLayout = input;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	getPipelineBlend(blendMode, &desc.BlendState.RenderTarget[0].SrcBlend,
	                 &desc.BlendState.RenderTarget[0].DestBlend);
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = depthTest || depthWrite;
	desc.DepthStencilState.DepthWriteMask = depthWrite ?
		D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = depthTest ?
		D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = type;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	return SUCCEEDED(device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(pipeline)));
}

static bool32
createScreenDropletResources(ID3D12Device *device)
{
	D3D12_DESCRIPTOR_RANGE ranges[2];
	memset(ranges, 0, sizeof(ranges));
	for(uint32 i = 0; i < 2; i++){
		ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[i].NumDescriptors = 1;
		ranges[i].BaseShaderRegister = i;
		ranges[i].OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	}
	D3D12_ROOT_PARAMETER params[3];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 2;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	for(uint32 i = 0; i < 2; i++){
		params[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[i+1].DescriptorTable.NumDescriptorRanges = 1;
		params[i+1].DescriptorTable.pDescriptorRanges = &ranges[i];
		params[i+1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	D3D12_STATIC_SAMPLER_DESC sampler;
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 3;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	signature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 screen droplets root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&screenDropletRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer ScreenConstants : register(b0) { float2 screenSize; };"
		"Texture2D maskTexture : register(t0);"
		"Texture2D sceneTexture : register(t1);"
		"SamplerState linearSampler : register(s0);"
		"struct VSIn { float4 position : POSITION; uint color : COLOR0;"
		" float2 maskUV : TEXCOORD0; float2 sceneUV : TEXCOORD1; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 maskUV : TEXCOORD0; float2 sceneUV : TEXCOORD1; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" output.position = float4(input.position.x * 2.0 / screenSize.x - 1.0,"
		" 1.0 - input.position.y * 2.0 / screenSize.y, input.position.z, 1.0);"
		" output.color = float4((input.color >> 16) & 255,"
		" (input.color >> 8) & 255, input.color & 255,"
		" (input.color >> 24) & 255) / 255.0;"
		" output.maskUV = input.maskUV; output.sceneUV = input.sceneUV;"
		" return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float4 mask = maskTexture.Sample(linearSampler, input.maskUV);"
		" float4 scene = sceneTexture.Sample(linearSampler, input.sceneUV);"
		" return float4(input.color.rgb * mask.rgb * scene.rgb,"
		" input.color.a * mask.a); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		return 0;
	}
	D3D12_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = screenDropletRootSignature;
	desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	desc.VS.BytecodeLength = vertexShader->GetBufferSize();
	desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	desc.PS.BytecodeLength = pixelShader->GetBufferSize();
	desc.InputLayout.pInputElementDescs = elements;
	desc.InputLayout.NumElements = (UINT)nelem(elements);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(&screenDropletPipeline));
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	return SUCCEEDED(hr);
}

bool32
initializeImmediate(void)
{
	if(immediateReady)
		return 1;
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_DESCRIPTOR_RANGE range;
	memset(&range, 0, sizeof(range));
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart =
		D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[2];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 2;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler;
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 2;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	signature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 Im2D root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&immediateRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer ScreenConstants : register(b0) { float2 screenSize; };"
		"Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
		"struct VSIn { float4 position : POSITION; uint color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" output.position = float4(input.position.x * 2.0 / screenSize.x - 1.0,"
		" 1.0 - input.position.y * 2.0 / screenSize.y, input.position.z, 1.0);"
		" output.color = float4((input.color >> 16) & 255,"
		" (input.color >> 8) & 255, input.color & 255,"
		" (input.color >> 24) & 255) / 255.0;"
		" output.uv = input.uv; return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" return input.color * image.Sample(imageSampler, input.uv); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		shutdownImmediate();
		return 0;
	}
	D3D12_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_INPUT_LAYOUT_DESC input = { elements, (UINT)nelem(elements) };
	D3D12_SHADER_BYTECODE vs = {
		vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()
	};
	D3D12_SHADER_BYTECODE ps = {
		pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()
	};
	static const D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyTypes[] = {
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
	};
	bool32 pipelinesReady = 1;
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 compare = 0; compare < DEPTH_COMPARE_COUNT; compare++)
			for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
				for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++){
				bool32 depthTest = depth == DEPTH_TEST_ONLY || depth == DEPTH_TEST_WRITE;
				bool32 depthWrite = depth == DEPTH_TEST_WRITE || depth == DEPTH_WRITE_ONLY;
				pipelinesReady = pipelinesReady && createPipeline(
					device, topologyTypes[topology], vs, ps, input,
					blend, depthTest, depthWrite,
					compare == DEPTH_COMPARE_LESS,
					&im2DPipelines[blend][compare][depth][topology]);
				}
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	if(!pipelinesReady){
		shutdownImmediate();
		return 0;
	}
	if(!createScreenDropletResources(device)){
		shutdownImmediate();
		return 0;
	}

	D3D12_ROOT_PARAMETER im3DParams[2];
	memset(im3DParams, 0, sizeof(im3DParams));
	im3DParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	im3DParams[0].Constants.ShaderRegister = 0;
	im3DParams[0].Constants.Num32BitValues = 55;
	im3DParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	im3DParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	im3DParams[1].DescriptorTable.NumDescriptorRanges = 1;
	im3DParams[1].DescriptorTable.pDescriptorRanges = &range;
	im3DParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC im3DSignature = signature;
	im3DSignature.pParameters = im3DParams;
	// RenderWare's immediate 3D effects (notably raindrop4) deliberately use
	// UVs outside 0..1 to tile their textures. Keep Im2D clamped, but preserve
	// the legacy wrapping behaviour for Im3D.
	D3D12_STATIC_SAMPLER_DESC im3DSampler = sampler;
	im3DSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	im3DSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	im3DSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	im3DSignature.pStaticSamplers = &im3DSampler;
	hr = D3D12SerializeRootSignature(
		&im3DSignature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 Im3D root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		shutdownImmediate();
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&im3DRootSignature));
	releaseCom(serialized);
	if(FAILED(hr)){
		shutdownImmediate();
		return 0;
	}
	static const char *im3DShaderSource =
		"cbuffer DrawConstants : register(b0) {"
		" row_major float4x4 world; row_major float4x4 view;"
		" row_major float4x4 projection; float4 drawFlags;"
		" float fogEnd; float fogRange; uint fogColorPacked; };"
		"Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
		"struct VSIn { float3 position : POSITION; uint color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 uv : TEXCOORD0; float fogFactor : TEXCOORD1; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" float4 p = mul(float4(input.position, 1.0), world);"
		" p = mul(p, view); output.position = mul(p, projection);"
		" output.fogFactor = fogRange < 0.0 ?"
		" saturate((p.z - fogEnd) * fogRange) : 1.0;"
		" output.color = float4((input.color >> 16) & 255,"
		" (input.color >> 8) & 255, input.color & 255,"
		" (input.color >> 24) & 255) / 255.0;"
		" output.uv = input.uv; return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float4 color = input.color;"
		" if(drawFlags.x > 0.5) color *= image.Sample(imageSampler, input.uv);"
		" clip(color.a - 0.02);"
		" float3 fogColor = float3(fogColorPacked & 255u,"
		" (fogColorPacked >> 8) & 255u, (fogColorPacked >> 16) & 255u) / 255.0;"
		" color.rgb = lerp(fogColor, color.rgb, input.fogFactor);"
		" return color; }";
	vertexShader = nil;
	pixelShader = nil;
	if(!compileShader(im3DShaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(im3DShaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		shutdownImmediate();
		return 0;
	}
	D3D12_INPUT_ELEMENT_DESC im3DElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 12,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_INPUT_LAYOUT_DESC im3DInput = {
		im3DElements, (UINT)nelem(im3DElements)
	};
	vs.pShaderBytecode = vertexShader->GetBufferPointer();
	vs.BytecodeLength = vertexShader->GetBufferSize();
	ps.pShaderBytecode = pixelShader->GetBufferPointer();
	ps.BytecodeLength = pixelShader->GetBufferSize();
	pipelinesReady = 1;
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
			for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++){
				bool32 depthTest = depth == DEPTH_TEST_ONLY || depth == DEPTH_TEST_WRITE;
				bool32 depthWrite = depth == DEPTH_TEST_WRITE || depth == DEPTH_WRITE_ONLY;
				pipelinesReady = pipelinesReady && createIm3DPipeline(
					device, topologyTypes[topology], vs, ps, im3DInput,
					blend, depthTest, depthWrite,
					&im3DPipelines[blend][depth][topology]);
			}
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	if(!pipelinesReady){
		shutdownImmediate();
		return 0;
	}

	D3D12_HEAP_PROPERTIES props = uploadHeapProperties();
	D3D12_RESOURCE_DESC buffer = bufferDesc(IMMEDIATE_UPLOAD_SIZE);
	for(uint32 i = 0; i < IMMEDIATE_FRAME_COUNT; i++){
		if(FAILED(device->CreateCommittedResource(
		       &props, D3D12_HEAP_FLAG_NONE, &buffer,
		       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
		       IID_PPV_ARGS(&arenas[i].resource)))){
			shutdownImmediate();
			return 0;
		}
		D3D12_RANGE readRange = { 0, 0 };
		if(FAILED(arenas[i].resource->Map(
		       0, &readRange, (void**)&arenas[i].mapped))){
			shutdownImmediate();
			return 0;
		}
	}

	Image *white = Image::create(1, 1, 32);
	if(white == nil){
		shutdownImmediate();
		return 0;
	}
	white->allocate();
	white->pixels[0] = white->pixels[1] = white->pixels[2] =
		white->pixels[3] = 0xFF;
	immediateWhiteRaster = Raster::createFromImage(white, PLATFORM_D3D12);
	white->destroy();
	if(immediateWhiteRaster == nil){
		shutdownImmediate();
		return 0;
	}
	memset(renderStates, 0, sizeof(renderStates));
	renderStates[ZTESTENABLE] = (void*)1;
	renderStates[ZWRITEENABLE] = (void*)1;
	renderStates[VERTEXALPHA] = (void*)1;
	renderStates[SRCBLEND] = (void*)BLENDSRCALPHA;
	renderStates[DESTBLEND] = (void*)BLENDINVSRCALPHA;
	activeArena = UINT32_MAX;
	immediateReady = 1;
	return 1;
}

void
shutdownImmediate(void)
{
	immediateReady = 0;
	activeArena = UINT32_MAX;
	if(immediateWhiteRaster){
		immediateWhiteRaster->destroy();
		immediateWhiteRaster = nil;
	}
	for(uint32 i = 0; i < IMMEDIATE_FRAME_COUNT; i++){
		if(arenas[i].resource && arenas[i].mapped)
			arenas[i].resource->Unmap(0, nil);
		arenas[i].mapped = nil;
		arenas[i].offset = 0;
		releaseCom(arenas[i].resource);
	}
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 compare = 0; compare < DEPTH_COMPARE_COUNT; compare++)
			for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
				for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++)
					releaseCom(im2DPipelines[blend][compare][depth][topology]);
	releaseCom(immediateRootSignature);
	releaseCom(screenDropletPipeline);
	releaseCom(screenDropletRootSignature);
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
			for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++)
				releaseCom(im3DPipelines[blend][depth][topology]);
	releaseCom(im3DRootSignature);
	memset(&im3DVertexView, 0, sizeof(im3DVertexView));
	num3DVertices = 0;
}

void
setRenderState(int32 state, void *value)
{
	if(state >= 0 && state < RENDER_STATE_COUNT)
		renderStates[state] = value;
}

void
setImmediate2DStrictDepth(bool32 enabled)
{
	immediate2DStrictDepth = enabled;
}

void*
getRenderState(int32 state)
{
	return state >= 0 && state < RENDER_STATE_COUNT ?
		renderStates[state] : nil;
}

static uint32
alignUpload(uint32 value)
{
	return (value + 15u) & ~15u;
}

static bool32
allocateUpload(const void *data, uint32 size, uint64 *gpuAddress)
{
	if(data == nil || size == 0 || gpuAddress == nil)
		return 0;
	uint32 frame = getFrameIndex() % IMMEDIATE_FRAME_COUNT;
	if(activeArena != frame){
		activeArena = frame;
		arenas[frame].offset = 0;
	}
	ImmediateArena &arena = arenas[frame];
	uint32 offset = alignUpload(arena.offset);
	if(arena.mapped == nil || offset + size > IMMEDIATE_UPLOAD_SIZE)
		return 0;
	memcpy(arena.mapped + offset, data, size);
	*gpuAddress = arena.resource->GetGPUVirtualAddress() + offset;
	arena.offset = offset + size;
	return 1;
}

static bool32
uploadImmediate(const void *vertices, uint32 vertexBytes,
	            const void *indices, uint32 indexBytes,
	            D3D12_VERTEX_BUFFER_VIEW *vertexView,
	            D3D12_INDEX_BUFFER_VIEW *indexView)
{
	uint64 vertexAddress;
	if(!allocateUpload(vertices, vertexBytes, &vertexAddress))
		return 0;
	vertexView->BufferLocation = vertexAddress;
	vertexView->SizeInBytes = vertexBytes;
	vertexView->StrideInBytes = sizeof(Im2DVertex);
	if(indexBytes){
		uint64 indexAddress;
		if(!allocateUpload(indices, indexBytes, &indexAddress))
			return 0;
		indexView->BufferLocation = indexAddress;
		indexView->SizeInBytes = indexBytes;
		indexView->Format = DXGI_FORMAT_R16_UINT;
	}
	return 1;
}

static ID3D12PipelineState*
selectPipeline(PrimitiveType type, D3D12_PRIMITIVE_TOPOLOGY *topology)
{
	uint32 blend = getActiveBlendMode();
	bool32 depthTest = renderStates[ZTESTENABLE] != nil;
	bool32 depthWrite = renderStates[ZWRITEENABLE] != nil;
	// The radar writes an invisible depth mask at exactly the same Z as its
	// nine textured tiles. Only that tile pass needs strict LESS; using it for
	// every Im2D draw clips font glyphs and the rest of the HUD.
	uint32 compare = immediate2DStrictDepth && depthTest && !depthWrite ?
		DEPTH_COMPARE_LESS : DEPTH_COMPARE_LESS_EQUAL;
	uint32 depth = depthTest ?
		(depthWrite ? DEPTH_TEST_WRITE : DEPTH_TEST_ONLY) :
		(depthWrite ? DEPTH_WRITE_ONLY : DEPTH_DISABLED);
	switch(type){
	case PRIMTYPELINELIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		return im2DPipelines[blend][compare][depth][PIPELINE_LINE];
	case PRIMTYPEPOLYLINE:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
		return im2DPipelines[blend][compare][depth][PIPELINE_LINE];
	case PRIMTYPEPOINTLIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		return im2DPipelines[blend][compare][depth][PIPELINE_POINT];
	case PRIMTYPETRISTRIP:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		return im2DPipelines[blend][compare][depth][PIPELINE_TRIANGLE];
	default:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		return im2DPipelines[blend][compare][depth][PIPELINE_TRIANGLE];
	}
}

static void
drawImmediate(PrimitiveType type, Im2DVertex *vertices, int32 numVertices,
	          uint16 *indices, int32 numIndices)
{
	if(!immediateReady || vertices == nil || numVertices <= 0)
		return;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return;

	uint16 *fanIndices = nil;
	if(type == PRIMTYPETRIFAN && numVertices >= 3){
		int32 sourceCount = indices ? numIndices : numVertices;
		int32 triangleCount = sourceCount - 2;
		fanIndices = rwNewT(uint16, triangleCount*3,
		                    MEMDUR_EVENT | ID_DRIVER);
		for(int32 i = 0; i < triangleCount; i++){
			fanIndices[i*3] = indices ? indices[0] : 0;
			fanIndices[i*3+1] = indices ? indices[i+1] : (uint16)(i+1);
			fanIndices[i*3+2] = indices ? indices[i+2] : (uint16)(i+2);
		}
		indices = fanIndices;
		numIndices = triangleCount*3;
		type = PRIMTYPETRILIST;
	}

	D3D12_VERTEX_BUFFER_VIEW vertexView;
	D3D12_INDEX_BUFFER_VIEW indexView;
	memset(&indexView, 0, sizeof(indexView));
	if(!uploadImmediate(vertices, numVertices*sizeof(Im2DVertex), indices,
	                    indices ? numIndices*sizeof(uint16) : 0,
	                    &vertexView, &indexView)){
		rwFree(fanIndices);
		return;
	}
	D3D12_PRIMITIVE_TOPOLOGY topology;
	ID3D12PipelineState *pipeline = selectPipeline(type, &topology);
	list->SetGraphicsRootSignature(immediateRootSignature);
	list->SetPipelineState(pipeline);
	list->IASetPrimitiveTopology(topology);
	list->IASetVertexBuffers(0, 1, &vertexView);
	if(indices)
		list->IASetIndexBuffer(&indexView);
	int32 width, height;
	getPresentSize(&width, &height);
	float screen[2] = {
		(float)(width > 0 ? width : 1), (float)(height > 0 ? height : 1)
	};
	list->SetGraphicsRoot32BitConstants(0, 2, screen, 0);
	Raster *raster = (Raster*)renderStates[TEXTURERASTER];
	D3D12_GPU_DESCRIPTOR_HANDLE texture;
	if(!getTextureView(raster, &texture, nil))
		getTextureView(immediateWhiteRaster, &texture, nil);
	list->SetGraphicsRootDescriptorTable(1, texture);
	if(indices)
		list->DrawIndexedInstanced(numIndices, 1, 0, 0, 0);
	else
		list->DrawInstanced(numVertices, 1, 0, 0);
	rwFree(fanIndices);
}

bool32
renderScreenDroplets(void *vertices, int32 numVertices, int32 vertexStride,
	                 void *indices, int32 numIndices,
	                 Raster *maskRaster, Raster *sceneRaster)
{
	if(!immediateReady || screenDropletPipeline == nil ||
	   vertices == nil || indices == nil || numVertices <= 0 ||
	   numIndices <= 0 || vertexStride < 36)
		return 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return 0;
	D3D12_GPU_DESCRIPTOR_HANDLE maskView, sceneView;
	if(!getTextureView(maskRaster, &maskView, nil) ||
	   !getTextureView(sceneRaster, &sceneView, nil))
		return 0;
	uint64 vertexAddress, indexAddress;
	uint32 vertexBytes = numVertices*vertexStride;
	uint32 indexBytes = numIndices*sizeof(uint16);
	if(!allocateUpload(vertices, vertexBytes, &vertexAddress) ||
	   !allocateUpload(indices, indexBytes, &indexAddress))
		return 0;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	vertexView.BufferLocation = vertexAddress;
	vertexView.SizeInBytes = vertexBytes;
	vertexView.StrideInBytes = vertexStride;
	D3D12_INDEX_BUFFER_VIEW indexView;
	indexView.BufferLocation = indexAddress;
	indexView.SizeInBytes = indexBytes;
	indexView.Format = DXGI_FORMAT_R16_UINT;
	list->SetGraphicsRootSignature(screenDropletRootSignature);
	list->SetPipelineState(screenDropletPipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->IASetVertexBuffers(0, 1, &vertexView);
	list->IASetIndexBuffer(&indexView);
	int32 width, height;
	getPresentSize(&width, &height);
	float screen[2] = {
		(float)(width > 0 ? width : 1), (float)(height > 0 ? height : 1)
	};
	list->SetGraphicsRoot32BitConstants(0, 2, screen, 0);
	list->SetGraphicsRootDescriptorTable(1, maskView);
	list->SetGraphicsRootDescriptorTable(2, sceneView);
	list->DrawIndexedInstanced(numIndices, 1, 0, 0, 0);
	return 1;
}

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	uint16 indices[2] = { (uint16)vert1, (uint16)vert2 };
	drawImmediate(PRIMTYPELINELIST, (Im2DVertex*)vertices, numVertices,
	              indices, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1,
	               int32 vert2, int32 vert3)
{
	uint16 indices[3] = {
		(uint16)vert1, (uint16)vert2, (uint16)vert3
	};
	drawImmediate(PRIMTYPETRILIST, (Im2DVertex*)vertices, numVertices,
	              indices, 3);
}

void
im2DRenderPrimitive(PrimitiveType type, void *vertices, int32 numVertices)
{
	drawImmediate(type, (Im2DVertex*)vertices, numVertices, nil, 0);
}

void
im2DRenderIndexedPrimitive(PrimitiveType type, void *vertices,
	                       int32 numVertices, void *indices, int32 numIndices)
{
	drawImmediate(type, (Im2DVertex*)vertices, numVertices,
	              (uint16*)indices, numIndices);
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

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	num3DVertices = 0;
	if(!immediateReady || vertices == nil || numVertices <= 0)
		return;
	uint64 address;
	uint32 size = numVertices*sizeof(Im3DVertex);
	if(!allocateUpload(vertices, size, &address))
		return;
	im3DVertexView.BufferLocation = address;
	im3DVertexView.SizeInBytes = size;
	im3DVertexView.StrideInBytes = sizeof(Im3DVertex);
	num3DVertices = numVertices;
	im3DFlags = flags;
	if(world)
		fillMatrix(im3DWorld, world);
	else{
		memset(im3DWorld, 0, sizeof(im3DWorld));
		im3DWorld[0] = im3DWorld[5] = im3DWorld[10] = im3DWorld[15] = 1.0f;
	}
	if((flags & im3d::VERTEXUV) == 0)
		renderStates[TEXTURERASTER] = nil;
}

static ID3D12PipelineState*
selectIm3DPipeline(PrimitiveType type,
	               D3D12_PRIMITIVE_TOPOLOGY *topology)
{
	uint32 blend = getActiveBlendMode();
	bool32 depthTest = renderStates[ZTESTENABLE] != nil;
	bool32 depthWrite = renderStates[ZWRITEENABLE] != nil;
	uint32 depth = depthTest ?
		(depthWrite ? DEPTH_TEST_WRITE : DEPTH_TEST_ONLY) :
		(depthWrite ? DEPTH_WRITE_ONLY : DEPTH_DISABLED);
	switch(type){
	case PRIMTYPELINELIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		return im3DPipelines[blend][depth][PIPELINE_LINE];
	case PRIMTYPEPOLYLINE:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
		return im3DPipelines[blend][depth][PIPELINE_LINE];
	case PRIMTYPEPOINTLIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		return im3DPipelines[blend][depth][PIPELINE_POINT];
	case PRIMTYPETRISTRIP:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		return im3DPipelines[blend][depth][PIPELINE_TRIANGLE];
	default:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		return im3DPipelines[blend][depth][PIPELINE_TRIANGLE];
	}
}

static void
drawIm3D(PrimitiveType type, uint16 *indices, int32 numIndices)
{
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(!immediateReady || list == nil || camera == nil || num3DVertices <= 0)
		return;

	uint16 *fanIndices = nil;
	if(type == PRIMTYPETRIFAN){
		int32 sourceCount = indices ? numIndices : num3DVertices;
		if(sourceCount < 3)
			return;
		int32 triangleCount = sourceCount - 2;
		fanIndices = rwNewT(uint16, triangleCount*3,
		                    MEMDUR_EVENT | ID_DRIVER);
		for(int32 i = 0; i < triangleCount; i++){
			fanIndices[i*3] = indices ? indices[0] : 0;
			fanIndices[i*3+1] = indices ? indices[i+1] : (uint16)(i+1);
			fanIndices[i*3+2] = indices ? indices[i+2] : (uint16)(i+2);
		}
		indices = fanIndices;
		numIndices = triangleCount*3;
		type = PRIMTYPETRILIST;
	}

	D3D12_INDEX_BUFFER_VIEW indexView;
	if(indices){
		uint64 address;
		if(!allocateUpload(indices, numIndices*sizeof(uint16), &address)){
			rwFree(fanIndices);
			return;
		}
		indexView.BufferLocation = address;
		indexView.SizeInBytes = numIndices*sizeof(uint16);
		indexView.Format = DXGI_FORMAT_R16_UINT;
	}
	D3D12_PRIMITIVE_TOPOLOGY topology;
	ID3D12PipelineState *pipeline = selectIm3DPipeline(type, &topology);
	list->SetGraphicsRootSignature(im3DRootSignature);
	list->SetPipelineState(pipeline);
	list->IASetPrimitiveTopology(topology);
	list->IASetVertexBuffers(0, 1, &im3DVertexView);
	if(indices)
		list->IASetIndexBuffer(&indexView);
	float constants[55];
	memset(constants, 0, sizeof(constants));
	memcpy(constants, im3DWorld, 16*sizeof(float));
	memcpy(constants + 16, &camera->devView, 16*sizeof(float));
	memcpy(constants + 32, &camera->devProj, 16*sizeof(float));
	Raster *raster = (Raster*)renderStates[TEXTURERASTER];
	D3D12_GPU_DESCRIPTOR_HANDLE texture;
	bool32 textured = (im3DFlags & im3d::VERTEXUV) &&
		getTextureView(raster, &texture, nil);
	if(!textured)
		getTextureView(immediateWhiteRaster, &texture, nil);
	constants[48] = textured ? 1.0f : 0.0f;
	if(getRenderState(FOGENABLE) != nil &&
	   camera->fogPlane < camera->farPlane){
		constants[52] = camera->farPlane;
		constants[53] = 1.0f/(camera->fogPlane - camera->farPlane);
	}
	uint32 packedFog = (uint32)(uintptr_t)getRenderState(FOGCOLOR);
	memcpy(&constants[54], &packedFog, sizeof(packedFog));
	list->SetGraphicsRoot32BitConstants(0, 55, constants, 0);
	list->SetGraphicsRootDescriptorTable(1, texture);
	if(indices)
		list->DrawIndexedInstanced(numIndices, 1, 0, 0, 0);
	else
		list->DrawInstanced(num3DVertices, 1, 0, 0);
	rwFree(fanIndices);
}

void
im3DRenderPrimitive(PrimitiveType type)
{
	drawIm3D(type, nil, 0);
}

void
im3DRenderIndexedPrimitive(PrimitiveType type, void *indices,
	                       int32 numIndices)
{
	drawIm3D(type, (uint16*)indices, numIndices);
}

void
im3DEnd(void)
{
	num3DVertices = 0;
}

#else

bool32 initializeImmediate(void) { return 0; }
void shutdownImmediate(void) { }
void setRenderState(int32, void*) { }
void *getRenderState(int32) { return nil; }
void setImmediate2DStrictDepth(bool32) { }
void im2DRenderLine(void*, int32, int32, int32) { }
void im2DRenderTriangle(void*, int32, int32, int32, int32) { }
void im2DRenderPrimitive(PrimitiveType, void*, int32) { }
void im2DRenderIndexedPrimitive(PrimitiveType, void*, int32, void*, int32) { }
void im3DTransform(void*, int32, Matrix*, uint32) { }
void im3DRenderPrimitive(PrimitiveType) { }
void im3DRenderIndexedPrimitive(PrimitiveType, void*, int32) { }
void im3DEnd(void) { }

#endif

}
}
