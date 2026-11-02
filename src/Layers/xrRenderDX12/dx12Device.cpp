// D3D12 backend: device, objects, resource creation, initial uploads, deferred release (see dx12.h)
#include "stdafx.h"
#pragma hdrstop

#include "dx12Internal.h"
#include <D3DCompiler.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxguid.lib")

namespace dx12
{
static Device* g_device = nullptr;
Device* TheDevice() { return g_device; }

//===================================================================================================================
// Object

u64 Object::NewId()
{
	static std::atomic<u64> next{ 1 };
	return next++;
}

Object::~Object()
{
	if (m_private)
	{
		for (PrivateEntry& e : *m_private)
			if (e.unknown)
				e.unknown->Release();
		delete m_private;
	}
}

std::atomic<void (*)(Object*)> g_defer_delete{ nullptr };

ULONG Object::Release()
{
	const LONG refs = InterlockedDecrement(&m_refs);
	if (refs == 0)
	{
		// Commands queued for the recording thread may still use it
		if (void (*defer)(Object*) = g_defer_delete.load())
			defer(this);
		else
			delete this;
	}
	return ULONG(refs);
}

HRESULT Object::QueryInterface(REFIID riid, void** out)
{
	if (!out)
		return E_POINTER;
	*out = nullptr;
	return E_NOINTERFACE;
}

HRESULT Object::SetPrivateData(REFGUID guid, UINT size, const void* data)
{
	if (!m_private)
		m_private = new std::vector<PrivateEntry>();
	for (auto it = m_private->begin(); it != m_private->end(); ++it)
		if (it->guid == guid)
		{
			if (it->unknown)
				it->unknown->Release();
			m_private->erase(it);
			break;
		}
	if (data && size)
	{
		PrivateEntry e;
		e.guid = guid;
		e.unknown = nullptr;
		e.data.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
		m_private->push_back(std::move(e));
	}
	return S_OK;
}

HRESULT Object::SetPrivateDataInterface(REFGUID guid, const IUnknown* data)
{
	SetPrivateData(guid, 0, nullptr);
	if (data)
	{
		PrivateEntry e;
		e.guid = guid;
		e.unknown = const_cast<IUnknown*>(data);
		e.unknown->AddRef();
		m_private->push_back(std::move(e));
	}
	return S_OK;
}

HRESULT Object::GetPrivateData(REFGUID guid, UINT* size, void* data)
{
	if (!size)
		return E_INVALIDARG;
	if (m_private)
		for (PrivateEntry& e : *m_private)
			if (e.guid == guid)
			{
				if (e.unknown)
				{
					if (!data)
					{
						*size = sizeof(IUnknown*);
						return S_OK;
					}
					if (*size < sizeof(IUnknown*))
						return DXGI_ERROR_MORE_DATA;
					e.unknown->AddRef();
					*static_cast<IUnknown**>(data) = e.unknown;
					*size = sizeof(IUnknown*);
					return S_OK;
				}
				if (!data)
				{
					*size = UINT(e.data.size());
					return S_OK;
				}
				if (*size < e.data.size())
					return DXGI_ERROR_MORE_DATA;
				memcpy(data, e.data.data(), e.data.size());
				*size = UINT(e.data.size());
				return S_OK;
			}
	*size = 0;
	return DXGI_ERROR_NOT_FOUND;
}

//===================================================================================================================
// Resources and views

Resource::~Resource()
{
	Device* device = TheDevice();
	if (!device)
		return;
	if (m_resource)
		device->m_impl->DeferRelease(m_resource, m_allocation);
	if (m_readback)
		device->m_impl->DeferRelease(m_readback, m_readback_allocation);
	m_resource = nullptr;
	m_allocation = nullptr;
	m_readback = nullptr;
	m_readback_allocation = nullptr;
}

#define DX12_QI_SELF(Class)                                                                                                \
	if (!out)                                                                                                              \
		return E_POINTER;                                                                                                  \
	if (riid == __uuidof(Class))                                                                                           \
	{                                                                                                                      \
		AddRef();                                                                                                          \
		*out = this;                                                                                                       \
		return S_OK;                                                                                                       \
	}

HRESULT Resource::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(Resource);
	return Object::QueryInterface(riid, out);
}

HRESULT Buffer::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(Buffer);
	return Resource::QueryInterface(riid, out);
}

HRESULT Texture1D::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(Texture1D);
	return Resource::QueryInterface(riid, out);
}

HRESULT Texture2D::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(Texture2D);
	return Resource::QueryInterface(riid, out);
}

HRESULT Texture3D::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(Texture3D);
	return Resource::QueryInterface(riid, out);
}

HRESULT ShaderResourceView::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(ShaderResourceView);
	return Object::QueryInterface(riid, out);
}

HRESULT RenderTargetView::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(RenderTargetView);
	return Object::QueryInterface(riid, out);
}

HRESULT DepthStencilView::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(DepthStencilView);
	return Object::QueryInterface(riid, out);
}

HRESULT UnorderedAccessView::QueryInterface(REFIID riid, void** out)
{
	DX12_QI_SELF(UnorderedAccessView);
	return Object::QueryInterface(riid, out);
}

View::~View()
{
	Device* device = TheDevice();
	if (device && m_slot != UINT(-1))
	{
		Device::Impl& d = *device->m_impl;
		switch (D3D12_DESCRIPTOR_HEAP_TYPE(m_heap))
		{
		case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: d.rtv_pool.Free(m_slot); break;
		case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: d.dsv_pool.Free(m_slot); break;
		default: d.srv_pool.Free(m_slot); break;
		}
	}
	if (m_res)
		m_res->Release();
}

void View::GetResource(Resource** out) const
{
	Resource* res = m_res;
	if (!res && TheDevice())
		res = TheDevice()->BackBufferAlias(); // the back buffer's view follows the current buffer
	if (res)
		res->AddRef();
	*out = res;
}

SamplerState::~SamplerState()
{
	if (TheDevice() && m_slot != UINT(-1))
		TheDevice()->m_impl->sampler_pool.Free(m_slot);
}

Query::~Query()
{
	Device* device = TheDevice();
	if (!device || m_index == UINT(-1))
		return;
	Device::Impl& d = *device->m_impl;
	std::lock_guard<std::mutex> lock(d.query_lock);
	if (m_desc.Query == D3D11_QUERY_TIMESTAMP)
		d.timestamp_free.push_back(m_index);
	else
		d.occlusion_free.push_back(m_index);
}

UINT Query::GetDataSize() const
{
	switch (m_desc.Query)
	{
	case D3D11_QUERY_OCCLUSION:
	case D3D11_QUERY_TIMESTAMP:
		return sizeof(UINT64);
	case D3D11_QUERY_TIMESTAMP_DISJOINT:
		return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
	default:
		return sizeof(BOOL);
	}
}

//===================================================================================================================
// Descriptor pools

void DescriptorPool::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	m_device = device;
	m_type = type;
	m_increment = device->GetDescriptorHandleIncrementSize(type);
}

void DescriptorPool::Destroy()
{
	for (ID3D12DescriptorHeap* heap : m_heaps)
		heap->Release();
	m_heaps.clear();
	m_starts.clear();
	m_free.clear();
	m_next = 0;
}

UINT DescriptorPool::Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* handle)
{
	std::lock_guard<std::mutex> lock(m_lock);
	UINT slot;
	if (!m_free.empty())
	{
		slot = m_free.back();
		m_free.pop_back();
	}
	else
	{
		if (m_next == m_heaps.size() * kHeapSize)
		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = m_type;
			desc.NumDescriptors = kHeapSize;
			ID3D12DescriptorHeap* heap = nullptr;
			if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
			{
				Msg("! D3D12: can't create a descriptor heap (type %d)", int(m_type));
				handle->ptr = 0;
				return UINT(-1);
			}
			m_heaps.push_back(heap);
			m_starts.push_back(heap->GetCPUDescriptorHandleForHeapStart());
		}
		slot = m_next++;
	}
	*handle = Handle(slot);
	return slot;
}

void DescriptorPool::Free(UINT slot)
{
	if (slot == UINT(-1))
		return;
	std::lock_guard<std::mutex> lock(m_lock);
	m_free.push_back(slot);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorPool::Handle(UINT slot) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = m_starts[slot / kHeapSize];
	h.ptr += SIZE_T(slot % kHeapSize) * m_increment;
	return h;
}

//===================================================================================================================
// Upload ring

void UploadRing::Destroy()
{
	auto free_page = [](UploadPage* p) {
		if (p->buffer)
			p->buffer->Release();
		if (p->allocation)
			p->allocation->Release();
		delete p;
	};
	for (UploadPage* p : m_used)
		free_page(p);
	for (UploadPage* p : m_in_flight)
		free_page(p);
	for (UploadPage* p : m_free)
		free_page(p);
	m_used.clear();
	m_in_flight.clear();
	m_free.clear();
	m_current = nullptr;
}

UploadPage* UploadRing::NewPage(u64 size)
{
	UploadPage* page = new UploadPage();
	page->size = size;
	D3D12MA::ALLOCATION_DESC ad = {};
	ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = size;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(m_allocator->CreateResource(&ad, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &page->allocation,
	                                       IID_PPV_ARGS(&page->buffer))))
	{
		Msg("! D3D12: can't create an upload page of %llu bytes", size);
		delete page;
		return nullptr;
	}
	D3D12_RANGE none = { 0, 0 };
	void* cpu = nullptr;
	page->buffer->Map(0, &none, &cpu);
	page->cpu = static_cast<u8*>(cpu);
	page->va = page->buffer->GetGPUVirtualAddress();
	return page;
}

bool UploadRing::Alloc(u64 size, u64 alignment, UploadSlice& slice)
{
	if (!size)
		size = 1;
	u64 aligned = m_current ? ((m_offset + alignment - 1) & ~(alignment - 1)) : 0;
	if (!m_current || aligned + size > m_current->size)
	{
		UploadPage* page = nullptr;
		if (size <= kPageSize)
		{
			if (!m_free.empty())
			{
				page = m_free.back();
				m_free.pop_back();
			}
			else
				page = NewPage(kPageSize);
		}
		else
			page = NewPage((size + 65535) & ~65535ull); // a large slice gets a page of its own
		if (!page)
			return false;
		m_used.push_back(page);
		if (size <= kPageSize || !m_current)
		{
			m_current = page;
			m_offset = 0;
		}
		else
		{
			// keep filling the current page after a large slice
			slice.cpu = page->cpu;
			slice.va = page->va;
			slice.buffer = page->buffer;
			slice.offset = 0;
			m_frame_bytes += size;
			return true;
		}
		aligned = 0;
	}
	slice.cpu = m_current->cpu + aligned;
	slice.va = m_current->va + aligned;
	slice.buffer = m_current->buffer;
	slice.offset = aligned;
	m_offset = aligned + size;
	m_frame_bytes += size;
	return true;
}

void UploadRing::Retire(u64 frame)
{
	for (UploadPage* p : m_used)
	{
		p->frame = frame;
		m_in_flight.push_back(p);
	}
	m_used.clear();
	m_current = nullptr;
	m_offset = 0;
	m_frame_bytes = 0;
}

void UploadRing::Reclaim(u64 completed)
{
	for (size_t i = 0; i < m_in_flight.size();)
	{
		UploadPage* p = m_in_flight[i];
		if (p->frame <= completed)
		{
			if (p->size == kPageSize && m_free.size() < 64)
				m_free.push_back(p);
			else
			{
				p->buffer->Release();
				p->allocation->Release();
				delete p;
			}
			m_in_flight[i] = m_in_flight.back();
			m_in_flight.pop_back();
		}
		else
			++i;
	}
}

//===================================================================================================================
// Device internals: fences, deferred release, initial uploads

u64 Device::Impl::Completed()
{
	const u64 value = fence->GetCompletedValue();
	if (value == UINT64_MAX && g_device)
		g_device->ReportRemoval("fence");
	return value;
}

void Device::Impl::WaitFence(u64 value)
{
	if (Completed() >= value)
		return;
	thread_local HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(value, event);
	if (WaitForSingleObject(event, 20000) == WAIT_TIMEOUT)
	{
		Msg("! D3D12: the GPU did not reach fence %llu in 20 s (completed %llu)", value, fence->GetCompletedValue());
		if (g_device)
			g_device->ReportRemoval("fence wait");
	}
}

u64 Device::Impl::Submit(ID3D12CommandList* const* lists, UINT count)
{
	std::lock_guard<std::mutex> lock(queue_lock);
	if (count)
		g_device->m_queue->ExecuteCommandLists(count, lists);
	const u64 value = next_fence++;
	g_device->m_queue->Signal(fence, value);
	pending_fence = next_fence;
	if (busy_on.load(std::memory_order_relaxed))
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		{
			std::lock_guard<std::mutex> busy(busy_lock);
			busy_submits.emplace_back(value, u64(now.QuadPart));
		}
		busy_cv.notify_one();
	}
	return value;
}

void Device::Impl::BusyTimer()
{
	HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	u64 last_done = 0;
	for (;;)
	{
		std::pair<u64, u64> next;
		{
			std::unique_lock<std::mutex> lock(busy_lock);
			busy_cv.wait(lock, [this] { return busy_quit || !busy_submits.empty(); });
			if (busy_quit)
				break;
			next = busy_submits.front();
		}
		if (Completed() < next.first)
		{
			fence->SetEventOnCompletion(next.first, event);
			WaitForSingleObject(event, 100);
		}
		const u64 completed = Completed();
		if (completed == UINT64_MAX)
			break; // device removed
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		std::lock_guard<std::mutex> lock(busy_lock);
		// Submissions completed together: the first takes the time since it could start, the others none
		while (!busy_submits.empty() && busy_submits.front().first <= completed)
		{
			const u64 start = _max(busy_submits.front().second, last_done);
			if (u64(now.QuadPart) > start)
				busy_ticks += u64(now.QuadPart) - start;
			last_done = u64(now.QuadPart);
			busy_submits.pop_front();
		}
	}
	CloseHandle(event);
}

void Device::StartGpuBusyTimer()
{
	Impl& d = *m_impl;
	if (d.busy_on)
		return;
	d.busy_on = true;
	d.busy_thread = std::thread([&d] { d.BusyTimer(); });
}

double Device::GpuBusyMs() const
{
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	return double(m_impl->busy_ticks.load()) * 1000.0 / double(freq.QuadPart);
}

void Device::Impl::DeferRelease(IUnknown* object, D3D12MA::Allocation* allocation)
{
	if (!object && !allocation)
		return;
	std::lock_guard<std::mutex> lock(deferred_lock);
	deferred.push_back({ pending_fence.load(), object, allocation });
}

void Device::Impl::ProcessDeferred(u64 completed, bool all)
{
	std::vector<Deferred> ready;
	{
		std::lock_guard<std::mutex> lock(deferred_lock);
		for (size_t i = 0; i < deferred.size();)
		{
			if (all || deferred[i].fence <= completed)
			{
				ready.push_back(deferred[i]);
				deferred[i] = deferred.back();
				deferred.pop_back();
			}
			else
				++i;
		}
	}
	for (Deferred& d : ready)
	{
		if (d.object)
			d.object->Release();
		if (d.allocation)
			d.allocation->Release();
	}
}

ID3D12GraphicsCommandList* Device::Impl::BeginInit()
{
	if (!init_open)
	{
		init_index = (init_index + 1) % (kFrames + 1);
		WaitFence(init_allocator_fence[init_index]);
		init_allocator[init_index]->Reset();
		init_list->Reset(init_allocator[init_index], nullptr);
		init_open = true;
	}
	return init_list;
}

static std::vector<Device::Impl::Deferred> g_init_uploads; // under init_lock

void Device::Impl::SubmitInitLocked()
{
	if (!init_open)
		return;
	if (!init_barriers.empty())
	{
		init_list->ResourceBarrier(UINT(init_barriers.size()), init_barriers.data());
		init_barriers.clear();
	}
	init_list->Close();
	init_open = false;
	ID3D12CommandList* lists[] = { init_list };
	const u64 value = Submit(lists, 1);
	init_allocator_fence[init_index] = value;
	{
		std::lock_guard<std::mutex> lock(deferred_lock);
		for (Deferred& d : g_init_uploads)
		{
			d.fence = value;
			deferred.push_back(d);
		}
	}
	g_init_uploads.clear();
	init_bytes = 0;
}

u64 Device::Impl::SubmitFrame(ID3D12CommandList* list)
{
	std::lock_guard<std::mutex> lock(init_lock);
	ID3D12CommandList* lists[2];
	UINT count = 0;
	const bool with_init = init_open;
	if (with_init)
	{
		if (!init_barriers.empty())
		{
			init_list->ResourceBarrier(UINT(init_barriers.size()), init_barriers.data());
			init_barriers.clear();
		}
		init_list->Close();
		init_open = false;
		lists[count++] = init_list;
	}
	lists[count++] = list;
	const u64 value = Submit(lists, count);
	if (with_init)
	{
		init_allocator_fence[init_index] = value;
		{
			std::lock_guard<std::mutex> dlock(deferred_lock);
			for (Deferred& d : g_init_uploads)
			{
				d.fence = value;
				deferred.push_back(d);
			}
		}
		g_init_uploads.clear();
		init_bytes = 0;
	}
	return value;
}

bool Device::Impl::UploadInitial(Resource* res, const D3D12_RESOURCE_DESC& desc, const D3D11_SUBRESOURCE_DATA* init,
                                 D3D12_RESOURCE_STATES rest_state, D3D12_RESOURCE_STATES texture_state)
{
	ID3D12Device* device = g_device->m_device;
	const bool buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
	const UINT count = buffer ? 1 : UINT(desc.MipLevels) * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize);

	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
	std::vector<UINT> rows(count);
	std::vector<UINT64> row_bytes(count);
	UINT64 total = 0;
	if (buffer)
	{
		total = desc.Width;
		footprints[0] = {};
		rows[0] = 1;
		row_bytes[0] = desc.Width;
	}
	else
		device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), row_bytes.data(), &total);

	D3D12MA::ALLOCATION_DESC ad = {};
	ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = total ? total : 1;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource* upload = nullptr;
	D3D12MA::Allocation* allocation = nullptr;
	if (FAILED(g_device->m_allocator->CreateResource(&ad, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &allocation,
	                                                 IID_PPV_ARGS(&upload))))
	{
		Msg("! D3D12: can't create an upload buffer of %llu bytes", total);
		return false;
	}
	u8* dst = nullptr;
	D3D12_RANGE none = { 0, 0 };
	upload->Map(0, &none, reinterpret_cast<void**>(&dst));
	if (buffer)
		memcpy(dst, init[0].pSysMem, size_t(desc.Width));
	else
		for (UINT s = 0; s < count; ++s)
		{
			const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[s];
			const u8* src = static_cast<const u8*>(init[s].pSysMem);
			if (!src)
				continue;
			for (UINT z = 0; z < fp.Footprint.Depth; ++z)
				for (UINT r = 0; r < rows[s]; ++r)
					memcpy(dst + fp.Offset + u64(z) * fp.Footprint.RowPitch * rows[s] + u64(r) * fp.Footprint.RowPitch,
					       src + size_t(z) * init[s].SysMemSlicePitch + size_t(r) * init[s].SysMemPitch, size_t(row_bytes[s]));
		}
	upload->Unmap(0, nullptr);

	std::lock_guard<std::mutex> lock(init_lock);
	ID3D12GraphicsCommandList* list = BeginInit();
	if (buffer)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res->m_resource;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		list->ResourceBarrier(1, &b);
		list->CopyBufferRegion(res->m_resource, 0, upload, 0, desc.Width);
	}
	else
	{
		if (texture_state != D3D12_RESOURCE_STATE_COPY_DEST)
		{
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = res->m_resource;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = texture_state;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			list->ResourceBarrier(1, &b);
		}
		for (UINT s = 0; s < count; ++s)
		{
			D3D12_TEXTURE_COPY_LOCATION d = {};
			d.pResource = res->m_resource;
			d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			d.SubresourceIndex = s;
			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource = upload;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = footprints[s];
			list->CopyTextureRegion(&d, 0, 0, 0, &src, nullptr);
		}
	}
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res->m_resource;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	b.Transition.StateAfter = rest_state;
	init_barriers.push_back(b);
	if (init_barriers.size() >= 256)
	{
		list->ResourceBarrier(UINT(init_barriers.size()), init_barriers.data());
		init_barriers.clear();
	}
	g_init_uploads.push_back({ 0, upload, allocation });
	res->m_state = rest_state;
	res->m_states.clear();

	// Loader threads can create a lot at once: past 256 MB the copies go to the GPU without waiting for a frame
	init_bytes += total;
	if (init_bytes > (256ull << 20))
		SubmitInitLocked();
	return true;
}

bool Device::Impl::UploadStaging(Resource* dst, Resource* src)
{
	if (!dst->m_resource || dst->IsBuffer() || !dst->m_states.empty() || src->m_layouts.empty())
		return false;
	const D3D12_RESOURCE_DESC desc = dst->m_resource->GetDesc();
	const UINT count = UINT(desc.MipLevels) * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize);
	std::vector<D3D11_SUBRESOURCE_DATA> init(count);
	for (UINT s = 0; s < count && s < src->m_layouts.size(); ++s)
	{
		const SubresourceLayout& l = src->m_layouts[s];
		init[s].pSysMem = src->m_cpu_data.data() + l.offset;
		init[s].SysMemPitch = l.row_pitch;
		init[s].SysMemSlicePitch = l.depth_pitch;
	}
	const D3D12_RESOURCE_STATES state = dst->m_state;
	return UploadInitial(dst, desc, init.data(), state == D3D12_RESOURCE_STATE_COMMON ? ShaderReadState() : state, state);
}

//===================================================================================================================
// Device

Device* Device::Create(IDXGIAdapter1* adapter, bool debug_layer)
{
	if (debug_layer)
	{
		ID3D12Debug* debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			debug->Release();
			Msg("* D3D12: debug layer on");
		}
	}
	bool dred = false;
	{
		ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
		{
			settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			settings->Release();
			dred = true;
		}
	}

	ID3D12Device* d3d = nullptr;
	HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d));
	if (FAILED(hr))
	{
		Msg("! D3D12CreateDevice failed: 0x%08x", hr);
		return nullptr;
	}

	Device* device = new Device();
	g_device = device;
	device->m_device = d3d;
	device->m_impl = new Impl();
	Impl& d = *device->m_impl;
	d.adapter = adapter;
	adapter->AddRef();
	d.debug_layer = debug_layer;
	d.dred = dred;

	if (debug_layer)
	{
		ID3D12InfoQueue* info = nullptr;
		if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&info))))
		{
			D3D12_MESSAGE_ID hide[] = {
				D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
				D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
			};
			D3D12_INFO_QUEUE_FILTER filter = {};
			filter.DenyList.NumIDs = _countof(hide);
			filter.DenyList.pIDList = hide;
			info->AddStorageFilterEntries(&filter);
			info->Release();
		}
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(d3d->CreateCommandQueue(&qd, IID_PPV_ARGS(&device->m_queue))))
	{
		Msg("! D3D12: can't create the command queue");
		return nullptr;
	}
	device->m_queue->GetTimestampFrequency(&d.timestamp_frequency);
	d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence));
	d.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	D3D12MA::ALLOCATOR_DESC ad = {};
	ad.pDevice = d3d;
	ad.pAdapter = adapter;
	ad.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
	if (FAILED(D3D12MA::CreateAllocator(&ad, &device->m_allocator)))
	{
		Msg("! D3D12: can't create the memory allocator");
		return nullptr;
	}

	d.rtv_pool.Init(d3d, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	d.dsv_pool.Init(d3d, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	d.srv_pool.Init(d3d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	d.sampler_pool.Init(d3d, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	// Null descriptors for unbound slots, per view dimension
	for (UINT dim = 1; dim <= 10; ++dim)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.ViewDimension = D3D12_SRV_DIMENSION(dim);
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		switch (sd.ViewDimension)
		{
		case D3D12_SRV_DIMENSION_BUFFER: sd.Format = DXGI_FORMAT_R32_UINT; break;
		case D3D12_SRV_DIMENSION_TEXTURE1D: sd.Texture1D.MipLevels = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: sd.Texture1DArray.MipLevels = 1; sd.Texture1DArray.ArraySize = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE2D: sd.Texture2D.MipLevels = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.ArraySize = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: sd.Texture2DMSArray.ArraySize = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE3D: sd.Texture3D.MipLevels = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURECUBE: sd.TextureCube.MipLevels = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: sd.TextureCubeArray.MipLevels = 1; sd.TextureCubeArray.NumCubes = 1; break;
		default: break;
		}
		d.srv_pool.Alloc(&d.null_srv[dim]);
		d3d->CreateShaderResourceView(nullptr, &sd, d.null_srv[dim]);
	}
	d.null_srv[0] = d.null_srv[D3D12_SRV_DIMENSION_TEXTURE2D];
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
		ud.Format = DXGI_FORMAT_R32_FLOAT;
		ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		d.srv_pool.Alloc(&d.null_uav);
		d3d->CreateUnorderedAccessView(nullptr, nullptr, &ud, d.null_uav);

		D3D12_SAMPLER_DESC smp = {};
		smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		smp.MaxAnisotropy = 1;
		smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		smp.MaxLOD = D3D12_FLOAT32_MAX;
		d.sampler_pool.Alloc(&d.default_sampler);
		d3d->CreateSampler(&smp, d.default_sampler);
	}

	// Queries
	{
		D3D12_QUERY_HEAP_DESC qh = {};
		qh.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
		qh.Count = Impl::kOcclusionQueries;
		d3d->CreateQueryHeap(&qh, IID_PPV_ARGS(&d.occlusion_heap));
		qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		qh.Count = Impl::kTimestampQueries;
		d3d->CreateQueryHeap(&qh, IID_PPV_ARGS(&d.timestamp_heap));

		auto readback = [&](UINT64 size, ID3D12Resource** res, D3D12MA::Allocation** alloc) {
			D3D12MA::ALLOCATION_DESC rad = {};
			rad.HeapType = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			rd.Width = size;
			rd.Height = 1;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = 1;
			rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			device->m_allocator->CreateResource(&rad, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, alloc, IID_PPV_ARGS(res));
		};
		readback(Impl::kOcclusionQueries * 8, &d.occlusion_readback, &d.occlusion_readback_allocation);
		readback(Impl::kTimestampQueries * 8, &d.timestamp_readback, &d.timestamp_readback_allocation);
		for (UINT i = Impl::kOcclusionQueries; i > 0; --i)
			d.occlusion_free.push_back(i - 1);
		for (UINT i = Impl::kTimestampQueries; i > 0; --i)
			d.timestamp_free.push_back(i - 1);
	}

	d.serialize_root_signature = D3D12SerializeRootSignature;

	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
		if (SUCCEEDED(d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) &&
		    o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2)
			device->m_vrs_tile = o6.ShadingRateImageTileSize;
		Msg("* D3D12: variable rate shading %s", device->m_vrs_tile ? make_string("tier 2, %u px tiles", device->m_vrs_tile).c_str() :
		    "with an image unsupported");
	}

	// Initial upload list
	for (UINT i = 0; i < kFrames + 1; ++i)
		d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.init_allocator[i]));
	d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.init_allocator[0], nullptr, IID_PPV_ARGS(&d.init_list));
	d.init_list->Close();
	d.init_list->SetName(L"xr init uploads");

	device->m_context = new Context(device);

	DXGI_ADAPTER_DESC1 desc = {};
	adapter->GetDesc1(&desc);
	Msg("* D3D12 device: %S, feature level 11_0%s%s", desc.Description, debug_layer ? ", debug layer" : "",
	    dred ? ", DRED" : "");
	return device;
}

Device::~Device()
{
	m_impl->StopPipelineCache();
	WaitIdle();
	if (m_impl->busy_thread.joinable())
	{
		{
			std::lock_guard<std::mutex> lock(m_impl->busy_lock);
			m_impl->busy_quit = true;
		}
		m_impl->busy_cv.notify_one();
		m_impl->busy_thread.join();
	}
	delete m_context;
	m_context = nullptr;
	Impl& d = *m_impl;
	ReleaseBackBuffers();
	if (d.back_buffer_alias)
	{
		d.back_buffer_alias->Release();
		d.back_buffer_alias = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(d.state_lock);
		for (auto& kv : d.states)
			for (Object* o : kv.second)
				o->Release();
		d.states.clear();
	}
	{
		std::lock_guard<std::mutex> lock(d.pipeline_lock);
		for (auto& kv : d.graphics_pipelines)
			if (kv.second)
				kv.second->Release();
		for (auto& kv : d.compute_pipelines)
			if (kv.second)
				kv.second->Release();
		for (auto& kv : d.root_signatures)
		{
			if (kv.second->signature)
				kv.second->signature->Release();
			delete kv.second;
		}
		d.graphics_pipelines.clear();
		d.compute_pipelines.clear();
		d.root_signatures.clear();
	}
	d.ProcessDeferred(0, true);
	if (d.occlusion_heap)
		d.occlusion_heap->Release();
	if (d.timestamp_heap)
		d.timestamp_heap->Release();
	if (d.occlusion_readback)
		d.occlusion_readback->Release();
	if (d.occlusion_readback_allocation)
		d.occlusion_readback_allocation->Release();
	if (d.timestamp_readback)
		d.timestamp_readback->Release();
	if (d.timestamp_readback_allocation)
		d.timestamp_readback_allocation->Release();
	if (d.init_list)
		d.init_list->Release();
	for (ID3D12CommandAllocator* a : d.init_allocator)
		if (a)
			a->Release();
	d.rtv_pool.Destroy();
	d.dsv_pool.Destroy();
	d.srv_pool.Destroy();
	d.sampler_pool.Destroy();
	if (m_allocator)
		m_allocator->Release();
	if (d.fence)
		d.fence->Release();
	if (d.fence_event)
		CloseHandle(d.fence_event);
	if (m_queue)
		m_queue->Release();
	if (d.adapter)
		d.adapter->Release();
	if (m_device)
		m_device->Release();
	delete m_impl;
	m_impl = nullptr;
	g_device = nullptr;
}

ULONG Device::Release()
{
	const LONG refs = InterlockedDecrement(&m_refs);
	if (refs == 0)
		delete this;
	return ULONG(refs);
}

HRESULT Device::QueryInterface(REFIID riid, void** out)
{
	if (!out)
		return E_POINTER;
	*out = nullptr;
	return E_NOINTERFACE;
}

void Device::GetImmediateContext(Context** out) { *out = m_context; }

HRESULT Device::GetDeviceRemovedReason() { return m_device->GetDeviceRemovedReason(); }

void Device::WaitIdle()
{
	if (m_context)
		m_context->Synchronize();
	m_impl->WaitQueueIdle();
}

void Device::Impl::WaitQueueIdle()
{
	{
		std::lock_guard<std::mutex> lock(init_lock);
		SubmitInitLocked();
	}
	const u64 value = Submit(nullptr, 0);
	WaitFence(value);
	ProcessDeferred(Completed(), false);
}

void Device::ReportRemoval(const char* where)
{
	Impl& d = *m_impl;
	const HRESULT reason = m_device->GetDeviceRemovedReason();
	if (reason == S_OK || d.removal_reported)
		return;
	d.removal_reported = true;
	Msg("! D3D12 device removed (%s): reason 0x%08x", where, reason);

	ID3D12DeviceRemovedExtendedData* dred = nullptr;
	if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&dred))))
	{
		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
		if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
		{
			for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
			{
				const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
				if (done >= node->BreadcrumbCount)
					continue; // finished
				Msg("! DRED: list %S, %u of %u operations done", node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?",
				    done, node->BreadcrumbCount);
				const UINT from = done > 8 ? done - 8 : 0;
				const UINT to = _min(node->BreadcrumbCount, done + 4);
				for (UINT i = from; i < to; ++i)
					Msg("!   %s op %u: %d", i == done ? ">" : " ", i, int(node->pCommandHistory[i]));
			}
		}
		D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
		if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA)
		{
			Msg("! DRED: page fault at GPU VA 0x%llx", fault.PageFaultVA);
			for (const D3D12_DRED_ALLOCATION_NODE* n = fault.pHeadExistingAllocationNode; n; n = n->pNext)
				Msg("!   existing: %S (type %d)", n->ObjectNameW ? n->ObjectNameW : L"?", int(n->AllocationType));
			for (const D3D12_DRED_ALLOCATION_NODE* n = fault.pHeadRecentFreedAllocationNode; n; n = n->pNext)
				Msg("!   recently freed: %S (type %d)", n->ObjectNameW ? n->ObjectNameW : L"?", int(n->AllocationType));
		}
		dred->Release();
	}
	xrLogger::FlushLog();
}

//-------------------------------------------------------------------------------------------------------------------
// Resource creation

static void FillTextureResource(Resource* t, D3D11_RESOURCE_DIMENSION dim, D3D11_USAGE usage, UINT bind, UINT cpu,
                                UINT misc, DXGI_FORMAT format, UINT width, UINT height, UINT depth_or_array, UINT mips,
                                UINT samples, UINT quality)
{
	t->m_dimension = dim;
	t->m_usage = usage;
	t->m_bind = bind;
	t->m_cpu_access = cpu;
	t->m_misc = misc;
	t->m_format = format;
	t->m_width = width;
	t->m_height = height;
	t->m_depth_or_array = depth_or_array;
	t->m_mips = mips;
	t->m_samples = samples ? samples : 1;
	t->m_sample_quality = quality;
}

static D3D12_RESOURCE_DESC TextureDesc12(const Resource* t)
{
	D3D12_RESOURCE_DESC rd = {};
	switch (t->m_dimension)
	{
	case D3D11_RESOURCE_DIMENSION_TEXTURE1D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; break;
	case D3D11_RESOURCE_DIMENSION_TEXTURE3D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; break;
	default: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; break;
	}
	rd.Width = t->m_width;
	rd.Height = t->m_height;
	rd.DepthOrArraySize = UINT16(t->m_depth_or_array);
	rd.MipLevels = UINT16(t->m_mips);
	rd.Format = t->m_format;
	rd.SampleDesc.Count = t->m_samples;
	rd.SampleDesc.Quality = t->m_sample_quality;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rd.Flags = ResourceFlags(t->m_bind);
	return rd;
}

// The common part of texture creation: GPU resource, staging memory or readback buffer, initial data
static HRESULT CreateTextureCommon(Device* device, Resource* t, const D3D11_SUBRESOURCE_DATA* init)
{
	Device::Impl& d = *device->m_impl;
	ID3D12Device* d3d = device->m_device;
	D3D12_RESOURCE_DESC rd = TextureDesc12(t);
	t->m_planes = FormatPlaneCount(d3d, t->m_format);
	t->m_subresources = t->m_mips * t->ArraySize() * t->m_planes;

	if (t->m_usage == D3D11_USAGE_STAGING)
	{
		rd.Flags = D3D12_RESOURCE_FLAG_NONE;
		const UINT count = t->m_subresources;
		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(count);
		std::vector<UINT> rows(count);
		std::vector<UINT64> row_bytes(count);
		UINT64 total = 0;
		d3d->GetCopyableFootprints(&rd, 0, count, 0, fp.data(), rows.data(), row_bytes.data(), &total);
		if (t->m_cpu_access & D3D11_CPU_ACCESS_READ)
		{
			D3D12MA::ALLOCATION_DESC ad = {};
			ad.HeapType = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = total ? total : 1;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(device->m_allocator->CreateResource(&ad, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                               &t->m_readback_allocation, IID_PPV_ARGS(&t->m_readback))))
				return E_OUTOFMEMORY;
			t->m_footprints = fp;
			t->m_layouts.resize(count);
			for (UINT s = 0; s < count; ++s)
			{
				SubresourceLayout& l = t->m_layouts[s];
				l.offset = fp[s].Offset;
				l.row_pitch = fp[s].Footprint.RowPitch;
				l.rows = rows[s];
				l.row_bytes = UINT(row_bytes[s]);
				l.depth = fp[s].Footprint.Depth;
				l.depth_pitch = l.row_pitch * l.rows;
			}
		}
		else
		{
			// Tightly packed in CPU memory, like D3D11 hands out staging memory
			t->m_layouts.resize(count);
			u64 offset = 0;
			for (UINT s = 0; s < count; ++s)
			{
				SubresourceLayout& l = t->m_layouts[s];
				l.offset = offset;
				l.row_bytes = UINT(row_bytes[s]);
				l.row_pitch = l.row_bytes;
				l.rows = rows[s];
				l.depth = fp[s].Footprint.Depth;
				l.depth_pitch = l.row_pitch * l.rows;
				offset += u64(l.depth_pitch) * l.depth;
			}
			t->m_cpu_data.resize(size_t(offset));
			if (init)
			{
				const UINT init_count = t->m_mips * t->ArraySize();
				for (UINT s = 0; s < init_count; ++s)
				{
					const SubresourceLayout& l = t->m_layouts[s];
					const u8* src = static_cast<const u8*>(init[s].pSysMem);
					if (!src)
						continue;
					for (UINT z = 0; z < l.depth; ++z)
						for (UINT r = 0; r < l.rows; ++r)
							memcpy(t->m_cpu_data.data() + l.offset + size_t(z) * l.depth_pitch + size_t(r) * l.row_pitch,
							       src + size_t(z) * init[s].SysMemSlicePitch + size_t(r) * init[s].SysMemPitch, l.row_bytes);
				}
			}
		}
		return S_OK;
	}

	D3D12MA::ALLOCATION_DESC ad = {};
	ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
	// Render, depth and UAV targets in their own (zeroed) memory: placed in a reused heap they start with what was there
	// (DX12 leaves that to a clear the engine doesn't do, D3D11 zeroes them). A target read before it's fully written
	// showed the old contents, after a vid_restart a frame of the previous size as a jittering grid with FSR (Deck)
	if (rd.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
	                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
		ad.Flags = D3D12MA::ALLOCATION_FLAG_COMMITTED;
	D3D12_CLEAR_VALUE clear = {};
	const D3D12_CLEAR_VALUE* clear_ptr = nullptr;
	if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
	{
		// The fast clear value for depth: 1.0, stencil 0 (the renderer clears to that)
		switch (rd.Format)
		{
		case DXGI_FORMAT_R24G8_TYPELESS: clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
		case DXGI_FORMAT_R32_TYPELESS: clear.Format = DXGI_FORMAT_D32_FLOAT; break;
		case DXGI_FORMAT_R16_TYPELESS: clear.Format = DXGI_FORMAT_D16_UNORM; break;
		case DXGI_FORMAT_R32G8X24_TYPELESS: clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; break;
		default: clear.Format = rd.Format; break;
		}
		clear.DepthStencil.Depth = 1.f;
		clear_ptr = &clear;
	}
	const D3D12_RESOURCE_STATES initial = init ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
	HRESULT hr = device->m_allocator->CreateResource(&ad, &rd, initial, clear_ptr, &t->m_allocation, IID_PPV_ARGS(&t->m_resource));
	if (FAILED(hr))
	{
		Msg("! D3D12: can't create a %ux%ux%u texture (format %d, mips %u, bind 0x%x): 0x%08x", t->m_width, t->m_height,
		    t->m_depth_or_array, int(t->m_format), t->m_mips, t->m_bind, hr);
		if (hr == DXGI_ERROR_DEVICE_REMOVED)
			device->ReportRemoval("texture creation");
		return hr;
	}
	t->m_state = initial;
	if (init)
	{
		D3D12_RESOURCE_DESC id = rd;
		if (!d.UploadInitial(t, id, init, ShaderReadState()))
			return E_OUTOFMEMORY;
	}
	return S_OK;
}

HRESULT Device::CreateTexture1D(const D3D11_TEXTURE1D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Texture1D** out)
{
	*out = nullptr;
	Texture1D* t = new Texture1D();
	t->m_desc = *desc;
	if (!t->m_desc.MipLevels)
		t->m_desc.MipLevels = MipCount(desc->Width, 1, 1);
	FillTextureResource(t, D3D11_RESOURCE_DIMENSION_TEXTURE1D, desc->Usage, desc->BindFlags, desc->CPUAccessFlags,
	                    desc->MiscFlags, desc->Format, desc->Width, 1, desc->ArraySize, t->m_desc.MipLevels, 1, 0);
	const HRESULT hr = CreateTextureCommon(this, t, init);
	if (FAILED(hr))
	{
		t->Release();
		return hr;
	}
	*out = t;
	return S_OK;
}

HRESULT Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Texture2D** out)
{
	*out = nullptr;
	Texture2D* t = new Texture2D();
	t->m_desc = *desc;
	if (!t->m_desc.MipLevels)
		t->m_desc.MipLevels = MipCount(desc->Width, desc->Height, 1);
	FillTextureResource(t, D3D11_RESOURCE_DIMENSION_TEXTURE2D, desc->Usage, desc->BindFlags, desc->CPUAccessFlags,
	                    desc->MiscFlags, desc->Format, desc->Width, desc->Height, desc->ArraySize, t->m_desc.MipLevels,
	                    desc->SampleDesc.Count, desc->SampleDesc.Quality);
	const HRESULT hr = CreateTextureCommon(this, t, init);
	if (FAILED(hr))
	{
		t->Release();
		return hr;
	}
	*out = t;
	return S_OK;
}

HRESULT Device::CreateTexture3D(const D3D11_TEXTURE3D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Texture3D** out)
{
	*out = nullptr;
	Texture3D* t = new Texture3D();
	t->m_desc = *desc;
	if (!t->m_desc.MipLevels)
		t->m_desc.MipLevels = MipCount(desc->Width, desc->Height, desc->Depth);
	FillTextureResource(t, D3D11_RESOURCE_DIMENSION_TEXTURE3D, desc->Usage, desc->BindFlags, desc->CPUAccessFlags,
	                    desc->MiscFlags, desc->Format, desc->Width, desc->Height, desc->Depth, t->m_desc.MipLevels, 1, 0);
	const HRESULT hr = CreateTextureCommon(this, t, init);
	if (FAILED(hr))
	{
		t->Release();
		return hr;
	}
	*out = t;
	return S_OK;
}

HRESULT Device::CreateBuffer(const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, Buffer** out)
{
	*out = nullptr;
	Buffer* b = new Buffer();
	b->m_desc = *desc;
	b->m_dimension = D3D11_RESOURCE_DIMENSION_BUFFER;
	b->m_usage = desc->Usage;
	b->m_bind = desc->BindFlags;
	b->m_cpu_access = desc->CPUAccessFlags;
	b->m_misc = desc->MiscFlags;
	b->m_width = desc->ByteWidth;
	b->m_subresources = 1;

	const u64 size = (u64(desc->ByteWidth) + 255) & ~255ull;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = size;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags = (desc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

	if (desc->Usage == D3D11_USAGE_DYNAMIC && desc->ByteWidth <= 65536)
	{
		b->m_shadowed = true;
		b->m_shadow.assign(desc->ByteWidth, 0);
		if (init && init->pSysMem)
			memcpy(b->m_shadow.data(), init->pSysMem, desc->ByteWidth);
		b->m_ptr_frame = 0; // uploaded at the first use
		*out = b;
		return S_OK;
	}
	if (desc->Usage == D3D11_USAGE_DYNAMIC)
	{
		// Large (vertex/index streams): lives in the upload ring once mapped, the renderer maps it every frame.
		// Initial data gets an upload buffer of its own until the first map
		if (init && init->pSysMem)
		{
			D3D12MA::ALLOCATION_DESC ad = {};
			ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
			if (FAILED(m_allocator->CreateResource(&ad, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &b->m_allocation,
			                                       IID_PPV_ARGS(&b->m_resource))))
			{
				b->Release();
				return E_OUTOFMEMORY;
			}
			void* cpu = nullptr;
			D3D12_RANGE none = { 0, 0 };
			b->m_resource->Map(0, &none, &cpu);
			memcpy(cpu, init->pSysMem, desc->ByteWidth);
			b->m_resource->Unmap(0, nullptr);
			b->m_va = b->m_resource->GetGPUVirtualAddress();
			b->m_state = D3D12_RESOURCE_STATE_GENERIC_READ;
		}
		*out = b;
		return S_OK;
	}
	if (desc->Usage == D3D11_USAGE_STAGING)
	{
		if (desc->CPUAccessFlags & D3D11_CPU_ACCESS_READ)
		{
			D3D12MA::ALLOCATION_DESC ad = {};
			ad.HeapType = D3D12_HEAP_TYPE_READBACK;
			if (FAILED(m_allocator->CreateResource(&ad, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                       &b->m_readback_allocation, IID_PPV_ARGS(&b->m_readback))))
			{
				b->Release();
				return E_OUTOFMEMORY;
			}
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
			fp.Footprint.Width = desc->ByteWidth;
			fp.Footprint.Height = 1;
			fp.Footprint.Depth = 1;
			fp.Footprint.RowPitch = desc->ByteWidth;
			b->m_footprints.push_back(fp);
		}
		else
		{
			b->m_cpu_data.resize(desc->ByteWidth);
			if (init && init->pSysMem)
				memcpy(b->m_cpu_data.data(), init->pSysMem, desc->ByteWidth);
		}
		SubresourceLayout l = {};
		l.row_pitch = l.depth_pitch = l.row_bytes = desc->ByteWidth;
		l.rows = l.depth = 1;
		b->m_layouts.push_back(l);
		*out = b;
		return S_OK;
	}

	D3D12MA::ALLOCATION_DESC ad = {};
	ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
	if (FAILED(m_allocator->CreateResource(&ad, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, &b->m_allocation,
	                                       IID_PPV_ARGS(&b->m_resource))))
	{
		Msg("! D3D12: can't create a buffer of %u bytes", desc->ByteWidth);
		b->Release();
		return E_OUTOFMEMORY;
	}
	b->m_va = b->m_resource->GetGPUVirtualAddress();
	b->m_state = D3D12_RESOURCE_STATE_COMMON;
	if (init && init->pSysMem)
	{
		D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_COMMON;
		if (desc->BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER))
			rest |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		if (desc->BindFlags & D3D11_BIND_INDEX_BUFFER)
			rest |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
		if (desc->BindFlags & D3D11_BIND_SHADER_RESOURCE)
			rest |= ShaderReadState();
		D3D12_RESOURCE_DESC id = rd;
		id.Width = desc->ByteWidth;
		if (!m_impl->UploadInitial(b, id, init, rest))
		{
			b->Release();
			return E_OUTOFMEMORY;
		}
	}
	*out = b;
	return S_OK;
}

//-------------------------------------------------------------------------------------------------------------------
// Views

static void SetViewRange(View* v, const Resource* r, UINT first_mip, UINT mips, UINT first_slice, UINT slices,
                         UINT first_plane, UINT planes)
{
	if (mips == UINT(-1) || first_mip + mips > r->m_mips)
		mips = r->m_mips - _min(first_mip, r->m_mips);
	const UINT array = r->ArraySize();
	if (slices == UINT(-1) || first_slice + slices > array)
		slices = array - _min(first_slice, array);
	v->m_first_mip = first_mip;
	v->m_mip_count = _max(mips, 1u);
	v->m_first_slice = first_slice;
	v->m_slice_count = _max(slices, 1u);
	v->m_first_plane = first_plane;
	v->m_plane_count = planes;
	v->m_whole = first_mip == 0 && v->m_mip_count == r->m_mips && first_slice == 0 && v->m_slice_count == array &&
		first_plane == 0 && planes == r->m_planes;
}

HRESULT Device::CreateShaderResourceView(Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ShaderResourceView** out)
{
	*out = nullptr;
	if (!res || !res->m_resource)
		return E_INVALIDARG;
	D3D11_SHADER_RESOURCE_VIEW_DESC d11 = {};
	if (desc)
		d11 = *desc;
	else
	{
		d11.Format = res->m_format;
		switch (res->m_dimension)
		{
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			d11.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			d11.Buffer.FirstElement = 0;
			d11.Buffer.NumElements = res->m_width / 4;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			d11.ViewDimension = res->m_depth_or_array > 1 ? D3D11_SRV_DIMENSION_TEXTURE1DARRAY : D3D11_SRV_DIMENSION_TEXTURE1D;
			d11.Texture1DArray.MipLevels = UINT(-1);
			d11.Texture1DArray.ArraySize = res->m_depth_or_array;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			d11.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			d11.Texture3D.MipLevels = UINT(-1);
			break;
		default:
			if (res->m_misc & D3D11_RESOURCE_MISC_TEXTURECUBE)
			{
				d11.ViewDimension = res->m_depth_or_array > 6 ? D3D11_SRV_DIMENSION_TEXTURECUBEARRAY : D3D11_SRV_DIMENSION_TEXTURECUBE;
				d11.TextureCubeArray.MipLevels = UINT(-1);
				d11.TextureCubeArray.NumCubes = res->m_depth_or_array / 6;
			}
			else if (res->m_samples > 1)
			{
				d11.ViewDimension = res->m_depth_or_array > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
				d11.Texture2DMSArray.ArraySize = res->m_depth_or_array;
			}
			else if (res->m_depth_or_array > 1)
			{
				d11.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				d11.Texture2DArray.MipLevels = UINT(-1);
				d11.Texture2DArray.ArraySize = res->m_depth_or_array;
			}
			else
			{
				d11.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				d11.Texture2D.MipLevels = UINT(-1);
			}
			break;
		}
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
	d.Format = d11.Format;
	d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	const UINT plane = PlaneOfViewFormat(d11.Format);
	ShaderResourceView* v = new ShaderResourceView();
	switch (d11.ViewDimension)
	{
	case D3D11_SRV_DIMENSION_BUFFER:
		d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		d.Buffer.FirstElement = d11.Buffer.FirstElement;
		d.Buffer.NumElements = d11.Buffer.NumElements;
		if (res->m_misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
		{
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.Buffer.StructureByteStride = static_cast<Buffer*>(res)->m_desc.StructureByteStride;
		}
		SetViewRange(v, res, 0, 1, 0, 1, 0, 1);
		break;
	case D3D11_SRV_DIMENSION_BUFFEREX:
		d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		d.Buffer.FirstElement = d11.BufferEx.FirstElement;
		d.Buffer.NumElements = d11.BufferEx.NumElements;
		if (d11.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW)
		{
			d.Format = DXGI_FORMAT_R32_TYPELESS;
			d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		}
		SetViewRange(v, res, 0, 1, 0, 1, 0, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURE1D:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
		d.Texture1D.MostDetailedMip = d11.Texture1D.MostDetailedMip;
		d.Texture1D.MipLevels = d11.Texture1D.MipLevels;
		SetViewRange(v, res, d11.Texture1D.MostDetailedMip, d11.Texture1D.MipLevels, 0, UINT(-1), 0, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
		d.Texture1DArray.MostDetailedMip = d11.Texture1DArray.MostDetailedMip;
		d.Texture1DArray.MipLevels = d11.Texture1DArray.MipLevels;
		d.Texture1DArray.FirstArraySlice = d11.Texture1DArray.FirstArraySlice;
		d.Texture1DArray.ArraySize = d11.Texture1DArray.ArraySize;
		SetViewRange(v, res, d11.Texture1DArray.MostDetailedMip, d11.Texture1DArray.MipLevels,
		             d11.Texture1DArray.FirstArraySlice, d11.Texture1DArray.ArraySize, 0, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2D:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		d.Texture2D.MostDetailedMip = d11.Texture2D.MostDetailedMip;
		d.Texture2D.MipLevels = d11.Texture2D.MipLevels;
		d.Texture2D.PlaneSlice = plane;
		SetViewRange(v, res, d11.Texture2D.MostDetailedMip, d11.Texture2D.MipLevels, 0, UINT(-1), plane, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		d.Texture2DArray.MostDetailedMip = d11.Texture2DArray.MostDetailedMip;
		d.Texture2DArray.MipLevels = d11.Texture2DArray.MipLevels;
		d.Texture2DArray.FirstArraySlice = d11.Texture2DArray.FirstArraySlice;
		d.Texture2DArray.ArraySize = d11.Texture2DArray.ArraySize;
		d.Texture2DArray.PlaneSlice = plane;
		SetViewRange(v, res, d11.Texture2DArray.MostDetailedMip, d11.Texture2DArray.MipLevels,
		             d11.Texture2DArray.FirstArraySlice, d11.Texture2DArray.ArraySize, plane, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2DMS:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
		SetViewRange(v, res, 0, 1, 0, UINT(-1), plane, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
		d.Texture2DMSArray.FirstArraySlice = d11.Texture2DMSArray.FirstArraySlice;
		d.Texture2DMSArray.ArraySize = d11.Texture2DMSArray.ArraySize;
		SetViewRange(v, res, 0, 1, d11.Texture2DMSArray.FirstArraySlice, d11.Texture2DMSArray.ArraySize, plane, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURE3D:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		d.Texture3D.MostDetailedMip = d11.Texture3D.MostDetailedMip;
		d.Texture3D.MipLevels = d11.Texture3D.MipLevels;
		SetViewRange(v, res, d11.Texture3D.MostDetailedMip, d11.Texture3D.MipLevels, 0, 1, 0, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURECUBE:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		d.TextureCube.MostDetailedMip = d11.TextureCube.MostDetailedMip;
		d.TextureCube.MipLevels = d11.TextureCube.MipLevels;
		SetViewRange(v, res, d11.TextureCube.MostDetailedMip, d11.TextureCube.MipLevels, 0, 6, 0, 1);
		break;
	case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
		d.TextureCubeArray.MostDetailedMip = d11.TextureCubeArray.MostDetailedMip;
		d.TextureCubeArray.MipLevels = d11.TextureCubeArray.MipLevels;
		d.TextureCubeArray.First2DArrayFace = d11.TextureCubeArray.First2DArrayFace;
		d.TextureCubeArray.NumCubes = d11.TextureCubeArray.NumCubes;
		SetViewRange(v, res, d11.TextureCubeArray.MostDetailedMip, d11.TextureCubeArray.MipLevels,
		             d11.TextureCubeArray.First2DArrayFace, d11.TextureCubeArray.NumCubes * 6, 0, 1);
		break;
	default:
		v->Release();
		Msg("! D3D12: unsupported SRV dimension %d", int(d11.ViewDimension));
		return E_INVALIDARG;
	}
	if (d.Format == DXGI_FORMAT_UNKNOWN && d.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
		d.Format = res->m_format;

	v->m_desc = d11;
	v->m_format = d.Format;
	v->m_res = res;
	res->AddRef();
	v->m_heap = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	v->m_slot = m_impl->srv_pool.Alloc(&v->m_cpu);
	m_device->CreateShaderResourceView(res->m_resource, &d, v->m_cpu);
	*out = v;
	return S_OK;
}

HRESULT Device::CreateRenderTargetView(Resource* res, const D3D11_RENDER_TARGET_VIEW_DESC* desc, RenderTargetView** out)
{
	*out = nullptr;
	if (!res || !res->m_resource)
		return E_INVALIDARG;
	D3D12_RENDER_TARGET_VIEW_DESC d = {};
	RenderTargetView* v = new RenderTargetView();
	if (desc)
	{
		v->m_desc = *desc;
		d.Format = desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : res->m_format;
		switch (desc->ViewDimension)
		{
		case D3D11_RTV_DIMENSION_TEXTURE1D:
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
			d.Texture1D.MipSlice = desc->Texture1D.MipSlice;
			SetViewRange(v, res, desc->Texture1D.MipSlice, 1, 0, UINT(-1), 0, 1);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2D:
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			d.Texture2D.MipSlice = desc->Texture2D.MipSlice;
			SetViewRange(v, res, desc->Texture2D.MipSlice, 1, 0, UINT(-1), 0, 1);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			d.Texture2DArray.MipSlice = desc->Texture2DArray.MipSlice;
			d.Texture2DArray.FirstArraySlice = desc->Texture2DArray.FirstArraySlice;
			d.Texture2DArray.ArraySize = desc->Texture2DArray.ArraySize;
			SetViewRange(v, res, desc->Texture2DArray.MipSlice, 1, desc->Texture2DArray.FirstArraySlice,
			             desc->Texture2DArray.ArraySize, 0, 1);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2DMS:
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
			SetViewRange(v, res, 0, 1, 0, UINT(-1), 0, 1);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
			d.Texture2DMSArray.FirstArraySlice = desc->Texture2DMSArray.FirstArraySlice;
			d.Texture2DMSArray.ArraySize = desc->Texture2DMSArray.ArraySize;
			SetViewRange(v, res, 0, 1, desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize, 0, 1);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE3D:
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
			d.Texture3D.MipSlice = desc->Texture3D.MipSlice;
			d.Texture3D.FirstWSlice = desc->Texture3D.FirstWSlice;
			d.Texture3D.WSize = desc->Texture3D.WSize;
			SetViewRange(v, res, desc->Texture3D.MipSlice, 1, 0, 1, 0, 1);
			break;
		default:
			v->Release();
			Msg("! D3D12: unsupported RTV dimension %d", int(desc->ViewDimension));
			return E_INVALIDARG;
		}
	}
	else
	{
		d.Format = res->m_format;
		if (res->m_dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
		{
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
			d.Texture3D.WSize = UINT(-1);
			SetViewRange(v, res, 0, 1, 0, 1, 0, 1);
		}
		else if (res->m_samples > 1)
		{
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
			SetViewRange(v, res, 0, 1, 0, UINT(-1), 0, 1);
		}
		else
		{
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			SetViewRange(v, res, 0, 1, 0, UINT(-1), 0, 1);
		}
		v->m_desc.Format = d.Format;
		v->m_desc.ViewDimension = D3D11_RTV_DIMENSION(d.ViewDimension);
	}
	v->m_format = d.Format;
	v->m_res = res;
	res->AddRef();
	v->m_heap = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	v->m_slot = m_impl->rtv_pool.Alloc(&v->m_cpu);
	m_device->CreateRenderTargetView(res->m_resource, &d, v->m_cpu);
	*out = v;
	return S_OK;
}

HRESULT Device::CreateDepthStencilView(Resource* res, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, DepthStencilView** out)
{
	*out = nullptr;
	if (!res || !res->m_resource)
		return E_INVALIDARG;
	D3D12_DEPTH_STENCIL_VIEW_DESC d = {};
	DepthStencilView* v = new DepthStencilView();
	D3D11_DEPTH_STENCIL_VIEW_DESC d11 = {};
	if (desc)
		d11 = *desc;
	else
	{
		d11.Format = res->m_format;
		d11.ViewDimension = res->m_samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
	}
	d.Format = d11.Format;
	d.Flags = D3D12_DSV_FLAGS(d11.Flags);
	switch (d11.ViewDimension)
	{
	case D3D11_DSV_DIMENSION_TEXTURE2D:
		d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		d.Texture2D.MipSlice = d11.Texture2D.MipSlice;
		SetViewRange(v, res, d11.Texture2D.MipSlice, 1, 0, UINT(-1), 0, res->m_planes);
		break;
	case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
		d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		d.Texture2DArray.MipSlice = d11.Texture2DArray.MipSlice;
		d.Texture2DArray.FirstArraySlice = d11.Texture2DArray.FirstArraySlice;
		d.Texture2DArray.ArraySize = d11.Texture2DArray.ArraySize;
		SetViewRange(v, res, d11.Texture2DArray.MipSlice, 1, d11.Texture2DArray.FirstArraySlice,
		             d11.Texture2DArray.ArraySize, 0, res->m_planes);
		break;
	case D3D11_DSV_DIMENSION_TEXTURE2DMS:
		d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
		SetViewRange(v, res, 0, 1, 0, UINT(-1), 0, res->m_planes);
		break;
	case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
		d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
		d.Texture2DMSArray.FirstArraySlice = d11.Texture2DMSArray.FirstArraySlice;
		d.Texture2DMSArray.ArraySize = d11.Texture2DMSArray.ArraySize;
		SetViewRange(v, res, 0, 1, d11.Texture2DMSArray.FirstArraySlice, d11.Texture2DMSArray.ArraySize, 0, res->m_planes);
		break;
	default:
		v->Release();
		Msg("! D3D12: unsupported DSV dimension %d", int(d11.ViewDimension));
		return E_INVALIDARG;
	}
	v->m_desc = d11;
	v->m_read_only = (d11.Flags & D3D11_DSV_READ_ONLY_DEPTH) &&
		((d11.Flags & D3D11_DSV_READ_ONLY_STENCIL) || res->m_planes < 2);
	v->m_format = d.Format;
	v->m_res = res;
	res->AddRef();
	v->m_heap = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	v->m_slot = m_impl->dsv_pool.Alloc(&v->m_cpu);
	m_device->CreateDepthStencilView(res->m_resource, &d, v->m_cpu);
	*out = v;
	return S_OK;
}

HRESULT Device::CreateUnorderedAccessView(Resource* res, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, UnorderedAccessView** out)
{
	*out = nullptr;
	if (!res || !res->m_resource)
		return E_INVALIDARG;
	D3D11_UNORDERED_ACCESS_VIEW_DESC d11 = {};
	if (desc)
		d11 = *desc;
	else
	{
		d11.Format = res->m_format;
		if (res->IsBuffer())
		{
			d11.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			d11.Buffer.NumElements = res->m_width / 4;
		}
		else if (res->m_dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
		{
			d11.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			d11.Texture3D.WSize = UINT(-1);
		}
		else if (res->m_depth_or_array > 1)
		{
			d11.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			d11.Texture2DArray.ArraySize = res->m_depth_or_array;
		}
		else
			d11.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
	d.Format = d11.Format;
	UnorderedAccessView* v = new UnorderedAccessView();
	switch (d11.ViewDimension)
	{
	case D3D11_UAV_DIMENSION_BUFFER:
		d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		d.Buffer.FirstElement = d11.Buffer.FirstElement;
		d.Buffer.NumElements = d11.Buffer.NumElements;
		if (d11.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW)
		{
			d.Format = DXGI_FORMAT_R32_TYPELESS;
			d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		}
		if (res->m_misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
		{
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.Buffer.StructureByteStride = static_cast<Buffer*>(res)->m_desc.StructureByteStride;
		}
		SetViewRange(v, res, 0, 1, 0, 1, 0, 1);
		break;
	case D3D11_UAV_DIMENSION_TEXTURE1D:
		d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
		d.Texture1D.MipSlice = d11.Texture1D.MipSlice;
		SetViewRange(v, res, d11.Texture1D.MipSlice, 1, 0, UINT(-1), 0, 1);
		break;
	case D3D11_UAV_DIMENSION_TEXTURE2D:
		d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		d.Texture2D.MipSlice = d11.Texture2D.MipSlice;
		SetViewRange(v, res, d11.Texture2D.MipSlice, 1, 0, UINT(-1), 0, 1);
		break;
	case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
		d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		d.Texture2DArray.MipSlice = d11.Texture2DArray.MipSlice;
		d.Texture2DArray.FirstArraySlice = d11.Texture2DArray.FirstArraySlice;
		d.Texture2DArray.ArraySize = d11.Texture2DArray.ArraySize;
		SetViewRange(v, res, d11.Texture2DArray.MipSlice, 1, d11.Texture2DArray.FirstArraySlice,
		             d11.Texture2DArray.ArraySize, 0, 1);
		break;
	case D3D11_UAV_DIMENSION_TEXTURE3D:
		d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
		d.Texture3D.MipSlice = d11.Texture3D.MipSlice;
		d.Texture3D.FirstWSlice = d11.Texture3D.FirstWSlice;
		d.Texture3D.WSize = d11.Texture3D.WSize;
		SetViewRange(v, res, d11.Texture3D.MipSlice, 1, 0, 1, 0, 1);
		break;
	default:
		v->Release();
		Msg("! D3D12: unsupported UAV dimension %d", int(d11.ViewDimension));
		return E_INVALIDARG;
	}
	if (d.Format == DXGI_FORMAT_UNKNOWN && d.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
		d.Format = res->m_format;
	v->m_desc = d11;
	v->m_format = d.Format;
	v->m_res = res;
	res->AddRef();
	v->m_heap = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	v->m_slot = m_impl->srv_pool.Alloc(&v->m_cpu);
	m_device->CreateUnorderedAccessView(res->m_resource, nullptr, &d, v->m_cpu);
	*out = v;
	return S_OK;
}

//-------------------------------------------------------------------------------------------------------------------
// Input layouts, shaders

HRESULT Device::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void*, SIZE_T, InputLayout** out)
{
	InputLayout* l = new InputLayout();
	l->m_names.reserve(count);
	l->m_elements.resize(count);
	for (UINT i = 0; i < count; ++i)
	{
		l->m_names.push_back(elements[i].SemanticName);
		D3D12_INPUT_ELEMENT_DESC& e = l->m_elements[i];
		e.SemanticIndex = elements[i].SemanticIndex;
		e.Format = elements[i].Format;
		e.InputSlot = elements[i].InputSlot;
		e.AlignedByteOffset = elements[i].AlignedByteOffset;
		e.InputSlotClass = D3D12_INPUT_CLASSIFICATION(elements[i].InputSlotClass);
		e.InstanceDataStepRate = elements[i].InstanceDataStepRate;
	}
	u64 h = 0x4C41594F5554ull; // the layout's content
	for (UINT i = 0; i < count; ++i)
	{
		l->m_elements[i].SemanticName = l->m_names[i].c_str();
		const D3D12_INPUT_ELEMENT_DESC& e = l->m_elements[i];
		h = HashBytes(e.SemanticName, strlen(e.SemanticName) + 1, h);
		const UINT fields[6] = { e.SemanticIndex, UINT(e.Format), e.InputSlot, e.AlignedByteOffset, UINT(e.InputSlotClass), e.InstanceDataStepRate };
		h = HashBytes(fields, sizeof(fields), h);
	}
	l->m_content = h;
	*out = l;
	return S_OK;
}

template <class T>
static HRESULT CreateShaderObject(const void* code, SIZE_T size, Stage stage, T** out)
{
	*out = nullptr;
	if (!code || !size)
		return E_INVALIDARG;
	T* s = new T();
	s->m_code.assign(static_cast<const u8*>(code), static_cast<const u8*>(code) + size);
	s->m_stage = stage;
	s->m_crc = crc32(code, u32(size));
	s->m_content = HashBytes(code, size, 14695981039346656037ull + u64(stage));
	if (!ReflectShader(code, size, stage, s->m_bindings))
		Msg("! D3D12: shader reflection failed (stage %d, %u bytes)", int(stage), u32(size));
	*out = s;
	return S_OK;
}

HRESULT Device::CreateVertexShader(const void* code, SIZE_T size, void*, VertexShader** out) { return CreateShaderObject(code, size, StageVS, out); }
HRESULT Device::CreateHullShader(const void* code, SIZE_T size, void*, HullShader** out) { return CreateShaderObject(code, size, StageHS, out); }
HRESULT Device::CreateDomainShader(const void* code, SIZE_T size, void*, DomainShader** out) { return CreateShaderObject(code, size, StageDS, out); }
HRESULT Device::CreateGeometryShader(const void* code, SIZE_T size, void*, GeometryShader** out) { return CreateShaderObject(code, size, StageGS, out); }
HRESULT Device::CreatePixelShader(const void* code, SIZE_T size, void*, PixelShader** out) { return CreateShaderObject(code, size, StagePS, out); }
HRESULT Device::CreateComputeShader(const void* code, SIZE_T size, void*, ComputeShader** out) { return CreateShaderObject(code, size, StageCS, out); }

//-------------------------------------------------------------------------------------------------------------------
// State objects: one object per distinct description

template <class T, class D>
T* Device::Impl::FindState(const D& desc, u64 kind)
{
	const u64 h = HashBytes(&desc, sizeof(desc), kind * 0x9E3779B97F4A7C15ull + 1469598103934665603ull);
	std::lock_guard<std::mutex> lock(state_lock);
	std::vector<Object*>& bucket = states[h];
	for (Object* o : bucket)
	{
		T* t = static_cast<T*>(o);
		if (memcmp(&t->m_desc, &desc, sizeof(desc)) == 0)
		{
			t->AddRef();
			return t;
		}
	}
	T* t = new T();
	t->m_desc = desc;
	t->m_content = h;
	bucket.push_back(t);
	t->AddRef(); // one reference stays with the device
	return t;
}

HRESULT Device::CreateBlendState(const D3D11_BLEND_DESC* desc, BlendState** out)
{
	// Field by field into a zeroed description: the padding after the write masks takes part in the comparison
	D3D11_BLEND_DESC n;
	memset(&n, 0, sizeof(n));
	n.AlphaToCoverageEnable = desc->AlphaToCoverageEnable ? TRUE : FALSE;
	n.IndependentBlendEnable = desc->IndependentBlendEnable ? TRUE : FALSE;
	for (UINT i = 0; i < 8; ++i)
	{
		const D3D11_RENDER_TARGET_BLEND_DESC& s = desc->RenderTarget[i];
		D3D11_RENDER_TARGET_BLEND_DESC& t = n.RenderTarget[i];
		t.BlendEnable = s.BlendEnable ? TRUE : FALSE;
		t.SrcBlend = s.SrcBlend;
		t.DestBlend = s.DestBlend;
		t.BlendOp = s.BlendOp;
		t.SrcBlendAlpha = s.SrcBlendAlpha;
		t.DestBlendAlpha = s.DestBlendAlpha;
		t.BlendOpAlpha = s.BlendOpAlpha;
		t.RenderTargetWriteMask = s.RenderTargetWriteMask;
	}
	*out = m_impl->FindState<BlendState>(n, 1);
	return S_OK;
}

HRESULT Device::CreateRasterizerState(const D3D11_RASTERIZER_DESC* desc, RasterizerState** out)
{
	D3D11_RASTERIZER_DESC n = *desc;
	n.FrontCounterClockwise = n.FrontCounterClockwise ? TRUE : FALSE;
	n.DepthClipEnable = n.DepthClipEnable ? TRUE : FALSE;
	n.ScissorEnable = n.ScissorEnable ? TRUE : FALSE;
	n.MultisampleEnable = n.MultisampleEnable ? TRUE : FALSE;
	n.AntialiasedLineEnable = n.AntialiasedLineEnable ? TRUE : FALSE;
	*out = m_impl->FindState<RasterizerState>(n, 2);
	return S_OK;
}

HRESULT Device::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* desc, DepthStencilState** out)
{
	D3D11_DEPTH_STENCIL_DESC n;
	memset(&n, 0, sizeof(n));
	n.DepthEnable = desc->DepthEnable ? TRUE : FALSE;
	n.DepthWriteMask = desc->DepthWriteMask;
	n.DepthFunc = desc->DepthFunc;
	n.StencilEnable = desc->StencilEnable ? TRUE : FALSE;
	n.StencilReadMask = desc->StencilReadMask;
	n.StencilWriteMask = desc->StencilWriteMask;
	n.FrontFace = desc->FrontFace;
	n.BackFace = desc->BackFace;
	*out = m_impl->FindState<DepthStencilState>(n, 3);
	return S_OK;
}

HRESULT Device::CreateSamplerState(const D3D11_SAMPLER_DESC* desc, SamplerState** out)
{
	D3D11_SAMPLER_DESC n = *desc;
	n.MaxAnisotropy = _max(1u, _min(16u, n.MaxAnisotropy));
	SamplerState* s = m_impl->FindState<SamplerState>(n, 4);
	if (s->m_slot == UINT(-1))
	{
		std::lock_guard<std::mutex> lock(m_impl->state_lock);
		if (s->m_slot == UINT(-1))
		{
			D3D12_SAMPLER_DESC d = {};
			d.Filter = D3D12_FILTER(n.Filter);
			d.AddressU = D3D12_TEXTURE_ADDRESS_MODE(n.AddressU);
			d.AddressV = D3D12_TEXTURE_ADDRESS_MODE(n.AddressV);
			d.AddressW = D3D12_TEXTURE_ADDRESS_MODE(n.AddressW);
			d.MipLODBias = n.MipLODBias;
			d.MaxAnisotropy = n.MaxAnisotropy;
			d.ComparisonFunc = D3D12_COMPARISON_FUNC(n.ComparisonFunc ? n.ComparisonFunc : D3D11_COMPARISON_NEVER);
			memcpy(d.BorderColor, n.BorderColor, sizeof(d.BorderColor));
			d.MinLOD = n.MinLOD;
			d.MaxLOD = n.MaxLOD;
			D3D12_CPU_DESCRIPTOR_HANDLE h;
			const UINT slot = m_impl->sampler_pool.Alloc(&h);
			m_device->CreateSampler(&d, h);
			s->m_cpu = h;
			s->m_slot = slot;
		}
	}
	*out = s;
	return S_OK;
}

HRESULT Device::CreateQuery(const D3D11_QUERY_DESC* desc, Query** out)
{
	*out = nullptr;
	Query* q = new Query();
	q->m_desc = *desc;
	if (desc->Query == D3D11_QUERY_OCCLUSION || desc->Query == D3D11_QUERY_OCCLUSION_PREDICATE ||
	    desc->Query == D3D11_QUERY_TIMESTAMP)
	{
		std::lock_guard<std::mutex> lock(m_impl->query_lock);
		std::vector<UINT>& pool = desc->Query == D3D11_QUERY_TIMESTAMP ? m_impl->timestamp_free : m_impl->occlusion_free;
		if (pool.empty())
		{
			q->Release();
			DX12_WARN_ONCE("! D3D12: out of queries (type %d)", int(desc->Query));
			return E_OUTOFMEMORY;
		}
		q->m_index = pool.back();
		pool.pop_back();
	}
	*out = q;
	return S_OK;
}

HRESULT Device::CheckFormatSupport(DXGI_FORMAT format, UINT* support)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
	fs.Format = format;
	if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
	{
		*support = 0;
		return E_FAIL;
	}
	*support = UINT(fs.Support1); // D3D12_FORMAT_SUPPORT1 has the values of D3D11_FORMAT_SUPPORT
	return S_OK;
}

HRESULT Device::CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels)
{
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms = {};
	ms.Format = format;
	ms.SampleCount = samples;
	if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ms, sizeof(ms))))
	{
		*levels = 0;
		return E_FAIL;
	}
	*levels = ms.NumQualityLevels;
	return S_OK;
}

//-------------------------------------------------------------------------------------------------------------------
// Swap chain buffers

HRESULT Device::AttachSwapChain(IDXGISwapChain1* swap_chain)
{
	Impl& d = *m_impl;
	ReleaseBackBuffers();
	if (FAILED(swap_chain->QueryInterface(IID_PPV_ARGS(&d.swap_chain))))
	{
		Msg("! D3D12: the swap chain has no IDXGISwapChain3");
		return E_NOINTERFACE;
	}
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	swap_chain->GetDesc1(&sd);
	d.swap_chain_format = sd.Format;
	// HDR output: the renderer sees an SDR back buffer, sdr_target
	if (d.hdr_output)
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = sd.Width;
		td.Height = sd.Height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (sd.Format != DXGI_FORMAT_R10G10B10A2_UNORM || !d.CreateHdrPipeline(m_device) ||
		    FAILED(CreateTexture2D(&td, nullptr, &d.sdr_target)) || FAILED(CreateTexture2D(&td, nullptr, &d.hdr_preui)))
		{
			Msg("! D3D12: HDR output unavailable, SDR");
			d.hdr_output = false; // the 10-bit buffers take the SDR frame as it is
			if (d.sdr_target)
				d.sdr_target->Release();
			d.sdr_target = nullptr;
		}
		else
		{
			d.sdr_target->m_debug_name = "hdr_output_sdr";
			d.hdr_preui->m_debug_name = "hdr_output_pre_ui";
			d.sdr_rtv_slot = d.rtv_pool.Alloc(&d.sdr_rtv);
			m_device->CreateRenderTargetView(d.sdr_target->m_resource, nullptr, d.sdr_rtv);
			Msg("* D3D12: HDR output, the frame drawn in SDR and converted to HDR10 at Present");
		}
	}
	const DXGI_FORMAT seen_format = d.sdr_target ? DXGI_FORMAT_R8G8B8A8_UNORM : sd.Format;
	d.back_buffer_format = seen_format;
	for (UINT i = 0; i < sd.BufferCount; ++i)
	{
		ID3D12Resource* res = nullptr;
		if (FAILED(swap_chain->GetBuffer(i, IID_PPV_ARGS(&res))))
		{
			Msg("! D3D12: can't get swap chain buffer %u", i);
			return E_FAIL;
		}
		Texture2D* t = new Texture2D();
		t->m_desc.Width = sd.Width;
		t->m_desc.Height = sd.Height;
		t->m_desc.MipLevels = 1;
		t->m_desc.ArraySize = 1;
		t->m_desc.Format = sd.Format;
		t->m_desc.SampleDesc.Count = 1;
		t->m_desc.Usage = D3D11_USAGE_DEFAULT;
		t->m_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		FillTextureResource(t, D3D11_RESOURCE_DIMENSION_TEXTURE2D, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0,
		                    sd.Format, sd.Width, sd.Height, 1, 1, 1, 0);
		t->m_resource = res;
		t->m_back_buffer = true;
		t->m_state = D3D12_RESOURCE_STATE_PRESENT;
		res->SetName(L"xr back buffer");
		d.back_buffers.push_back(t);
		if (i == 0)
		{
			// GetBackBuffer's alias: the description of the buffers, no resource of its own
			if (!d.back_buffer_alias)
			{
				d.back_buffer_alias = new Texture2D();
				d.back_buffer_alias->m_back_buffer_alias = true;
				d.back_buffer_alias->m_debug_name = "backbuffer";
			}
			Texture2D* a = d.back_buffer_alias;
			a->m_desc = t->m_desc;
			a->m_desc.Format = seen_format;
			FillTextureResource(a, D3D11_RESOURCE_DIMENSION_TEXTURE2D, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0,
			                    seen_format, sd.Width, sd.Height, 1, 1, 1, 0);
		}

		D3D12_CPU_DESCRIPTOR_HANDLE h;
		d.back_buffer_rtv_slots.push_back(d.rtv_pool.Alloc(&h));
		d.back_buffer_rtv.push_back(h);
		m_device->CreateRenderTargetView(res, nullptr, h);
	}
	return S_OK;
}

// The conversion: the SDR frame as an SDR display shows it (gamma 2.2), SDR white at hdr_params[0] nits, Rec.709 to
// Rec.2020, PQ. Highlights toward the peak (hdr_params[1] nits):
// - scene highlights (hdr_params[6], a scene map this frame): the scene's brightness s in blend_soft's terms (GAMMA's
//   combine_2 shows s / (1 + s) before its grading). Up to the knee (hdr_params[5]) the HDR curve is that one, above
//   it blends into s / (1 + s / peak's ratio), which reaches the peak where SDR reaches white; the pixel's colour, as
//   graded, scaled by the curves' ratio, by hdr_params[4] (0: none). The sky (its textures hold nothing above white)
//   at hdr_params[7] times its SDR brightness, a dark sky left as it is. Only where the frame is as it was before the
//   UI (the UI stays at SDR white)
// - otherwise the frame's own brightness above mid-grey raised by hdr_params[2] (0: none, 1: SDR white at the peak)
// hdr_params[3], the right half against the left as it is: 1 the raise, 2 red where highlights are raised (blue: the
// sky), 3 everything at the peak's ratio (does brighter output show at all)
bool Device::Impl::CreateHdrPipeline(ID3D12Device* d3d)
{
	if (hdr_pipeline)
		return true;
	static const char kShader[] = R"(
Texture2D<float4> sdr : register(t0);
Texture2D<float4> pre_ui : register(t1);
Texture2D<float2> scene : register(t2);
cbuffer P : register(b0)
{
	float paper_white; float peak; float expand; float split; float highlights; float knee; float scene_on; float sky;
};
float4 vs(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 pq(float3 nits)
{
	const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	float3 y = pow(saturate(nits / 10000.0), m1);
	return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float hdr_curve(float s, float k, float g)
{
	const float y = s / (1.0 + s);
	return lerp(y, s / (1.0 + s / g), smoothstep(k, 1.0, y));
}
float4 ps(float4 pos : SV_Position) : SV_Target
{
	const float3 c0 = sdr.Load(int3(pos.xy, 0)).rgb;
	float3 c = pow(saturate(c0), 2.2);
	float w, h;
	sdr.GetDimensions(w, h);
	const bool left = split > 0.5 && pos.x < w * 0.5;
	if (split > 0.5 && abs(pos.x - w * 0.5) < 1.0)
		return float4(pq(float3(1, 1, 1) * paper_white), 1);
	const float gain = max(peak / paper_white, 1.0);
	const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
	float raise = 1.0, raise_sky = 1.0;
	if (scene_on > 0.5)
	{
		const float3 d = abs(c0 - pre_ui.Load(int3(pos.xy, 0)).rgb);
		if (max(d.r, max(d.g, d.b)) < 0.5 / 255.0 && gain > 1.0)
		{
			// The map is at the scene's resolution and jittered (TAA, upscaler): what is raised is decided from what
			// is stable, the final pixel. Its brightness: the map's (filtered) but no more than the pixel's own SDR
			// brightness gives (a dark pixel next to a bright one is not raised, wherever the jitter puts the edge);
			// the sky: the map's mask over 4x4 texels (a soft edge), on pixels bright enough to be sky
			float sw, sh;
			scene.GetDimensions(sw, sh);
			const float2 p = pos.xy * float2(sw / w, sh / h) - 0.5;
			const int2 i = int2(floor(p));
			const float2 f = p - i;
			const int2 m = int2(sw, sh) - 1;
			const float s00 = scene.Load(int3(clamp(i, 0, m), 0)).x, s10 = scene.Load(int3(clamp(i + int2(1, 0), 0, m), 0)).x;
			const float s01 = scene.Load(int3(clamp(i + int2(0, 1), 0, m), 0)).x, s11 = scene.Load(int3(clamp(i + 1, 0, m), 0)).x;
			const float s_map = max(0.0, lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
			const float s = min(s_map, lum / max(1.0 - lum, 0.001));
			float sky_mask = 0.0;
			[unroll] for (int ky = -1; ky <= 2; ++ky)
				[unroll] for (int kx = -1; kx <= 2; ++kx)
					sky_mask += scene.Load(int3(clamp(i + int2(kx, ky), 0, m), 0)).y;
			sky_mask *= 1.0 / 16.0;
			const float y = s / (1.0 + s);
			if (y > knee)
				raise = hdr_curve(s, knee, gain) / y;
			raise = lerp(1.0, raise, highlights);
			raise_sky = lerp(1.0, min(sky, gain), sky_mask * smoothstep(0.1, 0.3, lum));
		}
	}
	else
	{
		// Inverse tone mapping: luminance above mid-grey scaled up smoothly, SDR white reaching the peak at expand 1
		const float t = saturate((lum - 0.18) / (1.0 - 0.18));
		raise = lerp(1.0, gain, expand * pow(t, 1.25));
	}
	if (!left)
	{
		if (split > 2.5)
			c *= gain;
		else if (split > 1.5)
			c = lerp(lerp(c, float3(0, 0, 1), saturate((raise_sky - 1.0) * 2.0)), float3(1, 0, 0), saturate((raise - 1.0) * 4.0));
		else
			c *= max(raise, raise_sky);
	}
	c = mul(float3x3(0.6274040, 0.3292820, 0.0433136, 0.0690970, 0.9195400, 0.0113612, 0.0163916, 0.0880132, 0.8955950), c);
	return float4(pq(c * paper_white), 1);
}
)";
	ID3DBlob* vs = nullptr;
	ID3DBlob* ps = nullptr;
	ID3DBlob* err = nullptr;
	HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "hdr_output", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
	if (SUCCEEDED(hr))
		hr = D3DCompile(kShader, sizeof(kShader) - 1, "hdr_output", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
	if (FAILED(hr))
	{
		Msg("! D3D12: HDR output shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
		if (err)
			err->Release();
		if (vs)
			vs->Release();
		return false;
	}

	// The SDR frame (t0), the frame before the UI (t1), the scene's brightness (t2), the parameters (b0, root constants)
	D3D12_DESCRIPTOR_RANGE range = {};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 3;
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[0].DescriptorTable.NumDescriptorRanges = 1;
	params[0].DescriptorTable.pDescriptorRanges = &range;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[1].Constants.Num32BitValues = 8;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rd = {};
	rd.NumParameters = 2;
	rd.pParameters = params;
	ID3DBlob* blob = nullptr;
	hr = serialize_root_signature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
	if (SUCCEEDED(hr))
	{
		hr = d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&hdr_signature));
		blob->Release();
	}
	if (SUCCEEDED(hr))
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = hdr_signature;
		pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
		pd.SampleDesc.Count = 1;
		hr = d3d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&hdr_pipeline));
	}
	if (err)
		err->Release();
	vs->Release();
	ps->Release();
	if (FAILED(hr))
	{
		Msg("! D3D12: HDR output pipeline: 0x%08x", hr);
		if (hdr_signature)
			hdr_signature->Release();
		hdr_signature = nullptr;
		return false;
	}
	return true;
}

void Device::SetHdrOutput(bool on) { m_impl->hdr_output = on; }
bool Device::HdrOutput() const { return m_impl->hdr_output && m_impl->sdr_target; }
DXGI_FORMAT Device::SwapChainFormat() const { return m_impl->swap_chain_format; }

void Device::SetHdrParams(float paper_white, float peak, float expand, int split, float highlights, float knee, float sky)
{
	float* p = m_impl->hdr_params;
	p[0] = paper_white;
	p[1] = peak;
	p[2] = expand;
	p[3] = float(split);
	p[4] = highlights;
	p[5] = knee;
	p[7] = sky;
}

Texture2D* Device::HdrPreUiTarget() { return m_impl->hdr_output ? m_impl->hdr_preui : nullptr; }

D3D12_CPU_DESCRIPTOR_HANDLE Device::HdrTargetRTV(Resource* target)
{
	Impl& d = *m_impl;
	std::lock_guard<std::mutex> lock(d.hdr_lock);
	auto it = d.hdr_rtvs.find(target->m_resource);
	if (it != d.hdr_rtvs.end())
		return it->second.second;
	D3D12_CPU_DESCRIPTOR_HANDLE h;
	const UINT slot = d.rtv_pool.Alloc(&h);
	D3D12_RENDER_TARGET_VIEW_DESC rd = {};
	rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	m_device->CreateRenderTargetView(target->m_resource, &rd, h);
	d.hdr_rtvs[target->m_resource] = { slot, h };
	return h;
}

void Device::ReleaseBackBuffers()
{
	Impl& d = *m_impl;
	if (d.back_buffers.empty() && !d.swap_chain)
		return;
	if (m_context)
		m_context->ClearState();
	WaitIdle();
	for (Texture2D* t : d.back_buffers)
	{
		ID3D12Resource* res = t->m_resource;
		t->m_resource = nullptr; // released here, now: ResizeBuffers wants every reference gone
		t->Release();
		if (res)
			res->Release();
	}
	d.back_buffers.clear();
	for (UINT slot : d.back_buffer_rtv_slots)
		d.rtv_pool.Free(slot);
	d.back_buffer_rtv_slots.clear();
	d.back_buffer_rtv.clear();
	if (d.sdr_target)
	{
		d.sdr_target->Release();
		d.sdr_target = nullptr;
		d.rtv_pool.Free(d.sdr_rtv_slot);
	}
	if (d.hdr_preui)
	{
		d.hdr_preui->Release();
		d.hdr_preui = nullptr;
	}
	for (auto& r : d.hdr_rtvs)
		d.rtv_pool.Free(r.second.first);
	d.hdr_rtvs.clear();
	if (d.swap_chain)
	{
		d.swap_chain->Release();
		d.swap_chain = nullptr;
	}
	d.ProcessDeferred(0, true);
}

Texture2D* Device::CurrentBackBuffer()
{
	if (m_impl->sdr_target)
		return m_impl->sdr_target;
	return SwapChainBuffer(nullptr);
}

D3D12_CPU_DESCRIPTOR_HANDLE Device::CurrentBackBufferRTV()
{
	if (m_impl->sdr_target)
		return m_impl->sdr_rtv;
	D3D12_CPU_DESCRIPTOR_HANDLE h = { 0 };
	SwapChainBuffer(&h);
	return h;
}

Texture2D* Device::SwapChainBuffer(D3D12_CPU_DESCRIPTOR_HANDLE* rtv)
{
	Impl& d = *m_impl;
	if (!d.swap_chain || d.back_buffers.empty())
		return nullptr;
	const UINT i = d.swap_chain->GetCurrentBackBufferIndex();
	if (rtv && !d.back_buffer_rtv.empty())
		*rtv = d.back_buffer_rtv[i % d.back_buffer_rtv.size()];
	return d.back_buffers[i % d.back_buffers.size()];
}

HRESULT Device::CreateBackBufferRTV(RenderTargetView** out)
{
	RenderTargetView* v = new RenderTargetView();
	v->m_back_buffer = true;
	v->m_format = m_impl->back_buffer_format;
	v->m_desc.Format = v->m_format;
	v->m_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	v->m_heap = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	*out = v;
	return S_OK;
}

Texture2D* Device::BackBufferAlias() { return m_impl->back_buffer_alias; }

HRESULT Device::GetBackBuffer(Texture2D** out)
{
	Texture2D* t = (m_impl->swap_chain && !m_impl->back_buffers.empty()) ? m_impl->back_buffer_alias : nullptr;
	if (!t)
	{
		*out = nullptr;
		return E_FAIL;
	}
	t->AddRef();
	*out = t;
	return S_OK;
}
} // namespace dx12
