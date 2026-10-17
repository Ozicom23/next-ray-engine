#pragma once
// DDS loading for the D3D12 renderer, which has no D3DX: every DXGI format as it lies in the file, the legacy D3D9
// formats DXGI lacks converted the way D3DX converted them (luminance and 16/24-bit RGB to RGBA8, A2R10G10B10 to
// R10G10B10A2), 2D, cube, volume; mip levels skipped for texture_lod / streaming

namespace dds12
{
struct Info
{
	u32 width = 0, height = 0, depth = 1, mips = 1, layers = 1;
	bool cube = false;
	bool volume = false;
};

// Reads the header; false for files this loader can't handle
bool info(const void* data, size_t length, Info& out);

// skip: mip levels to drop (at most all but one; block-compressed top levels stay multiples of 4). Returns the texture
// (STAGING with CPU write when staging, else IMMUTABLE), its mip count and the levels skipped
ID3DBaseTexture* load(const void* data, size_t length, int skip, bool staging, u32& mips_out, int& skipped);

// Files with a .dds name holding another image format (GAMMA's shaders\lut_atlas.dds is a PNG), which D3DX loaded
// by their content: decoded through WIC to R8G8B8A8_UNORM with one mip level, as D3DX made them
ID3DBaseTexture* load_image(const void* data, size_t length, bool staging, u32& width_out, u32& height_out);
} // namespace dds12
