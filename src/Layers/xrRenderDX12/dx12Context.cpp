// D3D12 backend: the immediate context (see dx12.h)
//
// Bindings are recorded per stage as D3D11 has them. A draw or dispatch then:
//  1. moves every resource it touches to the state it needs (barriers batched), after removing the shader views of
//     resources that are bound as outputs at the same time (D3D11 unbinds those)
//  2. picks the root signature from the bindings of the bound shaders, and the pipeline state from shaders, input
//     layout, state objects, target formats and topology
//  3. copies the views of each stage into the shader-visible heap (a table per stage, only when they changed), sets
//     constant buffers as root CBVs (their address changes whenever a dynamic buffer is mapped)
//  4. sets input assembler, targets, viewports, scissors, stencil reference and blend factor when they changed
//
// The calls are commands (Context::Front, at the end): with r__dx12_threaded a worker thread records them, as DXVK
// records Vulkan on its own thread, and the calling thread only queues them. Context::Impl is the recording side:
// it runs on the worker, or at once when not threaded. The bindings it keeps are plain pointers: the calling side
// holds the references, and an object whose last reference goes is deleted once the worker is past the commands
// queued until then.
#include "stdafx.h"
#pragma hdrstop

#include "dx12Internal.h"
#include <condition_variable>
#include <deque>
#include <thread>

namespace dx12
{
struct StageBinding
{
	Shader* shader = nullptr;
	Buffer* cb[kMaxCB] = {};
	ShaderResourceView* srv[kMaxSRV] = {};
	SamplerState* sampler[kMaxSamplers] = {};
	UnorderedAccessView* uav[kMaxUAV] = {};
	u32 hazard = 0; // SRV slots dropped at the last draw because their resource is an output
	bool srv_dirty = true, sampler_dirty = true, uav_dirty = true;
	bool srv_valid = false, sampler_valid = false, uav_valid = false, cbv_table_valid = false;
	D3D12_GPU_VIRTUAL_ADDRESS cb_applied[kMaxCB] = {};
	u64 srv_table_applied = 0; // GPU handle of the SRV table set (valid while srv_valid)
};

// stat_dx12: commands per frame, the barriers per PIX event
struct ContextStats
{
	u64 frames, draws, dispatches, barrier_calls, barriers, pso_sets, root_sets, srv_tables, srv_table_hits,
		srv_table_sets, sampler_tables, cbv_sets, rt_sets, vb_sets, ib_sets, vp_sets, sc_sets, clears, copies, submits;
};

struct Context::Impl : public Context::NativeRecorder
{
	Context* self = nullptr;
	Device* device = nullptr;
	Device::Impl* dev = nullptr;
	ID3D12Device* d3d = nullptr;

	ID3D12GraphicsCommandList* list = nullptr;
	ID3D12CommandAllocator* allocator[kFrames] = {};
	u64 frame_fence[kFrames] = {};
	UINT frame_index = 0;
	u64 frame_number = 1;
	bool recorded = false;
	std::vector<std::wstring> events; // open PIX events, opened again after a submission in the middle of a frame

	// Frames recorded: the fence of each frame's last submission, read by the calling side to recycle upload memory
	static const UINT kFrameRing = 64;
	u64 frame_fences[kFrameRing] = {};
	std::atomic<u64> recorded_frame{ 0 };
	std::atomic<HRESULT> present_result{ S_OK };

	// Shader-visible heaps: views (a static range, then one segment per frame in flight) and samplers (tables
	// cached by content)
	static const UINT kViewHeapSize = 500000;
	static const UINT kStaticViews = 1024;
	ID3D12DescriptorHeap* view_heap = nullptr;
	UINT view_inc = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE view_cpu0 = {};
	D3D12_GPU_DESCRIPTOR_HANDLE view_gpu0 = {};
	UINT segment_size = 0;
	UINT segment_next[kFrames] = {};
	bool ring_wrapped = false;
	std::vector<UINT> static_free;
	UINT static_next = 0;

	static const UINT kSamplerHeapSize = 2048;
	ID3D12DescriptorHeap* sampler_heap = nullptr;
	UINT sampler_inc = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu0 = {};
	D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu0 = {};
	UINT sampler_next = 0;
	std::unordered_map<u64, UINT> sampler_tables;

	// Bindings
	StageBinding stages[StageCount];
	InputLayout* layout = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	Buffer* vb[kMaxVB] = {};
	UINT vb_stride[kMaxVB] = {}, vb_offset[kMaxVB] = {};
	UINT vb_count = 0;
	Buffer* ib = nullptr;
	DXGI_FORMAT ib_format = DXGI_FORMAT_R16_UINT;
	UINT ib_offset = 0;
	RasterizerState* raster = nullptr;
	BlendState* blend = nullptr;
	DepthStencilState* depth = nullptr;
	FLOAT blend_factor[4] = {};
	UINT sample_mask = 0xFFFFFFFF;
	UINT stencil_ref = 0;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT viewport_count = 0;
	D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT scissor_count = 0;
	RenderTargetView* rtv[kMaxRT] = {};
	DepthStencilView* dsv = nullptr;

	BlendState* default_blend = nullptr;
	RasterizerState* default_raster = nullptr;
	DepthStencilState* default_depth = nullptr;
	Buffer* zero_buffer = nullptr; // bound for constant buffers and vertex buffers that have no data
	D3D12_CPU_DESCRIPTOR_HANDLE null_rtv = {};
	UINT null_rtv_slot = UINT(-1);

	// Applied to the command list
	bool shaders_changed = true, cs_changed = true;
	bool pso_dirty = true;
	bool rt_dirty = true, vp_dirty = true, sc_dirty = true, topo_dirty = true, ref_dirty = true, factor_dirty = true;
	bool heaps_set = false;
	RootSignature* gfx_root = nullptr;
	RootSignature* cs_root = nullptr;
	RootSignature* bound_gfx_root = nullptr;
	RootSignature* bound_cs_root = nullptr;
	ID3D12PipelineState* gfx_pso = nullptr;
	ID3D12PipelineState* cs_pso = nullptr;
	ID3D12PipelineState* applied_pso = nullptr;
	D3D12_VERTEX_BUFFER_VIEW vb_applied[kMaxVB] = {};
	UINT vb_applied_count = 0;
	bool vb_applied_valid = false;
	D3D12_INDEX_BUFFER_VIEW ib_applied = {};
	bool ib_applied_valid = false;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_applied[kMaxRT] = {};

	std::vector<D3D12_RESOURCE_BARRIER> barriers;

	Impl() { barriers.reserve(64); }

	//---------------------------------------------------------------------------------------------------------------
	// r__dx12_trace
	// Work goes to the GPU during the frame, not all at Present: at a change of render targets after kSubmitPass draws,
	// within a long pass every kSubmitDraws. With one submission per frame the GPU waited for the CPU to record the
	// whole frame, CPU and GPU time added up (40 fps under vsync where D3D11 made 60, GPU clocks down while it idled)
	static const UINT kSubmitPass = 48, kSubmitDraws = 256;

	// SRV tables of this frame by content (view ids, null slots by dimension): a draw with the textures of an earlier
	// one sets that table again, without allocating and copying descriptors
	std::unordered_map<u64, u64> srv_tables;

	ContextStats stats = {};
	bool stats_on = false;
	u64 stats_end_ms = 0;
	std::unordered_map<std::wstring, u64> stats_event_barriers;
	UINT draws_since_submit = 0;
	u32 submits_this_frame = 0, submits_last_frame = 0;

	void SubmitEarly()
	{
		if (!recorded || !draws_since_submit)
			return;
		Submit(true);
	}

	UINT trace_frames = 0, trace_requested = 0;
	UINT trace_index = 0;

	static std::string ShortName(const xr_string& path)
	{
		// "...\shaders_cache\r4\combine_2_naa.ps\<variant>" -> "combine_2_naa.ps"
		const char* p = strstr(path.c_str(), "\\r4\\");
		if (!p)
			return path.c_str();
		const char* end = strchr(p + 4, '\\');
		return end ? std::string(p + 4, end) : std::string(p + 4);
	}

	static std::string ResName(const Resource* r)
	{
		if (!r)
			return "null";
		if (!r->m_debug_name.empty())
			return r->m_debug_name.c_str();
		return make_string("#%llu:%ux%u:f%d", r->Id(), r->m_width, r->m_height, int(r->m_format));
	}

	void TraceStage(std::string& out, UINT s, const char* label)
	{
		const StageBinding& b = stages[s];
		if (!b.shader)
			return;
		out += make_string(" | %s %s", label, ShortName(b.shader->m_name).c_str());
		for (UINT i = 0; i < b.shader->m_bindings.srv_count; ++i)
		{
			if (!b.shader->m_bindings.srv_dim[i])
				continue;
			const ShaderResourceView* v = b.srv[i];
			out += make_string(" t%u=%s%s", i, (b.hazard & (1u << i)) ? "DROPPED:" : "", v ? ResName(v->m_res).c_str() : "null");
		}
		if (s == StageCS)
			for (UINT i = 0; i < b.shader->m_bindings.uav_count; ++i)
				if (b.shader->m_bindings.uav_dim[i])
					out += make_string(" u%u=%s", i, b.uav[i] ? ResName(b.uav[i]->m_res).c_str() : "null");
	}

	void TraceLine(std::string& out)
	{
		if (!events.empty())
		{
			const std::wstring& e = events.back();
			out += " @";
			for (wchar_t ch : e)
				out += char(ch < 128 ? ch : '?');
		}
		if (out.size() > 3900)
			out.resize(3900);
		Msg("%s", out.c_str());
	}

	void TraceDraw(const char* what, UINT count)
	{
		std::string out = make_string("[dx12 trace] %u %s %u", trace_index++, what, count);
		for (UINT i = 0; i < kMaxRT; ++i)
			if (rtv[i])
			{
				D3D12_CPU_DESCRIPTOR_HANDLE h;
				out += make_string(" rt%u=%s", i, rtv[i]->m_back_buffer ? "backbuffer" : ResName(ResolveRTV(rtv[i], h)).c_str());
			}
		if (dsv)
			out += make_string(" ds=%s%s", ResName(dsv->m_res).c_str(), dsv->m_read_only ? "(ro)" : "");
		if (vb_count && vb[0])
			out += make_string(" vb=%u/%u", vb_stride[0], vb_offset[0]);
		if (layout)
			out += make_string(" il=%u", UINT(layout->m_elements.size()));
		TraceStage(out, StageVS, "VS");
		TraceStage(out, StageGS, "GS");
		TraceStage(out, StagePS, "PS");
		TraceLine(out);
	}

	bool BeginDraw(const char* what, UINT count)
	{
		if (draws_since_submit >= kSubmitDraws)
			SubmitEarly();
		const bool ok = PrepareDraw();
		if (ok)
		{
			++draws_since_submit;
			if (stats_on)
				++stats.draws;
		}
		if (trace_frames)
			TraceDraw(ok ? what : "SKIPPED", count);
		return ok;
	}

	//---------------------------------------------------------------------------------------------------------------
	void Record() { recorded = true; }

	void InvalidateApplied()
	{
		bound_gfx_root = nullptr;
		bound_cs_root = nullptr;
		applied_pso = nullptr;
		heaps_set = false;
		for (StageBinding& b : stages)
		{
			b.srv_valid = b.sampler_valid = b.uav_valid = b.cbv_table_valid = false;
			memset(b.cb_applied, 0, sizeof(b.cb_applied));
		}
		vb_applied_valid = false;
		ib_applied_valid = false;
		rt_dirty = vp_dirty = sc_dirty = topo_dirty = ref_dirty = factor_dirty = true;
	}

	//---------------------------------------------------------------------------------------------------------------
	// Resource states

	static bool IsReadState(D3D12_RESOURCE_STATES s)
	{
		const D3D12_RESOURCE_STATES writes = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
			D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
			D3D12_RESOURCE_STATE_STREAM_OUT;
		return s != D3D12_RESOURCE_STATE_COMMON && !(s & writes);
	}

	static bool Satisfies(D3D12_RESOURCE_STATES current, D3D12_RESOURCE_STATES wanted)
	{
		if (current == wanted)
			return true;
		return IsReadState(current) && IsReadState(wanted) && (current & wanted) == wanted;
	}

	void PushTransition(Resource* r, UINT sub, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r->m_resource;
		b.Transition.Subresource = sub;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
		barriers.push_back(b);
		if (before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			r->m_written_uav = false;
	}

	void Require(Resource* r, UINT first_mip, UINT mips, UINT first_slice, UINT slices, UINT first_plane, UINT planes,
	             bool whole, D3D12_RESOURCE_STATES wanted)
	{
		if (!r || !r->m_resource)
			return;
		if (whole || r->m_subresources == 1)
		{
			if (r->m_states.empty())
			{
				if (!Satisfies(r->m_state, wanted))
				{
					PushTransition(r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, r->m_state, wanted);
					r->m_state = wanted;
				}
				return;
			}
			for (UINT s = 0; s < r->m_subresources; ++s)
				if (r->m_states[s] != wanted)
					PushTransition(r, s, r->m_states[s], wanted);
			r->m_states.clear();
			r->m_state = wanted;
			return;
		}
		if (r->m_states.empty())
		{
			if (Satisfies(r->m_state, wanted))
				return;
			r->m_states.assign(r->m_subresources, r->m_state);
		}
		for (UINT p = first_plane; p < first_plane + planes && p < r->m_planes; ++p)
			for (UINT a = first_slice; a < first_slice + slices; ++a)
				for (UINT m = first_mip; m < first_mip + mips; ++m)
				{
					const UINT s = r->Subresource(m, a, p);
					if (s >= r->m_subresources)
						continue;
					if (!Satisfies(r->m_states[s], wanted))
					{
						PushTransition(r, s, r->m_states[s], wanted);
						r->m_states[s] = wanted;
					}
				}
		const D3D12_RESOURCE_STATES first = r->m_states[0];
		for (UINT s = 1; s < r->m_subresources; ++s)
			if (r->m_states[s] != first)
				return;
		r->m_state = first;
		r->m_states.clear();
	}

	void RequireView(const View* v, Resource* r, D3D12_RESOURCE_STATES wanted)
	{
		Require(r, v->m_first_mip, v->m_mip_count, v->m_first_slice, v->m_slice_count, v->m_first_plane,
		        v->m_plane_count, v->m_whole, wanted);
	}

	void RequireWhole(Resource* r, D3D12_RESOURCE_STATES wanted) { Require(r, 0, 0, 0, 0, 0, 0, true, wanted); }

	void FlushBarriers()
	{
		if (barriers.empty())
			return;
		list->ResourceBarrier(UINT(barriers.size()), barriers.data());
		if (stats_on)
		{
			++stats.barrier_calls;
			stats.barriers += barriers.size();
			stats_event_barriers[events.empty() ? std::wstring(L"(none)") : events.back()] += barriers.size();
		}
		barriers.clear();
		Record();
	}

	// Subresource ranges of two views of one resource overlap
	static bool Overlap(const View* a, const View* b)
	{
		return a->m_first_mip < b->m_first_mip + b->m_mip_count && b->m_first_mip < a->m_first_mip + a->m_mip_count &&
			a->m_first_slice < b->m_first_slice + b->m_slice_count && b->m_first_slice < a->m_first_slice + a->m_slice_count &&
			a->m_first_plane < b->m_first_plane + b->m_plane_count && b->m_first_plane < a->m_first_plane + a->m_plane_count;
	}

	Resource* ResolveRTV(RenderTargetView* v, D3D12_CPU_DESCRIPTOR_HANDLE& h)
	{
		if (v->m_back_buffer)
		{
			h = device->CurrentBackBufferRTV();
			return device->CurrentBackBuffer();
		}
		h = v->m_cpu;
		return v->m_res;
	}

	//---------------------------------------------------------------------------------------------------------------
	// Descriptors

	bool AllocViews(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
	{
		UINT& next = segment_next[frame_index];
		if (next + count > segment_size)
		{
			// Never seen in practice: the GPU finishes everything, then the segment starts over
			DX12_WARN_ONCE("! D3D12: the frame's descriptor range is full (%u), waiting for the GPU", segment_size);
			FlushBackend();
			dev->WaitQueueIdle();
			for (UINT& n : segment_next)
				n = 0;
			srv_tables.clear();
			ring_wrapped = true;
		}
		const UINT index = kStaticViews + frame_index * segment_size + next;
		next += count;
		cpu.ptr = view_cpu0.ptr + SIZE_T(index) * view_inc;
		gpu.ptr = view_gpu0.ptr + u64(index) * view_inc;
		return true;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE SamplerTable(const StageBinding& b, UINT count)
	{
		u64 ids[kMaxSamplers];
		for (UINT i = 0; i < count; ++i)
			ids[i] = b.sampler[i] ? b.sampler[i]->Id() : 0;
		const u64 key = HashBytes(ids, sizeof(u64) * count, count);
		auto it = sampler_tables.find(key);
		UINT index;
		if (it != sampler_tables.end())
			index = it->second;
		else
		{
			if (sampler_next + count > kSamplerHeapSize)
			{
				DX12_WARN_ONCE("! D3D12: sampler heap full, starting it over");
				FlushBackend();
				dev->WaitQueueIdle();
				sampler_tables.clear();
				sampler_next = 0;
				for (StageBinding& s : stages)
					s.sampler_valid = false;
				ring_wrapped = true; // the list was submitted: the draw is prepared again
			}
			index = sampler_next;
			sampler_next += count;
			D3D12_CPU_DESCRIPTOR_HANDLE src[kMaxSamplers];
			static const UINT ones[kMaxSamplers] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
			for (UINT i = 0; i < count; ++i)
				src[i] = b.sampler[i] ? b.sampler[i]->m_cpu : dev->default_sampler;
			const D3D12_CPU_DESCRIPTOR_HANDLE dst = { sampler_cpu0.ptr + SIZE_T(index) * sampler_inc };
			d3d->CopyDescriptors(1, &dst, &count, count, src, ones, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
			sampler_tables[key] = index;
			if (stats_on)
				++stats.sampler_tables;
		}
		return D3D12_GPU_DESCRIPTOR_HANDLE{ sampler_gpu0.ptr + u64(index) * sampler_inc };
	}

	// A dynamic buffer's address is its current upload slice: the calling side renames it (Map, Unmap, and again
	// for a shadowed buffer used unchanged in a later frame) with a command ahead of the draws that use it
	D3D12_GPU_VIRTUAL_ADDRESS BufferAddress(Buffer* b)
	{
		if (!b || !b->m_va)
			return zero_buffer->m_va;
		return b->m_va;
	}

	// Tables and constant buffers of one stage
	void ApplyStage(UINT s, const RootSignature* root, bool compute)
	{
		StageBinding& b = stages[s];
		const RootSignature::StageParams& sp = root->stages[s];
		if (!sp.present)
			return;

		if (sp.srv_count && (b.srv_dirty || !b.srv_valid))
		{
			const ShaderBindings* bindings = b.shader ? &b.shader->m_bindings : nullptr;
			u64 ids[kMaxSRV];
			D3D12_CPU_DESCRIPTOR_HANDLE src[kMaxSRV];
			for (UINT i = 0; i < sp.srv_count; ++i)
			{
				const ShaderResourceView* v = b.srv[i];
				if (v && !(b.hazard & (1u << i)))
				{
					ids[i] = v->Id();
					src[i] = v->m_cpu;
				}
				else
				{
					UINT dim = bindings ? bindings->srv_dim[i] : 0;
					dim = dim <= 10 ? dim : 0;
					ids[i] = (1ull << 63) | dim; // a null view of that dimension (object ids count up from 1)
					src[i] = dev->null_srv[dim];
				}
			}
			const u64 key = HashBytes(ids, sizeof(u64) * sp.srv_count, sp.srv_count);
			u64 table;
			auto it = srv_tables.find(key);
			if (it != srv_tables.end())
			{
				table = it->second;
				if (stats_on)
					++stats.srv_table_hits;
			}
			else
			{
				D3D12_CPU_DESCRIPTOR_HANDLE cpu;
				D3D12_GPU_DESCRIPTOR_HANDLE gpu;
				AllocViews(sp.srv_count, cpu, gpu);
				// One call for the table: the destination range, one source range per view
				static const UINT ones[kMaxSRV] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
					1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
				const UINT count = sp.srv_count;
				d3d->CopyDescriptors(1, &cpu, &count, count, src, ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				table = gpu.ptr;
				srv_tables.emplace(key, table);
				if (stats_on)
					++stats.srv_tables;
			}
			if (!b.srv_valid || b.srv_table_applied != table)
			{
				const D3D12_GPU_DESCRIPTOR_HANDLE gpu = { table };
				if (compute)
					list->SetComputeRootDescriptorTable(sp.srv_param, gpu);
				else
					list->SetGraphicsRootDescriptorTable(sp.srv_param, gpu);
				b.srv_table_applied = table;
				if (stats_on)
					++stats.srv_table_sets;
			}
			b.srv_dirty = false;
			b.srv_valid = true;
		}

		if (sp.sampler_count && (b.sampler_dirty || !b.sampler_valid))
		{
			const D3D12_GPU_DESCRIPTOR_HANDLE gpu = SamplerTable(b, sp.sampler_count);
			if (compute)
				list->SetComputeRootDescriptorTable(sp.sampler_param, gpu);
			else
				list->SetGraphicsRootDescriptorTable(sp.sampler_param, gpu);
			b.sampler_dirty = false;
			b.sampler_valid = true;
		}

		if (compute && sp.uav_count && (b.uav_dirty || !b.uav_valid))
		{
			D3D12_CPU_DESCRIPTOR_HANDLE cpu;
			D3D12_GPU_DESCRIPTOR_HANDLE gpu;
			AllocViews(sp.uav_count, cpu, gpu);
			for (UINT i = 0; i < sp.uav_count; ++i)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE dst = { cpu.ptr + SIZE_T(i) * view_inc };
				const D3D12_CPU_DESCRIPTOR_HANDLE src = b.uav[i] ? b.uav[i]->m_cpu : dev->null_uav;
				d3d->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			list->SetComputeRootDescriptorTable(sp.uav_param, gpu);
			b.uav_dirty = false;
			b.uav_valid = true;
		}

		if (sp.cbv_table_param == 0xFF)
		{
			for (UINT slot = 0; slot < kMaxCB; ++slot)
			{
				if (!(sp.cb_mask & (1u << slot)))
					continue;
				const D3D12_GPU_VIRTUAL_ADDRESS va = BufferAddress(b.cb[slot]);
				if (va == b.cb_applied[slot])
					continue;
				if (compute)
					list->SetComputeRootConstantBufferView(sp.cb_param[slot], va);
				else
					list->SetGraphicsRootConstantBufferView(sp.cb_param[slot], va);
				b.cb_applied[slot] = va;
				if (stats_on)
					++stats.cbv_sets;
			}
		}
		else
		{
			bool changed = !b.cbv_table_valid;
			for (UINT slot = 0; slot < sp.cbv_table_count; ++slot)
				if (BufferAddress(b.cb[slot]) != b.cb_applied[slot])
					changed = true;
			if (changed)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE cpu;
				D3D12_GPU_DESCRIPTOR_HANDLE gpu;
				AllocViews(sp.cbv_table_count, cpu, gpu);
				for (UINT slot = 0; slot < sp.cbv_table_count; ++slot)
				{
					Buffer* buffer = b.cb[slot];
					D3D12_CONSTANT_BUFFER_VIEW_DESC cd = {};
					cd.BufferLocation = BufferAddress(buffer);
					cd.SizeInBytes = buffer && buffer->m_va ? ((buffer->m_desc.ByteWidth + 255) & ~255u) : 256;
					D3D12_CPU_DESCRIPTOR_HANDLE dst = { cpu.ptr + SIZE_T(slot) * view_inc };
					d3d->CreateConstantBufferView(&cd, dst);
					b.cb_applied[slot] = cd.BufferLocation;
				}
				if (compute)
					list->SetComputeRootDescriptorTable(sp.cbv_table_param, gpu);
				else
					list->SetGraphicsRootDescriptorTable(sp.cbv_table_param, gpu);
				b.cbv_table_valid = true;
			}
		}
	}

	void SetHeaps()
	{
		if (heaps_set)
			return;
		ID3D12DescriptorHeap* heaps[2] = { view_heap, sampler_heap };
		list->SetDescriptorHeaps(2, heaps);
		heaps_set = true;
	}

	//---------------------------------------------------------------------------------------------------------------
	// Draw preparation

	static u8 TopologyType(D3D11_PRIMITIVE_TOPOLOGY t)
	{
		switch (t)
		{
		case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
		case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
		case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
		case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		default:
			if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
				return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		}
	}

	ID3D12PipelineState* ResolveGraphicsPipeline()
	{
		GraphicsPipelineKey k;
		memset(&k, 0, sizeof(k));
		k.root = gfx_root->id;
		Shader* shaders[5] = { stages[StageVS].shader, stages[StageHS].shader, stages[StageDS].shader,
			stages[StageGS].shader, stages[StagePS].shader };
		for (UINT i = 0; i < 5; ++i)
			k.shader[i] = shaders[i] ? shaders[i]->m_content : 0;
		k.layout = layout ? layout->m_content : 0;
		BlendState* bs = blend ? blend : default_blend;
		RasterizerState* rs = raster ? raster : default_raster;
		DepthStencilState* ds = depth ? depth : default_depth;
		k.blend = bs->m_content;
		k.raster = rs->m_content;
		k.depth = ds->m_content;
		k.sample_mask = sample_mask;
		UINT count = 0;
		for (UINT i = 0; i < kMaxRT; ++i)
			if (rtv[i])
				count = i + 1;
		k.rt_count = u8(count);
		Resource* sized = nullptr;
		for (UINT i = 0; i < count; ++i)
		{
			if (rtv[i])
			{
				D3D12_CPU_DESCRIPTOR_HANDLE h;
				Resource* r = ResolveRTV(rtv[i], h);
				k.rtv[i] = rtv[i]->m_format;
				if (!sized)
					sized = r;
			}
			else
				k.rtv[i] = DXGI_FORMAT_R8G8B8A8_UNORM; // the null view's format
		}
		if (dsv)
		{
			k.dsv = dsv->m_format;
			k.dsv_read_only = dsv->m_read_only ? 1 : 0;
			if (!sized)
				sized = dsv->m_res;
		}
		k.samples = u8(sized ? sized->m_samples : 1);
		k.sample_quality = sized ? sized->m_sample_quality : 0;
		k.topology_type = TopologyType(topology);

		{
			std::lock_guard<std::mutex> lock(dev->pipeline_lock);
			auto it = dev->graphics_pipelines.find(k);
			if (it != dev->graphics_pipelines.end())
				return it->second;
		}
		ID3D12PipelineState* pso = CreateGraphicsPipeline(device, k, gfx_root, shaders, layout, bs->m_desc, rs->m_desc, ds->m_desc);
		{
			std::lock_guard<std::mutex> lock(dev->pipeline_lock);
			auto ins = dev->graphics_pipelines.emplace(k, pso);
			if (!ins.second)
			{
				if (pso)
					pso->Release();
				return ins.first->second;
			}
			++dev->pipelines_created;
		}
		if (pso)
			dev->RecordPipeline(k, shaders, layout, bs->m_desc, rs->m_desc, ds->m_desc);
		return pso;
	}

	// SRVs of one stage that read a resource written by the same draw are dropped, the others get readable
	bool PrepareStageViews(UINT s, View* const* outputs, Resource* const* output_res, UINT output_count)
	{
		StageBinding& b = stages[s];
		if (!b.shader)
			return true;
		const UINT n = b.shader->m_bindings.srv_count;
		u32 hazard = 0;
		for (UINT i = 0; i < n; ++i)
		{
			ShaderResourceView* v = b.srv[i];
			if (!v)
				continue;
			bool conflict = false;
			for (UINT o = 0; o < output_count; ++o)
				if (output_res[o] == v->m_res && Overlap(outputs[o], v))
				{
					conflict = true;
					break;
				}
			if (conflict)
				hazard |= 1u << i;
			else
				RequireView(v, v->m_res, ShaderReadState());
		}
		if (hazard != b.hazard)
		{
			b.hazard = hazard;
			b.srv_dirty = true;
		}
		return true;
	}

	bool PrepareDraw()
	{
		if (!stages[StageVS].shader)
		{
			DX12_WARN_ONCE("! D3D12: draw without a vertex shader skipped");
			return false;
		}
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			ring_wrapped = false;
			// Outputs and their states
			View* outputs[kMaxRT + 1];
			Resource* output_res[kMaxRT + 1];
			UINT output_count = 0;
			for (UINT i = 0; i < kMaxRT; ++i)
			{
				if (!rtv[i])
					continue;
				D3D12_CPU_DESCRIPTOR_HANDLE h;
				Resource* r = ResolveRTV(rtv[i], h);
				if (!r)
					continue;
				if (rtv[i]->m_back_buffer)
					RequireWhole(r, D3D12_RESOURCE_STATE_RENDER_TARGET);
				else
				{
					RequireView(rtv[i], r, D3D12_RESOURCE_STATE_RENDER_TARGET);
					outputs[output_count] = rtv[i];
					output_res[output_count++] = r;
				}
			}
			if (dsv && dsv->m_res)
			{
				if (dsv->m_read_only)
					RequireView(dsv, dsv->m_res, D3D12_RESOURCE_STATE_DEPTH_READ | ShaderReadState());
				else
				{
					RequireView(dsv, dsv->m_res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					outputs[output_count] = dsv;
					output_res[output_count++] = dsv->m_res;
				}
			}
			for (UINT s = StageVS; s <= StagePS; ++s)
				PrepareStageViews(s, outputs, output_res, output_count);
			// Static vertex and index buffers stay in their read state from creation on
			FlushBarriers();

			if (shaders_changed)
			{
				RootLayoutKey key;
				memset(&key, 0, sizeof(key));
				for (UINT s = StageVS; s <= StagePS; ++s)
					key.stage[s] = ShaderBindingKey(stages[s].shader);
				gfx_root = dev->GetRootSignature(key);
				shaders_changed = false;
				pso_dirty = true;
			}
			if (!gfx_root)
				return false;
			if (pso_dirty)
			{
				gfx_pso = ResolveGraphicsPipeline();
				pso_dirty = false;
			}
			if (!gfx_pso)
				return false;

			SetHeaps();
			if (bound_gfx_root != gfx_root)
			{
				list->SetGraphicsRootSignature(gfx_root->signature);
				bound_gfx_root = gfx_root;
				if (stats_on)
					++stats.root_sets;
				for (UINT s = StageVS; s <= StagePS; ++s)
				{
					StageBinding& b = stages[s];
					b.srv_valid = b.sampler_valid = b.cbv_table_valid = false;
					memset(b.cb_applied, 0, sizeof(b.cb_applied));
				}
			}
			if (applied_pso != gfx_pso)
			{
				list->SetPipelineState(gfx_pso);
				applied_pso = gfx_pso;
				if (stats_on)
					++stats.pso_sets;
			}
			for (UINT s = StageVS; s <= StagePS; ++s)
				ApplyStage(s, gfx_root, false);
			if (ring_wrapped)
			{
				InvalidateApplied();
				continue;
			}
			break;
		}

		// Input assembler
		if (topo_dirty)
		{
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY(topology));
			topo_dirty = false;
		}
		{
			D3D12_VERTEX_BUFFER_VIEW views[kMaxVB] = {};
			for (UINT i = 0; i < vb_count; ++i)
			{
				Buffer* b = vb[i];
				if (!b)
					continue;
				const D3D12_GPU_VIRTUAL_ADDRESS va = BufferAddress(b);
				const UINT size = b->m_va ? b->m_desc.ByteWidth : zero_buffer->m_desc.ByteWidth;
				views[i].BufferLocation = va + vb_offset[i];
				views[i].SizeInBytes = size > vb_offset[i] ? size - vb_offset[i] : 0;
				views[i].StrideInBytes = vb_stride[i];
			}
			if (!vb_applied_valid || vb_count != vb_applied_count || memcmp(views, vb_applied, sizeof(views[0]) * vb_count))
			{
				if (vb_count)
					list->IASetVertexBuffers(0, vb_count, views);
				if (stats_on)
					++stats.vb_sets;
				memcpy(vb_applied, views, sizeof(views));
				vb_applied_count = vb_count;
				vb_applied_valid = true;
			}
		}
		if (ib)
		{
			D3D12_INDEX_BUFFER_VIEW view = {};
			view.BufferLocation = BufferAddress(ib) + ib_offset;
			const UINT size = ib->m_va ? ib->m_desc.ByteWidth : zero_buffer->m_desc.ByteWidth;
			view.SizeInBytes = size > ib_offset ? size - ib_offset : 0;
			view.Format = ib_format;
			if (!ib_applied_valid || memcmp(&view, &ib_applied, sizeof(view)))
			{
				list->IASetIndexBuffer(&view);
				if (stats_on)
					++stats.ib_sets;
				ib_applied = view;
				ib_applied_valid = true;
			}
		}

		// Output merger, rasterizer
		{
			D3D12_CPU_DESCRIPTOR_HANDLE handles[kMaxRT];
			UINT count = 0;
			for (UINT i = 0; i < kMaxRT; ++i)
			{
				if (rtv[i])
				{
					ResolveRTV(rtv[i], handles[i]);
					count = i + 1;
				}
				else
					handles[i] = null_rtv;
			}
			if (!rt_dirty && memcmp(handles, rtv_applied, sizeof(handles[0]) * count))
				rt_dirty = true; // the back buffer moved on
			if (rt_dirty)
			{
				list->OMSetRenderTargets(count, count ? handles : nullptr, FALSE, dsv ? &dsv->m_cpu : nullptr);
				if (stats_on)
					++stats.rt_sets;
				memcpy(rtv_applied, handles, sizeof(handles));
				rt_dirty = false;
			}
		}
		if (vp_dirty)
		{
			if (viewport_count)
				list->RSSetViewports(viewport_count, reinterpret_cast<const D3D12_VIEWPORT*>(viewports));
			if (stats_on)
				++stats.vp_sets;
			vp_dirty = false;
		}
		if (sc_dirty)
		{
			const bool scissor = raster && raster->m_desc.ScissorEnable;
			D3D12_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
			UINT count;
			if (scissor && scissor_count)
			{
				count = scissor_count;
				memcpy(rects, scissors, sizeof(D3D12_RECT) * count);
			}
			else
			{
				count = _max(viewport_count, 1u);
				for (UINT i = 0; i < count; ++i)
					rects[i] = { 0, 0, D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX };
			}
			list->RSSetScissorRects(count, rects);
			sc_dirty = false;
			if (stats_on)
				++stats.sc_sets;
		}
		if (ref_dirty)
		{
			list->OMSetStencilRef(stencil_ref);
			ref_dirty = false;
		}
		if (factor_dirty)
		{
			list->OMSetBlendFactor(blend_factor);
			factor_dirty = false;
		}
		Record();
		return true;
	}

	bool PrepareDispatch()
	{
		StageBinding& b = stages[StageCS];
		if (!b.shader)
			return false;
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			ring_wrapped = false;
			View* outputs[kMaxUAV];
			Resource* output_res[kMaxUAV];
			UINT output_count = 0;
			for (UINT i = 0; i < b.shader->m_bindings.uav_count; ++i)
			{
				UnorderedAccessView* u = b.uav[i];
				if (!u || !u->m_res)
					continue;
				Resource* r = u->m_res;
				RequireView(u, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				// Still marked written: no transition out of UAV since (one clears the mark), also with per-subresource
				// states (mip chains), where the whole-resource state check missed it
				if (r->m_written_uav)
				{
					// D3D11 orders dispatches writing the same UAV
					D3D12_RESOURCE_BARRIER ub = {};
					ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
					ub.UAV.pResource = r->m_resource;
					barriers.push_back(ub);
				}
				outputs[output_count] = u;
				output_res[output_count++] = r;
			}
			PrepareStageViews(StageCS, outputs, output_res, output_count);
			FlushBarriers();

			if (cs_changed)
			{
				RootLayoutKey key;
				memset(&key, 0, sizeof(key));
				key.stage[StageCS] = ShaderBindingKey(b.shader);
				cs_root = dev->GetRootSignature(key);
				cs_changed = false;
				cs_pso = nullptr;
			}
			if (!cs_root)
				return false;
			if (!cs_pso)
			{
				const u64 pkey = cs_root->id * 0x9E3779B97F4A7C15ull ^ b.shader->m_content;
				{
					std::lock_guard<std::mutex> lock(dev->pipeline_lock);
					auto it = dev->compute_pipelines.find(pkey);
					if (it != dev->compute_pipelines.end())
						cs_pso = it->second;
				}
				if (!cs_pso)
				{
					cs_pso = CreateComputePipeline(device, cs_root, b.shader);
					std::lock_guard<std::mutex> lock(dev->pipeline_lock);
					dev->compute_pipelines[pkey] = cs_pso;
				}
			}
			if (!cs_pso)
				return false;
			SetHeaps();
			if (bound_cs_root != cs_root)
			{
				list->SetComputeRootSignature(cs_root->signature);
				bound_cs_root = cs_root;
				b.srv_valid = b.sampler_valid = b.uav_valid = b.cbv_table_valid = false;
				memset(b.cb_applied, 0, sizeof(b.cb_applied));
			}
			if (applied_pso != cs_pso)
			{
				list->SetPipelineState(cs_pso);
				applied_pso = cs_pso;
			}
			ApplyStage(StageCS, cs_root, true);
			if (ring_wrapped)
			{
				InvalidateApplied();
				continue;
			}
			break;
		}
		Record();
		return true;
	}

	//---------------------------------------------------------------------------------------------------------------
	// Submission and frames

	u64 last_submitted = 0;
	struct PendingSignal
	{
		ID3D12Fence* fence;
		u64 value;
	};
	std::vector<PendingSignal> signals;

	u64 Submit(bool reopen)
	{
		FlushBarriers();
		for (size_t i = 0; i < events.size(); ++i)
			list->EndEvent();
		list->Close();
		const u64 value = dev->SubmitFrame(list);
		last_submitted = value;
		if (!signals.empty())
		{
			std::lock_guard<std::mutex> lock(dev->queue_lock);
			for (const PendingSignal& p : signals)
			{
				device->m_queue->Signal(p.fence, p.value);
				p.fence->Release();
			}
			signals.clear();
		}
		recorded = false;
		draws_since_submit = 0;
		++submits_this_frame;
		if (stats_on)
			++stats.submits;
		if (reopen)
		{
			list->Reset(allocator[frame_index], nullptr);
			InvalidateApplied();
			for (const std::wstring& e : events)
				list->BeginEvent(0, e.c_str(), UINT((e.size() + 1) * sizeof(wchar_t)));
		}
		return value;
	}

	// The calling side's frame (its upload memory comes back once the GPU passed this fence)
	void NextFrame(u64 fence, u64 caller_frame)
	{
		frame_fences[caller_frame % kFrameRing] = fence;
		recorded_frame.store(caller_frame, std::memory_order_release);
		frame_fence[frame_index] = fence;
		frame_index = (frame_index + 1) % kFrames;
		dev->WaitFence(frame_fence[frame_index]);
		allocator[frame_index]->Reset();
		list->Reset(allocator[frame_index], nullptr);
		const u64 completed = dev->Completed();
		segment_next[frame_index] = 0;
		dev->ProcessDeferred(completed, false);
		++frame_number;
		InvalidateApplied();
		for (const std::wstring& e : events)
			list->BeginEvent(0, e.c_str(), UINT((e.size() + 1) * sizeof(wchar_t)));
		if (trace_frames && !--trace_frames)
			Msg("[dx12 trace] end, %u submissions in the frame", submits_this_frame);
		submits_last_frame = submits_this_frame;
		submits_this_frame = 0;
		srv_tables.clear();
		if (stats_on)
		{
			++stats.frames;
			if (GetTickCount64() >= stats_end_ms)
				ReportStats();
		}
		if (trace_requested && !trace_frames)
		{
			trace_frames = trace_requested;
			trace_requested = 0;
			trace_index = 0;
			Msg("[dx12 trace] frame %llu, %u frame(s)", frame_number, trace_frames);
		}
	}

	void ReportStats()
	{
		stats_on = false;
		const ContextStats& t = stats;
		const double n = double(t.frames ? t.frames : 1);
		Msg("* [stat_dx12] per frame over %llu frames: %.0f draws, %.0f dispatches, %.0f barriers in %.0f calls, %.0f pipeline "
		    "sets, %.0f root signature sets, SRV tables %.0f new + %.0f reused (%.0f sets), %.0f sampler tables new, %.0f root CBV "
		    "sets, %.0f render target sets, %.0f vertex / %.0f index buffer sets, %.0f viewport / %.0f scissor sets, %.0f clears, "
		    "%.0f copies, %.0f submissions",
		    t.frames, t.draws / n, t.dispatches / n, t.barriers / n, t.barrier_calls / n, t.pso_sets / n, t.root_sets / n,
		    t.srv_tables / n, t.srv_table_hits / n, t.srv_table_sets / n, t.sampler_tables / n, t.cbv_sets / n, t.rt_sets / n,
		    t.vb_sets / n, t.ib_sets / n, t.vp_sets / n, t.sc_sets / n, t.clears / n, t.copies / n, t.submits / n);
		std::vector<std::pair<u64, std::wstring>> top;
		for (const auto& e : stats_event_barriers)
			top.emplace_back(e.second, e.first);
		std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		for (size_t i = 0; i < top.size() && i < 12; ++i)
		{
			std::string name;
			for (wchar_t ch : top[i].second)
				name += char(ch < 128 ? ch : '?');
			Msg("*   barriers in %-40s %.1f per frame", name.c_str(), top[i].first / n);
		}
		stats_event_barriers.clear();
	}

	bool CommandsPending(u64 fence) const { return fence >= dev->pending_fence.load(); }

	// What the calling side's Flush queues: the recorded commands to the GPU (or the initial uploads alone)
	void FlushBackend()
	{
		if (!recorded && barriers.empty())
		{
			std::lock_guard<std::mutex> lock(dev->init_lock);
			dev->SubmitInitLocked();
			return;
		}
		Submit(true);
	}

	// GetBackBuffer's alias stands for the swap chain buffer current at recording
	Resource* Res(Resource* r) { return r && r->m_back_buffer_alias ? device->CurrentBackBuffer() : r; }

	// NativeRecorder
	void Transition(Resource* res, D3D12_RESOURCE_STATES state) override { RequireWhole(Res(res), state); }
	ID3D12GraphicsCommandList* List() override
	{
		FlushBarriers();
		Record();
		return list;
	}
	Texture2D* BackBuffer(D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override
	{
		if (rtv)
			*rtv = device->CurrentBackBufferRTV();
		return device->CurrentBackBuffer();
	}
};

//===================================================================================================================
// Commands: a call's recording work as a closure, stored in a chunk the worker runs in order (or run at once)

struct Cmd
{
	Cmd* next = nullptr;
	virtual void Run(Context::Impl& c) = 0;
	virtual ~Cmd() {}
};

template <class F>
struct CmdOf final : Cmd
{
	F fn;
	template <class G>
	explicit CmdOf(G&& g) : fn(std::forward<G>(g)) {}
	void Run(Context::Impl& c) override { fn(c); }
};

struct Chunk
{
	static const size_t kSize = 32 << 10;
	alignas(16) u8 data[kSize];
	size_t used = 0;
	Cmd* first = nullptr;
	Cmd* last = nullptr;
	u64 id = 0;
};

template <class T>
static void Rebind(T*& slot, T* value)
{
	if (slot == value)
		return;
	if (value)
		value->AddRef();
	if (slot)
		slot->Release();
	slot = value;
}

// The footprint of one subresource of a GPU texture, at offset 0
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint(ID3D12Device* d3d, Resource* r, UINT sub, UINT* rows, UINT64* row_bytes, UINT64* total)
{
	D3D12_RESOURCE_DESC rd = {};
	switch (r->m_dimension)
	{
	case D3D11_RESOURCE_DIMENSION_TEXTURE1D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; break;
	case D3D11_RESOURCE_DIMENSION_TEXTURE3D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; break;
	default: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; break;
	}
	rd.Width = r->m_width;
	rd.Height = r->m_height;
	rd.DepthOrArraySize = UINT16(r->m_depth_or_array);
	rd.MipLevels = UINT16(r->m_mips);
	rd.Format = r->m_format;
	rd.SampleDesc.Count = 1;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	d3d->GetCopyableFootprints(&rd, sub, 1, 0, &fp, rows, row_bytes, total);
	return fp;
}

static void NoteCopy(Context::Impl& c, Resource* dst, Resource* src)
{
	if (c.stats_on)
		++c.stats.copies;
	if (c.trace_frames)
	{
		std::string out = make_string("[dx12 trace] %u copy %s <- %s", c.trace_index++, Context::Impl::ResName(dst).c_str(),
		                              Context::Impl::ResName(src).c_str());
		c.TraceLine(out);
	}
}

struct StageState
{
	Shader* shader = nullptr;
	Buffer* cb[kMaxCB] = {};
	ShaderResourceView* srv[kMaxSRV] = {};
	SamplerState* sampler[kMaxSamplers] = {};
	UnorderedAccessView* uav[kMaxUAV] = {};
};

//===================================================================================================================
// The calling side: the bindings as set (the getters answer from them, unchanged sets queue nothing, references are
// held as D3D11 holds them), upload memory (Map answers at once), readiness of readbacks and queries, and the queue

struct Context::Front
{
	Context::Impl* impl = nullptr;
	Device* device = nullptr;
	Device::Impl* dev = nullptr;
	ID3D12Device* d3d = nullptr;

	StageState stages[StageCount];
	InputLayout* layout = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	Buffer* vb[kMaxVB] = {};
	UINT vb_stride[kMaxVB] = {}, vb_offset[kMaxVB] = {};
	Buffer* ib = nullptr;
	DXGI_FORMAT ib_format = DXGI_FORMAT_R16_UINT;
	UINT ib_offset = 0;
	RasterizerState* raster = nullptr;
	BlendState* blend = nullptr;
	FLOAT blend_factor[4] = {};
	UINT sample_mask = 0xFFFFFFFF;
	DepthStencilState* depth = nullptr;
	UINT stencil_ref = 0;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT viewport_count = 0;
	D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT scissor_count = 0;
	bool scissors_set = false;
	RenderTargetView* rtv[kMaxRT] = {};
	DepthStencilView* dsv = nullptr;
	INT event_depth = 0;

	// Upload memory in frames of this side; a frame's pages come back once the GPU passed the fence the recording
	// side gave that frame
	UploadRing upload;
	u64 frame = 1;
	u64 upload_epoch = 1; // changes when upload memory is recycled: dynamic buffers mapped before get renamed
	u64 reclaimed_frame = 0;
	u64 fresh_epoch = 0; // the bound dynamic buffers were checked for this epoch

	// The queue: chunks of commands to the worker; ids count up, done_id is the last chunk the worker finished
	bool threaded = false;
	std::thread worker;
	DWORD caller_thread = 0;
	Chunk* open = nullptr;
	std::atomic<u64> open_id{ 1 };
	u64 dispatched_id = 0;
	u64 present_chunk = 0; // the chunk with the last Present
	u64 flush_chunk = 0; // the chunk with the last flush asked for by a readback or query
	std::mutex queue_lock;
	std::condition_variable queue_cv;
	std::deque<Chunk*> queue;
	bool stop = false;
	std::mutex free_lock;
	std::vector<Chunk*> free_chunks;
	std::mutex done_lock;
	std::condition_variable done_cv;
	std::atomic<u64> done_id{ 0 };
	// Objects whose last reference went, with the chunk open at that time
	std::mutex delete_lock;
	std::vector<std::pair<u64, Object*>> deletes;
	std::vector<u8> scratch; // payloads when not threaded
	static Front* s_active;

	//---------------------------------------------------------------------------------------------------------------
	// Queue

	Chunk* NewChunk()
	{
		{
			std::lock_guard<std::mutex> lock(free_lock);
			if (!free_chunks.empty())
			{
				Chunk* c = free_chunks.back();
				free_chunks.pop_back();
				return c;
			}
		}
		return new Chunk();
	}

	void* Reserve(size_t size, size_t align, size_t keep_free)
	{
		size_t at = (open->used + align - 1) & ~(align - 1);
		if (at + size + keep_free > Chunk::kSize)
		{
			Dispatch();
			at = 0;
		}
		open->used = at + size;
		return open->data + at;
	}

	void Dispatch()
	{
		if (!threaded || !open->first)
			return;
		Chunk* ch = open;
		dispatched_id = ch->id;
		open = NewChunk();
		open->id = dispatched_id + 1;
		open_id.store(open->id);
		{
			std::lock_guard<std::mutex> lock(queue_lock);
			queue.push_back(ch);
		}
		queue_cv.notify_one();
	}

	template <class F>
	void Emit(F&& fn)
	{
		if (!threaded)
		{
			fn(*impl);
			return;
		}
		if (GetCurrentThreadId() != caller_thread)
			ForeignCall();
		typedef CmdOf<typename std::decay<F>::type> C;
		C* cmd = new (Reserve(sizeof(C), alignof(C), 0)) C(std::forward<F>(fn));
		if (open->last)
			open->last->next = cmd;
		else
			open->first = cmd;
		open->last = cmd;
	}

	// Room for a command's array data, valid until the command ran (the command must be emitted right after)
	template <class T>
	T* Payload(UINT count)
	{
		const size_t bytes = sizeof(T) * count;
		if (!threaded)
		{
			if (scratch.size() < bytes)
				scratch.resize(bytes);
			return reinterpret_cast<T*>(scratch.data());
		}
		return static_cast<T*>(Reserve(bytes, 16, 1024));
	}

	// A call from another thread than the one that started the recording: the level load task draws the loading
	// screen (under the engine's loading-title lock, never at the same time as the render thread); anything else
	// would race. The first ones go to the log with their stack (offsets in the exe, for the PDB)
	static void ForeignCall()
	{
		static std::atomic<int> logged{ 0 };
		if (logged++ >= 3)
			return;
		void* frames[12];
		const USHORT n = CaptureStackBackTrace(2, 12, frames, nullptr);
		const u64 base = u64(GetModuleHandle(nullptr));
		std::string out = make_string("! D3D12: a context call from thread %u while recording is threaded:", GetCurrentThreadId());
		for (USHORT i = 0; i < n; ++i)
			out += make_string(" +0x%llx", u64(frames[i]) - base);
		Msg("%s", out.c_str());
	}

	u64 Tag() const { return threaded ? open_id.load() : 0; } // the chunk the next command goes into
	bool Done(u64 id) const { return done_id.load(std::memory_order_acquire) >= id; }

	void WaitDone(u64 id)
	{
		if (Done(id))
			return;
		std::unique_lock<std::mutex> lock(done_lock);
		done_cv.wait(lock, [&] { return Done(id); });
	}

	void WorkerMain()
	{
		for (;;)
		{
			Chunk* ch;
			{
				std::unique_lock<std::mutex> lock(queue_lock);
				queue_cv.wait(lock, [this] { return stop || !queue.empty(); });
				if (queue.empty())
					return;
				ch = queue.front();
				queue.pop_front();
			}
			for (Cmd* cmd = ch->first; cmd;)
			{
				Cmd* next = cmd->next;
				cmd->Run(*impl);
				cmd->~Cmd();
				cmd = next;
			}
			const u64 id = ch->id;
			ch->used = 0;
			ch->first = ch->last = nullptr;
			{
				std::lock_guard<std::mutex> lock(free_lock);
				free_chunks.push_back(ch);
			}
			{
				std::lock_guard<std::mutex> lock(done_lock);
				done_id.store(id, std::memory_order_release);
			}
			done_cv.notify_all();
			// A call releases before it queues its command, which may then open the next chunk: two chunks of margin
			if (id > 2)
				DeleteDone(id - 2);
		}
	}

	static void DeferDelete(Object* o)
	{
		Front* f = s_active;
		if (!f)
		{
			delete o;
			return;
		}
		std::lock_guard<std::mutex> lock(f->delete_lock);
		f->deletes.emplace_back(f->open_id.load(), o);
	}

	void DeleteDone(u64 id)
	{
		std::vector<Object*> ready;
		{
			std::lock_guard<std::mutex> lock(delete_lock);
			for (size_t i = 0; i < deletes.size();)
			{
				if (deletes[i].first <= id)
				{
					ready.push_back(deletes[i].second);
					deletes[i] = deletes.back();
					deletes.pop_back();
				}
				else
					++i;
			}
		}
		for (Object* o : ready)
			delete o;
	}

	void Start()
	{
		if (threaded)
			return;
		open = NewChunk();
		open->id = dispatched_id + 1;
		open_id.store(open->id);
		stop = false;
		caller_thread = GetCurrentThreadId();
		threaded = true;
		s_active = this;
		g_defer_delete.store(&Front::DeferDelete);
		worker = std::thread([this] { WorkerMain(); });
	}

	void StopWorker()
	{
		if (!threaded)
			return;
		Synchronize();
		g_defer_delete.store(nullptr);
		{
			std::lock_guard<std::mutex> lock(queue_lock);
			stop = true;
		}
		queue_cv.notify_all();
		worker.join();
		threaded = false;
		{
			std::lock_guard<std::mutex> lock(free_lock);
			free_chunks.push_back(open);
		}
		open = nullptr;
		present_chunk = flush_chunk = 0;
		DeleteDone(UINT64_MAX);
	}

	void Flush()
	{
		Emit([](Impl& c) { c.FlushBackend(); });
		Dispatch();
	}

	void Synchronize()
	{
		Flush();
		if (threaded)
			WaitDone(dispatched_id);
	}

	// A readback or query waits for the GPU: the commands up to it go out, once
	void RequestFlush()
	{
		if (threaded && !Done(flush_chunk))
			return;
		Flush();
		flush_chunk = dispatched_id;
	}

	//---------------------------------------------------------------------------------------------------------------
	// Upload memory and dynamic buffers

	// A shadowed dynamic buffer's data into this frame's upload memory, the buffer renamed to it
	void UploadShadow(Buffer* b)
	{
		UploadSlice slice;
		const u64 size = (u64(b->m_desc.ByteWidth) + 255) & ~255ull;
		if (!upload.Alloc(size, 256, slice))
			return;
		memcpy(slice.cpu, b->m_shadow.data(), b->m_shadow.size());
		b->m_ptr = slice.cpu;
		b->m_ptr_frame = upload_epoch;
		b->m_map_buffer = slice.buffer;
		b->m_map_footprint.Offset = slice.offset;
		const D3D12_GPU_VIRTUAL_ADDRESS va = slice.va;
		Emit([b, va](Impl&) { b->m_va = va; });
	}

	// A bound dynamic buffer unchanged since an earlier epoch: its upload memory may be reused
	void Freshen(Buffer* b)
	{
		if (!b || b->m_usage != D3D11_USAGE_DYNAMIC)
			return;
		if (b->m_shadowed)
		{
			if (b->m_ptr_frame != upload_epoch)
				UploadShadow(b);
		}
		else if (b->m_ptr && b->m_ptr_frame != upload_epoch && !b->m_warned_stale)
		{
			b->m_warned_stale = true;
			Msg("! D3D12: a large dynamic buffer (%u bytes) is used in a later frame than it was written", b->m_desc.ByteWidth);
		}
	}

	// Before a draw or dispatch
	void BeforeWork()
	{
		if (fresh_epoch == upload_epoch)
			return;
		fresh_epoch = upload_epoch;
		for (StageState& st : stages)
			for (Buffer* b : st.cb)
				Freshen(b);
		for (Buffer* b : vb)
			Freshen(b);
		Freshen(ib);
	}

	// The last frame whose upload memory the GPU is done with
	u64 CompletedFrame()
	{
		const u64 recorded = impl->recorded_frame.load(std::memory_order_acquire);
		const u64 gpu = dev->Completed();
		while (reclaimed_frame < recorded)
		{
			const u64 next = reclaimed_frame + 1;
			if (impl->frame_fences[next % Impl::kFrameRing] > gpu)
				break;
			reclaimed_frame = next;
		}
		return reclaimed_frame;
	}

	// After the frame's Present (or EndFrame) command
	void FrameDone()
	{
		Dispatch();
		if (threaded)
		{
			// One frame ahead of the recording at most: the previous frame's Present is through
			if (present_chunk)
				WaitDone(present_chunk);
			present_chunk = dispatched_id;
		}
		upload.Retire(frame);
		upload.Reclaim(CompletedFrame());
		++frame;
		++upload_epoch;
	}

	// One subresource of a STAGING (CPU write) texture into upload memory; returns the copy source
	bool StageSubresource(Resource* src, UINT sub, D3D12_TEXTURE_COPY_LOCATION& loc)
	{
		// The first binds after a level load upload a lot at once: past 512 MB in a frame the GPU takes what is
		// recorded and the upload memory comes back, instead of growing for the whole frame
		if (upload.BytesThisFrame() > (512ull << 20))
		{
			Synchronize();
			dev->WaitQueueIdle();
			upload.Retire(frame);
			upload.Reclaim(frame);
			++upload_epoch;
		}
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = Footprint(d3d, src, sub, &rows, &row_bytes, &total);
		UploadSlice slice;
		if (!upload.Alloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, slice))
			return false;
		const SubresourceLayout& l = src->m_layouts[sub];
		for (UINT z = 0; z < fp.Footprint.Depth; ++z)
			for (UINT r = 0; r < rows; ++r)
				memcpy(slice.cpu + u64(z) * fp.Footprint.RowPitch * rows + u64(r) * fp.Footprint.RowPitch,
				       src->m_cpu_data.data() + l.offset + size_t(z) * l.depth_pitch + size_t(r) * l.row_pitch, size_t(row_bytes));
		fp.Offset = slice.offset;
		loc = {};
		loc.pResource = slice.buffer;
		loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		loc.PlacedFootprint = fp;
		return true;
	}
};

Context::Front* Context::Front::s_active = nullptr;

//===================================================================================================================

Context::Context(Device* device)
{
	m_impl = new Impl();
	Impl& c = *m_impl;
	c.self = this;
	c.device = device;
	c.dev = device->m_impl;
	c.d3d = device->m_device;
	ID3D12Device* d3d = c.d3d;

	for (UINT i = 0; i < kFrames; ++i)
		d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator[i]));
	d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator[0], nullptr, IID_PPV_ARGS(&c.list));
	c.list->SetName(L"xr frame");

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = Impl::kViewHeapSize;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(d3d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.view_heap))))
		Msg("! D3D12: can't create the shader-visible descriptor heap");
	c.view_inc = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	c.view_cpu0 = c.view_heap->GetCPUDescriptorHandleForHeapStart();
	c.view_gpu0 = c.view_heap->GetGPUDescriptorHandleForHeapStart();
	c.segment_size = (Impl::kViewHeapSize - Impl::kStaticViews) / kFrames;

	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	hd.NumDescriptors = Impl::kSamplerHeapSize;
	if (FAILED(d3d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.sampler_heap))))
		Msg("! D3D12: can't create the sampler heap");
	c.sampler_inc = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	c.sampler_cpu0 = c.sampler_heap->GetCPUDescriptorHandleForHeapStart();
	c.sampler_gpu0 = c.sampler_heap->GetGPUDescriptorHandleForHeapStart();

	// D3D11's default states for draws that set none
	{
		D3D11_BLEND_DESC bd = {};
		for (UINT i = 0; i < 8; ++i)
		{
			bd.RenderTarget[i].SrcBlend = bd.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[i].DestBlend = bd.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[i].BlendOp = bd.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		device->CreateBlendState(&bd, &c.default_blend);
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_BACK;
		rd.DepthClipEnable = TRUE;
		device->CreateRasterizerState(&rd, &c.default_raster);
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = TRUE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dd.DepthFunc = D3D11_COMPARISON_LESS;
		dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
		dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		dd.BackFace = dd.FrontFace;
		device->CreateDepthStencilState(&dd, &c.default_depth);
	}
	{
		std::vector<u8> zeros(65536, 0);
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = UINT(zeros.size());
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA init = { zeros.data(), 0, 0 };
		device->CreateBuffer(&bd, &init, &c.zero_buffer);
	}
	{
		D3D12_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		c.null_rtv_slot = c.dev->rtv_pool.Alloc(&c.null_rtv);
		d3d->CreateRenderTargetView(nullptr, &rd, c.null_rtv);
	}

	m_front = new Front();
	Front& f = *m_front;
	f.impl = m_impl;
	f.device = device;
	f.dev = c.dev;
	f.d3d = d3d;
	f.caller_thread = GetCurrentThreadId();
	f.upload.Init(device->m_allocator);
}

Context::~Context()
{
	m_front->StopWorker();
	ClearState();
	Impl& c = *m_impl;
	if (c.default_blend)
		c.default_blend->Release();
	if (c.default_raster)
		c.default_raster->Release();
	if (c.default_depth)
		c.default_depth->Release();
	if (c.zero_buffer)
		c.zero_buffer->Release();
	c.dev->rtv_pool.Free(c.null_rtv_slot);
	c.list->Close();
	m_front->upload.Destroy();
	c.list->Release();
	for (ID3D12CommandAllocator* a : c.allocator)
		if (a)
			a->Release();
	if (c.view_heap)
		c.view_heap->Release();
	if (c.sampler_heap)
		c.sampler_heap->Release();
	for (Chunk* ch : m_front->free_chunks)
		delete ch;
	delete m_front;
	delete m_impl;
}

HRESULT Context::QueryInterface(REFIID, void** out)
{
	if (!out)
		return E_POINTER;
	*out = nullptr;
	return E_NOINTERFACE;
}

void Context::SetThreaded(bool on)
{
	Front& f = *m_front;
	if (on == f.threaded)
		return;
	if (on)
		f.Start();
	else
		f.StopWorker();
	Msg("* D3D12: commands recorded %s", on ? "on a worker thread" : "on the calling thread");
}

bool Context::Threaded() const { return m_front->threaded; }

void Context::Synchronize() { m_front->Synchronize(); }

//-------------------------------------------------------------------------------------------------------------------
// Binding

void Context::SetShader(Stage stage, Shader* shader)
{
	Front& f = *m_front;
	Shader*& slot = f.stages[stage].shader;
	if (slot == shader)
		return;
	Rebind(slot, shader);
	f.Emit([stage, shader](Impl& c) {
		StageBinding& b = c.stages[stage];
		const u64 old_key = ShaderBindingKey(b.shader);
		b.shader = shader;
		if (stage == StageCS)
		{
			c.cs_pso = nullptr;
			if (ShaderBindingKey(shader) != old_key)
				c.cs_changed = true;
		}
		else
		{
			c.pso_dirty = true;
			if (ShaderBindingKey(shader) != old_key)
				c.shaders_changed = true;
			// The table's size and its null views' dimensions follow the shader (the table cache finds equal tables)
			b.srv_dirty = true;
		}
	});
}

void Context::SetSRVs(Stage stage, UINT start, UINT count, ShaderResourceView* const* views)
{
	Front& f = *m_front;
	ShaderResourceView** slots = f.stages[stage].srv;
	UINT lo = kMaxSRV, hi = 0;
	for (UINT i = 0; i < count && start + i < kMaxSRV; ++i)
	{
		ShaderResourceView* v = views ? views[i] : nullptr;
		if (slots[start + i] == v)
			continue;
		Rebind(slots[start + i], v);
		lo = _min(lo, start + i);
		hi = start + i + 1;
	}
	if (lo >= hi)
		return;
	ShaderResourceView** p = f.Payload<ShaderResourceView*>(hi - lo);
	memcpy(p, slots + lo, sizeof(*p) * (hi - lo));
	f.Emit([stage, lo, hi, p](Impl& c) {
		StageBinding& b = c.stages[stage];
		for (UINT i = lo; i < hi; ++i)
			b.srv[i] = p[i - lo];
		b.srv_dirty = true;
	});
}

void Context::SetSamplers(Stage stage, UINT start, UINT count, SamplerState* const* samplers)
{
	Front& f = *m_front;
	SamplerState** slots = f.stages[stage].sampler;
	UINT lo = kMaxSamplers, hi = 0;
	for (UINT i = 0; i < count && start + i < kMaxSamplers; ++i)
	{
		SamplerState* s = samplers ? samplers[i] : nullptr;
		if (slots[start + i] == s)
			continue;
		Rebind(slots[start + i], s);
		lo = _min(lo, start + i);
		hi = start + i + 1;
	}
	if (lo >= hi)
		return;
	SamplerState** p = f.Payload<SamplerState*>(hi - lo);
	memcpy(p, slots + lo, sizeof(*p) * (hi - lo));
	f.Emit([stage, lo, hi, p](Impl& c) {
		StageBinding& b = c.stages[stage];
		for (UINT i = lo; i < hi; ++i)
			b.sampler[i] = p[i - lo];
		b.sampler_dirty = true;
	});
}

void Context::SetCBs(Stage stage, UINT start, UINT count, Buffer* const* buffers)
{
	Front& f = *m_front;
	Buffer** slots = f.stages[stage].cb;
	UINT lo = kMaxCB, hi = 0;
	for (UINT i = 0; i < count && start + i < kMaxCB; ++i)
	{
		Buffer* b = buffers ? buffers[i] : nullptr;
		if (slots[start + i] == b)
			continue;
		Rebind(slots[start + i], b);
		f.Freshen(b);
		lo = _min(lo, start + i);
		hi = start + i + 1;
	}
	if (lo >= hi)
		return;
	Buffer** p = f.Payload<Buffer*>(hi - lo);
	memcpy(p, slots + lo, sizeof(*p) * (hi - lo));
	f.Emit([stage, lo, hi, p](Impl& c) {
		StageBinding& b = c.stages[stage];
		for (UINT i = lo; i < hi; ++i)
			b.cb[i] = p[i - lo];
	});
}

void Context::CSGetShader(ComputeShader** shader, void** instances, UINT* count)
{
	Shader* s = m_front->stages[StageCS].shader;
	if (s)
		s->AddRef();
	*shader = static_cast<ComputeShader*>(s);
	if (count)
		*count = 0;
}

void Context::CSGetShaderResources(UINT start, UINT count, ShaderResourceView** views)
{
	for (UINT i = 0; i < count; ++i)
	{
		ShaderResourceView* v = start + i < kMaxSRV ? m_front->stages[StageCS].srv[start + i] : nullptr;
		if (v)
			v->AddRef();
		views[i] = v;
	}
}

void Context::PSGetShaderResources(UINT start, UINT count, ShaderResourceView** views)
{
	for (UINT i = 0; i < count; ++i)
	{
		ShaderResourceView* v = start + i < kMaxSRV ? m_front->stages[StagePS].srv[start + i] : nullptr;
		if (v)
			v->AddRef();
		views[i] = v;
	}
}

void Context::CSGetConstantBuffers(UINT start, UINT count, Buffer** buffers)
{
	for (UINT i = 0; i < count; ++i)
	{
		Buffer* b = start + i < kMaxCB ? m_front->stages[StageCS].cb[start + i] : nullptr;
		if (b)
			b->AddRef();
		buffers[i] = b;
	}
}

void Context::CSSetUnorderedAccessViews(UINT start, UINT count, UnorderedAccessView* const* views, const UINT*)
{
	Front& f = *m_front;
	UnorderedAccessView** slots = f.stages[StageCS].uav;
	UINT lo = kMaxUAV, hi = 0;
	for (UINT i = 0; i < count && start + i < kMaxUAV; ++i)
	{
		UnorderedAccessView* v = views ? views[i] : nullptr;
		if (slots[start + i] == v)
			continue;
		Rebind(slots[start + i], v);
		lo = _min(lo, start + i);
		hi = start + i + 1;
	}
	if (lo >= hi)
		return;
	UnorderedAccessView** p = f.Payload<UnorderedAccessView*>(hi - lo);
	memcpy(p, slots + lo, sizeof(*p) * (hi - lo));
	f.Emit([lo, hi, p](Impl& c) {
		StageBinding& b = c.stages[StageCS];
		for (UINT i = lo; i < hi; ++i)
			b.uav[i] = p[i - lo];
		b.uav_dirty = true;
	});
}

void Context::CSGetUnorderedAccessViews(UINT start, UINT count, UnorderedAccessView** views)
{
	for (UINT i = 0; i < count; ++i)
	{
		UnorderedAccessView* v = start + i < kMaxUAV ? m_front->stages[StageCS].uav[start + i] : nullptr;
		if (v)
			v->AddRef();
		views[i] = v;
	}
}

void Context::IASetInputLayout(InputLayout* layout)
{
	Front& f = *m_front;
	if (f.layout == layout)
		return;
	Rebind(f.layout, layout);
	f.Emit([layout](Impl& c) {
		c.layout = layout;
		c.pso_dirty = true;
	});
}

void Context::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	Front& f = *m_front;
	if (f.topology == topology)
		return;
	f.topology = topology;
	f.Emit([topology](Impl& c) {
		if (Impl::TopologyType(c.topology) != Impl::TopologyType(topology))
			c.pso_dirty = true;
		c.topology = topology;
		c.topo_dirty = true;
	});
}

void Context::IASetVertexBuffers(UINT start, UINT count, Buffer* const* buffers, const UINT* strides, const UINT* offsets)
{
	Front& f = *m_front;
	bool changed = false;
	for (UINT i = 0; i < count && start + i < kMaxVB; ++i)
	{
		const UINT s = start + i;
		Buffer* b = buffers ? buffers[i] : nullptr;
		const UINT stride = strides ? strides[i] : 0, offset = offsets ? offsets[i] : 0;
		if (f.vb[s] == b && f.vb_stride[s] == stride && f.vb_offset[s] == offset)
			continue;
		if (f.vb[s] != b)
		{
			Rebind(f.vb[s], b);
			f.Freshen(b);
		}
		f.vb_stride[s] = stride;
		f.vb_offset[s] = offset;
		changed = true;
	}
	if (!changed)
		return;
	struct VertexBuffers
	{
		Buffer* vb[kMaxVB];
		UINT stride[kMaxVB], offset[kMaxVB];
	} v;
	memcpy(v.vb, f.vb, sizeof(v.vb));
	memcpy(v.stride, f.vb_stride, sizeof(v.stride));
	memcpy(v.offset, f.vb_offset, sizeof(v.offset));
	f.Emit([v](Impl& c) {
		memcpy(c.vb, v.vb, sizeof(v.vb));
		memcpy(c.vb_stride, v.stride, sizeof(v.stride));
		memcpy(c.vb_offset, v.offset, sizeof(v.offset));
		UINT n = 0;
		for (UINT i = 0; i < kMaxVB; ++i)
			if (c.vb[i])
				n = i + 1;
		c.vb_count = n;
	});
}

void Context::IASetIndexBuffer(Buffer* buffer, DXGI_FORMAT format, UINT offset)
{
	Front& f = *m_front;
	if (f.ib == buffer && f.ib_format == format && f.ib_offset == offset)
		return;
	if (f.ib != buffer)
	{
		Rebind(f.ib, buffer);
		f.Freshen(buffer);
	}
	f.ib_format = format;
	f.ib_offset = offset;
	f.Emit([buffer, format, offset](Impl& c) {
		c.ib = buffer;
		c.ib_format = format;
		c.ib_offset = offset;
	});
}

void Context::RSSetState(RasterizerState* state)
{
	Front& f = *m_front;
	if (f.raster == state)
		return;
	Rebind(f.raster, state);
	f.Emit([state](Impl& c) {
		const BOOL scissor_before = c.raster ? c.raster->m_desc.ScissorEnable : FALSE;
		c.raster = state;
		c.pso_dirty = true;
		if ((state ? state->m_desc.ScissorEnable : FALSE) != scissor_before)
			c.sc_dirty = true;
	});
}

void Context::RSSetViewports(UINT count, const D3D11_VIEWPORT* viewports)
{
	Front& f = *m_front;
	count = _min(count, UINT(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
	if (count == f.viewport_count && !memcmp(f.viewports, viewports, sizeof(D3D11_VIEWPORT) * count))
		return;
	if (count)
		memcpy(f.viewports, viewports, sizeof(D3D11_VIEWPORT) * count);
	f.viewport_count = count;
	D3D11_VIEWPORT* p = f.Payload<D3D11_VIEWPORT>(_max(count, 1u));
	memcpy(p, f.viewports, sizeof(D3D11_VIEWPORT) * count);
	f.Emit([count, p](Impl& c) {
		if (count)
			memcpy(c.viewports, p, sizeof(D3D11_VIEWPORT) * count);
		if (count != c.viewport_count)
			c.sc_dirty = true;
		c.viewport_count = count;
		c.vp_dirty = true;
	});
}

void Context::RSGetViewports(UINT* count, D3D11_VIEWPORT* viewports)
{
	Front& f = *m_front;
	if (viewports)
		memcpy(viewports, f.viewports, sizeof(D3D11_VIEWPORT) * _min(*count, f.viewport_count));
	*count = f.viewport_count;
}

void Context::RSSetScissorRects(UINT count, const D3D11_RECT* rects)
{
	Front& f = *m_front;
	count = _min(count, UINT(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
	if (f.scissors_set && count == f.scissor_count && !memcmp(f.scissors, rects, sizeof(D3D11_RECT) * count))
		return;
	if (count)
		memcpy(f.scissors, rects, sizeof(D3D11_RECT) * count);
	f.scissor_count = count;
	f.scissors_set = true;
	D3D11_RECT* p = f.Payload<D3D11_RECT>(_max(count, 1u));
	memcpy(p, f.scissors, sizeof(D3D11_RECT) * count);
	f.Emit([count, p](Impl& c) {
		if (count)
			memcpy(c.scissors, p, sizeof(D3D11_RECT) * count);
		c.scissor_count = count;
		c.sc_dirty = true;
	});
}

void Context::OMSetRenderTargets(UINT count, RenderTargetView* const* rtvs, DepthStencilView* dsv)
{
	Front& f = *m_front;
	bool changed = f.dsv != dsv;
	for (UINT i = 0; i < kMaxRT; ++i)
	{
		RenderTargetView* v = (rtvs && i < count) ? rtvs[i] : nullptr;
		if (f.rtv[i] != v)
		{
			Rebind(f.rtv[i], v);
			changed = true;
		}
	}
	if (!changed)
		return;
	Rebind(f.dsv, dsv);
	struct Targets
	{
		RenderTargetView* rtv[kMaxRT];
		DepthStencilView* dsv;
	} t;
	memcpy(t.rtv, f.rtv, sizeof(t.rtv));
	t.dsv = dsv;
	f.Emit([t](Impl& c) {
		if (c.draws_since_submit >= Impl::kSubmitPass)
			c.SubmitEarly();
		bool formats = false;
		for (UINT i = 0; i < kMaxRT; ++i)
		{
			RenderTargetView* v = t.rtv[i];
			if (c.rtv[i] == v)
				continue;
			if (!c.rtv[i] || !v || c.rtv[i]->m_format != v->m_format || c.rtv[i]->m_res != v->m_res)
				formats = true;
			c.rtv[i] = v;
			c.rt_dirty = true;
		}
		if (c.dsv != t.dsv)
		{
			DepthStencilView* d = t.dsv;
			if (!c.dsv || !d || c.dsv->m_format != d->m_format || c.dsv->m_read_only != d->m_read_only || c.dsv->m_res != d->m_res)
				formats = true;
			c.dsv = d;
			c.rt_dirty = true;
		}
		if (formats)
			c.pso_dirty = true;
		// The views of the stages may have hazards with the new outputs
		if (c.rt_dirty)
			for (StageBinding& b : c.stages)
				if (b.hazard)
					b.srv_dirty = true;
	});
}

void Context::OMGetRenderTargets(UINT count, RenderTargetView** rtvs, DepthStencilView** dsv)
{
	Front& f = *m_front;
	if (rtvs)
		for (UINT i = 0; i < count; ++i)
		{
			RenderTargetView* v = i < kMaxRT ? f.rtv[i] : nullptr;
			if (v)
				v->AddRef();
			rtvs[i] = v;
		}
	if (dsv)
	{
		if (f.dsv)
			f.dsv->AddRef();
		*dsv = f.dsv;
	}
}

void Context::OMSetBlendState(BlendState* state, const FLOAT factor[4], UINT sample_mask)
{
	Front& f = *m_front;
	const FLOAT ones[4] = { 1.f, 1.f, 1.f, 1.f };
	const FLOAT* fac = factor ? factor : ones;
	if (f.blend == state && f.sample_mask == sample_mask && !memcmp(f.blend_factor, fac, sizeof(f.blend_factor)))
		return;
	Rebind(f.blend, state);
	f.sample_mask = sample_mask;
	memcpy(f.blend_factor, fac, sizeof(f.blend_factor));
	struct Blend
	{
		FLOAT factor[4];
	} b;
	memcpy(b.factor, fac, sizeof(b.factor));
	f.Emit([state, sample_mask, b](Impl& c) {
		if (c.blend != state)
		{
			c.blend = state;
			c.pso_dirty = true;
		}
		if (c.sample_mask != sample_mask)
		{
			c.sample_mask = sample_mask;
			c.pso_dirty = true;
		}
		if (memcmp(c.blend_factor, b.factor, sizeof(c.blend_factor)))
		{
			memcpy(c.blend_factor, b.factor, sizeof(c.blend_factor));
			c.factor_dirty = true;
		}
	});
}

void Context::OMSetDepthStencilState(DepthStencilState* state, UINT stencil_ref)
{
	Front& f = *m_front;
	if (f.depth == state && f.stencil_ref == stencil_ref)
		return;
	Rebind(f.depth, state);
	f.stencil_ref = stencil_ref;
	f.Emit([state, stencil_ref](Impl& c) {
		if (c.depth != state)
		{
			c.depth = state;
			c.pso_dirty = true;
		}
		if (c.stencil_ref != stencil_ref)
		{
			c.stencil_ref = stencil_ref;
			c.ref_dirty = true;
		}
	});
}

//-------------------------------------------------------------------------------------------------------------------
// Work

void Context::Draw(UINT vertex_count, UINT start_vertex)
{
	Front& f = *m_front;
	f.BeforeWork();
	f.Emit([vertex_count, start_vertex](Impl& c) {
		if (c.BeginDraw("draw", vertex_count))
			c.list->DrawInstanced(vertex_count, 1, start_vertex, 0);
	});
}

void Context::DrawIndexed(UINT index_count, UINT start_index, INT base_vertex)
{
	Front& f = *m_front;
	f.BeforeWork();
	f.Emit([index_count, start_index, base_vertex](Impl& c) {
		if (c.BeginDraw("draw_indexed", index_count))
			c.list->DrawIndexedInstanced(index_count, 1, start_index, base_vertex, 0);
	});
}

void Context::DrawInstanced(UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance)
{
	Front& f = *m_front;
	f.BeforeWork();
	f.Emit([vertex_count, instance_count, start_vertex, start_instance](Impl& c) {
		if (c.BeginDraw("draw", vertex_count))
			c.list->DrawInstanced(vertex_count, instance_count, start_vertex, start_instance);
	});
}

void Context::DrawIndexedInstanced(UINT index_count, UINT instance_count, UINT start_index, INT base_vertex, UINT start_instance)
{
	Front& f = *m_front;
	f.BeforeWork();
	f.Emit([index_count, instance_count, start_index, base_vertex, start_instance](Impl& c) {
		if (c.BeginDraw("draw_indexed", index_count))
			c.list->DrawIndexedInstanced(index_count, instance_count, start_index, base_vertex, start_instance);
	});
}

void Context::Dispatch(UINT x, UINT y, UINT z)
{
	Front& f = *m_front;
	f.BeforeWork();
	f.Emit([x, y, z](Impl& c) {
		const bool ok = c.PrepareDispatch();
		if (c.trace_frames)
		{
			std::string out = make_string("[dx12 trace] %u %s %ux%ux%u", c.trace_index++, ok ? "dispatch" : "SKIPPED dispatch", x, y, z);
			c.TraceStage(out, StageCS, "CS");
			c.TraceLine(out);
		}
		if (!ok)
			return;
		++c.draws_since_submit;
		if (c.stats_on)
			++c.stats.dispatches;
		c.list->Dispatch(x, y, z);
		StageBinding& b = c.stages[StageCS];
		for (UINT i = 0; i < kMaxUAV; ++i)
			if (b.uav[i] && b.uav[i]->m_res)
				b.uav[i]->m_res->m_written_uav = true;
	});
}

void Context::ClearRenderTargetView(RenderTargetView* rtv, const FLOAT color[4])
{
	if (!rtv)
		return;
	struct Color
	{
		FLOAT v[4];
	} col;
	memcpy(col.v, color, sizeof(col.v));
	m_front->Emit([rtv, col](Impl& c) {
		D3D12_CPU_DESCRIPTOR_HANDLE h;
		Resource* r = c.ResolveRTV(rtv, h);
		if (!r)
			return;
		if (rtv->m_back_buffer)
			c.RequireWhole(r, D3D12_RESOURCE_STATE_RENDER_TARGET);
		else
			c.RequireView(rtv, r, D3D12_RESOURCE_STATE_RENDER_TARGET);
		c.FlushBarriers();
		if (c.stats_on)
			++c.stats.clears;
		if (c.trace_frames)
		{
			std::string out = make_string("[dx12 trace] %u clear %s (%.3f %.3f %.3f %.3f)", c.trace_index++,
			                              rtv->m_back_buffer ? "backbuffer" : Impl::ResName(r).c_str(), col.v[0], col.v[1], col.v[2], col.v[3]);
			c.TraceLine(out);
		}
		c.list->ClearRenderTargetView(h, col.v, 0, nullptr);
		c.Record();
	});
}

void Context::ClearDepthStencilView(DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
	if (!dsv || !dsv->m_res)
		return;
	m_front->Emit([dsv, flags, depth, stencil](Impl& c) {
		c.RequireView(dsv, dsv->m_res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		c.FlushBarriers();
		c.list->ClearDepthStencilView(dsv->m_cpu, D3D12_CLEAR_FLAGS(flags), depth, stencil, 0, nullptr);
		c.Record();
	});
}

void Context::ClearUnorderedAccessViewUint(UnorderedAccessView* uav, const UINT values[4])
{
	if (!uav || !uav->m_res)
		return;
	struct Values
	{
		UINT v[4];
	} val;
	memcpy(val.v, values, sizeof(val.v));
	m_front->Emit([uav, val](Impl& c) {
		c.RequireView(uav, uav->m_res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		c.FlushBarriers();
		c.SetHeaps();
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu;
		c.AllocViews(1, cpu, gpu);
		c.d3d->CopyDescriptorsSimple(1, cpu, uav->m_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		c.list->ClearUnorderedAccessViewUint(gpu, uav->m_cpu, uav->m_res->m_resource, val.v, 0, nullptr);
		uav->m_res->m_written_uav = true;
		c.Record();
	});
}

void Context::ClearUnorderedAccessViewFloat(UnorderedAccessView* uav, const FLOAT values[4])
{
	if (!uav || !uav->m_res)
		return;
	struct Values
	{
		FLOAT v[4];
	} val;
	memcpy(val.v, values, sizeof(val.v));
	m_front->Emit([uav, val](Impl& c) {
		c.RequireView(uav, uav->m_res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		c.FlushBarriers();
		c.SetHeaps();
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu;
		c.AllocViews(1, cpu, gpu);
		c.d3d->CopyDescriptorsSimple(1, cpu, uav->m_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		c.list->ClearUnorderedAccessViewFloat(gpu, uav->m_cpu, uav->m_res->m_resource, val.v, 0, nullptr);
		uav->m_res->m_written_uav = true;
		c.Record();
	});
}

//-------------------------------------------------------------------------------------------------------------------
// Copies. Data from CPU memory (staging textures, shadowed buffers, UpdateSubresource) goes into upload memory at the
// call, as D3D11 takes it at the call

void Context::CopyResource(Resource* dst, Resource* src)
{
	if (!dst || !src)
		return;
	Front& f = *m_front;
	if (dst->IsBuffer())
	{
		// Buffers: from a GPU buffer, a dynamic buffer's shadow or staging memory
		const UINT size = _min(dst->m_width, src->m_width);
		if (dst->m_readback)
		{
			dst->m_readback_chunk = f.Tag();
			f.Emit([dst, src, size](Impl& c) {
				NoteCopy(c, dst, src);
				if (src->m_resource && src->m_usage != D3D11_USAGE_DYNAMIC)
				{
					c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
					c.FlushBarriers();
					c.list->CopyBufferRegion(dst->m_readback, 0, src->m_resource, 0, size);
					dst->m_readback_fence = c.dev->pending_fence.load();
					c.Record();
				}
			});
			return;
		}
		const void* cpu = nullptr;
		if (src->IsBuffer() && static_cast<Buffer*>(src)->m_shadowed)
			cpu = static_cast<Buffer*>(src)->m_shadow.data();
		else if (!src->m_cpu_data.empty())
			cpu = src->m_cpu_data.data();
		UploadSlice slice = {};
		if (cpu)
		{
			if (!f.upload.Alloc(size, 256, slice))
				return;
			memcpy(slice.cpu, cpu, size);
		}
		f.Emit([dst, src, size, slice](Impl& c) {
			NoteCopy(c, dst, src);
			if (!dst->m_resource || dst->m_usage == D3D11_USAGE_DYNAMIC)
			{
				DX12_WARN_ONCE("! D3D12: CopyResource into a dynamic or staging buffer is not supported");
				return;
			}
			ID3D12Resource* from = slice.buffer;
			u64 offset = slice.offset;
			if (!from)
			{
				if (!src->m_resource)
					return;
				c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
				from = src->m_resource;
				offset = 0;
			}
			c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
			c.FlushBarriers();
			c.list->CopyBufferRegion(dst->m_resource, 0, from, offset, size);
			c.Record();
		});
		return;
	}

	if (dst->m_readback)
	{
		// GPU texture into a STAGING (CPU read) texture: every subresource into its footprint
		dst->m_readback_chunk = f.Tag();
		f.Emit([dst, src](Impl& c) {
			NoteCopy(c, dst, src);
			Resource* from_res = c.Res(src);
			if (!from_res || !from_res->m_resource)
				return;
			c.RequireWhole(from_res, D3D12_RESOURCE_STATE_COPY_SOURCE);
			c.FlushBarriers();
			const UINT count = _min(UINT(dst->m_footprints.size()), from_res->m_subresources);
			for (UINT s = 0; s < count; ++s)
			{
				D3D12_TEXTURE_COPY_LOCATION d = {};
				d.pResource = dst->m_readback;
				d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				d.PlacedFootprint = dst->m_footprints[s];
				D3D12_TEXTURE_COPY_LOCATION from = {};
				from.pResource = from_res->m_resource;
				from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				from.SubresourceIndex = s;
				c.list->CopyTextureRegion(&d, 0, 0, 0, &from, nullptr);
			}
			dst->m_readback_fence = c.dev->pending_fence.load();
			c.Record();
		});
		return;
	}

	if (!src->m_cpu_data.empty())
	{
		// From a loader thread (parallel texture loads): the texture is new to the renderer, its data goes with the
		// initial uploads, ahead of the next submission
		if (GetCurrentThreadId() != f.caller_thread && f.dev->UploadStaging(dst, src))
			return;
		// STAGING (CPU write) texture into a GPU texture: through upload memory, subresource by subresource (each
		// copy queued before the next is staged: a recycling of upload memory in between finds it submitted)
		const UINT count = _min(UINT(src->m_layouts.size()), dst->m_subresources);
		for (UINT s = 0; s < count; ++s)
		{
			D3D12_TEXTURE_COPY_LOCATION from = {};
			if (!f.StageSubresource(src, s, from))
				return;
			f.Emit([dst, src, s, from](Impl& c) {
				if (s == 0)
					NoteCopy(c, dst, src);
				if (!dst->m_resource)
					return;
				c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
				c.FlushBarriers();
				D3D12_TEXTURE_COPY_LOCATION d = {};
				d.pResource = dst->m_resource;
				d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				d.SubresourceIndex = s;
				c.list->CopyTextureRegion(&d, 0, 0, 0, &from, nullptr);
				c.Record();
			});
		}
		return;
	}

	f.Emit([dst, src](Impl& c) {
		NoteCopy(c, dst, src);
		Resource* to = c.Res(dst);
		Resource* from = c.Res(src);
		if (!to || !from || !to->m_resource || !from->m_resource)
			return;
		c.RequireWhole(from, D3D12_RESOURCE_STATE_COPY_SOURCE);
		c.RequireWhole(to, D3D12_RESOURCE_STATE_COPY_DEST);
		c.FlushBarriers();
		c.list->CopyResource(to->m_resource, from->m_resource);
		c.Record();
	});
}

void Context::CopySubresourceRegion(Resource* dst, UINT dst_sub, UINT x, UINT y, UINT z, Resource* src, UINT src_sub, const D3D11_BOX* box)
{
	if (!dst || !src)
		return;
	Front& f = *m_front;
	const bool has_box = box != nullptr;
	const D3D11_BOX bx = box ? *box : D3D11_BOX{};
	if (dst->IsBuffer())
	{
		f.Emit([dst, dst_sub, x, src, has_box, bx](Impl& c) {
			if (!dst->m_resource || !src->m_resource)
				return;
			const UINT from = has_box ? bx.left : 0;
			const UINT size = has_box ? bx.right - bx.left : src->m_width;
			c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
			c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
			c.FlushBarriers();
			c.list->CopyBufferRegion(dst->m_resource, x, src->m_resource, from, size);
			c.Record();
		});
		return;
	}
	D3D12_TEXTURE_COPY_LOCATION staged = {};
	const bool cpu = !src->m_cpu_data.empty();
	if (cpu && !f.StageSubresource(src, src_sub, staged))
		return;
	if (dst->m_readback)
		dst->m_readback_chunk = f.Tag();
	f.Emit([dst, dst_sub, x, y, z, src, src_sub, has_box, bx, staged, cpu](Impl& c) {
		D3D12_TEXTURE_COPY_LOCATION s = staged;
		if (!cpu)
		{
			Resource* from = c.Res(src);
			if (!from || !from->m_resource)
				return;
			c.Require(from, src_sub % from->m_mips, 1, (src_sub / from->m_mips) % from->ArraySize(), 1,
			          src_sub / from->SubresourcesPerPlane(), 1, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
			s.pResource = from->m_resource;
			s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			s.SubresourceIndex = src_sub;
		}
		Resource* to = c.Res(dst);
		D3D12_TEXTURE_COPY_LOCATION d = {};
		if (to->m_readback)
		{
			d.pResource = to->m_readback;
			d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			d.PlacedFootprint = to->m_footprints[dst_sub];
			to->m_readback_fence = c.dev->pending_fence.load();
		}
		else if (to->m_resource)
		{
			c.Require(to, dst_sub % to->m_mips, 1, (dst_sub / to->m_mips) % to->ArraySize(), 1,
			          dst_sub / to->SubresourcesPerPlane(), 1, false, D3D12_RESOURCE_STATE_COPY_DEST);
			d.pResource = to->m_resource;
			d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			d.SubresourceIndex = dst_sub;
		}
		else
			return;
		c.FlushBarriers();
		c.list->CopyTextureRegion(&d, x, y, z, &s, has_box ? reinterpret_cast<const D3D12_BOX*>(&bx) : nullptr);
		c.Record();
	});
}

void Context::UpdateSubresource(Resource* dst, UINT dst_sub, const D3D11_BOX* box, const void* data, UINT row_pitch, UINT depth_pitch)
{
	if (!dst || !dst->m_resource || !data)
		return;
	Front& f = *m_front;
	if (dst->IsBuffer())
	{
		const UINT from = box ? box->left : 0;
		const UINT size = box ? box->right - box->left : dst->m_width;
		UploadSlice slice;
		if (!f.upload.Alloc(size, 256, slice))
			return;
		memcpy(slice.cpu, data, size);
		f.Emit([dst, from, size, slice](Impl& c) {
			c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
			c.FlushBarriers();
			c.list->CopyBufferRegion(dst->m_resource, from, slice.buffer, slice.offset, size);
			c.Record();
		});
		return;
	}
	UINT rows = 0;
	UINT64 row_bytes = 0, total = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = Footprint(f.d3d, dst, dst_sub, &rows, &row_bytes, &total);
	if (box)
	{
		// Only the box: a footprint of its size
		const UINT bw = box->right - box->left, bh = box->bottom - box->top, bd = box->back - box->front;
		const UINT scale = fp.Footprint.Width ? UINT(row_bytes / fp.Footprint.Width) : 4;
		fp.Footprint.Width = bw;
		fp.Footprint.Height = bh;
		fp.Footprint.Depth = _max(bd, 1u);
		row_bytes = u64(bw) * scale;
		fp.Footprint.RowPitch = UINT((row_bytes + 255) & ~255ull);
		rows = bh;
		total = u64(fp.Footprint.RowPitch) * rows * fp.Footprint.Depth;
	}
	UploadSlice slice;
	if (!f.upload.Alloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, slice))
		return;
	for (UINT zz = 0; zz < fp.Footprint.Depth; ++zz)
		for (UINT r = 0; r < rows; ++r)
			memcpy(slice.cpu + u64(zz) * fp.Footprint.RowPitch * rows + u64(r) * fp.Footprint.RowPitch,
			       static_cast<const u8*>(data) + size_t(zz) * depth_pitch + size_t(r) * row_pitch, size_t(row_bytes));
	fp.Offset = slice.offset;
	D3D12_TEXTURE_COPY_LOCATION s = {};
	s.pResource = slice.buffer;
	s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	s.PlacedFootprint = fp;
	const UINT dx = box ? box->left : 0, dy = box ? box->top : 0, dz = box ? box->front : 0;
	f.Emit([dst, dst_sub, s, dx, dy, dz](Impl& c) {
		D3D12_TEXTURE_COPY_LOCATION d = {};
		d.pResource = dst->m_resource;
		d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		d.SubresourceIndex = dst_sub;
		c.Require(dst, dst_sub % dst->m_mips, 1, (dst_sub / dst->m_mips) % dst->ArraySize(), 1, 0, 1, false,
		          D3D12_RESOURCE_STATE_COPY_DEST);
		c.FlushBarriers();
		c.list->CopyTextureRegion(&d, dx, dy, dz, &s, nullptr);
		c.Record();
	});
}

void Context::ResolveSubresource(Resource* dst, UINT dst_sub, Resource* src, UINT src_sub, DXGI_FORMAT format)
{
	if (!dst || !src)
		return;
	m_front->Emit([dst, dst_sub, src, src_sub, format](Impl& c) {
		Resource* to = c.Res(dst);
		Resource* from = c.Res(src);
		if (!to || !from || !to->m_resource || !from->m_resource)
			return;
		c.Require(from, src_sub % from->m_mips, 1, (src_sub / from->m_mips) % from->ArraySize(), 1, 0, 1, false,
		          D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		c.Require(to, dst_sub % to->m_mips, 1, (dst_sub / to->m_mips) % to->ArraySize(), 1, 0, 1, false,
		          D3D12_RESOURCE_STATE_RESOLVE_DEST);
		c.FlushBarriers();
		c.list->ResolveSubresource(to->m_resource, dst_sub, from->m_resource, src_sub, format);
		c.Record();
	});
}

//-------------------------------------------------------------------------------------------------------------------
// Map

HRESULT Context::Map(Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
	if (!res || !mapped)
		return E_INVALIDARG;
	Front& f = *m_front;
	if (res->m_usage == D3D11_USAGE_DYNAMIC)
	{
		if (res->IsBuffer())
		{
			Buffer* b = static_cast<Buffer*>(res);
			const UINT size = b->m_desc.ByteWidth;
			if (b->m_shadowed)
			{
				// Written in CPU memory, uploaded at Unmap (the contents stay valid as in D3D11)
				mapped->pData = b->m_shadow.data();
				mapped->RowPitch = mapped->DepthPitch = size;
				return S_OK;
			}
			if (type == D3D11_MAP_WRITE_NO_OVERWRITE && b->m_ptr && b->m_ptr_frame == f.upload_epoch)
			{
				mapped->pData = b->m_ptr;
				mapped->RowPitch = mapped->DepthPitch = size;
				return S_OK;
			}
			if (type == D3D11_MAP_WRITE_NO_OVERWRITE)
				DX12_WARN_ONCE("* D3D12: Map(NO_OVERWRITE) of a buffer whose upload memory was recycled: renamed (earlier contents dropped)");
			UploadSlice slice;
			if (!f.upload.Alloc(size, 256, slice))
				return E_OUTOFMEMORY;
			// The upload buffer of the initial data is no longer needed, once the commands before are recorded
			ID3D12Resource* initial = b->m_resource;
			D3D12MA::Allocation* initial_allocation = b->m_allocation;
			b->m_resource = nullptr;
			b->m_allocation = nullptr;
			b->m_ptr = slice.cpu;
			b->m_ptr_frame = f.upload_epoch;
			b->m_map_buffer = slice.buffer;
			b->m_map_footprint.Offset = slice.offset;
			const D3D12_GPU_VIRTUAL_ADDRESS va = slice.va;
			f.Emit([b, va, initial, initial_allocation](Impl& c) {
				if (initial)
					c.dev->DeferRelease(initial, initial_allocation);
				b->m_va = va;
			});
			mapped->pData = slice.cpu;
			mapped->RowPitch = mapped->DepthPitch = size;
			return S_OK;
		}
		// Dynamic texture: written into upload memory, copied at Unmap
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = Footprint(f.d3d, res, sub, &rows, &row_bytes, &total);
		UploadSlice slice;
		if (!f.upload.Alloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, slice))
			return E_OUTOFMEMORY;
		fp.Offset = slice.offset;
		res->m_map_ptr = slice.cpu;
		res->m_map_footprint = fp;
		res->m_map_buffer = slice.buffer;
		mapped->pData = slice.cpu;
		mapped->RowPitch = fp.Footprint.RowPitch;
		mapped->DepthPitch = fp.Footprint.RowPitch * rows;
		return S_OK;
	}
	if (res->m_usage == D3D11_USAGE_STAGING)
	{
		if (sub >= res->m_layouts.size())
			return E_INVALIDARG;
		const SubresourceLayout& l = res->m_layouts[sub];
		if (res->m_readback)
		{
			const bool no_wait = (flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0;
			// The copy into it is still queued for the recording thread
			if (f.threaded && !f.Done(res->m_readback_chunk))
			{
				if (no_wait)
				{
					f.RequestFlush();
					return DXGI_ERROR_WAS_STILL_DRAWING;
				}
				f.Synchronize();
			}
			const u64 fence = res->m_readback_fence;
			if (f.dev->Completed() < fence)
			{
				const bool pending = m_impl->CommandsPending(fence);
				if (no_wait)
				{
					if (pending)
						f.RequestFlush();
					return DXGI_ERROR_WAS_STILL_DRAWING;
				}
				if (pending)
					f.Synchronize();
				f.dev->WaitFence(fence);
			}
			D3D12_RANGE range = { SIZE_T(l.offset), SIZE_T(l.offset + u64(l.depth_pitch) * l.depth) };
			void* p = nullptr;
			if (FAILED(res->m_readback->Map(0, &range, &p)))
				return E_FAIL;
			res->m_readback_ptr = static_cast<u8*>(p);
			mapped->pData = static_cast<u8*>(p) + l.offset;
		}
		else
			mapped->pData = res->m_cpu_data.data() + l.offset;
		mapped->RowPitch = l.row_pitch;
		mapped->DepthPitch = l.depth_pitch;
		return S_OK;
	}
	DX12_WARN_ONCE("! D3D12: Map of a resource that is neither dynamic nor staging (usage %d)", int(res->m_usage));
	return E_INVALIDARG;
}

void Context::Unmap(Resource* res, UINT sub)
{
	if (!res)
		return;
	Front& f = *m_front;
	if (res->m_usage == D3D11_USAGE_DYNAMIC && res->IsBuffer())
	{
		Buffer* b = static_cast<Buffer*>(res);
		if (b->m_shadowed)
			f.UploadShadow(b);
		return;
	}
	if (res->m_usage == D3D11_USAGE_DYNAMIC && !res->IsBuffer() && res->m_map_ptr)
	{
		const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = res->m_map_footprint;
		ID3D12Resource* buffer = res->m_map_buffer;
		res->m_map_ptr = nullptr;
		f.Emit([res, sub, fp, buffer](Impl& c) {
			c.Require(res, sub % res->m_mips, 1, (sub / res->m_mips) % res->ArraySize(), 1, 0, 1, false,
			          D3D12_RESOURCE_STATE_COPY_DEST);
			c.FlushBarriers();
			D3D12_TEXTURE_COPY_LOCATION d = {};
			d.pResource = res->m_resource;
			d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			d.SubresourceIndex = sub;
			D3D12_TEXTURE_COPY_LOCATION s = {};
			s.pResource = buffer;
			s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			s.PlacedFootprint = fp;
			c.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
			c.Record();
		});
		return;
	}
	if (res->m_readback && res->m_readback_ptr)
	{
		D3D12_RANGE none = { 0, 0 };
		res->m_readback->Unmap(0, &none);
		res->m_readback_ptr = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------------------------
// Queries

void Context::Begin(Query* q)
{
	if (!q)
		return;
	q->m_ended = false;
	if (q->m_desc.Query == D3D11_QUERY_OCCLUSION || q->m_desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE)
		m_front->Emit([q](Impl& c) {
			c.list->BeginQuery(c.dev->occlusion_heap,
			                   q->m_desc.Query == D3D11_QUERY_OCCLUSION ? D3D12_QUERY_TYPE_OCCLUSION : D3D12_QUERY_TYPE_BINARY_OCCLUSION,
			                   q->m_index);
			c.Record();
		});
}

void Context::End(Query* q)
{
	if (!q)
		return;
	Front& f = *m_front;
	q->m_end_chunk = f.Tag();
	q->m_ended = true;
	f.Emit([q](Impl& c) {
		switch (q->m_desc.Query)
		{
		case D3D11_QUERY_OCCLUSION:
		case D3D11_QUERY_OCCLUSION_PREDICATE:
		{
			const D3D12_QUERY_TYPE type = q->m_desc.Query == D3D11_QUERY_OCCLUSION ? D3D12_QUERY_TYPE_OCCLUSION : D3D12_QUERY_TYPE_BINARY_OCCLUSION;
			c.list->EndQuery(c.dev->occlusion_heap, type, q->m_index);
			c.list->ResolveQueryData(c.dev->occlusion_heap, type, q->m_index, 1, c.dev->occlusion_readback, u64(q->m_index) * 8);
			c.Record();
			break;
		}
		case D3D11_QUERY_TIMESTAMP:
			c.list->EndQuery(c.dev->timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, q->m_index);
			c.list->ResolveQueryData(c.dev->timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, q->m_index, 1, c.dev->timestamp_readback,
			                         u64(q->m_index) * 8);
			c.Record();
			break;
		default:
			break;
		}
		q->m_fence = c.dev->pending_fence.load();
	});
}

HRESULT Context::GetData(Query* q, void* data, UINT size, UINT flags)
{
	if (!q)
		return E_INVALIDARG;
	Front& f = *m_front;
	if (!q->m_ended)
		return S_FALSE;
	const bool flush = !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH);
	// The End is still queued for the recording thread
	if (f.threaded && !f.Done(q->m_end_chunk))
	{
		if (flush)
			f.RequestFlush();
		return S_FALSE;
	}
	if (f.dev->Completed() < q->m_fence)
	{
		if (flush && m_impl->CommandsPending(q->m_fence))
			f.RequestFlush();
		return S_FALSE;
	}
	if (!data)
		return S_OK;
	Device::Impl* dev = f.dev;
	auto read = [&](ID3D12Resource* buffer, UINT index) -> u64 {
		D3D12_RANGE range = { SIZE_T(index) * 8, SIZE_T(index) * 8 + 8 };
		void* p = nullptr;
		u64 value = 0;
		if (SUCCEEDED(buffer->Map(0, &range, &p)))
		{
			value = static_cast<const u64*>(p)[index];
			D3D12_RANGE none = { 0, 0 };
			buffer->Unmap(0, &none);
		}
		return value;
	};
	switch (q->m_desc.Query)
	{
	case D3D11_QUERY_OCCLUSION:
		if (size >= sizeof(UINT64))
			*static_cast<UINT64*>(data) = read(dev->occlusion_readback, q->m_index);
		break;
	case D3D11_QUERY_OCCLUSION_PREDICATE:
		if (size >= sizeof(BOOL))
			*static_cast<BOOL*>(data) = read(dev->occlusion_readback, q->m_index) ? TRUE : FALSE;
		break;
	case D3D11_QUERY_TIMESTAMP:
		if (size >= sizeof(UINT64))
			*static_cast<UINT64*>(data) = read(dev->timestamp_readback, q->m_index);
		break;
	case D3D11_QUERY_TIMESTAMP_DISJOINT:
		if (size >= sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT))
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT* d = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(data);
			d->Frequency = dev->timestamp_frequency;
			d->Disjoint = FALSE;
		}
		break;
	default:
		if (size >= sizeof(BOOL))
			*static_cast<BOOL*>(data) = TRUE;
		break;
	}
	return S_OK;
}

//-------------------------------------------------------------------------------------------------------------------
// State, submission

void Context::ClearState()
{
	Front& f = *m_front;
	for (StageState& st : f.stages)
	{
		Rebind(st.shader, (Shader*)nullptr);
		for (Buffer*& p : st.cb)
			Rebind(p, (Buffer*)nullptr);
		for (ShaderResourceView*& p : st.srv)
			Rebind(p, (ShaderResourceView*)nullptr);
		for (SamplerState*& p : st.sampler)
			Rebind(p, (SamplerState*)nullptr);
		for (UnorderedAccessView*& p : st.uav)
			Rebind(p, (UnorderedAccessView*)nullptr);
	}
	Rebind(f.layout, (InputLayout*)nullptr);
	for (Buffer*& p : f.vb)
		Rebind(p, (Buffer*)nullptr);
	memset(f.vb_stride, 0, sizeof(f.vb_stride));
	memset(f.vb_offset, 0, sizeof(f.vb_offset));
	Rebind(f.ib, (Buffer*)nullptr);
	f.ib_format = DXGI_FORMAT_R16_UINT;
	f.ib_offset = 0;
	Rebind(f.raster, (RasterizerState*)nullptr);
	Rebind(f.blend, (BlendState*)nullptr);
	Rebind(f.depth, (DepthStencilState*)nullptr);
	for (RenderTargetView*& p : f.rtv)
		Rebind(p, (RenderTargetView*)nullptr);
	Rebind(f.dsv, (DepthStencilView*)nullptr);
	f.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	f.viewport_count = 0;
	f.scissor_count = 0;
	f.scissors_set = false;
	f.stencil_ref = 0;
	f.sample_mask = 0xFFFFFFFF;
	const FLOAT ones[4] = { 1.f, 1.f, 1.f, 1.f };
	memcpy(f.blend_factor, ones, sizeof(ones));

	f.Emit([](Impl& c) {
		for (StageBinding& b : c.stages)
		{
			b.shader = nullptr;
			memset(b.cb, 0, sizeof(b.cb));
			memset(b.srv, 0, sizeof(b.srv));
			memset(b.sampler, 0, sizeof(b.sampler));
			memset(b.uav, 0, sizeof(b.uav));
			b.hazard = 0;
			b.srv_dirty = b.sampler_dirty = b.uav_dirty = true;
		}
		c.layout = nullptr;
		memset(c.vb, 0, sizeof(c.vb));
		memset(c.vb_stride, 0, sizeof(c.vb_stride));
		memset(c.vb_offset, 0, sizeof(c.vb_offset));
		c.vb_count = 0;
		c.ib = nullptr;
		c.ib_format = DXGI_FORMAT_R16_UINT;
		c.ib_offset = 0;
		c.raster = nullptr;
		c.blend = nullptr;
		c.depth = nullptr;
		memset(c.rtv, 0, sizeof(c.rtv));
		c.dsv = nullptr;
		c.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		c.viewport_count = 0;
		c.scissor_count = 0;
		c.stencil_ref = 0;
		c.sample_mask = 0xFFFFFFFF;
		const FLOAT one[4] = { 1.f, 1.f, 1.f, 1.f };
		memcpy(c.blend_factor, one, sizeof(one));
		c.shaders_changed = c.cs_changed = true;
		c.pso_dirty = true;
		c.gfx_pso = nullptr;
		c.cs_pso = nullptr;
		c.rt_dirty = c.vp_dirty = c.sc_dirty = c.topo_dirty = c.ref_dirty = c.factor_dirty = true;
	});
}

void Context::Flush() { m_front->Flush(); }

INT Context::BeginEvent(LPCWSTR name)
{
	Front& f = *m_front;
	std::wstring e = name ? name : L"";
	f.Emit([e](Impl& c) {
		c.events.push_back(e);
		c.list->BeginEvent(0, e.c_str(), UINT((e.size() + 1) * sizeof(wchar_t)));
	});
	return f.event_depth++;
}

INT Context::EndEvent()
{
	Front& f = *m_front;
	if (!f.event_depth)
		return -1;
	--f.event_depth;
	f.Emit([](Impl& c) {
		if (c.events.empty())
			return;
		c.list->EndEvent();
		c.events.pop_back();
	});
	return f.event_depth;
}

void Context::SetMarker(LPCWSTR name)
{
	if (!name)
		return;
	std::wstring e = name;
	m_front->Emit([e](Impl& c) { c.list->SetMarker(0, e.c_str(), UINT((e.size() + 1) * sizeof(wchar_t))); });
}

HRESULT Context::Present(IDXGISwapChain1* swap_chain, UINT interval, UINT flags)
{
	Front& f = *m_front;
	const u64 frame = f.frame;
	f.Emit([swap_chain, interval, flags, frame](Impl& c) {
		if (Texture2D* bb = c.device->CurrentBackBuffer())
			c.RequireWhole(bb, D3D12_RESOURCE_STATE_PRESENT);
		const u64 fence = c.Submit(false);
		const HRESULT hr = swap_chain ? swap_chain->Present(interval, flags) : S_OK;
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
			c.device->ReportRemoval("Present");
		c.present_result.store(hr);
		c.NextFrame(fence, frame);
	});
	f.FrameDone();
	// Threaded: the result of the last Present done
	return m_impl->present_result.load();
}

void Context::EndFrame()
{
	Front& f = *m_front;
	const u64 frame = f.frame;
	f.Emit([frame](Impl& c) {
		const u64 fence = c.Submit(false);
		c.NextFrame(fence, frame);
	});
	f.FrameDone();
}

void Context::TraceFrames(UINT frames)
{
	m_front->Emit([frames](Impl& c) { c.trace_requested = frames ? frames : 1; });
}

void Context::StartStats(float seconds)
{
	m_front->Emit([seconds](Impl& c) {
		c.stats = ContextStats();
		c.stats_event_barriers.clear();
		c.stats_end_ms = GetTickCount64() + u64(seconds * 1000.f);
		c.stats_on = true;
		Msg("* [stat_dx12] measuring for %.0f s", seconds);
	});
}

void Context::ExecuteNative(std::function<void(NativeRecorder&)> fn)
{
	Front& f = *m_front;
	f.BeforeWork();
	f.Emit([fn](Impl& c) {
		fn(c);
		// The native code set its own pipeline, root signature, heaps and targets
		c.InvalidateApplied();
	});
}

ID3D12DescriptorHeap* Context::ShaderVisibleHeap() const { return m_impl->view_heap; }

void Context::SignalAfterSubmit(ID3D12Fence* fence, u64 value)
{
	if (!fence)
		return;
	fence->AddRef();
	m_front->Emit([fence, value](Impl& c) { c.signals.push_back({ fence, value }); });
}

bool Context::AllocStaticDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
	// The static range is not touched by recording
	Impl& c = *m_impl;
	UINT index;
	if (!c.static_free.empty())
	{
		index = c.static_free.back();
		c.static_free.pop_back();
	}
	else if (c.static_next < Impl::kStaticViews)
		index = c.static_next++;
	else
		return false;
	cpu->ptr = c.view_cpu0.ptr + SIZE_T(index) * c.view_inc;
	gpu->ptr = c.view_gpu0.ptr + u64(index) * c.view_inc;
	return true;
}

void Context::FreeStaticDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
	Impl& c = *m_impl;
	if (!cpu.ptr || cpu.ptr < c.view_cpu0.ptr)
		return;
	c.static_free.push_back(UINT((cpu.ptr - c.view_cpu0.ptr) / c.view_inc));
}
} // namespace dx12
