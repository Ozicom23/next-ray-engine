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
#include "stdafx.h"
#pragma hdrstop

#include "dx12Internal.h"

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
};

struct Context::Impl
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
	u64 upload_epoch = 1; // changes when upload memory is recycled: dynamic buffers mapped before get renamed
	bool recorded = false;
	std::vector<std::wstring> events; // open PIX events, opened again after a submission in the middle of a frame

	UploadRing upload;

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
		const bool ok = PrepareDraw();
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
			self->Flush();
			device->WaitIdle();
			for (UINT& n : segment_next)
				n = 0;
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
				self->Flush();
				device->WaitIdle();
				sampler_tables.clear();
				sampler_next = 0;
				for (StageBinding& s : stages)
					s.sampler_valid = false;
				ring_wrapped = true; // the list was submitted: the draw is prepared again
			}
			index = sampler_next;
			sampler_next += count;
			for (UINT i = 0; i < count; ++i)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE dst = { sampler_cpu0.ptr + SIZE_T(index + i) * sampler_inc };
				const D3D12_CPU_DESCRIPTOR_HANDLE src = b.sampler[i] ? b.sampler[i]->m_cpu : dev->default_sampler;
				d3d->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
			}
			sampler_tables[key] = index;
		}
		return D3D12_GPU_DESCRIPTOR_HANDLE{ sampler_gpu0.ptr + u64(index) * sampler_inc };
	}

	// A shadowed dynamic buffer's data into this frame's upload memory
	void UploadShadow(Buffer* b)
	{
		UploadSlice slice;
		const u64 size = (u64(b->m_desc.ByteWidth) + 255) & ~255ull;
		if (!upload.Alloc(size, 256, slice))
			return;
		memcpy(slice.cpu, b->m_shadow.data(), b->m_shadow.size());
		b->m_va = slice.va;
		b->m_ptr = slice.cpu;
		b->m_ptr_frame = upload_epoch;
		b->m_map_buffer = slice.buffer;
		b->m_map_footprint.Offset = slice.offset;
	}

	D3D12_GPU_VIRTUAL_ADDRESS BufferAddress(Buffer* b)
	{
		if (!b)
			return zero_buffer->m_va;
		if (b->m_usage == D3D11_USAGE_DYNAMIC)
		{
			if (b->m_shadowed)
			{
				if (b->m_ptr_frame != upload_epoch)
					UploadShadow(b); // unchanged since an earlier frame, its upload memory may be reused
			}
			else if (b->m_ptr && b->m_ptr_frame != upload_epoch && !b->m_warned_stale)
			{
				b->m_warned_stale = true;
				Msg("! D3D12: a large dynamic buffer (%u bytes) is used in a later frame than it was written", b->m_desc.ByteWidth);
			}
		}
		if (!b->m_va)
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
			D3D12_CPU_DESCRIPTOR_HANDLE cpu;
			D3D12_GPU_DESCRIPTOR_HANDLE gpu;
			AllocViews(sp.srv_count, cpu, gpu);
			const ShaderBindings* bindings = b.shader ? &b.shader->m_bindings : nullptr;
			for (UINT i = 0; i < sp.srv_count; ++i)
			{
				const ShaderResourceView* v = b.srv[i];
				D3D12_CPU_DESCRIPTOR_HANDLE src;
				if (v && !(b.hazard & (1u << i)))
					src = v->m_cpu;
				else
				{
					const UINT dim = bindings ? bindings->srv_dim[i] : 0;
					src = dev->null_srv[dim <= 10 ? dim : 0];
				}
				D3D12_CPU_DESCRIPTOR_HANDLE dst = { cpu.ptr + SIZE_T(i) * view_inc };
				d3d->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			if (compute)
				list->SetComputeRootDescriptorTable(sp.srv_param, gpu);
			else
				list->SetGraphicsRootDescriptorTable(sp.srv_param, gpu);
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
			k.shader[i] = shaders[i] ? shaders[i]->Id() : 0;
		k.layout = layout ? layout->Id() : 0;
		BlendState* bs = blend ? blend : default_blend;
		RasterizerState* rs = raster ? raster : default_raster;
		DepthStencilState* ds = depth ? depth : default_depth;
		k.blend = bs->Id();
		k.raster = rs->Id();
		k.depth = ds->Id();
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
		std::lock_guard<std::mutex> lock(dev->pipeline_lock);
		auto ins = dev->graphics_pipelines.emplace(k, pso);
		if (!ins.second)
		{
			if (pso)
				pso->Release();
			return ins.first->second;
		}
		++dev->pipelines_created;
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
				memcpy(rtv_applied, handles, sizeof(handles));
				rt_dirty = false;
			}
		}
		if (vp_dirty)
		{
			if (viewport_count)
				list->RSSetViewports(viewport_count, reinterpret_cast<const D3D12_VIEWPORT*>(viewports));
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
				const bool was_uav = r->m_states.empty() && r->m_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				RequireView(u, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				if (was_uav && r->m_written_uav)
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
				const u64 pkey = cs_root->id * 0x9E3779B97F4A7C15ull ^ b.shader->Id();
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
		if (reopen)
		{
			list->Reset(allocator[frame_index], nullptr);
			InvalidateApplied();
			for (const std::wstring& e : events)
				list->BeginEvent(0, e.c_str(), UINT((e.size() + 1) * sizeof(wchar_t)));
		}
		return value;
	}

	void NextFrame(u64 fence)
	{
		frame_fence[frame_index] = fence;
		upload.Retire(fence);
		frame_index = (frame_index + 1) % kFrames;
		dev->WaitFence(frame_fence[frame_index]);
		allocator[frame_index]->Reset();
		list->Reset(allocator[frame_index], nullptr);
		const u64 completed = dev->Completed();
		upload.Reclaim(completed);
		segment_next[frame_index] = 0;
		dev->ProcessDeferred(completed, false);
		++frame_number;
		++upload_epoch;
		InvalidateApplied();
		for (const std::wstring& e : events)
			list->BeginEvent(0, e.c_str(), UINT((e.size() + 1) * sizeof(wchar_t)));
		if (trace_frames && !--trace_frames)
			Msg("[dx12 trace] end");
		if (trace_requested && !trace_frames)
		{
			trace_frames = trace_requested;
			trace_requested = 0;
			trace_index = 0;
			Msg("[dx12 trace] frame %llu, %u frame(s)", frame_number, trace_frames);
		}
	}

	bool CommandsPending(u64 fence) const { return fence >= dev->pending_fence.load(); }
};

//===================================================================================================================

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
	c.upload.Init(device->m_allocator);

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
}

Context::~Context()
{
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
	c.upload.Destroy();
	c.list->Release();
	for (ID3D12CommandAllocator* a : c.allocator)
		if (a)
			a->Release();
	if (c.view_heap)
		c.view_heap->Release();
	if (c.sampler_heap)
		c.sampler_heap->Release();
	delete m_impl;
}

HRESULT Context::QueryInterface(REFIID, void** out)
{
	if (!out)
		return E_POINTER;
	*out = nullptr;
	return E_NOINTERFACE;
}

//-------------------------------------------------------------------------------------------------------------------
// Binding

void Context::SetShader(Stage stage, Shader* shader)
{
	Impl& c = *m_impl;
	StageBinding& b = c.stages[stage];
	if (b.shader == shader)
		return;
	const u64 old_key = ShaderBindingKey(b.shader);
	Rebind(b.shader, shader);
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
		b.srv_dirty = true; // the null views' dimensions follow the shader
	}
}

void Context::SetSRVs(Stage stage, UINT start, UINT count, ShaderResourceView* const* views)
{
	StageBinding& b = m_impl->stages[stage];
	for (UINT i = 0; i < count && start + i < kMaxSRV; ++i)
	{
		ShaderResourceView* v = views ? views[i] : nullptr;
		if (b.srv[start + i] != v)
		{
			Rebind(b.srv[start + i], v);
			b.srv_dirty = true;
		}
	}
}

void Context::SetSamplers(Stage stage, UINT start, UINT count, SamplerState* const* samplers)
{
	StageBinding& b = m_impl->stages[stage];
	for (UINT i = 0; i < count && start + i < kMaxSamplers; ++i)
	{
		SamplerState* s = samplers ? samplers[i] : nullptr;
		if (b.sampler[start + i] != s)
		{
			Rebind(b.sampler[start + i], s);
			b.sampler_dirty = true;
		}
	}
}

void Context::SetCBs(Stage stage, UINT start, UINT count, Buffer* const* buffers)
{
	StageBinding& b = m_impl->stages[stage];
	for (UINT i = 0; i < count && start + i < kMaxCB; ++i)
		Rebind(b.cb[start + i], buffers ? buffers[i] : nullptr);
}

void Context::CSGetShader(ComputeShader** shader, void** instances, UINT* count)
{
	Shader* s = m_impl->stages[StageCS].shader;
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
		ShaderResourceView* v = start + i < kMaxSRV ? m_impl->stages[StageCS].srv[start + i] : nullptr;
		if (v)
			v->AddRef();
		views[i] = v;
	}
}

void Context::PSGetShaderResources(UINT start, UINT count, ShaderResourceView** views)
{
	for (UINT i = 0; i < count; ++i)
	{
		ShaderResourceView* v = start + i < kMaxSRV ? m_impl->stages[StagePS].srv[start + i] : nullptr;
		if (v)
			v->AddRef();
		views[i] = v;
	}
}

void Context::CSGetConstantBuffers(UINT start, UINT count, Buffer** buffers)
{
	for (UINT i = 0; i < count; ++i)
	{
		Buffer* b = start + i < kMaxCB ? m_impl->stages[StageCS].cb[start + i] : nullptr;
		if (b)
			b->AddRef();
		buffers[i] = b;
	}
}

void Context::CSSetUnorderedAccessViews(UINT start, UINT count, UnorderedAccessView* const* views, const UINT*)
{
	StageBinding& b = m_impl->stages[StageCS];
	for (UINT i = 0; i < count && start + i < kMaxUAV; ++i)
	{
		UnorderedAccessView* v = views ? views[i] : nullptr;
		if (b.uav[start + i] != v)
		{
			Rebind(b.uav[start + i], v);
			b.uav_dirty = true;
		}
	}
}

void Context::CSGetUnorderedAccessViews(UINT start, UINT count, UnorderedAccessView** views)
{
	for (UINT i = 0; i < count; ++i)
	{
		UnorderedAccessView* v = start + i < kMaxUAV ? m_impl->stages[StageCS].uav[start + i] : nullptr;
		if (v)
			v->AddRef();
		views[i] = v;
	}
}

void Context::IASetInputLayout(InputLayout* layout)
{
	Impl& c = *m_impl;
	if (c.layout == layout)
		return;
	Rebind(c.layout, layout);
	c.pso_dirty = true;
}

void Context::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	Impl& c = *m_impl;
	if (c.topology == topology)
		return;
	if (Impl::TopologyType(c.topology) != Impl::TopologyType(topology))
		c.pso_dirty = true;
	c.topology = topology;
	c.topo_dirty = true;
}

void Context::IASetVertexBuffers(UINT start, UINT count, Buffer* const* buffers, const UINT* strides, const UINT* offsets)
{
	Impl& c = *m_impl;
	for (UINT i = 0; i < count && start + i < kMaxVB; ++i)
	{
		Rebind(c.vb[start + i], buffers ? buffers[i] : nullptr);
		c.vb_stride[start + i] = strides ? strides[i] : 0;
		c.vb_offset[start + i] = offsets ? offsets[i] : 0;
	}
	UINT n = 0;
	for (UINT i = 0; i < kMaxVB; ++i)
		if (c.vb[i])
			n = i + 1;
	c.vb_count = n;
}

void Context::IASetIndexBuffer(Buffer* buffer, DXGI_FORMAT format, UINT offset)
{
	Impl& c = *m_impl;
	Rebind(c.ib, buffer);
	c.ib_format = format;
	c.ib_offset = offset;
}

void Context::RSSetState(RasterizerState* state)
{
	Impl& c = *m_impl;
	if (c.raster == state)
		return;
	Rebind(c.raster, state);
	c.pso_dirty = true;
	c.sc_dirty = true;
}

void Context::RSSetViewports(UINT count, const D3D11_VIEWPORT* viewports)
{
	Impl& c = *m_impl;
	count = _min(count, UINT(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
	if (count == c.viewport_count && !memcmp(c.viewports, viewports, sizeof(D3D11_VIEWPORT) * count))
		return;
	if (count)
		memcpy(c.viewports, viewports, sizeof(D3D11_VIEWPORT) * count);
	if (count != c.viewport_count)
		c.sc_dirty = true;
	c.viewport_count = count;
	c.vp_dirty = true;
}

void Context::RSGetViewports(UINT* count, D3D11_VIEWPORT* viewports)
{
	Impl& c = *m_impl;
	if (viewports)
		memcpy(viewports, c.viewports, sizeof(D3D11_VIEWPORT) * _min(*count, c.viewport_count));
	*count = c.viewport_count;
}

void Context::RSSetScissorRects(UINT count, const D3D11_RECT* rects)
{
	Impl& c = *m_impl;
	count = _min(count, UINT(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
	if (count)
		memcpy(c.scissors, rects, sizeof(D3D11_RECT) * count);
	c.scissor_count = count;
	c.sc_dirty = true;
}

void Context::OMSetRenderTargets(UINT count, RenderTargetView* const* rtvs, DepthStencilView* dsv)
{
	Impl& c = *m_impl;
	bool formats = false;
	for (UINT i = 0; i < kMaxRT; ++i)
	{
		RenderTargetView* v = (rtvs && i < count) ? rtvs[i] : nullptr;
		if (c.rtv[i] == v)
			continue;
		if (!c.rtv[i] || !v || c.rtv[i]->m_format != v->m_format || c.rtv[i]->m_res != v->m_res)
			formats = true;
		Rebind(c.rtv[i], v);
		c.rt_dirty = true;
	}
	if (c.dsv != dsv)
	{
		if (!c.dsv || !dsv || c.dsv->m_format != dsv->m_format || c.dsv->m_read_only != dsv->m_read_only ||
		    c.dsv->m_res != dsv->m_res)
			formats = true;
		Rebind(c.dsv, dsv);
		c.rt_dirty = true;
	}
	if (formats)
		c.pso_dirty = true;
	// The views of the stages may have hazards with the new outputs
	if (c.rt_dirty)
		for (StageBinding& b : c.stages)
			if (b.hazard)
				b.srv_dirty = true;
}

void Context::OMGetRenderTargets(UINT count, RenderTargetView** rtvs, DepthStencilView** dsv)
{
	Impl& c = *m_impl;
	if (rtvs)
		for (UINT i = 0; i < count; ++i)
		{
			RenderTargetView* v = i < kMaxRT ? c.rtv[i] : nullptr;
			if (v)
				v->AddRef();
			rtvs[i] = v;
		}
	if (dsv)
	{
		if (c.dsv)
			c.dsv->AddRef();
		*dsv = c.dsv;
	}
}

void Context::OMSetBlendState(BlendState* state, const FLOAT factor[4], UINT sample_mask)
{
	Impl& c = *m_impl;
	if (c.blend != state)
	{
		Rebind(c.blend, state);
		c.pso_dirty = true;
	}
	if (c.sample_mask != sample_mask)
	{
		c.sample_mask = sample_mask;
		c.pso_dirty = true;
	}
	const FLOAT ones[4] = { 1.f, 1.f, 1.f, 1.f };
	const FLOAT* f = factor ? factor : ones;
	if (memcmp(c.blend_factor, f, sizeof(c.blend_factor)))
	{
		memcpy(c.blend_factor, f, sizeof(c.blend_factor));
		c.factor_dirty = true;
	}
}

void Context::OMSetDepthStencilState(DepthStencilState* state, UINT stencil_ref)
{
	Impl& c = *m_impl;
	if (c.depth != state)
	{
		Rebind(c.depth, state);
		c.pso_dirty = true;
	}
	if (c.stencil_ref != stencil_ref)
	{
		c.stencil_ref = stencil_ref;
		c.ref_dirty = true;
	}
}

//-------------------------------------------------------------------------------------------------------------------
// Work

void Context::Draw(UINT vertex_count, UINT start_vertex)
{
	if (m_impl->BeginDraw("draw", vertex_count))
		m_impl->list->DrawInstanced(vertex_count, 1, start_vertex, 0);
}

void Context::DrawIndexed(UINT index_count, UINT start_index, INT base_vertex)
{
	if (m_impl->BeginDraw("draw_indexed", index_count))
		m_impl->list->DrawIndexedInstanced(index_count, 1, start_index, base_vertex, 0);
}

void Context::DrawInstanced(UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance)
{
	if (m_impl->BeginDraw("draw", vertex_count))
		m_impl->list->DrawInstanced(vertex_count, instance_count, start_vertex, start_instance);
}

void Context::DrawIndexedInstanced(UINT index_count, UINT instance_count, UINT start_index, INT base_vertex, UINT start_instance)
{
	if (m_impl->BeginDraw("draw_indexed", index_count))
		m_impl->list->DrawIndexedInstanced(index_count, instance_count, start_index, base_vertex, start_instance);
}

void Context::Dispatch(UINT x, UINT y, UINT z)
{
	Impl& c = *m_impl;
	const bool ok = c.PrepareDispatch();
	if (c.trace_frames)
	{
		std::string out = make_string("[dx12 trace] %u %s %ux%ux%u", c.trace_index++, ok ? "dispatch" : "SKIPPED dispatch", x, y, z);
		c.TraceStage(out, StageCS, "CS");
		c.TraceLine(out);
	}
	if (!ok)
		return;
	c.list->Dispatch(x, y, z);
	StageBinding& b = c.stages[StageCS];
	for (UINT i = 0; i < kMaxUAV; ++i)
		if (b.uav[i] && b.uav[i]->m_res)
			b.uav[i]->m_res->m_written_uav = true;
}

void Context::ClearRenderTargetView(RenderTargetView* rtv, const FLOAT color[4])
{
	if (!rtv)
		return;
	Impl& c = *m_impl;
	D3D12_CPU_DESCRIPTOR_HANDLE h;
	Resource* r = c.ResolveRTV(rtv, h);
	if (!r)
		return;
	if (rtv->m_back_buffer)
		c.RequireWhole(r, D3D12_RESOURCE_STATE_RENDER_TARGET);
	else
		c.RequireView(rtv, r, D3D12_RESOURCE_STATE_RENDER_TARGET);
	c.FlushBarriers();
	if (c.trace_frames)
	{
		std::string out = make_string("[dx12 trace] %u clear %s (%.3f %.3f %.3f %.3f)", c.trace_index++,
		                            rtv->m_back_buffer ? "backbuffer" : Impl::ResName(r).c_str(), color[0], color[1], color[2], color[3]);
		c.TraceLine(out);
	}
	c.list->ClearRenderTargetView(h, color, 0, nullptr);
	c.Record();
}

void Context::ClearDepthStencilView(DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
	if (!dsv || !dsv->m_res)
		return;
	Impl& c = *m_impl;
	c.RequireView(dsv, dsv->m_res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	c.FlushBarriers();
	c.list->ClearDepthStencilView(dsv->m_cpu, D3D12_CLEAR_FLAGS(flags), depth, stencil, 0, nullptr);
	c.Record();
}

void Context::ClearUnorderedAccessViewUint(UnorderedAccessView* uav, const UINT values[4])
{
	if (!uav || !uav->m_res)
		return;
	Impl& c = *m_impl;
	c.RequireView(uav, uav->m_res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	c.FlushBarriers();
	c.SetHeaps();
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	c.AllocViews(1, cpu, gpu);
	c.d3d->CopyDescriptorsSimple(1, cpu, uav->m_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	c.list->ClearUnorderedAccessViewUint(gpu, uav->m_cpu, uav->m_res->m_resource, values, 0, nullptr);
	uav->m_res->m_written_uav = true;
	c.Record();
}

void Context::ClearUnorderedAccessViewFloat(UnorderedAccessView* uav, const FLOAT values[4])
{
	if (!uav || !uav->m_res)
		return;
	Impl& c = *m_impl;
	c.RequireView(uav, uav->m_res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	c.FlushBarriers();
	c.SetHeaps();
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	c.AllocViews(1, cpu, gpu);
	c.d3d->CopyDescriptorsSimple(1, cpu, uav->m_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	c.list->ClearUnorderedAccessViewFloat(gpu, uav->m_cpu, uav->m_res->m_resource, values, 0, nullptr);
	uav->m_res->m_written_uav = true;
	c.Record();
}

//-------------------------------------------------------------------------------------------------------------------
// Copies

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

// Puts one subresource of a STAGING (CPU write) texture into upload memory; returns the copy source
static bool StageSubresource(Context::Impl& c, Resource* src, UINT sub, D3D12_TEXTURE_COPY_LOCATION& loc)
{
	// The first binds after a level load upload a lot at once: past 512 MB in a frame the GPU takes what is
	// recorded and the upload memory comes back, instead of growing for the whole frame
	if (c.upload.BytesThisFrame() > (512ull << 20))
	{
		c.self->Flush();
		c.device->WaitIdle();
		c.upload.Retire(c.last_submitted);
		c.upload.Reclaim(c.dev->Completed());
		++c.upload_epoch;
	}
	UINT rows = 0;
	UINT64 row_bytes = 0, total = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = Footprint(c.d3d, src, sub, &rows, &row_bytes, &total);
	UploadSlice slice;
	if (!c.upload.Alloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, slice))
		return false;
	const SubresourceLayout& l = src->m_layouts[sub];
	for (UINT z = 0; z < fp.Footprint.Depth; ++z)
		for (UINT r = 0; r < rows; ++r)
			memcpy(slice.cpu + u64(z) * fp.Footprint.RowPitch * rows + u64(r) * fp.Footprint.RowPitch,
			       src->m_cpu_data.data() + l.offset + size_t(z) * l.depth_pitch + size_t(r) * l.row_pitch, size_t(row_bytes));
	fp.Offset = slice.offset;
	loc.pResource = slice.buffer;
	loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	loc.PlacedFootprint = fp;
	return true;
}

void Context::CopyResource(Resource* dst, Resource* src)
{
	if (!dst || !src)
		return;
	Impl& c = *m_impl;
	if (c.trace_frames)
	{
		std::string out = make_string("[dx12 trace] %u copy %s <- %s", c.trace_index++, Impl::ResName(dst).c_str(), Impl::ResName(src).c_str());
		c.TraceLine(out);
	}
	if (dst->IsBuffer())
	{
		// Buffers: from a GPU buffer, a dynamic buffer's ring slice or staging memory
		const UINT size = _min(dst->m_width, src->m_width);
		if (dst->m_readback)
		{
			if (src->m_resource && src->m_usage != D3D11_USAGE_DYNAMIC)
			{
				c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
				c.FlushBarriers();
				c.list->CopyBufferRegion(dst->m_readback, 0, src->m_resource, 0, size);
				dst->m_readback_fence = c.dev->pending_fence.load();
				c.Record();
			}
			return;
		}
		if (!dst->m_resource || dst->m_usage == D3D11_USAGE_DYNAMIC)
		{
			DX12_WARN_ONCE("! D3D12: CopyResource into a dynamic or staging buffer is not supported");
			return;
		}
		ID3D12Resource* from = nullptr;
		u64 offset = 0;
		if (src->IsBuffer() && static_cast<Buffer*>(src)->m_shadowed)
		{
			UploadSlice slice;
			if (!c.upload.Alloc(size, 256, slice))
				return;
			memcpy(slice.cpu, static_cast<Buffer*>(src)->m_shadow.data(), size);
			from = slice.buffer;
			offset = slice.offset;
		}
		else if (!src->m_cpu_data.empty())
		{
			UploadSlice slice;
			if (!c.upload.Alloc(size, 256, slice))
				return;
			memcpy(slice.cpu, src->m_cpu_data.data(), size);
			from = slice.buffer;
			offset = slice.offset;
		}
		else if (src->m_resource)
		{
			c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
			from = src->m_resource;
		}
		if (!from)
			return;
		c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
		c.FlushBarriers();
		c.list->CopyBufferRegion(dst->m_resource, 0, from, offset, size);
		c.Record();
		return;
	}

	if (dst->m_readback)
	{
		// GPU texture into a STAGING (CPU read) texture: every subresource into its footprint
		if (!src->m_resource)
			return;
		c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
		c.FlushBarriers();
		const UINT count = _min(UINT(dst->m_footprints.size()), src->m_subresources);
		for (UINT s = 0; s < count; ++s)
		{
			D3D12_TEXTURE_COPY_LOCATION d = {};
			d.pResource = dst->m_readback;
			d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			d.PlacedFootprint = dst->m_footprints[s];
			D3D12_TEXTURE_COPY_LOCATION from = {};
			from.pResource = src->m_resource;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			from.SubresourceIndex = s;
			c.list->CopyTextureRegion(&d, 0, 0, 0, &from, nullptr);
		}
		dst->m_readback_fence = c.dev->pending_fence.load();
		c.Record();
		return;
	}
	if (!dst->m_resource)
		return;

	if (!src->m_cpu_data.empty())
	{
		// STAGING (CPU write) texture into a GPU texture: through upload memory, subresource by subresource
		c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
		c.FlushBarriers();
		const UINT count = _min(UINT(src->m_layouts.size()), dst->m_subresources);
		for (UINT s = 0; s < count; ++s)
		{
			D3D12_TEXTURE_COPY_LOCATION from = {};
			if (!StageSubresource(c, src, s, from))
				return;
			D3D12_TEXTURE_COPY_LOCATION d = {};
			d.pResource = dst->m_resource;
			d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			d.SubresourceIndex = s;
			c.list->CopyTextureRegion(&d, 0, 0, 0, &from, nullptr);
		}
		c.Record();
		return;
	}
	if (!src->m_resource)
		return;
	c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
	c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
	c.FlushBarriers();
	c.list->CopyResource(dst->m_resource, src->m_resource);
	c.Record();
}

void Context::CopySubresourceRegion(Resource* dst, UINT dst_sub, UINT x, UINT y, UINT z, Resource* src, UINT src_sub, const D3D11_BOX* box)
{
	if (!dst || !src)
		return;
	Impl& c = *m_impl;
	if (dst->IsBuffer())
	{
		if (!dst->m_resource || !src->m_resource)
			return;
		const UINT from = box ? box->left : 0;
		const UINT size = box ? box->right - box->left : src->m_width;
		c.RequireWhole(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
		c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
		c.FlushBarriers();
		c.list->CopyBufferRegion(dst->m_resource, x, src->m_resource, from, size);
		c.Record();
		return;
	}
	D3D12_TEXTURE_COPY_LOCATION s = {};
	if (!src->m_cpu_data.empty())
	{
		if (!StageSubresource(c, src, src_sub, s))
			return;
	}
	else if (src->m_resource)
	{
		c.Require(src, src_sub % src->m_mips, 1, (src_sub / src->m_mips) % src->ArraySize(), 1,
		          src_sub / src->SubresourcesPerPlane(), 1, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
		s.pResource = src->m_resource;
		s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		s.SubresourceIndex = src_sub;
	}
	else
		return;

	D3D12_TEXTURE_COPY_LOCATION d = {};
	if (dst->m_readback)
	{
		d.pResource = dst->m_readback;
		d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		d.PlacedFootprint = dst->m_footprints[dst_sub];
		dst->m_readback_fence = c.dev->pending_fence.load();
	}
	else if (dst->m_resource)
	{
		c.Require(dst, dst_sub % dst->m_mips, 1, (dst_sub / dst->m_mips) % dst->ArraySize(), 1,
		          dst_sub / dst->SubresourcesPerPlane(), 1, false, D3D12_RESOURCE_STATE_COPY_DEST);
		d.pResource = dst->m_resource;
		d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		d.SubresourceIndex = dst_sub;
	}
	else
		return;
	c.FlushBarriers();
	c.list->CopyTextureRegion(&d, x, y, z, &s, reinterpret_cast<const D3D12_BOX*>(box));
	c.Record();
}

void Context::UpdateSubresource(Resource* dst, UINT dst_sub, const D3D11_BOX* box, const void* data, UINT row_pitch, UINT depth_pitch)
{
	if (!dst || !dst->m_resource || !data)
		return;
	Impl& c = *m_impl;
	if (dst->IsBuffer())
	{
		const UINT from = box ? box->left : 0;
		const UINT size = box ? box->right - box->left : dst->m_width;
		UploadSlice slice;
		if (!c.upload.Alloc(size, 256, slice))
			return;
		memcpy(slice.cpu, data, size);
		c.RequireWhole(dst, D3D12_RESOURCE_STATE_COPY_DEST);
		c.FlushBarriers();
		c.list->CopyBufferRegion(dst->m_resource, from, slice.buffer, slice.offset, size);
		c.Record();
		return;
	}
	UINT rows = 0;
	UINT64 row_bytes = 0, total = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = Footprint(c.d3d, dst, dst_sub, &rows, &row_bytes, &total);
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
	if (!c.upload.Alloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, slice))
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
	D3D12_TEXTURE_COPY_LOCATION d = {};
	d.pResource = dst->m_resource;
	d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	d.SubresourceIndex = dst_sub;
	c.Require(dst, dst_sub % dst->m_mips, 1, (dst_sub / dst->m_mips) % dst->ArraySize(), 1, 0, 1, false,
	          D3D12_RESOURCE_STATE_COPY_DEST);
	c.FlushBarriers();
	c.list->CopyTextureRegion(&d, box ? box->left : 0, box ? box->top : 0, box ? box->front : 0, &s, nullptr);
	c.Record();
}

void Context::ResolveSubresource(Resource* dst, UINT dst_sub, Resource* src, UINT src_sub, DXGI_FORMAT format)
{
	if (!dst || !src || !dst->m_resource || !src->m_resource)
		return;
	Impl& c = *m_impl;
	c.Require(src, src_sub % src->m_mips, 1, (src_sub / src->m_mips) % src->ArraySize(), 1, 0, 1, false,
	          D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
	c.Require(dst, dst_sub % dst->m_mips, 1, (dst_sub / dst->m_mips) % dst->ArraySize(), 1, 0, 1, false,
	          D3D12_RESOURCE_STATE_RESOLVE_DEST);
	c.FlushBarriers();
	c.list->ResolveSubresource(dst->m_resource, dst_sub, src->m_resource, src_sub, format);
	c.Record();
}

//-------------------------------------------------------------------------------------------------------------------
// Map

HRESULT Context::Map(Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
	if (!res || !mapped)
		return E_INVALIDARG;
	Impl& c = *m_impl;
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
			if (type == D3D11_MAP_WRITE_NO_OVERWRITE && b->m_ptr && b->m_ptr_frame == c.upload_epoch)
			{
				mapped->pData = b->m_ptr;
				mapped->RowPitch = mapped->DepthPitch = size;
				return S_OK;
			}
			if (type == D3D11_MAP_WRITE_NO_OVERWRITE)
				DX12_WARN_ONCE("* D3D12: Map(NO_OVERWRITE) of a buffer whose upload memory was recycled: renamed (earlier contents dropped)");
			UploadSlice slice;
			if (!c.upload.Alloc(size, 256, slice))
				return E_OUTOFMEMORY;
			// The upload buffer of the initial data is no longer needed
			if (b->m_resource)
			{
				c.dev->DeferRelease(b->m_resource, b->m_allocation);
				b->m_resource = nullptr;
				b->m_allocation = nullptr;
			}
			b->m_va = slice.va;
			b->m_ptr = slice.cpu;
			b->m_ptr_frame = c.upload_epoch;
			b->m_map_buffer = slice.buffer;
			b->m_map_footprint.Offset = slice.offset;
			mapped->pData = slice.cpu;
			mapped->RowPitch = mapped->DepthPitch = size;
			return S_OK;
		}
		// Dynamic texture: written into upload memory, copied at Unmap
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = Footprint(c.d3d, res, sub, &rows, &row_bytes, &total);
		UploadSlice slice;
		if (!c.upload.Alloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, slice))
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
			const u64 fence = res->m_readback_fence;
			if (c.dev->Completed() < fence)
			{
				if (c.CommandsPending(fence))
					Flush();
				if (flags & D3D11_MAP_FLAG_DO_NOT_WAIT)
					return DXGI_ERROR_WAS_STILL_DRAWING;
				c.dev->WaitFence(fence);
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
	Impl& c = *m_impl;
	if (res->m_usage == D3D11_USAGE_DYNAMIC && res->IsBuffer())
	{
		Buffer* b = static_cast<Buffer*>(res);
		if (b->m_shadowed)
			c.UploadShadow(b);
		return;
	}
	if (res->m_usage == D3D11_USAGE_DYNAMIC && !res->IsBuffer() && res->m_map_ptr)
	{
		c.Require(res, sub % res->m_mips, 1, (sub / res->m_mips) % res->ArraySize(), 1, 0, 1, false,
		          D3D12_RESOURCE_STATE_COPY_DEST);
		c.FlushBarriers();
		D3D12_TEXTURE_COPY_LOCATION d = {};
		d.pResource = res->m_resource;
		d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		d.SubresourceIndex = sub;
		D3D12_TEXTURE_COPY_LOCATION s = {};
		s.pResource = res->m_map_buffer;
		s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		s.PlacedFootprint = res->m_map_footprint;
		c.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
		res->m_map_ptr = nullptr;
		c.Record();
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
	Impl& c = *m_impl;
	if (q->m_desc.Query == D3D11_QUERY_OCCLUSION || q->m_desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE)
	{
		c.list->BeginQuery(c.dev->occlusion_heap,
		                   q->m_desc.Query == D3D11_QUERY_OCCLUSION ? D3D12_QUERY_TYPE_OCCLUSION : D3D12_QUERY_TYPE_BINARY_OCCLUSION,
		                   q->m_index);
		c.Record();
	}
	q->m_ended = false;
}

void Context::End(Query* q)
{
	if (!q)
		return;
	Impl& c = *m_impl;
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
	q->m_ended = true;
}

HRESULT Context::GetData(Query* q, void* data, UINT size, UINT flags)
{
	if (!q)
		return E_INVALIDARG;
	Impl& c = *m_impl;
	if (!q->m_ended)
		return S_FALSE;
	if (c.dev->Completed() < q->m_fence)
	{
		if (c.CommandsPending(q->m_fence) && !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH))
			Flush();
		return S_FALSE;
	}
	if (!data)
		return S_OK;
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
			*static_cast<UINT64*>(data) = read(c.dev->occlusion_readback, q->m_index);
		break;
	case D3D11_QUERY_OCCLUSION_PREDICATE:
		if (size >= sizeof(BOOL))
			*static_cast<BOOL*>(data) = read(c.dev->occlusion_readback, q->m_index) ? TRUE : FALSE;
		break;
	case D3D11_QUERY_TIMESTAMP:
		if (size >= sizeof(UINT64))
			*static_cast<UINT64*>(data) = read(c.dev->timestamp_readback, q->m_index);
		break;
	case D3D11_QUERY_TIMESTAMP_DISJOINT:
		if (size >= sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT))
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT* d = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(data);
			d->Frequency = c.dev->timestamp_frequency;
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
	Impl& c = *m_impl;
	for (StageBinding& b : c.stages)
	{
		Rebind(b.shader, (Shader*)nullptr);
		for (Buffer*& p : b.cb)
			Rebind(p, (Buffer*)nullptr);
		for (ShaderResourceView*& p : b.srv)
			Rebind(p, (ShaderResourceView*)nullptr);
		for (SamplerState*& p : b.sampler)
			Rebind(p, (SamplerState*)nullptr);
		for (UnorderedAccessView*& p : b.uav)
			Rebind(p, (UnorderedAccessView*)nullptr);
		b.hazard = 0;
		b.srv_dirty = b.sampler_dirty = b.uav_dirty = true;
	}
	Rebind(c.layout, (InputLayout*)nullptr);
	for (Buffer*& p : c.vb)
		Rebind(p, (Buffer*)nullptr);
	c.vb_count = 0;
	Rebind(c.ib, (Buffer*)nullptr);
	Rebind(c.raster, (RasterizerState*)nullptr);
	Rebind(c.blend, (BlendState*)nullptr);
	Rebind(c.depth, (DepthStencilState*)nullptr);
	for (RenderTargetView*& p : c.rtv)
		Rebind(p, (RenderTargetView*)nullptr);
	Rebind(c.dsv, (DepthStencilView*)nullptr);
	c.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	c.viewport_count = 0;
	c.scissor_count = 0;
	c.stencil_ref = 0;
	c.sample_mask = 0xFFFFFFFF;
	const FLOAT ones[4] = { 1.f, 1.f, 1.f, 1.f };
	memcpy(c.blend_factor, ones, sizeof(ones));
	c.shaders_changed = c.cs_changed = true;
	c.pso_dirty = true;
	c.gfx_pso = nullptr;
	c.cs_pso = nullptr;
	c.rt_dirty = c.vp_dirty = c.sc_dirty = c.topo_dirty = c.ref_dirty = c.factor_dirty = true;
}

void Context::Flush()
{
	Impl& c = *m_impl;
	if (!c.recorded && c.barriers.empty())
	{
		// Nothing of ours, but initial uploads may wait for a submission
		std::lock_guard<std::mutex> lock(c.dev->init_lock);
		c.dev->SubmitInitLocked();
		return;
	}
	c.Submit(true);
}

INT Context::BeginEvent(LPCWSTR name)
{
	Impl& c = *m_impl;
	c.events.emplace_back(name ? name : L"");
	const std::wstring& e = c.events.back();
	c.list->BeginEvent(0, e.c_str(), UINT((e.size() + 1) * sizeof(wchar_t)));
	return INT(c.events.size()) - 1;
}

INT Context::EndEvent()
{
	Impl& c = *m_impl;
	if (c.events.empty())
		return -1;
	c.list->EndEvent();
	c.events.pop_back();
	return INT(c.events.size());
}

void Context::SetMarker(LPCWSTR name)
{
	if (name)
		m_impl->list->SetMarker(0, name, UINT((wcslen(name) + 1) * sizeof(wchar_t)));
}

HRESULT Context::Present(IDXGISwapChain1* swap_chain, UINT interval, UINT flags)
{
	Impl& c = *m_impl;
	if (Texture2D* bb = c.device->CurrentBackBuffer())
		c.RequireWhole(bb, D3D12_RESOURCE_STATE_PRESENT);
	const u64 fence = c.Submit(false);
	HRESULT hr = swap_chain ? swap_chain->Present(interval, flags) : S_OK;
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		c.device->ReportRemoval("Present");
	c.NextFrame(fence);
	return hr;
}

void Context::EndFrame()
{
	Impl& c = *m_impl;
	const u64 fence = c.Submit(false);
	c.NextFrame(fence);
}

void Context::TraceFrames(UINT frames) { m_impl->trace_requested = frames ? frames : 1; }

ID3D12GraphicsCommandList* Context::NativeList()
{
	Impl& c = *m_impl;
	c.FlushBarriers();
	c.Record();
	return c.list;
}

void Context::Transition(Resource* res, D3D12_RESOURCE_STATES state)
{
	m_impl->RequireWhole(res, state);
}

void Context::InvalidateNative() { m_impl->InvalidateApplied(); }

ID3D12DescriptorHeap* Context::ShaderVisibleHeap() const { return m_impl->view_heap; }

void Context::SignalAfterSubmit(ID3D12Fence* fence, u64 value)
{
	if (!fence)
		return;
	fence->AddRef();
	m_impl->signals.push_back({ fence, value });
}

u64 Context::LastSubmittedFence() const { return m_impl->last_submitted; }
u64 Context::PendingFence() const { return m_impl->dev->pending_fence.load(); }

bool Context::AllocStaticDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
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
