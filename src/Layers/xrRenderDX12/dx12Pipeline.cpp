// D3D12 backend: shader reflection, root signatures from the reflected bindings, pipeline states (see dx12.h)
#include "stdafx.h"
#pragma hdrstop

#include "dx12Internal.h"
#include <D3DCompiler.h>

namespace shader_progress { void begin(const char* what, u32 total); void step(); void end(bool completed); } // r4_perf_overlay.cpp

namespace dx12
{
bool ReflectShader(const void* code, SIZE_T size, Stage stage, ShaderBindings& b)
{
	b = ShaderBindings();
	ID3D11ShaderReflection* refl = nullptr;
	if (FAILED(D3DReflect(code, size, IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&refl))) || !refl)
		return false;
	D3D11_SHADER_DESC sd = {};
	refl->GetDesc(&sd);
	for (UINT i = 0; i < sd.BoundResources; ++i)
	{
		D3D11_SHADER_INPUT_BIND_DESC bd = {};
		if (FAILED(refl->GetResourceBindingDesc(i, &bd)))
			continue;
		const UINT count = bd.BindCount ? bd.BindCount : 1;
		for (UINT k = 0; k < count; ++k)
		{
			const UINT slot = bd.BindPoint + k;
			switch (bd.Type)
			{
			case D3D_SIT_CBUFFER:
				if (slot < kMaxCB)
					b.cb_mask |= u16(1u << slot);
				else
					DX12_WARN_ONCE("! D3D12: constant buffer slot %u of '%s' is out of range", slot, bd.Name);
				break;
			case D3D_SIT_TBUFFER:
			case D3D_SIT_TEXTURE:
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
				if (slot < kMaxSRV)
				{
					UINT dim = bd.Dimension;
					if (bd.Type != D3D_SIT_TEXTURE || dim == D3D_SRV_DIMENSION_BUFFEREX || dim == D3D_SRV_DIMENSION_UNKNOWN)
						dim = D3D_SRV_DIMENSION_BUFFER;
					b.srv_dim[slot] = u8(dim);
					b.srv_count = u8(_max(UINT(b.srv_count), slot + 1));
				}
				else
					DX12_WARN_ONCE("! D3D12: texture slot %u of '%s' is out of range", slot, bd.Name);
				break;
			case D3D_SIT_SAMPLER:
				if (slot < kMaxSamplers)
					b.sampler_count = u8(_max(UINT(b.sampler_count), slot + 1));
				break;
			case D3D_SIT_UAV_RWTYPED:
			case D3D_SIT_UAV_RWSTRUCTURED:
			case D3D_SIT_UAV_RWBYTEADDRESS:
			case D3D_SIT_UAV_APPEND_STRUCTURED:
			case D3D_SIT_UAV_CONSUME_STRUCTURED:
			case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
				if (stage != StageCS)
				{
					DX12_WARN_ONCE("! D3D12: UAVs outside compute shaders are not supported ('%s')", bd.Name);
					break;
				}
				if (slot < kMaxUAV)
				{
					UINT dim = bd.Dimension;
					if (bd.Type != D3D_SIT_UAV_RWTYPED)
						dim = D3D12_UAV_DIMENSION_BUFFER;
					b.uav_dim[slot] = u8(dim);
					b.uav_count = u8(_max(UINT(b.uav_count), slot + 1));
				}
				break;
			default:
				break;
			}
		}
	}
	refl->Release();
	return true;
}

static D3D12_SHADER_VISIBILITY Visibility(UINT stage)
{
	switch (stage)
	{
	case StageVS: return D3D12_SHADER_VISIBILITY_VERTEX;
	case StageHS: return D3D12_SHADER_VISIBILITY_HULL;
	case StageDS: return D3D12_SHADER_VISIBILITY_DOMAIN;
	case StageGS: return D3D12_SHADER_VISIBILITY_GEOMETRY;
	case StagePS: return D3D12_SHADER_VISIBILITY_PIXEL;
	default: return D3D12_SHADER_VISIBILITY_ALL;
	}
}

static UINT PopCount16(u16 v)
{
	UINT n = 0;
	for (; v; v &= v - 1)
		++n;
	return n;
}

RootSignature* Device::Impl::GetRootSignature(const RootLayoutKey& key)
{
	std::lock_guard<std::mutex> lock(pipeline_lock);
	auto it = root_signatures.find(key);
	if (it != root_signatures.end())
		return it->second;

	RootSignature* rs = new RootSignature();
	rs->id = Object::NewId();
	rs->compute = key.stage[StageCS] != 0;
	const UINT first = rs->compute ? StageCS : StageVS;
	const UINT last = rs->compute ? StageCS : StagePS;

	UINT cost = 0;
	for (UINT s = first; s <= last; ++s)
	{
		const u64 k = key.stage[s];
		if (!k)
			continue;
		cost += 2 * PopCount16(u16(k >> 1)) + (((k >> 17) & 0xFF) ? 1 : 0) + (((k >> 25) & 0xFF) ? 1 : 0) +
			(((k >> 33) & 0xFF) ? 1 : 0);
	}
	const bool cbv_tables = cost > 64;

	std::vector<D3D12_ROOT_PARAMETER> params;
	std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
	ranges.reserve(StageCount * 4);
	params.reserve(64);
	auto add_table = [&](D3D12_DESCRIPTOR_RANGE_TYPE type, UINT count, UINT stage) -> u8 {
		D3D12_DESCRIPTOR_RANGE r = {};
		r.RangeType = type;
		r.NumDescriptors = count;
		r.BaseShaderRegister = 0;
		r.RegisterSpace = 0;
		r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		ranges.push_back(r);
		D3D12_ROOT_PARAMETER p = {};
		p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p.DescriptorTable.NumDescriptorRanges = 1;
		p.DescriptorTable.pDescriptorRanges = &ranges.back();
		p.ShaderVisibility = Visibility(stage);
		params.push_back(p);
		return u8(params.size() - 1);
	};

	for (UINT s = first; s <= last; ++s)
	{
		const u64 k = key.stage[s];
		RootSignature::StageParams& sp = rs->stages[s];
		memset(sp.cb_param, 0xFF, sizeof(sp.cb_param));
		if (!k)
			continue;
		sp.present = true;
		sp.cb_mask = u16(k >> 1);
		const UINT srv = UINT((k >> 17) & 0xFF), smp = UINT((k >> 25) & 0xFF), uav = UINT((k >> 33) & 0xFF);
		if (sp.cb_mask)
		{
			if (!cbv_tables)
			{
				for (UINT slot = 0; slot < kMaxCB; ++slot)
				{
					if (!(sp.cb_mask & (1u << slot)))
						continue;
					D3D12_ROOT_PARAMETER p = {};
					p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
					p.Descriptor.ShaderRegister = slot;
					p.Descriptor.RegisterSpace = 0;
					p.ShaderVisibility = Visibility(s);
					params.push_back(p);
					sp.cb_param[slot] = u8(params.size() - 1);
				}
			}
			else
			{
				UINT count = 0;
				for (UINT slot = 0; slot < kMaxCB; ++slot)
					if (sp.cb_mask & (1u << slot))
						count = slot + 1;
				sp.cbv_table_count = u8(count);
				sp.cbv_table_param = add_table(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, count, s);
			}
		}
		if (srv)
		{
			sp.srv_count = u8(srv);
			sp.srv_param = add_table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srv, s);
		}
		if (smp)
		{
			sp.sampler_count = u8(smp);
			sp.sampler_param = add_table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, smp, s);
		}
		if (uav && rs->compute)
		{
			sp.uav_count = u8(uav);
			sp.uav_param = add_table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uav, s);
		}
	}

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = UINT(params.size());
	desc.pParameters = params.data();
	if (!rs->compute)
	{
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		if (!key.stage[StageHS])
			desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
		if (!key.stage[StageDS])
			desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
		if (!key.stage[StageGS])
			desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
		if (!key.stage[StagePS])
			desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
	}
	ID3DBlob* blob = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
	if (FAILED(hr))
	{
		Msg("! D3D12: root signature serialization failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
		if (errors)
			errors->Release();
		delete rs;
		root_signatures[key] = nullptr;
		return nullptr;
	}
	if (errors)
		errors->Release();
	hr = TheDevice()->m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs->signature));
	blob->Release();
	if (FAILED(hr))
	{
		Msg("! D3D12: CreateRootSignature failed: 0x%08x", hr);
		delete rs;
		root_signatures[key] = nullptr;
		return nullptr;
	}
	root_signatures[key] = rs;
	return rs;
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(UINT t)
{
	return D3D12_PRIMITIVE_TOPOLOGY_TYPE(t);
}


//-------------------------------------------------------------------------------------------------------------------
// Stage linkage. D3D11 drew with a pixel shader reading inputs its vertex shader doesn't write (GAMMA:
// models_selflight.vs with accum_emissive.ps, ssfx_water.vs writing TEXCOORD2.xyz to a float4 input); D3D12 refuses
// such a pipeline. The vertex shader gets a copy declaring the missing outputs and setting them to 0 (D3D11 left them
// undefined); the shaders on disk are untouched

namespace
{
const u32 kFourccDXBC = MAKEFOURCC('D', 'X', 'B', 'C');
const u32 kFourccISGN = MAKEFOURCC('I', 'S', 'G', 'N');
const u32 kFourccOSGN = MAKEFOURCC('O', 'S', 'G', 'N');
const u32 kFourccSHEX = MAKEFOURCC('S', 'H', 'E', 'X');
const u32 kFourccSHDR = MAKEFOURCC('S', 'H', 'D', 'R');

u32 ReadU32(const u8* p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

void AppendU32(std::vector<u8>& out, u32 v)
{
	const u8* p = reinterpret_cast<const u8*>(&v);
	out.insert(out.end(), p, p + 4);
}

struct DxbcChunk
{
	u32 fourcc;
	std::vector<u8> data;
};

bool ReadContainer(const std::vector<u8>& code, std::vector<DxbcChunk>& chunks)
{
	const size_t size = code.size();
	if (size < 32 || ReadU32(code.data()) != kFourccDXBC)
		return false;
	const u32 count = ReadU32(&code[28]);
	if (32 + size_t(count) * 4 > size)
		return false;
	for (u32 i = 0; i < count; ++i)
	{
		const u32 offset = ReadU32(&code[32 + i * 4]);
		if (size_t(offset) + 8 > size)
			return false;
		const u32 chunk_size = ReadU32(&code[offset + 4]);
		if (size_t(offset) + 8 + chunk_size > size)
			return false;
		chunks.push_back({ ReadU32(&code[offset]), std::vector<u8>(code.begin() + offset + 8, code.begin() + offset + 8 + chunk_size) });
	}
	return true;
}

const DxbcChunk* FindChunk(const std::vector<DxbcChunk>& chunks, u32 fourcc)
{
	for (const DxbcChunk& c : chunks)
		if (c.fourcc == fourcc)
			return &c;
	return nullptr;
}

// MD5 with the container's own padding (the bit count leads the last block), over everything after the checksum
void DxbcChecksum(const u8* data, size_t size, u32 out[4])
{
	static const u32 K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
		0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
		0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
		0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
		0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
		0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
	static const u32 R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
	u32 st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	auto block = [&](const u8* b) {
		u32 m[16];
		memcpy(m, b, 64);
		u32 a = st[0], bb = st[1], c = st[2], d = st[3];
		for (u32 i = 0; i < 64; ++i)
		{
			u32 f, g;
			if (i < 16)
				f = (bb & c) | (~bb & d), g = i;
			else if (i < 32)
				f = (d & bb) | (~d & c), g = (5 * i + 1) & 15;
			else if (i < 48)
				f = bb ^ c ^ d, g = (3 * i + 5) & 15;
			else
				f = c ^ (bb | ~d), g = (7 * i) & 15;
			f += a + K[i] + m[g];
			a = d;
			d = c;
			c = bb;
			const u32 s = R[(i >> 4) * 4 + (i & 3)];
			bb += (f << s) | (f >> (32 - s));
		}
		st[0] += a, st[1] += bb, st[2] += c, st[3] += d;
	};
	const size_t full = size & ~size_t(63);
	for (size_t o = 0; o < full; o += 64)
		block(data + o);
	const size_t rest = size - full;
	const u32 bits = u32(size * 8);
	const u32 tail = (bits >> 2) | 1;
	u8 last[64] = {};
	if (rest >= 56)
	{
		memcpy(last, data + full, rest);
		last[rest] = 0x80;
		block(last);
		memset(last, 0, 64);
		memcpy(last, &bits, 4);
		memcpy(last + 60, &tail, 4);
		block(last);
	}
	else
	{
		memcpy(last, &bits, 4);
		memcpy(last + 4, data + full, rest);
		last[4 + rest] = 0x80;
		memcpy(last + 60, &tail, 4);
		block(last);
	}
	memcpy(out, st, 16);
}

struct SigElement
{
	std::string name;
	u32 index, system_value, component_type, reg;
	u8 mask, rw_mask;
};

// ISGN/OSGN: count, 8, then 24 byte elements; names follow, at offsets from the chunk start
bool ReadSignature(const DxbcChunk& c, std::vector<SigElement>& out)
{
	if (c.data.size() < 8)
		return false;
	const u32 count = ReadU32(c.data.data());
	if (8 + size_t(count) * 24 > c.data.size())
		return false;
	for (u32 i = 0; i < count; ++i)
	{
		const u8* e = &c.data[8 + i * 24];
		const u32 name_offset = ReadU32(e);
		if (name_offset >= c.data.size())
			return false;
		const char* name = reinterpret_cast<const char*>(&c.data[name_offset]);
		const size_t len = strnlen(name, c.data.size() - name_offset);
		if (name_offset + len >= c.data.size())
			return false;
		out.push_back({ std::string(name, len), ReadU32(e + 4), ReadU32(e + 8), ReadU32(e + 12), ReadU32(e + 16), e[20], e[21] });
	}
	return true;
}

std::vector<u8> WriteSignature(const std::vector<SigElement>& elements)
{
	std::vector<u8> out;
	AppendU32(out, u32(elements.size()));
	AppendU32(out, 8);
	u32 name_offset = 8 + u32(elements.size()) * 24;
	for (const SigElement& e : elements)
	{
		AppendU32(out, name_offset);
		AppendU32(out, e.index);
		AppendU32(out, e.system_value);
		AppendU32(out, e.component_type);
		AppendU32(out, e.reg);
		out.push_back(e.mask);
		out.push_back(e.rw_mask);
		out.push_back(0);
		out.push_back(0);
		name_offset += u32(e.name.size() + 1);
	}
	for (const SigElement& e : elements)
		out.insert(out.end(), e.name.c_str(), e.name.c_str() + e.name.size() + 1);
	while (out.size() & 3)
		out.push_back(0);
	return out;
}

bool IsDeclaration(u32 opcode)
{
	// customdata (immediate constant buffer), the SM4 declarations, the SM5 ones (dcl_stream .. dcl_resource_structured,
	// dcl_gs_instance_count)
	return opcode == 0x35 || (opcode >= 0x58 && opcode <= 0x6a) || (opcode >= 0x8f && opcode <= 0xa2) || opcode == 0xce;
}

const u32 kOpMov = 0x36;
const u32 kOpDclOutput = 0x65;
const u32 kOperandOutput = 0x00102002; // o#, one immediate index, 4 components, mask selection (mask in bits 4..7)
const u32 kOperandImmediate4 = 0x00004002;

struct OutputFix
{
	u32 reg;
	u8 mask; // components to declare and set to 0
	u8 old_mask; // of the element whose mask grows (0: a new element)
};
} // namespace

// Output register moves of LinkVertexOutputs
struct RegisterMove
{
	u32 from, to;
};

bool MoveRegister(u32& reg, const std::vector<RegisterMove>& moves)
{
	for (const RegisterMove& m : moves)
		if (reg == m.from)
		{
			reg = m.to;
			return true;
		}
	return false;
}

// One operand at p[i] (and its index operands); output register indices (o#) renumbered. False on a layout this
// doesn't know or an output indexed by a register
bool RemapOperand(std::vector<u32>& p, u32& i, u32 end, const std::vector<RegisterMove>& moves)
{
	if (i >= end)
		return false;
	const u32 tok = p[i++];
	for (u32 ext = tok; ext & 0x80000000u;)
	{
		if (i >= end)
			return false;
		ext = p[i++];
	}
	const u32 type = (tok >> 12) & 0xff;
	const u32 components = tok & 3;
	if (type == 4 || type == 5) // immediate32 / immediate64
	{
		i += (components == 2 ? 4 : 1) * (type == 5 ? 2 : 1);
		return i <= end;
	}
	const u32 dims = (tok >> 20) & 3;
	for (u32 d = 0; d < dims; ++d)
	{
		const u32 rep = (tok >> (22 + 3 * d)) & 7;
		const u32 at = i;
		switch (rep)
		{
		case 0: i += 1; break; // immediate32
		case 1: i += 2; break; // immediate64
		case 2: // relative
			if (!RemapOperand(p, i, end, moves))
				return false;
			break;
		case 3: // immediate32 + relative
			i += 1;
			if (!RemapOperand(p, i, end, moves))
				return false;
			break;
		case 4: // immediate64 + relative
			i += 2;
			if (!RemapOperand(p, i, end, moves))
				return false;
			break;
		default: return false;
		}
		if (i > end)
			return false;
		if (type == 2 && d == 0 && !moves.empty())
		{
			if (rep != 0)
				return false;
			MoveRegister(p[at], moves);
		}
	}
	return true;
}

// The vertex shader's outputs moved to the pixel shader's input registers and grown to cover its inputs; false when
// nothing differs or the mismatch isn't one this fixes. why: the fixes, for the log
static bool LinkVertexOutputs(const std::vector<u8>& vs, const std::vector<u8>& ps, std::vector<u8>& patched, xr_string& why)
{
	std::vector<DxbcChunk> vs_chunks, ps_chunks;
	if (!ReadContainer(vs, vs_chunks) || !ReadContainer(ps, ps_chunks))
		return false;
	const DxbcChunk* osgn = FindChunk(vs_chunks, kFourccOSGN);
	const DxbcChunk* isgn = FindChunk(ps_chunks, kFourccISGN);
	const DxbcChunk* shex = FindChunk(vs_chunks, kFourccSHEX);
	if (!shex)
		shex = FindChunk(vs_chunks, kFourccSHDR);
	std::vector<SigElement> outputs, inputs;
	if (!osgn || !isgn || !shex || !ReadSignature(*osgn, outputs) || !ReadSignature(*isgn, inputs))
		return false;

	// Output registers move onto the registers the pixel shader reads them from (deffer_model_flat.vs writes
	// SV_Position to o6, models_scope_zwrite.ps reads it from v0); registers in the way of an input, holding outputs the
	// pixel shader doesn't read (models_selflight.vs writes only SV_Position, on the register accum_emissive.ps reads
	// TEXCOORD0 from), move to free registers
	auto ps_input = [&](const SigElement& o) -> const SigElement* {
		for (const SigElement& in : inputs)
			if (in.index == o.index && !_stricmp(o.name.c_str(), in.name.c_str()))
				return &in;
		return nullptr;
	};
	auto moved = [](const std::vector<RegisterMove>& moves, u32 reg) {
		for (const RegisterMove& m : moves)
			if (m.from == reg)
				return &m;
		return (const RegisterMove*)nullptr;
	};
	std::vector<RegisterMove> moves;
	u32 free_reg = 0;
	for (const SigElement& o : outputs)
		free_reg = _max(free_reg, o.reg + 1);
	for (const SigElement& in : inputs)
		free_reg = _max(free_reg, in.reg + 1);
	for (const SigElement& o : outputs)
	{
		const SigElement* in = ps_input(o);
		if (!in || in->reg == o.reg)
			continue;
		if (const RegisterMove* m = moved(moves, o.reg))
		{
			if (m->to != in->reg)
				return false; // elements of one register read from different ones
			continue;
		}
		for (const RegisterMove& m : moves)
			if (m.to == in->reg)
				return false;
		for (const SigElement& other : outputs)
			if (other.reg == o.reg && ps_input(other) && ps_input(other)->reg != in->reg)
				return false; // elements of one register read from different ones
		moves.push_back({ o.reg, in->reg });
	}
	for (const SigElement& o : outputs)
	{
		if (moved(moves, o.reg))
			continue;
		bool in_way = false, read = false;
		for (const RegisterMove& m : moves)
			in_way |= m.to == o.reg;
		for (const SigElement& in : inputs)
			in_way |= in.reg == o.reg && !ps_input(o);
		if (!in_way)
			continue;
		for (const SigElement& other : outputs)
			read |= other.reg == o.reg && ps_input(other);
		if (read || free_reg >= 32)
			return false;
		moves.push_back({ o.reg, free_reg++ });
	}
	for (SigElement& o : outputs)
		if (MoveRegister(o.reg, moves))
			why.append(make_string(" %s%u moved to o%u", o.name.c_str(), o.index, o.reg).c_str());

	std::vector<OutputFix> fixes;
	for (const SigElement& in : inputs)
	{
		if (in.system_value != 0)
			continue; // system values (SV_Position, SV_IsFrontFace...) come from the rasterizer or match by value
		SigElement* match = nullptr;
		for (SigElement& o : outputs)
			if (o.index == in.index && !_stricmp(o.name.c_str(), in.name.c_str()))
				match = &o;
		const u8 missing = match ? u8(in.mask & ~match->mask) : in.mask;
		if (match && match->reg != in.reg)
			return false;
		if (!missing)
			continue;
		for (const SigElement& o : outputs)
			if (o.reg == in.reg && &o != match && (o.mask & missing))
				return false; // another output packed into those components
		if (match)
		{
			fixes.push_back({ in.reg, missing, match->mask });
			match->mask |= missing;
		}
		else
		{
			fixes.push_back({ in.reg, missing, 0 });
			outputs.push_back({ in.name, in.index, 0, in.component_type, in.reg, missing, 0 });
		}
		why.append(make_string(" %s%u.%s%s%s%s", in.name.c_str(), in.index, (missing & 1) ? "x" : "", (missing & 2) ? "y" : "",
		                       (missing & 4) ? "z" : "", (missing & 8) ? "w" : "").c_str());
	}
	if (fixes.empty() && moves.empty())
		return false;
	std::stable_sort(outputs.begin(), outputs.end(), [](const SigElement& a, const SigElement& b) {
		return a.reg != b.reg ? a.reg < b.reg : (a.mask & -a.mask) < (b.mask & -b.mask);
	});

	// Program: version, length in dwords, then instructions; declarations first
	const u32* tok = reinterpret_cast<const u32*>(shex->data.data());
	const size_t tok_count = shex->data.size() / 4;
	if (tok_count < 2 || tok[1] > tok_count)
		return false;
	const u32 length = tok[1];
	std::vector<u32> program(tok, tok + length);
	u32 body = 2;
	while (body < length)
	{
		const u32 opcode = program[body] & 0x7ff;
		if (!IsDeclaration(opcode))
			break;
		const u32 len = opcode == 0x35 ? (body + 1 < length ? program[body + 1] : 0) : (program[body] >> 24) & 0x7f;
		if (!len || body + len > length)
			return false;
		if ((opcode == kOpDclOutput || opcode == 0x66 || opcode == 0x67) && len >= 3 && ((program[body + 1] >> 12) & 0xff) == 2)
			MoveRegister(program[body + 2], moves); // dcl_output, dcl_output_sgv, dcl_output_siv
		if (opcode == 0x5b && !moves.empty())
			return false; // dcl_indexRange
		if (opcode == kOpDclOutput && len == 3 && (program[body + 1] & ~0xf0u) == kOperandOutput)
		{
			for (OutputFix& f : fixes)
				if (f.old_mask && program[body + 2] == f.reg && ((program[body + 1] >> 4) & 0xf) == f.old_mask)
				{
					program[body + 1] |= u32(f.mask) << 4;
					f.old_mask = 0xff; // declared
				}
		}
		body += len;
	}
	if (!moves.empty())
	{
		for (u32 i = body; i < length;)
		{
			const u32 opcode = program[i] & 0x7ff;
			const u32 len = opcode == 0x35 ? (i + 1 < length ? program[i + 1] : 0) : (program[i] >> 24) & 0x7f;
			if (!len || i + len > length)
				return false;
			if (opcode == 0x35)
			{
				i += len;
				continue;
			}
			if (IsDeclaration(opcode) || opcode == 0x78 /* fcall */)
				return false;
			u32 j = i + 1;
			for (bool ext = (program[i] & 0x80000000u) != 0; ext && j < i + len; ++j)
				ext = (program[j] & 0x80000000u) != 0;
			while (j < i + len)
				if (!RemapOperand(program, j, i + len, moves))
					return false;
			if (j != i + len)
				return false;
			i += len;
		}
	}

	std::vector<u32> added;
	for (const OutputFix& f : fixes)
		if (f.old_mask != 0xff)
		{
			added.push_back(kOpDclOutput | (3u << 24));
			added.push_back(kOperandOutput | (u32(f.mask) << 4));
			added.push_back(f.reg);
		}
	for (const OutputFix& f : fixes)
	{
		added.push_back(kOpMov | (8u << 24));
		added.push_back(kOperandOutput | (u32(f.mask) << 4));
		added.push_back(f.reg);
		added.push_back(kOperandImmediate4);
		for (u32 i = 0; i < 4; ++i)
			added.push_back(0);
	}
	program.insert(program.begin() + body, added.begin(), added.end());
	program[1] = u32(program.size());

	// Container: the same chunks in the same order, two of them replaced
	std::vector<u8> new_osgn = WriteSignature(outputs);
	std::vector<u8> new_shex(reinterpret_cast<const u8*>(program.data()), reinterpret_cast<const u8*>(program.data() + program.size()));
	const u32 count = u32(vs_chunks.size());
	patched.clear();
	AppendU32(patched, kFourccDXBC);
	patched.resize(20, 0); // checksum
	AppendU32(patched, ReadU32(&vs[20])); // container version
	AppendU32(patched, 0); // total size
	AppendU32(patched, count);
	const size_t offsets = patched.size();
	patched.resize(offsets + count * 4);
	for (u32 i = 0; i < count; ++i)
	{
		const DxbcChunk& c = vs_chunks[i];
		const std::vector<u8>& data = &c == osgn ? new_osgn : &c == shex ? new_shex : c.data;
		const u32 offset = u32(patched.size());
		memcpy(&patched[offsets + i * 4], &offset, 4);
		AppendU32(patched, c.fourcc);
		AppendU32(patched, u32(data.size()));
		patched.insert(patched.end(), data.begin(), data.end());
	}
	const u32 total = u32(patched.size());
	memcpy(&patched[24], &total, 4);
	u32 checksum[4];
	DxbcChecksum(patched.data() + 20, patched.size() - 20, checksum);
	memcpy(&patched[4], checksum, 16);
	return true;
}

// Why a pipeline state was refused, in the log: names, formats, states, and the signature checks D3D11 only did at
// draw time (a vertex shader input the input layout lacks, a pixel shader input the previous stage doesn't write)
static void DiagnosePipeline(const GraphicsPipelineKey& key, Shader* const shaders[5], const InputLayout* layout,
                             const D3D11_BLEND_DESC& blend, const D3D11_RASTERIZER_DESC& raster,
                             const D3D11_DEPTH_STENCIL_DESC& depth)
{
	static const char* stage_names[5] = { "VS", "HS", "DS", "GS", "PS" };
	for (UINT i = 0; i < 5; ++i)
		if (shaders[i])
			Msg("!   %s %s (crc %u, cb 0x%x, srv %u, smp %u)", stage_names[i], shaders[i]->m_name.c_str(), shaders[i]->m_crc,
			    UINT(shaders[i]->m_bindings.cb_mask), UINT(shaders[i]->m_bindings.srv_count), UINT(shaders[i]->m_bindings.sampler_count));
	Msg("!   RTs %u [%d %d %d %d], DSV %d (read-only %u), samples %u/%u, mask 0x%x, topology type %u", UINT(key.rt_count),
	    int(key.rtv[0]), int(key.rtv[1]), int(key.rtv[2]), int(key.rtv[3]), int(key.dsv), UINT(key.dsv_read_only),
	    UINT(key.samples), key.sample_quality, key.sample_mask, UINT(key.topology_type));
	Msg("!   blend a2c %d indep %d rt0 on %d %d/%d op %d alpha %d/%d op %d mask 0x%x", blend.AlphaToCoverageEnable,
	    blend.IndependentBlendEnable, blend.RenderTarget[0].BlendEnable, int(blend.RenderTarget[0].SrcBlend),
	    int(blend.RenderTarget[0].DestBlend), int(blend.RenderTarget[0].BlendOp), int(blend.RenderTarget[0].SrcBlendAlpha),
	    int(blend.RenderTarget[0].DestBlendAlpha), int(blend.RenderTarget[0].BlendOpAlpha), UINT(blend.RenderTarget[0].RenderTargetWriteMask));
	Msg("!   depth %d write %d func %d stencil %d r/w 0x%x/0x%x; raster fill %d cull %d ccw %d bias %d/%f/%f clip %d ms %d aa %d",
	    depth.DepthEnable, int(depth.DepthWriteMask), int(depth.DepthFunc), depth.StencilEnable, UINT(depth.StencilReadMask),
	    UINT(depth.StencilWriteMask), int(raster.FillMode), int(raster.CullMode), raster.FrontCounterClockwise, raster.DepthBias,
	    raster.DepthBiasClamp, raster.SlopeScaledDepthBias, raster.DepthClipEnable, raster.MultisampleEnable, raster.AntialiasedLineEnable);

	auto reflect = [](const Shader* sh) -> ID3D11ShaderReflection* {
		ID3D11ShaderReflection* r = nullptr;
		if (sh && FAILED(D3DReflect(sh->m_code.data(), sh->m_code.size(), IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&r))))
			r = nullptr;
		return r;
	};
	if (ID3D11ShaderReflection* vs = reflect(shaders[0]))
	{
		D3D11_SHADER_DESC sd = {};
		vs->GetDesc(&sd);
		for (UINT i = 0; i < sd.InputParameters; ++i)
		{
			D3D11_SIGNATURE_PARAMETER_DESC p = {};
			vs->GetInputParameterDesc(i, &p);
			if (p.SystemValueType != D3D_NAME_UNDEFINED)
				continue;
			bool found = false;
			if (layout)
				for (const D3D12_INPUT_ELEMENT_DESC& e : layout->m_elements)
					if (!_stricmp(e.SemanticName, p.SemanticName) && e.SemanticIndex == p.SemanticIndex)
						found = true;
			if (!found)
				Msg("!   VS input %s%u is not in the input layout (%u elements)", p.SemanticName, p.SemanticIndex,
				    layout ? UINT(layout->m_elements.size()) : 0u);
		}
		vs->Release();
	}
	const Shader* producer = shaders[3] ? shaders[3] : shaders[2] ? shaders[2] : shaders[0];
	ID3D11ShaderReflection* out = reflect(producer);
	ID3D11ShaderReflection* ps = reflect(shaders[4]);
	if (out && ps)
	{
		D3D11_SHADER_DESC od = {}, pd = {};
		out->GetDesc(&od);
		ps->GetDesc(&pd);
		for (UINT i = 0; i < pd.InputParameters; ++i)
		{
			D3D11_SIGNATURE_PARAMETER_DESC p = {};
			ps->GetInputParameterDesc(i, &p);
			if (p.SystemValueType != D3D_NAME_UNDEFINED && p.SystemValueType != D3D_NAME_POSITION)
				continue;
			bool found = false;
			for (UINT j = 0; j < od.OutputParameters && !found; ++j)
			{
				D3D11_SIGNATURE_PARAMETER_DESC o = {};
				out->GetOutputParameterDesc(j, &o);
				if (!_stricmp(o.SemanticName, p.SemanticName) && o.SemanticIndex == p.SemanticIndex)
				{
					found = true;
					if (o.Register != p.Register || (p.Mask & ~o.Mask))
						Msg("!   PS input %s%u: register %u mask 0x%x, written as register %u mask 0x%x", p.SemanticName,
						    p.SemanticIndex, p.Register, UINT(p.Mask), o.Register, UINT(o.Mask));
				}
			}
			if (!found)
				Msg("!   PS input %s%u (register %u) is not written by the previous stage", p.SemanticName, p.SemanticIndex, p.Register);
		}
	}
	if (out)
		out->Release();
	if (ps)
		ps->Release();
}

ID3D12PipelineState* CreateGraphicsPipeline(Device* device, const GraphicsPipelineKey& key, const RootSignature* root,
                                            Shader* const shaders[5], const InputLayout* layout,
                                            const D3D11_BLEND_DESC& blend, const D3D11_RASTERIZER_DESC& raster,
                                            const D3D11_DEPTH_STENCIL_DESC& depth)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = root->signature;
	std::vector<u8> linked_vs;
	if (shaders[0])
	{
		d.VS = shaders[0]->Bytecode();
		xr_string why;
		if (shaders[4] && !shaders[1] && !shaders[2] && !shaders[3] && LinkVertexOutputs(shaders[0]->m_code, shaders[4]->m_code, linked_vs, why))
		{
			d.VS = { linked_vs.data(), linked_vs.size() };
			static std::mutex log_lock;
			static xr_set<u64> logged;
			std::lock_guard<std::mutex> lock(log_lock);
			if (logged.insert(key.shader[0] * 31 + key.shader[4]).second)
				Msg("* D3D12: %s linked to %s (missing outputs set to 0):%s", shaders[0]->m_name.c_str(), shaders[4]->m_name.c_str(), why.c_str());
		}
	}
	if (shaders[1])
		d.HS = shaders[1]->Bytecode();
	if (shaders[2])
		d.DS = shaders[2]->Bytecode();
	if (shaders[3])
		d.GS = shaders[3]->Bytecode();
	if (shaders[4])
		d.PS = shaders[4]->Bytecode();

	d.BlendState.AlphaToCoverageEnable = blend.AlphaToCoverageEnable;
	d.BlendState.IndependentBlendEnable = blend.IndependentBlendEnable;
	for (UINT i = 0; i < 8; ++i)
	{
		const D3D11_RENDER_TARGET_BLEND_DESC& s = blend.RenderTarget[i];
		D3D12_RENDER_TARGET_BLEND_DESC& t = d.BlendState.RenderTarget[i];
		t.BlendEnable = s.BlendEnable;
		t.LogicOpEnable = FALSE;
		t.SrcBlend = D3D12_BLEND(s.SrcBlend);
		t.DestBlend = D3D12_BLEND(s.DestBlend);
		t.BlendOp = D3D12_BLEND_OP(s.BlendOp);
		t.SrcBlendAlpha = D3D12_BLEND(s.SrcBlendAlpha);
		t.DestBlendAlpha = D3D12_BLEND(s.DestBlendAlpha);
		t.BlendOpAlpha = D3D12_BLEND_OP(s.BlendOpAlpha);
		t.LogicOp = D3D12_LOGIC_OP_NOOP;
		t.RenderTargetWriteMask = s.RenderTargetWriteMask;
		if (!t.BlendEnable)
		{
			// D3D12 validates the factors even when blending is off
			t.SrcBlend = t.SrcBlendAlpha = D3D12_BLEND_ONE;
			t.DestBlend = t.DestBlendAlpha = D3D12_BLEND_ZERO;
			t.BlendOp = t.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		}
	}
	d.SampleMask = key.sample_mask;

	d.RasterizerState.FillMode = D3D12_FILL_MODE(raster.FillMode ? raster.FillMode : D3D11_FILL_SOLID);
	d.RasterizerState.CullMode = D3D12_CULL_MODE(raster.CullMode ? raster.CullMode : D3D11_CULL_BACK);
	d.RasterizerState.FrontCounterClockwise = raster.FrontCounterClockwise;
	d.RasterizerState.DepthBias = raster.DepthBias;
	d.RasterizerState.DepthBiasClamp = raster.DepthBiasClamp;
	d.RasterizerState.SlopeScaledDepthBias = raster.SlopeScaledDepthBias;
	d.RasterizerState.DepthClipEnable = raster.DepthClipEnable;
	d.RasterizerState.MultisampleEnable = raster.MultisampleEnable;
	d.RasterizerState.AntialiasedLineEnable = raster.MultisampleEnable ? FALSE : raster.AntialiasedLineEnable;
	d.RasterizerState.ForcedSampleCount = 0;
	d.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	d.DepthStencilState.DepthEnable = depth.DepthEnable;
	d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK(depth.DepthWriteMask);
	d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(depth.DepthFunc ? depth.DepthFunc : D3D11_COMPARISON_LESS);
	d.DepthStencilState.StencilEnable = depth.StencilEnable;
	d.DepthStencilState.StencilReadMask = depth.StencilReadMask;
	d.DepthStencilState.StencilWriteMask = depth.StencilWriteMask;
	auto face = [](const D3D11_DEPTH_STENCILOP_DESC& s) {
		D3D12_DEPTH_STENCILOP_DESC t;
		t.StencilFailOp = D3D12_STENCIL_OP(s.StencilFailOp ? s.StencilFailOp : D3D11_STENCIL_OP_KEEP);
		t.StencilDepthFailOp = D3D12_STENCIL_OP(s.StencilDepthFailOp ? s.StencilDepthFailOp : D3D11_STENCIL_OP_KEEP);
		t.StencilPassOp = D3D12_STENCIL_OP(s.StencilPassOp ? s.StencilPassOp : D3D11_STENCIL_OP_KEEP);
		t.StencilFunc = D3D12_COMPARISON_FUNC(s.StencilFunc ? s.StencilFunc : D3D11_COMPARISON_ALWAYS);
		return t;
	};
	d.DepthStencilState.FrontFace = face(depth.FrontFace);
	d.DepthStencilState.BackFace = face(depth.BackFace);
	if (key.dsv == DXGI_FORMAT_UNKNOWN)
	{
		d.DepthStencilState.DepthEnable = FALSE;
		d.DepthStencilState.StencilEnable = FALSE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	}
	else if (key.dsv_read_only)
	{
		// D3D11 drops writes through a read-only view; D3D12 needs the writes off for the DEPTH_READ state
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		d.DepthStencilState.StencilWriteMask = 0;
	}

	if (layout && !layout->m_elements.empty())
	{
		d.InputLayout.pInputElementDescs = layout->m_elements.data();
		d.InputLayout.NumElements = UINT(layout->m_elements.size());
	}
	d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	d.PrimitiveTopologyType = TopologyType(key.topology_type);
	d.NumRenderTargets = key.rt_count;
	for (UINT i = 0; i < key.rt_count; ++i)
		d.RTVFormats[i] = key.rtv[i];
	d.DSVFormat = key.dsv;
	d.SampleDesc.Count = key.samples ? key.samples : 1;
	d.SampleDesc.Quality = key.sample_quality;

	ID3D12PipelineState* pso = nullptr;
	const HRESULT hr = device->m_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
	if (FAILED(hr))
	{
		Msg("! D3D12: CreateGraphicsPipelineState failed 0x%08x (VS %s, PS %s)", hr, shaders[0] ? shaders[0]->m_name.c_str() : "-",
		    shaders[4] ? shaders[4]->m_name.c_str() : "-");
		if (hr == DXGI_ERROR_DEVICE_REMOVED)
			device->ReportRemoval("pipeline creation");
		else
			DiagnosePipeline(key, shaders, layout, blend, raster, depth);
		return nullptr;
	}
	return pso;
}

ID3D12PipelineState* CreateComputePipeline(Device* device, const RootSignature* root, const Shader* shader)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
	d.pRootSignature = root->signature;
	d.CS = shader->Bytecode();
	ID3D12PipelineState* pso = nullptr;
	const HRESULT hr = device->m_device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
	if (FAILED(hr))
	{
		Msg("! D3D12: CreateComputePipelineState failed 0x%08x (CS %s, crc %u)", hr, shader->m_name.c_str(), shader->m_crc);
		return nullptr;
	}
	return pso;
}

//===================================================================================================================
// Pipeline cache on disk. Each graphics pipeline created is added to the file as its shaders (DXBC, each once), its
// input layout (once) and its key with the state descriptions; the keys are content hashes, as at run time. At the
// next start a background thread creates every pipeline of the file before the renderer asks for it, so a pipeline's
// first use no longer waits for its creation (the DXBC translation and the driver's compile) from the second run on.

namespace
{
const u32 kDiskMagic = MAKEFOURCC('X', 'P', 'S', 'O');
const u32 kDiskVersion = 1;
const long kDiskMaxSize = 128 << 20; // past it the file starts over (shaders of earlier mod versions pile up)
enum : u8
{
	RecShader = 1,
	RecLayout = 2,
	RecPipeline = 3,
};

struct DiskPipeline
{
	GraphicsPipelineKey key; // root 0
	D3D11_BLEND_DESC blend;
	D3D11_RASTERIZER_DESC raster;
	D3D11_DEPTH_STENCIL_DESC depth;
};

struct DiskShader
{
	u8 stage;
	const u8* code;
	u32 size;
};

struct DiskLayout
{
	std::vector<xr_string> names;
	std::vector<D3D11_INPUT_ELEMENT_DESC> elements; // semantic names set where used (the strings move)
};

struct DiskWarmup
{
	std::vector<u8> data;
	std::unordered_map<u64, DiskShader> shaders;
	std::unordered_map<u64, DiskLayout> layouts;
	std::vector<DiskPipeline> pipelines;
};

struct Reader
{
	const u8* p;
	const u8* end;
	bool Get(void* out, size_t n)
	{
		if (size_t(end - p) < n)
			return false;
		memcpy(out, p, n);
		p += n;
		return true;
	}
	template <class T>
	bool Get(T& v) { return Get(&v, sizeof(T)); }
};

Shader* MakeShader(Device* device, u8 stage, const u8* code, u32 size)
{
	switch (stage)
	{
	case StageVS: { VertexShader* s = nullptr; device->CreateVertexShader(code, size, nullptr, &s); return s; }
	case StageHS: { HullShader* s = nullptr; device->CreateHullShader(code, size, nullptr, &s); return s; }
	case StageDS: { DomainShader* s = nullptr; device->CreateDomainShader(code, size, nullptr, &s); return s; }
	case StageGS: { GeometryShader* s = nullptr; device->CreateGeometryShader(code, size, nullptr, &s); return s; }
	case StagePS: { PixelShader* s = nullptr; device->CreatePixelShader(code, size, nullptr, &s); return s; }
	default: return nullptr;
	}
}
} // namespace

void Device::Impl::RecordPipeline(const GraphicsPipelineKey& key, Shader* const shaders[5], const InputLayout* layout,
                                  const D3D11_BLEND_DESC& blend, const D3D11_RASTERIZER_DESC& raster,
                                  const D3D11_DEPTH_STENCIL_DESC& depth)
{
	std::lock_guard<std::mutex> lock(disk_lock);
	if (!disk_file)
		return;
	DiskPipeline d;
	memset(&d, 0, sizeof(d));
	d.key = key;
	d.key.root = 0;
	if (!disk_pipelines.insert(d.key).second)
		return;
	d.blend = blend;
	d.raster = raster;
	d.depth = depth;
	std::vector<u8> out;
	auto put = [&](const void* data, size_t n) {
		const u8* b = static_cast<const u8*>(data);
		out.insert(out.end(), b, b + n);
	};
	for (UINT i = 0; i < 5; ++i)
	{
		const Shader* sh = shaders[i];
		if (!sh || !disk_shaders.insert(sh->m_content).second)
			continue;
		const u8 type = RecShader, stage = u8(sh->m_stage);
		const u32 size = u32(sh->m_code.size());
		put(&type, 1);
		put(&sh->m_content, 8);
		put(&stage, 1);
		put(&size, 4);
		put(sh->m_code.data(), size);
	}
	if (layout && disk_layouts.insert(layout->m_content).second)
	{
		const u8 type = RecLayout;
		const u32 count = u32(layout->m_elements.size());
		put(&type, 1);
		put(&layout->m_content, 8);
		put(&count, 4);
		for (const D3D12_INPUT_ELEMENT_DESC& e : layout->m_elements)
		{
			const u16 len = u16(strlen(e.SemanticName));
			put(&len, 2);
			put(e.SemanticName, len);
			const UINT fields[6] = { e.SemanticIndex, UINT(e.Format), e.InputSlot, e.AlignedByteOffset, UINT(e.InputSlotClass),
			                         e.InstanceDataStepRate };
			put(fields, sizeof(fields));
		}
	}
	const u8 type = RecPipeline;
	put(&type, 1);
	put(&d, sizeof(d));
	fwrite(out.data(), 1, out.size(), disk_file);
	fflush(disk_file);
}

void Device::Impl::StopPipelineCache()
{
	disk_stop = true;
	if (disk_warmup.joinable())
		disk_warmup.join();
	std::lock_guard<std::mutex> lock(disk_lock);
	if (disk_file)
	{
		fclose(disk_file);
		disk_file = nullptr;
	}
}

void Device::StartPipelineCache(const char* path)
{
	Impl& d = *m_impl;
	if (d.disk_file || d.disk_warmup.joinable())
		return;

	// What earlier runs wrote, up to the last complete record
	DiskWarmup* job = new DiskWarmup();
	if (FILE* f = fopen(path, "rb"))
	{
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size > 8 && size <= kDiskMaxSize)
		{
			job->data.resize(size_t(size));
			if (fread(job->data.data(), 1, job->data.size(), f) != job->data.size())
				job->data.clear();
		}
		fclose(f);
	}
	size_t valid = 0;
	if (job->data.size() >= 8)
	{
		Reader r = { job->data.data(), job->data.data() + job->data.size() };
		u32 magic = 0, version = 0;
		r.Get(magic);
		r.Get(version);
		if (magic == kDiskMagic && version == kDiskVersion)
		{
			valid = 8;
			for (;;)
			{
				u8 type = 0;
				if (!r.Get(type))
					break;
				if (type == RecShader)
				{
					u64 hash;
					DiskShader sh;
					if (!r.Get(hash) || !r.Get(sh.stage) || !r.Get(sh.size) || size_t(r.end - r.p) < sh.size)
						break;
					sh.code = r.p;
					r.p += sh.size;
					job->shaders[hash] = sh;
				}
				else if (type == RecLayout)
				{
					u64 hash;
					u32 count;
					if (!r.Get(hash) || !r.Get(count) || count > 64)
						break;
					DiskLayout l;
					bool ok = true;
					for (u32 i = 0; i < count && ok; ++i)
					{
						u16 len;
						UINT fields[6];
						ok = r.Get(len) && size_t(r.end - r.p) >= len;
						if (!ok)
							break;
						l.names.emplace_back(reinterpret_cast<const char*>(r.p), len);
						r.p += len;
						ok = r.Get(fields, sizeof(fields));
						D3D11_INPUT_ELEMENT_DESC e = {};
						e.SemanticIndex = fields[0];
						e.Format = DXGI_FORMAT(fields[1]);
						e.InputSlot = fields[2];
						e.AlignedByteOffset = fields[3];
						e.InputSlotClass = D3D11_INPUT_CLASSIFICATION(fields[4]);
						e.InstanceDataStepRate = fields[5];
						l.elements.push_back(e);
					}
					if (!ok)
						break;
					job->layouts[hash] = std::move(l);
				}
				else if (type == RecPipeline)
				{
					DiskPipeline p;
					if (!r.Get(p))
						break;
					job->pipelines.push_back(p);
				}
				else
					break;
				valid = size_t(r.p - job->data.data());
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(d.disk_lock);
		for (const auto& kv : job->shaders)
			d.disk_shaders.insert(kv.first);
		for (const auto& kv : job->layouts)
			d.disk_layouts.insert(kv.first);
		for (const DiskPipeline& p : job->pipelines)
			d.disk_pipelines.insert(p.key);
		VerifyPath(path);
		if (valid && valid == job->data.size())
			d.disk_file = fopen(path, "ab");
		else
		{
			// New, of another version, or cut short by a crash: written again from the records that are whole
			d.disk_file = fopen(path, "wb");
			if (d.disk_file)
			{
				if (valid)
					fwrite(job->data.data(), 1, valid, d.disk_file);
				else
				{
					const u32 header[2] = { kDiskMagic, kDiskVersion };
					fwrite(header, 1, sizeof(header), d.disk_file);
				}
				fflush(d.disk_file);
			}
		}
		if (!d.disk_file)
			Msg("! D3D12: can't open the pipeline cache %s", path);
	}
	Msg("* D3D12: pipeline cache %s: %u pipelines, %u shaders", path, UINT(job->pipelines.size()), UINT(job->shaders.size()));
	if (job->pipelines.empty())
	{
		delete job;
		return;
	}

	// The pipelines of earlier runs, created again in the background (below normal priority: loading goes first)
	d.disk_warmup = std::thread([this, job]() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		Impl& d = *m_impl;
		const u64 start = GetTickCount64();
		std::unordered_map<u64, Shader*> shaders;
		std::unordered_map<u64, InputLayout*> layouts;
		auto shader = [&](u64 hash) -> Shader* {
			auto it = shaders.find(hash);
			if (it != shaders.end())
				return it->second;
			Shader* sh = nullptr;
			auto disk = job->shaders.find(hash);
			if (disk != job->shaders.end())
			{
				sh = MakeShader(this, disk->second.stage, disk->second.code, disk->second.size);
				if (sh && sh->m_content != hash)
				{
					sh->Release();
					sh = nullptr;
				}
			}
			shaders[hash] = sh;
			return sh;
		};
		auto layout = [&](u64 hash) -> InputLayout* {
			auto it = layouts.find(hash);
			if (it != layouts.end())
				return it->second;
			InputLayout* l = nullptr;
			auto disk = job->layouts.find(hash);
			if (disk != job->layouts.end())
			{
				std::vector<D3D11_INPUT_ELEMENT_DESC> elements = disk->second.elements;
				for (size_t i = 0; i < elements.size(); ++i)
					elements[i].SemanticName = disk->second.names[i].c_str();
				CreateInputLayout(elements.data(), UINT(elements.size()), nullptr, 0, &l);
			}
			if (l && l->m_content != hash)
			{
				l->Release();
				l = nullptr;
			}
			layouts[hash] = l;
			return l;
		};
		shader_progress::begin("Compiling shader pipelines", UINT(job->pipelines.size()));
		UINT created = 0, present = 0, incomplete = 0;
		for (const DiskPipeline& p : job->pipelines)
		{
			if (d.disk_stop)
				break;
			struct Step { ~Step() { shader_progress::step(); } } counted; // also on continue
			Shader* sh[5] = {};
			bool ok = true;
			for (UINT i = 0; i < 5; ++i)
				if (p.key.shader[i] && !(sh[i] = shader(p.key.shader[i])))
					ok = false;
			InputLayout* il = nullptr;
			if (p.key.layout && !(il = layout(p.key.layout)))
				ok = false;
			if (!ok)
			{
				++incomplete;
				continue;
			}
			BlendState* bs = nullptr;
			RasterizerState* rs = nullptr;
			DepthStencilState* ds = nullptr;
			CreateBlendState(&p.blend, &bs);
			CreateRasterizerState(&p.raster, &rs);
			CreateDepthStencilState(&p.depth, &ds);
			RootLayoutKey rk;
			memset(&rk, 0, sizeof(rk));
			for (UINT s = StageVS; s <= StagePS; ++s)
				rk.stage[s] = ShaderBindingKey(sh[s]);
			RootSignature* root = d.GetRootSignature(rk);
			GraphicsPipelineKey k = p.key;
			k.root = root ? root->id : 0;
			k.blend = bs->m_content;
			k.raster = rs->m_content;
			k.depth = ds->m_content;
			bool have;
			{
				std::lock_guard<std::mutex> lock(d.pipeline_lock);
				have = d.graphics_pipelines.count(k) != 0;
			}
			if (root && !have)
			{
				ID3D12PipelineState* pso = CreateGraphicsPipeline(this, k, root, sh, il, bs->m_desc, rs->m_desc, ds->m_desc);
				std::lock_guard<std::mutex> lock(d.pipeline_lock);
				if (d.graphics_pipelines.emplace(k, pso).second)
					++created;
				else if (pso)
					pso->Release();
			}
			else
				++present;
			bs->Release();
			rs->Release();
			ds->Release();
		}
		for (auto& kv : shaders)
			if (kv.second)
				kv.second->Release();
		for (auto& kv : layouts)
			if (kv.second)
				kv.second->Release();
		shader_progress::end(!d.disk_stop);
		Msg("* D3D12: %u pipelines created ahead from the cache in %.1f s (%u there already, %u incomplete)", created,
		    float(GetTickCount64() - start) / 1000.f, present, incomplete);
		delete job;
	});
}
} // namespace dx12
