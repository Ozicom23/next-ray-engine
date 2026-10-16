// D3D12 backend: shader reflection, root signatures from the reflected bindings, pipeline states (see dx12.h)
#include "stdafx.h"
#pragma hdrstop

#include "dx12Internal.h"
#include <D3DCompiler.h>

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
	if (shaders[0])
		d.VS = shaders[0]->Bytecode();
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
} // namespace dx12
