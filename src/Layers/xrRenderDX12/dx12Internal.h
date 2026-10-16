#pragma once
// Internals shared by the D3D12 backend's sources (see dx12.h)

#include "dx12.h"
#include <unordered_map>
#include <D3D12MemoryAllocator/D3D12MemAlloc.h>

namespace dx12
{
// Frames the CPU may record ahead of the GPU (each with its own command allocator and upload memory)
const UINT kFrames = 3;

// Logs once per call site
#define DX12_WARN_ONCE(...)                                                                                                \
	do                                                                                                                     \
	{                                                                                                                      \
		static bool _warned = false;                                                                                       \
		if (!_warned)                                                                                                      \
		{                                                                                                                  \
			_warned = true;                                                                                                \
			Msg(__VA_ARGS__);                                                                                              \
		}                                                                                                                  \
	} while (0)

inline u64 HashBytes(const void* data, size_t size, u64 seed = 14695981039346656037ull)
{
	const u8* p = static_cast<const u8*>(data);
	u64 h = seed;
	for (size_t i = 0; i < size; ++i)
	{
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

//-------------------------------------------------------------------------------------------------------------------
// CPU (non shader-visible) descriptor pool: heaps of 1024 descriptors, slots recycled through a free list. Used from
// any thread (textures are created on loader threads)
class DescriptorPool
{
public:
	void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type);
	void Destroy();
	UINT Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* handle);
	void Free(UINT slot);
	D3D12_CPU_DESCRIPTOR_HANDLE Handle(UINT slot) const;

private:
	static const UINT kHeapSize = 1024;
	ID3D12Device* m_device = nullptr;
	D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	UINT m_increment = 0;
	std::vector<ID3D12DescriptorHeap*> m_heaps;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_starts;
	std::vector<UINT> m_free;
	UINT m_next = 0;
	std::mutex m_lock;
};

//-------------------------------------------------------------------------------------------------------------------
// Upload memory: 4 MB pages of an UPLOAD heap, persistently mapped. A frame takes slices linearly; its pages come back
// when the GPU has passed the frame. Main thread only (the context)
struct UploadPage
{
	ID3D12Resource* buffer = nullptr;
	D3D12MA::Allocation* allocation = nullptr;
	u8* cpu = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS va = 0;
	u64 size = 0;
	u64 fence = 0;
};

struct UploadSlice
{
	u8* cpu;
	D3D12_GPU_VIRTUAL_ADDRESS va;
	ID3D12Resource* buffer;
	u64 offset;
};

class UploadRing
{
public:
	void Init(D3D12MA::Allocator* allocator) { m_allocator = allocator; }
	void Destroy();
	bool Alloc(u64 size, u64 alignment, UploadSlice& slice);
	void Retire(u64 fence); // the pages used so far belong to the submission signalling fence
	void Reclaim(u64 completed);
	u64 BytesThisFrame() const { return m_frame_bytes; }

private:
	UploadPage* NewPage(u64 size);
	static const u64 kPageSize = 4ull << 20;
	D3D12MA::Allocator* m_allocator = nullptr;
	UploadPage* m_current = nullptr;
	u64 m_offset = 0;
	std::vector<UploadPage*> m_used; // this frame
	std::vector<UploadPage*> m_in_flight;
	std::vector<UploadPage*> m_free;
	u64 m_frame_bytes = 0;
};

//-------------------------------------------------------------------------------------------------------------------
// Root signatures: built from the bindings of the shaders of a draw (or a dispatch)
struct RootLayoutKey
{
	u64 stage[StageCount]; // per stage: present, cb mask, srv/sampler/uav counts
	bool operator==(const RootLayoutKey& o) const { return memcmp(stage, o.stage, sizeof(stage)) == 0; }
};

struct RootSignature
{
	ID3D12RootSignature* signature = nullptr;
	u64 id = 0;
	bool compute = false;
	struct StageParams
	{
		bool present = false;
		u16 cb_mask = 0;
		u8 cb_param[kMaxCB]; // root parameter of each constant buffer slot (root CBV), 0xFF: none
		u8 cbv_table_param = 0xFF; // when the CBVs did not fit as root CBVs: one table of cbv_table_count
		u8 cbv_table_count = 0;
		u8 srv_param = 0xFF, srv_count = 0;
		u8 sampler_param = 0xFF, sampler_count = 0;
		u8 uav_param = 0xFF, uav_count = 0;
	} stages[StageCount];
};

//-------------------------------------------------------------------------------------------------------------------
// Pipeline states
struct GraphicsPipelineKey
{
	u64 root;
	u64 shader[5]; // VS HS DS GS PS ids
	u64 layout;
	u64 blend, raster, depth;
	UINT sample_mask;
	DXGI_FORMAT rtv[kMaxRT];
	DXGI_FORMAT dsv;
	u8 rt_count;
	u8 topology_type;
	u8 samples;
	u8 dsv_read_only;
	UINT sample_quality;
	bool operator==(const GraphicsPipelineKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

struct KeyHash
{
	template <class K>
	size_t operator()(const K& k) const { return size_t(HashBytes(&k, sizeof(k))); }
};

//-------------------------------------------------------------------------------------------------------------------
// Device internals
struct Device::Impl
{
	IDXGIAdapter1* adapter = nullptr;
	bool debug_layer = false;
	bool dred = false;
	bool removal_reported = false;

	// Queue submissions (main thread, and loader threads flushing initial uploads) are serialized: fence values
	// are signalled in order
	std::mutex queue_lock;
	ID3D12Fence* fence = nullptr;
	HANDLE fence_event = nullptr;
	u64 next_fence = 1; // the value the next submission signals (under queue_lock)
	std::atomic<u64> pending_fence{ 1 }; // copy of next_fence readable without the lock
	u64 Completed();
	void WaitFence(u64 value);
	u64 Submit(ID3D12CommandList* const* lists, UINT count); // returns the signalled value

	DescriptorPool rtv_pool, dsv_pool, srv_pool, sampler_pool;

	// Objects to release once the GPU passed a fence value
	struct Deferred
	{
		u64 fence;
		IUnknown* object;
		D3D12MA::Allocation* allocation;
	};
	std::mutex deferred_lock;
	std::vector<Deferred> deferred;
	void DeferRelease(IUnknown* object, D3D12MA::Allocation* allocation);
	void ProcessDeferred(u64 completed, bool all);

	// Initial data uploads (any thread): copies recorded in a list submitted ahead of the next frame submission
	std::mutex init_lock;
	ID3D12CommandAllocator* init_allocator[kFrames + 1] = {};
	u64 init_allocator_fence[kFrames + 1] = {};
	UINT init_index = 0;
	ID3D12GraphicsCommandList* init_list = nullptr;
	bool init_open = false;
	u64 init_bytes = 0;
	std::vector<D3D12_RESOURCE_BARRIER> init_barriers;
	ID3D12GraphicsCommandList* BeginInit(); // under init_lock
	void SubmitInitLocked(); // under init_lock; queue_lock taken inside
	u64 SubmitFrame(ID3D12CommandList* list); // the pending initial uploads, then the list, in one submission
	bool UploadInitial(Resource* res, const D3D12_RESOURCE_DESC& desc, const D3D11_SUBRESOURCE_DATA* init,
	                   D3D12_RESOURCE_STATES rest_state);

	// State object deduplication (the device keeps one reference of each)
	std::mutex state_lock;
	std::unordered_map<u64, std::vector<Object*>> states;
	template <class T, class D>
	T* FindState(const D& desc, u64 kind);

	// Swap chain
	IDXGISwapChain3* swap_chain = nullptr;
	std::vector<Texture2D*> back_buffers;
	std::vector<UINT> back_buffer_rtv_slots;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> back_buffer_rtv;
	DXGI_FORMAT back_buffer_format = DXGI_FORMAT_UNKNOWN;

	// Null descriptors for unbound slots
	D3D12_CPU_DESCRIPTOR_HANDLE null_srv[11] = {}; // per D3D12_SRV_DIMENSION (0: 2D)
	D3D12_CPU_DESCRIPTOR_HANDLE null_uav = {};
	D3D12_CPU_DESCRIPTOR_HANDLE default_sampler = {};

	// Queries
	std::mutex query_lock;
	ID3D12QueryHeap* occlusion_heap = nullptr;
	ID3D12QueryHeap* timestamp_heap = nullptr;
	ID3D12Resource* occlusion_readback = nullptr;
	ID3D12Resource* timestamp_readback = nullptr;
	D3D12MA::Allocation* occlusion_readback_allocation = nullptr;
	D3D12MA::Allocation* timestamp_readback_allocation = nullptr;
	const u64* occlusion_data = nullptr;
	const u64* timestamp_data = nullptr;
	std::vector<UINT> occlusion_free, timestamp_free;
	static const UINT kOcclusionQueries = 8192;
	static const UINT kTimestampQueries = 4096;
	u64 timestamp_frequency = 1;

	// Root signatures and pipeline states (shared with the warm-up thread)
	std::mutex pipeline_lock;
	std::unordered_map<RootLayoutKey, RootSignature*, KeyHash> root_signatures;
	std::unordered_map<GraphicsPipelineKey, ID3D12PipelineState*, KeyHash> graphics_pipelines;
	std::unordered_map<u64, ID3D12PipelineState*> compute_pipelines; // root id ^ shader id
	u64 pipelines_created = 0;
	PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize_root_signature = nullptr;

	RootSignature* GetRootSignature(const RootLayoutKey& key);
};

// Format and description helpers (dx12Format.cpp)
UINT FormatPlaneCount(ID3D12Device* device, DXGI_FORMAT format);
bool IsDepthFormat(DXGI_FORMAT format);
UINT PlaneOfViewFormat(DXGI_FORMAT view_format); // stencil views read plane 1
UINT MipCount(UINT width, UINT height, UINT depth);
D3D12_RESOURCE_FLAGS ResourceFlags(UINT bind_flags);
D3D12_RESOURCE_STATES ShaderReadState();
u64 ShaderBindingKey(const Shader* shader);

// Pipeline helpers (dx12Pipeline.cpp)
bool ReflectShader(const void* code, SIZE_T size, Stage stage, ShaderBindings& bindings);
ID3D12PipelineState* CreateGraphicsPipeline(Device* device, const GraphicsPipelineKey& key, const RootSignature* root,
                                            Shader* const shaders[5], const InputLayout* layout,
                                            const D3D11_BLEND_DESC& blend, const D3D11_RASTERIZER_DESC& raster,
                                            const D3D11_DEPTH_STENCIL_DESC& depth);
ID3D12PipelineState* CreateComputePipeline(Device* device, const RootSignature* root, const Shader* shader);

Device* TheDevice();
} // namespace dx12

namespace std
{
template <>
struct hash<dx12::RootLayoutKey>
{
	size_t operator()(const dx12::RootLayoutKey& k) const { return size_t(dx12::HashBytes(&k, sizeof(k))); }
};
} // namespace std
