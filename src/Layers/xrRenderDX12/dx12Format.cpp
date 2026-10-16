// Format and description helpers of the D3D12 backend
#include "stdafx.h"
#pragma hdrstop

#include "dx12Internal.h"

namespace dx12
{
UINT FormatPlaneCount(ID3D12Device* device, DXGI_FORMAT format)
{
	D3D12_FEATURE_DATA_FORMAT_INFO info = {};
	info.Format = format;
	if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) && info.PlaneCount)
		return info.PlaneCount;
	return 1;
}

bool IsDepthFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
		return true;
	default:
		return false;
	}
}

UINT PlaneOfViewFormat(DXGI_FORMAT view_format)
{
	switch (view_format)
	{
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		return 1;
	default:
		return 0;
	}
}

UINT MipCount(UINT width, UINT height, UINT depth)
{
	UINT size = _max(width, _max(height, depth));
	UINT mips = 1;
	while (size > 1)
	{
		size >>= 1;
		++mips;
	}
	return mips;
}

D3D12_RESOURCE_FLAGS ResourceFlags(UINT bind_flags)
{
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	if (bind_flags & D3D11_BIND_RENDER_TARGET)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (bind_flags & D3D11_BIND_DEPTH_STENCIL)
	{
		flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		if (!(bind_flags & D3D11_BIND_SHADER_RESOURCE))
			flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	}
	if (bind_flags & D3D11_BIND_UNORDERED_ACCESS)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	return flags;
}

D3D12_RESOURCE_STATES ShaderReadState()
{
	return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

u64 ShaderBindingKey(const Shader* shader)
{
	if (!shader)
		return 0;
	const ShaderBindings& b = shader->m_bindings;
	return 1ull | (u64(b.cb_mask) << 1) | (u64(b.srv_count) << 17) | (u64(b.sampler_count) << 25) |
		(u64(b.uav_count) << 33);
}
} // namespace dx12
