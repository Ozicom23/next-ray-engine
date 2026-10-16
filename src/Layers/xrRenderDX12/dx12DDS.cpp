// DDS loading for the D3D12 renderer (see dx12DDS.h)
#include "stdafx.h"
#pragma hdrstop

#include "dx12DDS.h"

namespace dds12
{
namespace
{
struct PixelFormat
{
	u32 size, flags, fourcc, bits, r_mask, g_mask, b_mask, a_mask;
};

struct Header
{
	u32 size, flags, height, width, pitch, depth, mips;
	u32 reserved1[11];
	PixelFormat pf;
	u32 caps, caps2, caps3, caps4, reserved2;
};

struct HeaderDX10
{
	u32 format, dimension, misc, array_size, misc2;
};

const u32 kMagic = MAKEFOURCC('D', 'D', 'S', ' ');
const u32 kAlphaPixels = 0x1, kAlpha = 0x2, kFourCC = 0x4, kRGB = 0x40, kLuminance = 0x20000, kBumpDuDv = 0x80000;
const u32 kCubemap = 0x200, kAllFaces = 0xFC00, kVolume = 0x200000;

// Conversions of legacy layouts DXGI has no format for
enum Conv
{
	ConvNone,
	ConvL8, // -> RGBA8 (L, L, L, 1)
	ConvA8L8, // -> RGBA8 (L, L, L, A)
	ConvL16, // -> RGBA16 (L, L, L, 1)
	ConvRGB24, // B, G, R bytes -> BGRA8
	ConvR5G6B5, // -> BGRA8
	ConvX1R5G5B5,
	ConvA1R5G5B5,
	ConvX4R4G4B4,
	ConvA4R4G4B4,
	ConvX8B8G8R8, // RGBA8 with undefined alpha -> alpha 1
	ConvA2R10G10B10, // -> R10G10B10A2 (red and blue swapped)
};

struct Layout
{
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // of the texture
	Conv conv = ConvNone;
	u32 src_bytes = 0; // per pixel in the file (uncompressed) or per 4x4 block (compressed)
	u32 dst_bytes = 0; // per pixel in the texture (uncompressed)
	bool bc = false;
	u32 width = 0, height = 0, depth = 1, mips = 1, layers = 1;
	bool cube = false, volume = false;
	size_t offset = 0; // of the pixel data in the file
};

u32 bc_block_bytes(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
		return 8;
	case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
		return 16;
	default:
		return 0;
	}
}

u32 pixel_bytes(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
		return 16;
	case DXGI_FORMAT_R32G32B32_FLOAT:
		return 12;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
		return 8;
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
		return 4;
	case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
		return 2;
	case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_A8_UNORM:
		return 1;
	default:
		return 0;
	}
}

bool masks(const PixelFormat& pf, u32 r, u32 g, u32 b, u32 a)
{
	return pf.r_mask == r && pf.g_mask == g && pf.b_mask == b && pf.a_mask == a;
}

// The texture format and conversion of a file without the DX10 header
bool legacy(const PixelFormat& pf, Layout& l)
{
	const bool alpha = (pf.flags & kAlphaPixels) != 0;
	if (pf.flags & kFourCC)
	{
		switch (pf.fourcc)
		{
		case MAKEFOURCC('D', 'X', 'T', '1'): l.format = DXGI_FORMAT_BC1_UNORM; return true;
		case MAKEFOURCC('D', 'X', 'T', '2'):
		case MAKEFOURCC('D', 'X', 'T', '3'): l.format = DXGI_FORMAT_BC2_UNORM; return true;
		case MAKEFOURCC('D', 'X', 'T', '4'):
		case MAKEFOURCC('D', 'X', 'T', '5'): l.format = DXGI_FORMAT_BC3_UNORM; return true;
		case MAKEFOURCC('A', 'T', 'I', '1'):
		case MAKEFOURCC('B', 'C', '4', 'U'): l.format = DXGI_FORMAT_BC4_UNORM; return true;
		case MAKEFOURCC('B', 'C', '4', 'S'): l.format = DXGI_FORMAT_BC4_SNORM; return true;
		case MAKEFOURCC('A', 'T', 'I', '2'):
		case MAKEFOURCC('B', 'C', '5', 'U'): l.format = DXGI_FORMAT_BC5_UNORM; return true;
		case MAKEFOURCC('B', 'C', '5', 'S'): l.format = DXGI_FORMAT_BC5_SNORM; return true;
		case 36: l.format = DXGI_FORMAT_R16G16B16A16_UNORM; return true; // D3DFMT_A16B16G16R16
		case 110: l.format = DXGI_FORMAT_R16G16B16A16_SNORM; return true; // D3DFMT_Q16W16V16U16
		case 111: l.format = DXGI_FORMAT_R16_FLOAT; return true; // D3DFMT_R16F
		case 112: l.format = DXGI_FORMAT_R16G16_FLOAT; return true; // D3DFMT_G16R16F
		case 113: l.format = DXGI_FORMAT_R16G16B16A16_FLOAT; return true; // D3DFMT_A16B16G16R16F
		case 114: l.format = DXGI_FORMAT_R32_FLOAT; return true; // D3DFMT_R32F
		case 115: l.format = DXGI_FORMAT_R32G32_FLOAT; return true; // D3DFMT_G32R32F
		case 116: l.format = DXGI_FORMAT_R32G32B32A32_FLOAT; return true; // D3DFMT_A32B32G32R32F
		default: return false;
		}
	}
	if (pf.flags & kRGB)
	{
		switch (pf.bits)
		{
		case 32:
			if (masks(pf, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000) && alpha)
				l.format = DXGI_FORMAT_B8G8R8A8_UNORM;
			else if (pf.r_mask == 0x00ff0000 && pf.g_mask == 0x0000ff00 && pf.b_mask == 0x000000ff)
				l.format = DXGI_FORMAT_B8G8R8X8_UNORM;
			else if (masks(pf, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000) && alpha)
				l.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			else if (pf.r_mask == 0x000000ff && pf.g_mask == 0x0000ff00 && pf.b_mask == 0x00ff0000)
			{
				l.format = DXGI_FORMAT_R8G8B8A8_UNORM;
				l.conv = ConvX8B8G8R8;
			}
			else if (pf.r_mask == 0x0000ffff && pf.g_mask == 0xffff0000)
				l.format = DXGI_FORMAT_R16G16_UNORM;
			else if (masks(pf, 0x000003ff, 0x000ffc00, 0x3ff00000, 0xc0000000))
				l.format = DXGI_FORMAT_R10G10B10A2_UNORM;
			else if (masks(pf, 0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000))
			{
				l.format = DXGI_FORMAT_R10G10B10A2_UNORM;
				l.conv = ConvA2R10G10B10;
			}
			else if (pf.r_mask == 0xffffffff)
				l.format = DXGI_FORMAT_R32_FLOAT;
			else
				return false;
			return true;
		case 24:
			if (pf.r_mask == 0xff0000 && pf.g_mask == 0xff00 && pf.b_mask == 0xff)
			{
				l.format = DXGI_FORMAT_B8G8R8A8_UNORM;
				l.conv = ConvRGB24;
				return true;
			}
			return false;
		case 16:
			if (pf.r_mask == 0xf800 && pf.g_mask == 0x07e0 && pf.b_mask == 0x001f)
				l.conv = ConvR5G6B5;
			else if (pf.r_mask == 0x7c00 && pf.g_mask == 0x03e0 && pf.b_mask == 0x001f)
				l.conv = alpha && pf.a_mask == 0x8000 ? ConvA1R5G5B5 : ConvX1R5G5B5;
			else if (pf.r_mask == 0x0f00 && pf.g_mask == 0x00f0 && pf.b_mask == 0x000f)
				l.conv = alpha && pf.a_mask == 0xf000 ? ConvA4R4G4B4 : ConvX4R4G4B4;
			else
				return false;
			l.format = DXGI_FORMAT_B8G8R8A8_UNORM;
			return true;
		default:
			return false;
		}
	}
	if (pf.flags & kLuminance)
	{
		if (pf.bits == 8 && pf.r_mask == 0xff)
		{
			l.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			l.conv = ConvL8;
			return true;
		}
		if (pf.bits == 16 && pf.r_mask == 0x00ff && alpha && pf.a_mask == 0xff00)
		{
			l.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			l.conv = ConvA8L8;
			return true;
		}
		if (pf.bits == 16 && pf.r_mask == 0xffff)
		{
			l.format = DXGI_FORMAT_R16G16B16A16_UNORM;
			l.conv = ConvL16;
			return true;
		}
		return false;
	}
	if ((pf.flags & kAlpha) && pf.bits == 8)
	{
		l.format = DXGI_FORMAT_A8_UNORM;
		return true;
	}
	if (pf.flags & kBumpDuDv)
	{
		if (pf.bits == 16 && pf.r_mask == 0x00ff && pf.g_mask == 0xff00)
			l.format = DXGI_FORMAT_R8G8_SNORM;
		else if (pf.bits == 32 && pf.r_mask == 0x0000ffff && pf.g_mask == 0xffff0000)
			l.format = DXGI_FORMAT_R16G16_SNORM;
		else if (pf.bits == 32 && pf.r_mask == 0x000000ff && pf.g_mask == 0x0000ff00 && pf.b_mask == 0x00ff0000)
			l.format = DXGI_FORMAT_R8G8B8A8_SNORM;
		else
			return false;
		return true;
	}
	return false;
}

u32 conv_src_bytes(Conv c, const PixelFormat& pf)
{
	switch (c)
	{
	case ConvL8: return 1;
	case ConvA8L8: case ConvL16: case ConvR5G6B5: case ConvX1R5G5B5: case ConvA1R5G5B5: case ConvX4R4G4B4: case ConvA4R4G4B4:
		return 2;
	case ConvRGB24: return 3;
	case ConvX8B8G8R8: case ConvA2R10G10B10: return 4;
	default: return pf.bits / 8;
	}
}

bool parse(const void* data, size_t length, Layout& l)
{
	const u8* bytes = static_cast<const u8*>(data);
	if (length < 4 + sizeof(Header) || *reinterpret_cast<const u32*>(bytes) != kMagic)
		return false;
	const Header& hdr = *reinterpret_cast<const Header*>(bytes + 4);
	if (hdr.size != sizeof(Header) || hdr.pf.size != sizeof(PixelFormat))
		return false;
	l.offset = 4 + sizeof(Header);
	l.width = _max(hdr.width, 1u);
	l.height = _max(hdr.height, 1u);
	l.mips = _max(hdr.mips, 1u);
	if ((hdr.pf.flags & kFourCC) && hdr.pf.fourcc == MAKEFOURCC('D', 'X', '1', '0'))
	{
		if (length < l.offset + sizeof(HeaderDX10))
			return false;
		const HeaderDX10& dx10 = *reinterpret_cast<const HeaderDX10*>(bytes + l.offset);
		l.offset += sizeof(HeaderDX10);
		l.format = DXGI_FORMAT(dx10.format);
		switch (dx10.dimension)
		{
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			l.height = 1;
			// fall through: loaded as a 2D texture of height 1
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			l.cube = (dx10.misc & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
			l.layers = _max(dx10.array_size, 1u) * (l.cube ? 6 : 1);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			l.volume = true;
			l.depth = _max(hdr.depth, 1u);
			break;
		default:
			return false;
		}
		l.bc = bc_block_bytes(l.format) != 0;
		l.src_bytes = l.bc ? bc_block_bytes(l.format) : pixel_bytes(l.format);
		l.dst_bytes = l.src_bytes;
		return l.src_bytes != 0;
	}
	if (!legacy(hdr.pf, l))
		return false;
	if (hdr.caps2 & kVolume)
	{
		l.volume = true;
		l.depth = _max(hdr.depth, 1u);
	}
	else if (hdr.caps2 & kCubemap)
	{
		if ((hdr.caps2 & kAllFaces) != kAllFaces)
			return false;
		l.cube = true;
		l.layers = 6;
	}
	l.bc = bc_block_bytes(l.format) != 0;
	if (l.bc)
		l.src_bytes = l.dst_bytes = bc_block_bytes(l.format);
	else
	{
		l.dst_bytes = pixel_bytes(l.format);
		l.src_bytes = l.conv == ConvNone ? l.dst_bytes : conv_src_bytes(l.conv, hdr.pf);
	}
	return l.src_bytes != 0 && l.dst_bytes != 0;
}

// Bytes of one mip of one layer (one depth slice for volumes), and its row pitch
size_t surface_bytes(const Layout& l, u32 w, u32 h, u32 bytes, u32& pitch)
{
	if (l.bc)
	{
		pitch = _max(1u, (w + 3) / 4) * bytes;
		return size_t(pitch) * _max(1u, (h + 3) / 4);
	}
	pitch = w * bytes;
	return size_t(pitch) * h;
}

inline u8 expand5(u32 v) { return u8((v << 3) | (v >> 2)); }
inline u8 expand6(u32 v) { return u8((v << 2) | (v >> 4)); }
inline u8 expand4(u32 v) { return u8((v << 4) | v); }

void convert(Conv c, const u8* src, u8* dst, size_t pixels)
{
	switch (c)
	{
	case ConvL8:
		for (size_t i = 0; i < pixels; ++i, dst += 4)
			dst[0] = dst[1] = dst[2] = src[i], dst[3] = 255;
		break;
	case ConvA8L8:
		for (size_t i = 0; i < pixels; ++i, dst += 4, src += 2)
			dst[0] = dst[1] = dst[2] = src[0], dst[3] = src[1];
		break;
	case ConvL16:
		for (size_t i = 0; i < pixels; ++i, dst += 8, src += 2)
		{
			u16* d = reinterpret_cast<u16*>(dst);
			d[0] = d[1] = d[2] = *reinterpret_cast<const u16*>(src);
			d[3] = 0xffff;
		}
		break;
	case ConvRGB24:
		for (size_t i = 0; i < pixels; ++i, dst += 4, src += 3)
			dst[0] = src[0], dst[1] = src[1], dst[2] = src[2], dst[3] = 255;
		break;
	case ConvR5G6B5:
		for (size_t i = 0; i < pixels; ++i, dst += 4, src += 2)
		{
			const u32 v = *reinterpret_cast<const u16*>(src);
			dst[0] = expand5(v & 0x1f), dst[1] = expand6((v >> 5) & 0x3f), dst[2] = expand5((v >> 11) & 0x1f), dst[3] = 255;
		}
		break;
	case ConvX1R5G5B5:
	case ConvA1R5G5B5:
		for (size_t i = 0; i < pixels; ++i, dst += 4, src += 2)
		{
			const u32 v = *reinterpret_cast<const u16*>(src);
			dst[0] = expand5(v & 0x1f), dst[1] = expand5((v >> 5) & 0x1f), dst[2] = expand5((v >> 10) & 0x1f);
			dst[3] = c == ConvA1R5G5B5 ? ((v & 0x8000) ? 255 : 0) : 255;
		}
		break;
	case ConvX4R4G4B4:
	case ConvA4R4G4B4:
		for (size_t i = 0; i < pixels; ++i, dst += 4, src += 2)
		{
			const u32 v = *reinterpret_cast<const u16*>(src);
			dst[0] = expand4(v & 0xf), dst[1] = expand4((v >> 4) & 0xf), dst[2] = expand4((v >> 8) & 0xf);
			dst[3] = c == ConvA4R4G4B4 ? expand4((v >> 12) & 0xf) : 255;
		}
		break;
	case ConvX8B8G8R8:
		for (size_t i = 0; i < pixels; ++i, dst += 4, src += 4)
			dst[0] = src[0], dst[1] = src[1], dst[2] = src[2], dst[3] = 255;
		break;
	case ConvA2R10G10B10:
		for (size_t i = 0; i < pixels; ++i, dst += 4, src += 4)
		{
			const u32 v = *reinterpret_cast<const u32*>(src);
			const u32 r = (v >> 20) & 0x3ff, g = (v >> 10) & 0x3ff, b = v & 0x3ff, a = v >> 30;
			*reinterpret_cast<u32*>(dst) = r | (g << 10) | (b << 20) | (a << 30);
		}
		break;
	default:
		break;
	}
}
} // namespace

bool info(const void* data, size_t length, Info& out)
{
	Layout l;
	if (!parse(data, length, l))
		return false;
	out.width = l.width;
	out.height = l.height;
	out.depth = l.depth;
	out.mips = l.mips;
	out.layers = l.layers;
	out.cube = l.cube;
	out.volume = l.volume;
	return true;
}

ID3DBaseTexture* load(const void* data, size_t length, int skip, bool staging, u32& mips_out, int& skipped)
{
	Layout l;
	if (!parse(data, length, l) || l.mips > 16)
		return nullptr;

	// The data: the mip chains of all layers (volumes: each mip with its depth slices), tightly packed
	size_t needed = 0;
	u32 pitch;
	for (u32 a = 0; a < l.layers; ++a)
		for (u32 m = 0; m < l.mips; ++m)
			needed += surface_bytes(l, _max(l.width >> m, 1u), _max(l.height >> m, 1u), l.src_bytes, pitch) *
				(l.volume ? _max(l.depth >> m, 1u) : 1u);
	if (length - l.offset < needed)
		return nullptr;

	// Keep at least one level, and block-compressed top levels a multiple of 4
	skip = _max(0, _min(skip, int(l.mips) - 1));
	while (skip > 0 && l.bc && (((l.width >> skip) & 3) || ((l.height >> skip) & 3)))
		--skip;

	const u32 new_mips = l.mips - u32(skip);
	xr_vector<D3D11_SUBRESOURCE_DATA> init(size_t(new_mips) * l.layers);
	xr_vector<u8> converted;
	if (l.conv != ConvNone)
	{
		size_t total = 0;
		for (u32 a = 0; a < l.layers; ++a)
			for (u32 m = u32(skip); m < l.mips; ++m)
				total += size_t(_max(l.width >> m, 1u)) * _max(l.height >> m, 1u) * (l.volume ? _max(l.depth >> m, 1u) : 1u) * l.dst_bytes;
		converted.resize(total);
	}
	const u8* src = static_cast<const u8*>(data) + l.offset;
	size_t out_offset = 0;
	for (u32 a = 0; a < l.layers; ++a)
	{
		for (u32 m = 0; m < l.mips; ++m)
		{
			const u32 w = _max(l.width >> m, 1u), h = _max(l.height >> m, 1u), d = l.volume ? _max(l.depth >> m, 1u) : 1u;
			const size_t size = surface_bytes(l, w, h, l.src_bytes, pitch) * d;
			if (m >= u32(skip))
			{
				D3D11_SUBRESOURCE_DATA& sd = init[a * new_mips + (m - skip)];
				if (l.conv == ConvNone)
				{
					sd.pSysMem = src;
					sd.SysMemPitch = pitch;
					sd.SysMemSlicePitch = UINT(size / d);
				}
				else
				{
					u8* dst = converted.data() + out_offset;
					convert(l.conv, src, dst, size_t(w) * h * d);
					sd.pSysMem = dst;
					sd.SysMemPitch = w * l.dst_bytes;
					sd.SysMemSlicePitch = w * h * l.dst_bytes;
					out_offset += size_t(w) * h * d * l.dst_bytes;
				}
			}
			src += size;
		}
	}

	const u32 width = _max(l.width >> skip, 1u), height = _max(l.height >> skip, 1u);
	const UINT usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_IMMUTABLE;
	const UINT bind = staging ? 0 : D3D11_BIND_SHADER_RESOURCE;
	const UINT cpu = staging ? D3D11_CPU_ACCESS_WRITE : 0;
	ID3DBaseTexture* result = nullptr;
	if (l.volume)
	{
		D3D11_TEXTURE3D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.Depth = _max(l.depth >> skip, 1u);
		desc.MipLevels = new_mips;
		desc.Format = l.format;
		desc.Usage = D3D11_USAGE(usage);
		desc.BindFlags = bind;
		desc.CPUAccessFlags = cpu;
		ID3DTexture3D* t = nullptr;
		if (FAILED(HW.pDevice->CreateTexture3D(&desc, init.data(), &t)))
			return nullptr;
		result = t;
	}
	else
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = new_mips;
		desc.ArraySize = l.layers;
		desc.Format = l.format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE(usage);
		desc.BindFlags = bind;
		desc.CPUAccessFlags = cpu;
		desc.MiscFlags = l.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
		ID3DTexture2D* t = nullptr;
		if (FAILED(HW.pDevice->CreateTexture2D(&desc, init.data(), &t)))
			return nullptr;
		result = t;
	}
	mips_out = new_mips;
	skipped = skip;
	return result;
}
} // namespace dds12
