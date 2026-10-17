#include "stdafx.h"
#include "r4_gpu_memory.h"

#include <dxgi1_4.h>
#include <atomic>
#include <algorithm>

namespace gpu_memory
{
namespace
{
	// {5B1C3C8E-6A41-4C1F-9E57-0D2B8A6F4C31}
	const GUID kTicketGuid = { 0x5b1c3c8e, 0x6a41, 0x4c1f, { 0x9e, 0x57, 0x0d, 0x2b, 0x8a, 0x6f, 0x4c, 0x31 } };

	const u32 kNameSize = 96;

	// One per counted resource, held by the resource as private data and released by D3D with it
	struct Ticket final : public IUnknown
	{
		std::atomic<ULONG> refs;
		category cat;
		u64 bytes;
		u32 width, height;
		u16 format;
		u8 mips;
		Ticket* prev;
		Ticket* next;
		char name[kNameSize];

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (!out)
				return E_POINTER;
			if (riid == __uuidof(IUnknown))
			{
				*out = static_cast<IUnknown*>(this);
				AddRef();
				return S_OK;
			}
			*out = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
		ULONG STDMETHODCALLTYPE Release() override;
	};

	// Only trivially destructible state: resources leaked past the DLL's static destructors still release
	// their tickets at exit
	std::atomic<u64> s_bytes[count];
	std::atomic<u32> s_resources[count];
	SRWLOCK s_lock = SRWLOCK_INIT;
	Ticket* s_head = nullptr;

	thread_local category t_scope_cat = automatic;
	thread_local const char* t_scope_name = nullptr;

	ULONG STDMETHODCALLTYPE Ticket::Release()
	{
		const ULONG left = --refs;
		if (left)
			return left;

		AcquireSRWLockExclusive(&s_lock);
		if (prev)
			prev->next = next;
		else
			s_head = next;
		if (next)
			next->prev = prev;
		s_bytes[cat] -= bytes;
		--s_resources[cat];
		ReleaseSRWLockExclusive(&s_lock);

		this->~Ticket();
		free(this);
		return 0;
	}

	bool is_block_compressed(DXGI_FORMAT f)
	{
		return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
			(f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
	}

	u32 bits_per_pixel(DXGI_FORMAT f)
	{
		if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return 128;
		if (f >= DXGI_FORMAT_R32G32B32_TYPELESS && f <= DXGI_FORMAT_R32G32B32_SINT) return 96;
		if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) return 64;
		if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) return 32;
		if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return 16;
		if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return 8;
		if (f == DXGI_FORMAT_R1_UNORM) return 1;
		if (f == DXGI_FORMAT_R8G8_B8G8_UNORM || f == DXGI_FORMAT_G8R8_G8B8_UNORM) return 16;
		if ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) ||
			(f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM)) return 4;
		if (is_block_compressed(f)) return 8;
		if (f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM || f == DXGI_FORMAT_B4G4R4A4_UNORM) return 16;
		return 32; // B8G8R8A8 family, R9G9B9E5 and anything rare
	}

	u64 texture_bytes(DXGI_FORMAT f, u32 w, u32 h, u32 d, u32 mips, u32 layers, u32 samples)
	{
		const bool bc = is_block_compressed(f);
		const u64 bpp = bits_per_pixel(f);
		u64 total = 0;
		for (u32 i = 0; i < _max(mips, 1u); ++i)
		{
			const u64 mw = _max(w >> i, 1u), mh = _max(h >> i, 1u), md = _max(d >> i, 1u);
			if (bc)
				total += ((mw + 3) / 4) * ((mh + 3) / 4) * md * (bpp * 2); // 4x4 blocks of 8 or 16 bytes
			else
				total += (mw * mh * md * bpp + 7) / 8;
		}
		return total * _max(layers, 1u) * _max(samples, 1u);
	}

	category texture_category(D3D11_USAGE usage, UINT bind, DXGI_FORMAT f, u32 mips)
	{
		if (usage == D3D11_USAGE_STAGING)
			return tex_staging;
		if (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS))
			return render_target;
		if (is_block_compressed(f))
			return mips > 1 ? tex_bc : tex_bc_nomips;
		return mips > 1 ? tex_raw : tex_raw_nomips;
	}

	// Fills size, category and description; false for resource types not counted
	bool describe(ID3DResource* res, category cat, Ticket& t)
	{
		D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
		res->GetType(&dim);
		t.width = t.height = 0;
		t.format = 0;
		t.mips = 0;

		switch (dim)
		{
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			{
				D3D11_BUFFER_DESC d;
				static_cast<ID3DBuffer*>(res)->GetDesc(&d);
				t.bytes = d.ByteWidth;
				t.cat = cat != automatic ? cat : (t_scope_cat != automatic ? t_scope_cat : buf_other);
				return true;
			}
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			{
				D3D11_TEXTURE1D_DESC d;
				static_cast<ID3DTexture1D*>(res)->GetDesc(&d);
				t.bytes = texture_bytes(d.Format, d.Width, 1, 1, d.MipLevels, d.ArraySize, 1);
				t.cat = texture_category(d.Usage, d.BindFlags, d.Format, d.MipLevels);
				t.width = d.Width;
				t.height = 1;
				t.format = u16(d.Format);
				t.mips = u8(_min(d.MipLevels, 255u));
				return true;
			}
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			{
				D3D11_TEXTURE2D_DESC d;
				static_cast<ID3DTexture2D*>(res)->GetDesc(&d);
				t.bytes = texture_bytes(d.Format, d.Width, d.Height, 1, d.MipLevels, d.ArraySize, d.SampleDesc.Count);
				t.cat = texture_category(d.Usage, d.BindFlags, d.Format, d.MipLevels);
				t.width = d.Width;
				t.height = d.Height;
				t.format = u16(d.Format);
				t.mips = u8(_min(d.MipLevels, 255u));
				return true;
			}
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			{
				D3D11_TEXTURE3D_DESC d;
				static_cast<ID3DTexture3D*>(res)->GetDesc(&d);
				t.bytes = texture_bytes(d.Format, d.Width, d.Height, d.Depth, d.MipLevels, 1, 1);
				t.cat = texture_category(d.Usage, d.BindFlags, d.Format, d.MipLevels);
				t.width = d.Width;
				t.height = d.Height;
				t.format = u16(d.Format);
				t.mips = u8(_min(d.MipLevels, 255u));
				return true;
			}
		default:
			return false;
		}
	}

	const char* format_label(u16 f, string32& buf)
	{
		switch (DXGI_FORMAT(f))
		{
		case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS: return "BC1";
		case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC2_TYPELESS: return "BC2";
		case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS: return "BC3";
		case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: case DXGI_FORMAT_BC4_TYPELESS: return "BC4";
		case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC5_TYPELESS: return "BC5";
		case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16: case DXGI_FORMAT_BC6H_TYPELESS: return "BC6H";
		case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: case DXGI_FORMAT_BC7_TYPELESS: return "BC7";
		case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8";
		case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "BGRA8";
		case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "BGRX8";
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
		case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
		case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: return "R32F";
		case DXGI_FORMAT_R16_FLOAT: return "R16F";
		case DXGI_FORMAT_R8_UNORM: return "R8";
		case DXGI_FORMAT_R8G8_UNORM: return "RG8";
		case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
		case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
		case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
		default:
			xr_sprintf(buf, "fmt%u", u32(f));
			return buf;
		}
	}

	float mb(u64 bytes) { return float(double(bytes) / (1024.0 * 1024.0)); }
	float gb(u64 bytes) { return float(double(bytes) / (1024.0 * 1024.0 * 1024.0)); }

	struct Entry
	{
		category cat;
		u64 bytes;
		u32 width, height;
		u16 format;
		u8 mips;
		u32 resources;
		char name[kNameSize];
	};

	// Adds up entries with the same name (a model's vertex and index buffers), largest first
	void merge_by_name(xr_vector<Entry>& list)
	{
		std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return xr_strcmp(a.name, b.name) < 0; });
		u32 out = 0;
		for (u32 i = 0; i < list.size(); ++i)
		{
			if (out && !xr_strcmp(list[out - 1].name, list[i].name))
			{
				list[out - 1].bytes += list[i].bytes;
				list[out - 1].resources += list[i].resources;
			}
			else
				list[out++] = list[i];
		}
		list.resize(out);
		std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
	}

	void print_textures(const char* title, xr_vector<Entry>& list, u32 top_n)
	{
		merge_by_name(list);
		Msg("* [GPU mem] %s:", title);
		string32 fmt;
		for (u32 i = 0; i < _min(top_n, u32(list.size())); ++i)
		{
			const Entry& e = list[i];
			Msg("*   %7.1f MB  %5ux%-5u %-8s %2u mips  %s", mb(e.bytes), e.width, e.height, format_label(e.format, fmt),
			    u32(e.mips), e.name[0] ? e.name : "(unnamed)");
		}
	}
}

u64 totals::gpu() const
{
	u64 sum = 0;
	for (u32 i = 0; i < count; ++i)
		if (i != tex_staging)
			sum += bytes[i];
	return sum;
}

void track(ID3DResource* res, const char* name, category cat)
{
	if (!res)
		return;

	// Quick way out for resources counted already: some surfaces are set on textures every frame (sky, envmaps)
	UINT size = 0;
	if (SUCCEEDED(res->GetPrivateData(kTicketGuid, &size, nullptr)) && size)
		return;

	void* mem = malloc(sizeof(Ticket));
	if (!mem)
		return;
	Ticket* t = new (mem) Ticket();
	t->refs = 1;
	t->prev = t->next = nullptr;
	if (!describe(res, cat, *t))
	{
		t->~Ticket();
		free(t);
		return;
	}

	// Keep the end of long paths, it holds the file name
	if (!name)
		name = t_scope_name;
	t->name[0] = 0;
	if (name)
	{
		const size_t len = xr_strlen(name);
		xr_strcpy(t->name, name + (len >= kNameSize ? len - kNameSize + 1 : 0));
	}

	// Check and attach under the lock: a surface can be handed to the counter from several threads
	AcquireSRWLockExclusive(&s_lock);
	size = 0;
	const bool counted = SUCCEEDED(res->GetPrivateData(kTicketGuid, &size, nullptr)) && size;
	if (counted || FAILED(res->SetPrivateDataInterface(kTicketGuid, t)))
	{
		ReleaseSRWLockExclusive(&s_lock);
		t->~Ticket();
		free(t);
		return;
	}
	t->next = s_head;
	if (s_head)
		s_head->prev = t;
	s_head = t;
	s_bytes[t->cat] += t->bytes;
	++s_resources[t->cat];
	ReleaseSRWLockExclusive(&s_lock);

	t->Release(); // the resource holds the remaining reference
}

scope::scope(category cat, const char* name) : m_prev_cat(t_scope_cat), m_prev_name(t_scope_name)
{
	t_scope_cat = cat;
	t_scope_name = name;
}

scope::~scope()
{
	t_scope_cat = m_prev_cat;
	t_scope_name = m_prev_name;
}

void get_totals(totals& out)
{
	for (u32 i = 0; i < count; ++i)
	{
		out.bytes[i] = s_bytes[i].load(std::memory_order_relaxed);
		out.resources[i] = s_resources[i].load(std::memory_order_relaxed);
	}
}

void dump(u32 top_n)
{
	totals t;
	get_totals(t);

	// What the driver reports for the whole device, including memory the engine doesn't create itself
	u64 used = 0, budget = 0;
	IDXGIAdapter3* adapter3 = nullptr;
	if (HW.m_pAdapter && SUCCEEDED(HW.m_pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3)))
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
		if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
		{
			used = info.CurrentUsage;
			budget = info.Budget;
		}
		adapter3->Release();
	}

	const u64 counted = t.gpu();
	if (used)
		Msg("* [GPU mem] in use %.2f GB, budget %.2f GB, counted %.2f GB, not counted %.2f GB (driver, DXVK, upscaler, shaders)",
		    gb(used), gb(budget), gb(counted), used > counted ? gb(used - counted) : 0.f);
	else
		Msg("* [GPU mem] counted %.2f GB (driver usage not available)", gb(counted));

	Msg("* [GPU mem] textures %.2f GB: BC %.2f GB (%u), BC without mips %.2f GB (%u), uncompressed %.2f GB (%u), uncompressed without mips %.2f GB (%u)",
	    gb(t.textures()), gb(t.bytes[tex_bc]), t.resources[tex_bc], gb(t.bytes[tex_bc_nomips]), t.resources[tex_bc_nomips],
	    gb(t.bytes[tex_raw]), t.resources[tex_raw], gb(t.bytes[tex_raw_nomips]), t.resources[tex_raw_nomips]);
	Msg("* [GPU mem] render targets %.2f GB (%u)", gb(t.bytes[render_target]), t.resources[render_target]);
	Msg("* [GPU mem] geometry %.2f GB: level %.2f GB (%u buffers), models %.2f GB (%u), grass %.2f GB (%u)",
	    gb(t.geometry()), gb(t.bytes[buf_level]), t.resources[buf_level], gb(t.bytes[buf_model]), t.resources[buf_model],
	    gb(t.bytes[buf_grass]), t.resources[buf_grass]);
	Msg("* [GPU mem] dynamic buffers %.1f MB (%u), other buffers %.1f MB (%u)", mb(t.bytes[buf_dynamic]), t.resources[buf_dynamic],
	    mb(t.bytes[buf_other]), t.resources[buf_other]);
	if (t.resources[tex_staging])
		Msg("* [GPU mem] CPU-side texture copies (not yet on the GPU) %.2f GB (%u)", gb(t.bytes[tex_staging]), t.resources[tex_staging]);

	if (!top_n)
		return;

	// Copy under the lock, sort and print without it
	xr_vector<Entry> textures, raw_textures, models, targets;
	AcquireSRWLockShared(&s_lock);
	for (const Ticket* k = s_head; k; k = k->next)
	{
		Entry e;
		e.cat = k->cat;
		e.bytes = k->bytes;
		e.width = k->width;
		e.height = k->height;
		e.format = k->format;
		e.mips = k->mips;
		e.resources = 1;
		xr_strcpy(e.name, k->name);

		switch (k->cat)
		{
		case tex_raw:
		case tex_raw_nomips:
			raw_textures.push_back(e);
			// fall through
		case tex_bc:
		case tex_bc_nomips:
			textures.push_back(e);
			break;
		case render_target:
			targets.push_back(e);
			break;
		case buf_model:
			models.push_back(e);
			break;
		default:
			break;
		}
	}
	ReleaseSRWLockShared(&s_lock);

	print_textures("largest textures", textures, top_n);
	print_textures("largest uncompressed textures", raw_textures, top_n);

	merge_by_name(models);
	Msg("* [GPU mem] largest models (vertex and index buffers, %u models):", u32(models.size()));
	for (u32 i = 0; i < _min(top_n, u32(models.size())); ++i)
		Msg("*   %7.1f MB  %3u buffers  %s", mb(models[i].bytes), models[i].resources, models[i].name[0] ? models[i].name : "(unnamed)");

	print_textures("largest render targets", targets, top_n);
}
}
