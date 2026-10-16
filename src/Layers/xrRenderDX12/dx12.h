#pragma once
// D3D12 backend of the R4 renderer, built into the DX12 exe (xrRender_R5, USE_DX12).
//
// The renderer sources talk to the GPU through a device and an immediate context, with D3D11 call shapes and the
// D3D11 description structures (D3D11_TEXTURE2D_DESC, D3D11_BLEND_DESC, ...). Under USE_DX12 those calls land on the
// classes here, which do the work natively in D3D12:
//  - resources are D3D12 resources from the D3D12 Memory Allocator; dynamic buffers live in a per-frame upload ring,
//    staging textures in CPU memory (write) or readback buffers (read)
//  - views own a slot in a CPU descriptor heap; at a draw the bound views of each stage are copied into the
//    shader-visible heap and set as descriptor tables
//  - shaders keep their DXBC (FXC SM 5.0, as R4 compiles it) and the bindings their reflection reports; the root
//    signature of a draw is built from the bindings of its shaders (root CBVs, one SRV table, one sampler table per
//    stage, UAVs for compute) and cached
//  - the state objects, shaders, input layout and target formats of a draw select a cached pipeline state
//  - resource states are tracked per subresource and the barriers are issued before the draw, copy or clear that
//    needs them; D3D11's hazard rule (a resource bound as output is not readable at the same draw) is kept
//
// Nothing here talks to D3D11: d3d11 headers provide the description structures only.

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace D3D12MA
{
class Allocator;
class Allocation;
}

namespace dx12
{
class Device;
class Context;
class Resource;

// Binding limits per shader stage (the renderer uses up to 16 SRVs and samplers, 14 constant buffers)
const UINT kMaxCB = 14;
const UINT kMaxSRV = 32;
const UINT kMaxSamplers = 16;
const UINT kMaxUAV = 8;
const UINT kMaxRT = 8;
const UINT kMaxVB = 4;

enum Stage
{
	StageVS = 0,
	StageHS,
	StageDS,
	StageGS,
	StagePS,
	StageCS,
	StageCount
};

//-------------------------------------------------------------------------------------------------------------------
// Reference-counted base (COM-like lifetime without COM). Release at zero deletes the object (with threaded recording
// once the worker is past the commands queued so far); GPU objects it owns go to the device's deferred-release list
// until the GPU is past the frame that could still use them.
class Object
{
public:
	Object() {}
	virtual ~Object();

	ULONG AddRef() { return ULONG(InterlockedIncrement(&m_refs)); }
	ULONG Release();

	virtual HRESULT QueryInterface(REFIID riid, void** out);
	template <class Q>
	HRESULT QueryInterface(Q** out) { return QueryInterface(__uuidof(Q), reinterpret_cast<void**>(out)); }

	HRESULT SetPrivateData(REFGUID guid, UINT size, const void* data);
	HRESULT SetPrivateDataInterface(REFGUID guid, const IUnknown* data);
	HRESULT GetPrivateData(REFGUID guid, UINT* size, void* data);

	u64 Id() const { return m_id; }
	static u64 NewId();

protected:
	volatile LONG m_refs = 1;
	u64 m_id = NewId();

private:
	struct PrivateEntry
	{
		GUID guid;
		IUnknown* unknown;
		std::vector<u8> data;
	};
	std::vector<PrivateEntry>* m_private = nullptr;

	Object(const Object&) = delete;
	Object& operator=(const Object&) = delete;
};

//-------------------------------------------------------------------------------------------------------------------
// Resources

// Layout of one subresource in a staging texture's CPU memory or in an upload/readback buffer
struct SubresourceLayout
{
	u64 offset;
	UINT row_pitch;
	UINT depth_pitch;
	UINT rows;
	UINT row_bytes;
	UINT depth;
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c01")) Resource : public Object
{
public:
	~Resource() override;
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;

	void GetType(D3D11_RESOURCE_DIMENSION* dim) const { *dim = m_dimension; }
	UINT GetEvictionPriority() const { return 0; }
	void SetEvictionPriority(UINT) {}

	ID3D12Resource* Native() const { return m_resource; }
	bool IsBuffer() const { return m_dimension == D3D11_RESOURCE_DIMENSION_BUFFER; }
	UINT ArraySize() const { return m_dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : m_depth_or_array; }
	UINT Subresource(UINT mip, UINT slice, UINT plane) const { return mip + slice * m_mips + plane * m_mips * ArraySize(); }
	UINT SubresourcesPerPlane() const { return m_mips * ArraySize(); }

	// Whole-resource state, or per subresource (m_states) once views touched parts of it differently
	D3D12_RESOURCE_STATES StateOf(UINT sub) const { return m_states.empty() ? m_state : m_states[sub]; }

	D3D11_RESOURCE_DIMENSION m_dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
	D3D11_USAGE m_usage = D3D11_USAGE_DEFAULT;
	UINT m_bind = 0;
	UINT m_cpu_access = 0;
	UINT m_misc = 0;
	DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
	UINT m_width = 1, m_height = 1, m_depth_or_array = 1, m_mips = 1;
	UINT m_samples = 1, m_sample_quality = 0;
	UINT m_planes = 1;
	UINT m_subresources = 1;

	ID3D12Resource* m_resource = nullptr;
	D3D12MA::Allocation* m_allocation = nullptr;
	xr_string m_debug_name; // the engine's render target name, for r__dx12_trace

	D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
	std::vector<D3D12_RESOURCE_STATES> m_states;

	// STAGING with CPU write: the data in CPU memory, uploaded by CopyResource / CopySubresourceRegion
	std::vector<u8> m_cpu_data;
	std::vector<SubresourceLayout> m_layouts;

	// STAGING with CPU read: a readback buffer laid out per D3D12 footprints, and the fence of the last copy into it
	ID3D12Resource* m_readback = nullptr;
	D3D12MA::Allocation* m_readback_allocation = nullptr;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> m_footprints;
	u64 m_readback_fence = 0; // set where the copy is recorded
	u64 m_readback_chunk = 0; // command chunk of the last copy into it (threaded recording)
	u8* m_readback_ptr = nullptr;

	// DYNAMIC buffers: the current slice of the upload ring (renamed by Map(WRITE_DISCARD))
	D3D12_GPU_VIRTUAL_ADDRESS m_va = 0;
	u8* m_ptr = nullptr;
	u64 m_ptr_frame = 0;
	// DYNAMIC textures: the pending upload of a mapped subresource
	u8* m_map_ptr = nullptr;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_map_footprint = {};
	ID3D12Resource* m_map_buffer = nullptr;

	bool m_back_buffer = false; // a swap chain buffer (owned by the swap chain, never deferred-released)
	bool m_back_buffer_alias = false; // stands for the current swap chain buffer where the commands are recorded
	bool m_written_uav = false; // written through a UAV since the last barrier
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c02")) Buffer : public Resource
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_BUFFER_DESC* desc) const { *desc = m_desc; }
	D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_va; }
	D3D11_BUFFER_DESC m_desc = {};
	// Small DYNAMIC buffers (constant buffers among them) keep their contents in CPU memory: D3D11 keeps a dynamic
	// buffer's data until the next map, while upload memory comes back after a few frames. Maps write here, the data
	// goes to the upload ring at Unmap and again in any later frame that uses the buffer unchanged
	std::vector<u8> m_shadow;
	bool m_shadowed = false;
	bool m_warned_stale = false;
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c03")) Texture1D : public Resource
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_TEXTURE1D_DESC* desc) const { *desc = m_desc; }
	D3D11_TEXTURE1D_DESC m_desc = {};
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c04")) Texture2D : public Resource
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_TEXTURE2D_DESC* desc) const { *desc = m_desc; }
	void GetDesc1(D3D11_TEXTURE2D_DESC* desc) const { *desc = m_desc; }
	D3D11_TEXTURE2D_DESC m_desc = {};
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c05")) Texture3D : public Resource
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_TEXTURE3D_DESC* desc) const { *desc = m_desc; }
	D3D11_TEXTURE3D_DESC m_desc = {};
};

//-------------------------------------------------------------------------------------------------------------------
// Views: a CPU descriptor and the subresources they cover

class View : public Object
{
public:
	~View() override;
	void GetResource(Resource** out) const; // the back buffer's view gives the current buffer

	Resource* m_res = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE m_cpu = {};
	UINT m_heap = 0; // descriptor heap type of m_cpu
	UINT m_slot = UINT(-1);
	DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
	UINT m_first_mip = 0, m_mip_count = 1;
	UINT m_first_slice = 0, m_slice_count = 1;
	UINT m_first_plane = 0, m_plane_count = 1;
	bool m_whole = true; // every subresource of the resource
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c11")) ShaderResourceView : public View
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* desc) const { *desc = m_desc; }
	D3D11_SHADER_RESOURCE_VIEW_DESC m_desc = {};
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c12")) RenderTargetView : public View
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* desc) const { *desc = m_desc; }
	D3D11_RENDER_TARGET_VIEW_DESC m_desc = {};
	bool m_back_buffer = false; // the swap chain's current buffer, whichever it is at use time
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c13")) DepthStencilView : public View
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* desc) const { *desc = m_desc; }
	D3D11_DEPTH_STENCIL_VIEW_DESC m_desc = {};
	bool m_read_only = false; // D3D11_DSV_READ_ONLY_DEPTH (and stencil, if the format has it)
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c14")) UnorderedAccessView : public View
{
public:
	HRESULT QueryInterface(REFIID riid, void** out) override;
	using Object::QueryInterface;
	void GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC* desc) const { *desc = m_desc; }
	D3D11_UNORDERED_ACCESS_VIEW_DESC m_desc = {};
};

//-------------------------------------------------------------------------------------------------------------------
// Shaders: DXBC and the bindings reflection reports

struct ShaderBindings
{
	u16 cb_mask = 0; // constant buffer slots used
	u8 srv_count = 0; // highest SRV slot used + 1
	u8 sampler_count = 0;
	u8 uav_count = 0;
	u8 srv_dim[kMaxSRV] = {}; // D3D_SRV_DIMENSION per slot (0: unused)
	u8 uav_dim[kMaxUAV] = {}; // D3D12_UAV_DIMENSION per slot (0: unused)
};

class Shader : public Object
{
public:
	D3D12_SHADER_BYTECODE Bytecode() const { return { m_code.data(), m_code.size() }; }
	std::vector<u8> m_code;
	ShaderBindings m_bindings;
	Stage m_stage = StageVS;
	u32 m_crc = 0; // of the bytecode, for the pipeline key written to disk
	xr_string m_name; // the engine's shader file, for logs
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c21")) VertexShader : public Shader {};
class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c22")) HullShader : public Shader {};
class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c23")) DomainShader : public Shader {};
class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c24")) GeometryShader : public Shader {};
class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c25")) PixelShader : public Shader {};
class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c26")) ComputeShader : public Shader {};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c27")) InputLayout : public Object
{
public:
	std::vector<D3D12_INPUT_ELEMENT_DESC> m_elements;
	std::vector<xr_string> m_names;
};

//-------------------------------------------------------------------------------------------------------------------
// State objects: the D3D11 description, deduplicated by the device (equal descriptions give the same object)

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c31")) BlendState : public Object
{
public:
	void GetDesc(D3D11_BLEND_DESC* desc) const { *desc = m_desc; }
	D3D11_BLEND_DESC m_desc = {};
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c32")) RasterizerState : public Object
{
public:
	void GetDesc(D3D11_RASTERIZER_DESC* desc) const { *desc = m_desc; }
	D3D11_RASTERIZER_DESC m_desc = {};
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c33")) DepthStencilState : public Object
{
public:
	void GetDesc(D3D11_DEPTH_STENCIL_DESC* desc) const { *desc = m_desc; }
	D3D11_DEPTH_STENCIL_DESC m_desc = {};
};

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c34")) SamplerState : public Object
{
public:
	~SamplerState() override;
	void GetDesc(D3D11_SAMPLER_DESC* desc) const { *desc = m_desc; }
	D3D11_SAMPLER_DESC m_desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE m_cpu = {};
	UINT m_slot = UINT(-1);
};

//-------------------------------------------------------------------------------------------------------------------
// Queries: occlusion and timestamps in query heaps, resolved into readback memory; read once their fence passed

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c41")) Query : public Object
{
public:
	~Query() override;
	void GetDesc(D3D11_QUERY_DESC* desc) const { *desc = m_desc; }
	UINT GetDataSize() const;
	D3D11_QUERY_DESC m_desc = {};
	UINT m_index = UINT(-1); // in the query heap of its type
	u64 m_fence = 0; // fence value of the submission that ends it (set where the End is recorded)
	u64 m_end_chunk = 0; // command chunk of the End (threaded recording)
	bool m_ended = false;
};

//-------------------------------------------------------------------------------------------------------------------
// Device: creates the objects. One per process, with one immediate context.

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c51")) Device
{
public:
	static Device* Create(IDXGIAdapter1* adapter, bool debug_layer);

	ULONG AddRef() { return ULONG(InterlockedIncrement(&m_refs)); }
	ULONG Release();
	HRESULT QueryInterface(REFIID riid, void** out);
	template <class Q>
	HRESULT QueryInterface(Q** out) { return QueryInterface(__uuidof(Q), reinterpret_cast<void**>(out)); }

	HRESULT CreateBuffer(const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Buffer** out);
	HRESULT CreateTexture1D(const D3D11_TEXTURE1D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Texture1D** out);
	HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Texture2D** out);
	HRESULT CreateTexture3D(const D3D11_TEXTURE3D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Texture3D** out);

	HRESULT CreateShaderResourceView(Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ShaderResourceView** out);
	HRESULT CreateRenderTargetView(Resource* res, const D3D11_RENDER_TARGET_VIEW_DESC* desc, RenderTargetView** out);
	HRESULT CreateDepthStencilView(Resource* res, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, DepthStencilView** out);
	HRESULT CreateUnorderedAccessView(Resource* res, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, UnorderedAccessView** out);

	HRESULT CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void* signature, SIZE_T signature_size,
	                          InputLayout** out);
	HRESULT CreateVertexShader(const void* code, SIZE_T size, void* linkage, VertexShader** out);
	HRESULT CreateHullShader(const void* code, SIZE_T size, void* linkage, HullShader** out);
	HRESULT CreateDomainShader(const void* code, SIZE_T size, void* linkage, DomainShader** out);
	HRESULT CreateGeometryShader(const void* code, SIZE_T size, void* linkage, GeometryShader** out);
	HRESULT CreatePixelShader(const void* code, SIZE_T size, void* linkage, PixelShader** out);
	HRESULT CreateComputeShader(const void* code, SIZE_T size, void* linkage, ComputeShader** out);

	HRESULT CreateBlendState(const D3D11_BLEND_DESC* desc, BlendState** out);
	HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC* desc, RasterizerState** out);
	HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* desc, DepthStencilState** out);
	HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC* desc, SamplerState** out);
	HRESULT CreateQuery(const D3D11_QUERY_DESC* desc, Query** out);

	HRESULT CheckFormatSupport(DXGI_FORMAT format, UINT* support);
	HRESULT CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels);
	D3D_FEATURE_LEVEL GetFeatureLevel() const { return D3D_FEATURE_LEVEL_11_0; }
	void GetImmediateContext(Context** out);
	HRESULT GetDeviceRemovedReason();

	// Swap chain support: the queue to create it on, its buffers wrapped for the renderer (released before
	// ResizeBuffers), the render target view of the current buffer, the current buffer itself
	ID3D12CommandQueue* Queue() const { return m_queue; }
	HRESULT AttachSwapChain(IDXGISwapChain1* swap_chain);
	void ReleaseBackBuffers();
	HRESULT CreateBackBufferRTV(RenderTargetView** out);
	HRESULT GetBackBuffer(Texture2D** out); // the alias below: copies from it read the buffer current at recording
	Texture2D* BackBufferAlias();
	Texture2D* CurrentBackBuffer(); // where commands are recorded (the context's worker when threaded)
	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRTV();

	// Native access (upscalers, ImGui, frame pacing)
	ID3D12Device* Native() const { return m_device; }
	Context* ImmediateContext() const { return m_context; }
	void WaitIdle();

	// Logs the device removal reason and the DRED breadcrumbs, once
	void ReportRemoval(const char* where);

	// Internal (dx12Device.cpp / dx12Context.cpp)
	struct Impl;
	Impl* m_impl = nullptr;
	ID3D12Device* m_device = nullptr;
	ID3D12CommandQueue* m_queue = nullptr;
	Context* m_context = nullptr;
	D3D12MA::Allocator* m_allocator = nullptr;

private:
	Device() {}
	~Device();
	volatile LONG m_refs = 1;
};

//-------------------------------------------------------------------------------------------------------------------
// Immediate context: D3D11 binding model on a D3D12 command list

class __declspec(uuid("6f1d2b3a-0c41-4f7a-9a51-1d0b6e3a7c61")) Context
{
public:
	explicit Context(Device* device);
	~Context();

	ULONG AddRef() { return 1; }
	ULONG Release() { return 1; } // owned by the device
	HRESULT QueryInterface(REFIID riid, void** out);
	template <class Q>
	HRESULT QueryInterface(Q** out) { return QueryInterface(__uuidof(Q), reinterpret_cast<void**>(out)); }

	// Shaders
	void VSSetShader(VertexShader* shader, void* const* instances, UINT count) { SetShader(StageVS, shader); }
	void HSSetShader(HullShader* shader, void* const* instances, UINT count) { SetShader(StageHS, shader); }
	void DSSetShader(DomainShader* shader, void* const* instances, UINT count) { SetShader(StageDS, shader); }
	void GSSetShader(GeometryShader* shader, void* const* instances, UINT count) { SetShader(StageGS, shader); }
	void PSSetShader(PixelShader* shader, void* const* instances, UINT count) { SetShader(StagePS, shader); }
	void CSSetShader(ComputeShader* shader, void* const* instances, UINT count) { SetShader(StageCS, shader); }
	void CSGetShader(ComputeShader** shader, void** instances, UINT* count);

	// Resources per stage
	void VSSetShaderResources(UINT start, UINT count, ShaderResourceView* const* views) { SetSRVs(StageVS, start, count, views); }
	void HSSetShaderResources(UINT start, UINT count, ShaderResourceView* const* views) { SetSRVs(StageHS, start, count, views); }
	void DSSetShaderResources(UINT start, UINT count, ShaderResourceView* const* views) { SetSRVs(StageDS, start, count, views); }
	void GSSetShaderResources(UINT start, UINT count, ShaderResourceView* const* views) { SetSRVs(StageGS, start, count, views); }
	void PSSetShaderResources(UINT start, UINT count, ShaderResourceView* const* views) { SetSRVs(StagePS, start, count, views); }
	void CSSetShaderResources(UINT start, UINT count, ShaderResourceView* const* views) { SetSRVs(StageCS, start, count, views); }
	void CSGetShaderResources(UINT start, UINT count, ShaderResourceView** views);
	void PSGetShaderResources(UINT start, UINT count, ShaderResourceView** views);

	void VSSetSamplers(UINT start, UINT count, SamplerState* const* samplers) { SetSamplers(StageVS, start, count, samplers); }
	void HSSetSamplers(UINT start, UINT count, SamplerState* const* samplers) { SetSamplers(StageHS, start, count, samplers); }
	void DSSetSamplers(UINT start, UINT count, SamplerState* const* samplers) { SetSamplers(StageDS, start, count, samplers); }
	void GSSetSamplers(UINT start, UINT count, SamplerState* const* samplers) { SetSamplers(StageGS, start, count, samplers); }
	void PSSetSamplers(UINT start, UINT count, SamplerState* const* samplers) { SetSamplers(StagePS, start, count, samplers); }
	void CSSetSamplers(UINT start, UINT count, SamplerState* const* samplers) { SetSamplers(StageCS, start, count, samplers); }

	void VSSetConstantBuffers(UINT start, UINT count, Buffer* const* buffers) { SetCBs(StageVS, start, count, buffers); }
	void HSSetConstantBuffers(UINT start, UINT count, Buffer* const* buffers) { SetCBs(StageHS, start, count, buffers); }
	void DSSetConstantBuffers(UINT start, UINT count, Buffer* const* buffers) { SetCBs(StageDS, start, count, buffers); }
	void GSSetConstantBuffers(UINT start, UINT count, Buffer* const* buffers) { SetCBs(StageGS, start, count, buffers); }
	void PSSetConstantBuffers(UINT start, UINT count, Buffer* const* buffers) { SetCBs(StagePS, start, count, buffers); }
	void CSSetConstantBuffers(UINT start, UINT count, Buffer* const* buffers) { SetCBs(StageCS, start, count, buffers); }
	void CSGetConstantBuffers(UINT start, UINT count, Buffer** buffers);

	void CSSetUnorderedAccessViews(UINT start, UINT count, UnorderedAccessView* const* views, const UINT* initial_counts);
	void CSGetUnorderedAccessViews(UINT start, UINT count, UnorderedAccessView** views);

	// Input assembler
	void IASetInputLayout(InputLayout* layout);
	void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology);
	void IASetVertexBuffers(UINT start, UINT count, Buffer* const* buffers, const UINT* strides, const UINT* offsets);
	void IASetIndexBuffer(Buffer* buffer, DXGI_FORMAT format, UINT offset);

	// Rasterizer, output merger
	void RSSetState(RasterizerState* state);
	void RSSetViewports(UINT count, const D3D11_VIEWPORT* viewports);
	void RSSetScissorRects(UINT count, const D3D11_RECT* rects);
	void RSGetViewports(UINT* count, D3D11_VIEWPORT* viewports);
	void OMSetRenderTargets(UINT count, RenderTargetView* const* rtvs, DepthStencilView* dsv);
	void OMGetRenderTargets(UINT count, RenderTargetView** rtvs, DepthStencilView** dsv);
	void OMSetBlendState(BlendState* state, const FLOAT factor[4], UINT sample_mask);
	void OMSetDepthStencilState(DepthStencilState* state, UINT stencil_ref);

	// Work
	void Draw(UINT vertex_count, UINT start_vertex);
	void DrawIndexed(UINT index_count, UINT start_index, INT base_vertex);
	void DrawInstanced(UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance);
	void DrawIndexedInstanced(UINT index_count, UINT instance_count, UINT start_index, INT base_vertex, UINT start_instance);
	void Dispatch(UINT x, UINT y, UINT z);

	void ClearRenderTargetView(RenderTargetView* rtv, const FLOAT color[4]);
	void ClearDepthStencilView(DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil);
	void ClearUnorderedAccessViewUint(UnorderedAccessView* uav, const UINT values[4]);
	void ClearUnorderedAccessViewFloat(UnorderedAccessView* uav, const FLOAT values[4]);

	void CopyResource(Resource* dst, Resource* src);
	void CopySubresourceRegion(Resource* dst, UINT dst_sub, UINT x, UINT y, UINT z, Resource* src, UINT src_sub, const D3D11_BOX* box);
	void UpdateSubresource(Resource* dst, UINT dst_sub, const D3D11_BOX* box, const void* data, UINT row_pitch, UINT depth_pitch);
	void ResolveSubresource(Resource* dst, UINT dst_sub, Resource* src, UINT src_sub, DXGI_FORMAT format);

	HRESULT Map(Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped);
	void Unmap(Resource* res, UINT sub);

	void Begin(Query* query);
	void End(Query* query);
	HRESULT GetData(Query* query, void* data, UINT size, UINT flags);

	void ClearState();
	void Flush();

	// Annotation (ID3DUserDefinedAnnotation)
	INT BeginEvent(LPCWSTR name);
	INT EndEvent();
	void SetMarker(LPCWSTR name);
	BOOL GetStatus() { return TRUE; }

	// Frame: the frame's commands are submitted and the next frame starts (with Present when a swap chain is given)
	HRESULT Present(IDXGISwapChain1* swap_chain, UINT interval, UINT flags);
	void EndFrame();
	// r__dx12_trace: every draw, dispatch, clear and copy of the next frames in the log (shaders, targets, views)
	void TraceFrames(UINT frames);
	// stat_dx12: commands per frame over some seconds in the log, the barriers by PIX event
	void StartStats(float seconds);

	// Threaded recording (r__dx12_threaded): the calls are queued as commands and a worker thread records the
	// D3D12 command list from them, as DXVK records Vulkan on its own thread; off, every call records at once.
	// Synchronize returns once everything called so far is recorded and submitted
	void SetThreaded(bool on);
	bool Threaded() const;
	void Synchronize();

	// Native recording (DLSS, ImGui): the function runs where the commands are recorded, in order with them, so it
	// gets the command list from the recorder and works only with what it owns or captured. Transition moves a
	// resource to a state with the tracking kept right; List issues the pending barriers. The context sets its own
	// bindings again afterwards
	class NativeRecorder
	{
	public:
		virtual void Transition(Resource* res, D3D12_RESOURCE_STATES state) = 0;
		virtual ID3D12GraphicsCommandList* List() = 0;
		virtual Texture2D* BackBuffer(D3D12_CPU_DESCRIPTOR_HANDLE* rtv) = 0; // the swap chain buffer being drawn
	};
	void ExecuteNative(std::function<void(NativeRecorder&)> fn);
	ID3D12DescriptorHeap* ShaderVisibleHeap() const;
	// Signals a fence on the queue right after the next submission of the frame's commands (frame pacing: the GPU
	// reached the end of the frame)
	void SignalAfterSubmit(ID3D12Fence* fence, u64 value);
	// A range of the shader-visible heap reserved for long-lived descriptors (ImGui's font)
	bool AllocStaticDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
	void FreeStaticDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu);

	struct Impl; // the recording side
	struct Front; // the calling side: bindings as set, upload memory, the command queue
	Impl* m_impl = nullptr;
	Front* m_front = nullptr;

private:
	void SetShader(Stage stage, Shader* shader);
	void SetSRVs(Stage stage, UINT start, UINT count, ShaderResourceView* const* views);
	void SetSamplers(Stage stage, UINT start, UINT count, SamplerState* const* samplers);
	void SetCBs(Stage stage, UINT start, UINT count, Buffer* const* buffers);
};

} // namespace dx12
