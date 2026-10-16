#include "stdafx.h"
//#include "../../xrEngine/xr_effgamma.h"
#include "xr_effgamma.h"
#include "dxRenderDeviceRender.h"
#include "../xrRender/tga.h"
#include "../../xrEngine/xrImage_Resampler.h"

#if (defined(USE_DX10) || defined(USE_DX11)) && !defined(USE_DX12)
#include "d3dx10tex.h"
#endif	//	USE_DX10

#define	GAMESAVE_SIZE	128

extern u32 ps_r_screenshot_token;

IC u32 convert(float c)
{
	u32 C = iFloor(c);
	if (C > 255) C = 255;
	return C;
}

IC void MouseRayFromPoint(Fvector& direction, int x, int y, Fmatrix& m_CamMat)
{
	int halfwidth = Device.dwRenderWidth / 2;
	int halfheight = Device.dwRenderHeight / 2;

	Ivector2 point2;
	point2.set(x - halfwidth, halfheight - y);

	float size_y = VIEWPORT_NEAR * tanf(deg2rad(60.f) * 0.5f);
	float size_x = size_y / (Device.fHeight_2 / Device.fWidth_2);

	float r_pt = float(point2.x) * size_x / (float)halfwidth;
	float u_pt = float(point2.y) * size_y / (float)halfheight;

	direction.mul(m_CamMat.k, VIEWPORT_NEAR);
	direction.mad(direction, m_CamMat.j, u_pt);
	direction.mad(direction, m_CamMat.i, r_pt);
	direction.normalize();
}

#define SM_FOR_SEND_WIDTH 640
#define SM_FOR_SEND_HEIGHT 480

extern int ps_r4_hdr10_on;

#if defined(USE_DX12)
// D3D12 renderer: no D3DX. The back buffer is read back through a staging copy and encoded here: JPEG with libjpeg
// (linked into the exe), PNG with stored deflate blocks, DDS (BC1/BC3 or 32-bit) and TGA written directly.
#include <setjmp.h>
#pragma push_macro("FAR")
#undef FAR
#define XMD_H
#define boolean jpeg_boolean // libjpeg is built with boolean = int, rpcndr.h's boolean is unsigned char
extern "C" {
#include <jpeg/jpeglib.h>
}
#undef boolean
#undef XMD_H
#pragma pop_macro("FAR")

namespace dx12_screenshot
{
	// Back buffer pixels to D3DCOLOR (0xAARRGGBB) with alpha killed, rows tightly packed
	static bool convert(DXGI_FORMAT f, const u8* src, u32 pitch, u32 w, u32 h, u32* dst)
	{
		switch (f)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			for (u32 y = 0; y < h; y++)
			{
				const u8* p = src + size_t(y) * pitch;
				for (u32 x = 0; x < w; x++, p += 4)
					dst[size_t(y) * w + x] = color_xrgb(p[0], p[1], p[2]);
			}
			return true;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
			for (u32 y = 0; y < h; y++)
			{
				const u8* p = src + size_t(y) * pitch;
				for (u32 x = 0; x < w; x++, p += 4)
					dst[size_t(y) * w + x] = color_xrgb(p[2], p[1], p[0]);
			}
			return true;
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			for (u32 y = 0; y < h; y++)
			{
				const u8* p = src + size_t(y) * pitch;
				for (u32 x = 0; x < w; x++, p += 4)
				{
					u32 v;
					CopyMemory(&v, p, 4);
					dst[size_t(y) * w + x] = color_xrgb((v & 1023) >> 2, ((v >> 10) & 1023) >> 2, ((v >> 20) & 1023) >> 2);
				}
			}
			return true;
		default:
			return false;
		}
	}

	// The current back buffer through a staging texture (CPU read), as D3DCOLOR
	static bool read_back_buffer(xr_vector<u32>& pixels, u32& w, u32& h)
	{
		ID3DTexture2D* bb = nullptr;
		if (FAILED(HW.pDevice->GetBackBuffer(&bb)) || !bb)
		{
			Msg("! Screenshot: no back buffer");
			return false;
		}
		D3D11_TEXTURE2D_DESC desc;
		bb->GetDesc(&desc);
		D3D11_TEXTURE2D_DESC sd = desc;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		ID3DTexture2D* staging = nullptr;
		if (FAILED(HW.pDevice->CreateTexture2D(&sd, nullptr, &staging)) || !staging)
		{
			Msg("! Screenshot: staging texture %ux%u (format %d) creation failed", sd.Width, sd.Height, int(sd.Format));
			_RELEASE(bb);
			return false;
		}
		HW.pContext->CopyResource(staging, bb);
		_RELEASE(bb);

		bool ok = false;
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(HW.pContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
		{
			w = desc.Width;
			h = desc.Height;
			pixels.resize(size_t(w) * h);
			ok = convert(desc.Format, static_cast<const u8*>(mapped.pData), mapped.RowPitch, w, h, pixels.data());
			HW.pContext->Unmap(staging, 0);
			if (!ok)
				Msg("! Screenshot: back buffer format %d not supported", int(desc.Format));
		}
		else
			Msg("! Screenshot: staging texture map failed");
		_RELEASE(staging);
		return ok;
	}

	static void resample(xr_vector<u32>& pixels, u32 w, u32 h, u32 dw, u32 dh, xr_vector<u32>& out, EIMF_Type filter)
	{
		out.resize(size_t(dw) * dh);
		if (dw == w && dh == h)
			CopyMemory(out.data(), pixels.data(), out.size() * 4);
		else
			imf_Process(out.data(), dw, dh, pixels.data(), w, h, filter);
	}

	// ------------------------------------------------------------------------------------------------------------
	// DDS

	static u16 to565(int r, int g, int b) { return u16(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }

	static void from565(u16 c, int* rgb)
	{
		const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
		rgb[0] = (r << 3) | (r >> 2);
		rgb[1] = (g << 2) | (g >> 4);
		rgb[2] = (b << 3) | (b >> 2);
	}

	// BC1 color block of 16 D3DCOLOR pixels: endpoints from the block's bounding box, four color mode
	static void bc1_block(const u32* px, u8* out)
	{
		int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
		for (u32 i = 0; i < 16; i++)
		{
			const int c[3] = { int(color_get_R(px[i])), int(color_get_G(px[i])), int(color_get_B(px[i])) };
			for (u32 k = 0; k < 3; k++)
			{
				mn[k] = _min(mn[k], c[k]);
				mx[k] = _max(mx[k], c[k]);
			}
		}
		for (u32 k = 0; k < 3; k++)
		{
			const int inset = (mx[k] - mn[k]) >> 4;
			mn[k] += inset;
			mx[k] -= inset;
		}
		u16 c0 = to565(mx[0], mx[1], mx[2]);
		u16 c1 = to565(mn[0], mn[1], mn[2]);
		if (c0 < c1)
			std::swap(c0, c1);
		u32 indices = 0;
		if (c0 != c1)
		{
			int pal[4][3];
			from565(c0, pal[0]);
			from565(c1, pal[1]);
			for (u32 k = 0; k < 3; k++)
			{
				pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
				pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
			}
			for (u32 i = 0; i < 16; i++)
			{
				const int c[3] = { int(color_get_R(px[i])), int(color_get_G(px[i])), int(color_get_B(px[i])) };
				u32 best = 0;
				int best_d = 0x7fffffff;
				for (u32 j = 0; j < 4; j++)
				{
					const int dr = c[0] - pal[j][0], dg = c[1] - pal[j][1], db = c[2] - pal[j][2];
					const int d = dr * dr + dg * dg + db * db;
					if (d < best_d)
					{
						best_d = d;
						best = j;
					}
				}
				indices |= best << (2 * i);
			}
		}
		out[0] = u8(c0 & 0xff);
		out[1] = u8(c0 >> 8);
		out[2] = u8(c1 & 0xff);
		out[3] = u8(c1 >> 8);
		out[4] = u8(indices & 0xff);
		out[5] = u8((indices >> 8) & 0xff);
		out[6] = u8((indices >> 16) & 0xff);
		out[7] = u8(indices >> 24);
	}

	// BC1, or BC3 with opaque alpha
	static void compress_bc(const u32* px, u32 w, u32 h, bool bc3, xr_vector<u8>& out)
	{
		const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
		for (u32 by = 0; by < bh; by++)
			for (u32 bx = 0; bx < bw; bx++)
			{
				u32 block[16];
				for (u32 j = 0; j < 4; j++)
					for (u32 i = 0; i < 4; i++)
					{
						const u32 x = _min(bx * 4 + i, w - 1), y = _min(by * 4 + j, h - 1);
						block[j * 4 + i] = px[size_t(y) * w + x];
					}
				if (bc3)
				{
					const u8 alpha[8] = { 255, 255, 0, 0, 0, 0, 0, 0 };
					out.insert(out.end(), alpha, alpha + 8);
				}
				u8 color[8];
				bc1_block(block, color);
				out.insert(out.end(), color, color + 8);
			}
	}

	// Next mip level, 2x2 box filter
	static void half_size(const xr_vector<u32>& src, u32 sw, u32 sh, xr_vector<u32>& dst, u32& dw, u32& dh)
	{
		dw = _max(1u, sw / 2);
		dh = _max(1u, sh / 2);
		dst.resize(size_t(dw) * dh);
		for (u32 y = 0; y < dh; y++)
			for (u32 x = 0; x < dw; x++)
			{
				const u32 x0 = _min(x * 2, sw - 1), x1 = _min(x * 2 + 1, sw - 1);
				const u32 y0 = _min(y * 2, sh - 1), y1 = _min(y * 2 + 1, sh - 1);
				const u32 c[4] = { src[size_t(y0) * sw + x0], src[size_t(y0) * sw + x1], src[size_t(y1) * sw + x0], src[size_t(y1) * sw + x1] };
				u32 r = 0, g = 0, b = 0, a = 0;
				for (u32 i = 0; i < 4; i++)
				{
					r += color_get_R(c[i]);
					g += color_get_G(c[i]);
					b += color_get_B(c[i]);
					a += color_get_A(c[i]);
				}
				dst[size_t(y) * dw + x] = color_argb((a + 2) / 4, (r + 2) / 4, (g + 2) / 4, (b + 2) / 4);
			}
	}

	struct dds_pixelformat
	{
		u32 size, flags, fourcc, rgb_bits, r_mask, g_mask, b_mask, a_mask;
	};
	struct dds_header
	{
		u32 size, flags, height, width, pitch_or_linear_size, depth, mip_count, reserved1[11];
		dds_pixelformat pf;
		u32 caps, caps2, caps3, caps4, reserved2;
	};
	static_assert(sizeof(dds_header) == 124, "DDS header size");

	enum dds_format
	{
		dds_bgra8,
		dds_bc1,
		dds_bc3,
	};

	// A DDS of the image resized to w x h; the 32-bit format with a full mip chain if asked
	static void make_dds(xr_vector<u32>& pixels, u32 src_w, u32 src_h, u32 w, u32 h, dds_format format, bool mips, xr_vector<u8>& out)
	{
		xr_vector<u32> level;
		resample(pixels, src_w, src_h, w, h, level, imf_triangle);

		u32 levels = 1;
		if (mips && format == dds_bgra8)
			for (u32 d = _max(w, h); d > 1; d /= 2)
				levels++;

		dds_header hdr;
		ZeroMemory(&hdr, sizeof(hdr));
		hdr.size = sizeof(dds_header);
		hdr.flags = 0x1 | 0x2 | 0x4 | 0x1000; // caps, height, width, pixel format
		hdr.height = h;
		hdr.width = w;
		hdr.mip_count = levels;
		hdr.pf.size = sizeof(dds_pixelformat);
		hdr.caps = 0x1000; // texture
		if (levels > 1)
		{
			hdr.flags |= 0x20000; // mip count
			hdr.caps |= 0x8 | 0x400000; // complex, mipmap
		}
		if (format == dds_bgra8)
		{
			hdr.flags |= 0x8; // pitch
			hdr.pitch_or_linear_size = w * 4;
			hdr.pf.flags = 0x40 | 0x1; // RGB, alpha pixels
			hdr.pf.rgb_bits = 32;
			hdr.pf.r_mask = 0x00ff0000;
			hdr.pf.g_mask = 0x0000ff00;
			hdr.pf.b_mask = 0x000000ff;
			hdr.pf.a_mask = 0xff000000;
		}
		else
		{
			hdr.flags |= 0x80000; // linear size
			hdr.pitch_or_linear_size = ((w + 3) / 4) * ((h + 3) / 4) * (format == dds_bc1 ? 8 : 16);
			hdr.pf.flags = 0x4; // fourcc
			hdr.pf.fourcc = u32('D') | (u32('X') << 8) | (u32('T') << 16) | (u32(format == dds_bc1 ? '1' : '5') << 24);
		}

		out.clear();
		const u32 magic = u32('D') | (u32('D') << 8) | (u32('S') << 16) | (u32(' ') << 24);
		out.insert(out.end(), reinterpret_cast<const u8*>(&magic), reinterpret_cast<const u8*>(&magic) + 4);
		out.insert(out.end(), reinterpret_cast<const u8*>(&hdr), reinterpret_cast<const u8*>(&hdr) + sizeof(hdr));

		if (format != dds_bgra8)
		{
			compress_bc(level.data(), w, h, format == dds_bc3, out);
			return;
		}
		u32 lw = w, lh = h;
		xr_vector<u32> next;
		for (u32 l = 0; l < levels; l++)
		{
			const u8* data = reinterpret_cast<const u8*>(level.data());
			out.insert(out.end(), data, data + level.size() * 4);
			if (l + 1 < levels)
			{
				u32 nw, nh;
				half_size(level, lw, lh, next, nw, nh);
				level.swap(next);
				lw = nw;
				lh = nh;
			}
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// PNG: 8-bit RGB, zlib stream of stored (uncompressed) deflate blocks

	static u32 crc_table[256];
	static bool crc_ready = false;

	static u32 png_crc(u32 crc, const u8* data, size_t size)
	{
		if (!crc_ready)
		{
			for (u32 n = 0; n < 256; n++)
			{
				u32 c = n;
				for (u32 k = 0; k < 8; k++)
					c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
				crc_table[n] = c;
			}
			crc_ready = true;
		}
		crc = ~crc;
		for (size_t i = 0; i < size; i++)
			crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
		return ~crc;
	}

	static void put_be32(xr_vector<u8>& out, u32 v)
	{
		const u8 b[4] = { u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v) };
		out.insert(out.end(), b, b + 4);
	}

	static void png_chunk(xr_vector<u8>& out, const char* type, const u8* data, size_t size)
	{
		put_be32(out, u32(size));
		const size_t start = out.size();
		out.insert(out.end(), reinterpret_cast<const u8*>(type), reinterpret_cast<const u8*>(type) + 4);
		if (size)
			out.insert(out.end(), data, data + size);
		put_be32(out, png_crc(0, out.data() + start, out.size() - start));
	}

	static void encode_png(const u32* px, u32 w, u32 h, xr_vector<u8>& out)
	{
		// Scanlines: filter type 0, then RGB
		xr_vector<u8> raw;
		raw.resize(size_t(h) * (size_t(w) * 3 + 1));
		u8* r = raw.data();
		for (u32 y = 0; y < h; y++)
		{
			*r++ = 0;
			for (u32 x = 0; x < w; x++)
			{
				const u32 c = px[size_t(y) * w + x];
				*r++ = u8(color_get_R(c));
				*r++ = u8(color_get_G(c));
				*r++ = u8(color_get_B(c));
			}
		}

		// zlib: header, stored blocks of up to 65535 bytes, adler32
		xr_vector<u8> z;
		z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
		z.push_back(0x78);
		z.push_back(0x01);
		size_t pos = 0;
		do
		{
			const size_t len = _min(raw.size() - pos, size_t(65535));
			const bool last = pos + len == raw.size();
			const u8 head[5] = { u8(last ? 1 : 0), u8(len & 0xff), u8(len >> 8), u8(~len & 0xff), u8((~len >> 8) & 0xff) };
			z.insert(z.end(), head, head + 5);
			z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
			pos += len;
		} while (pos < raw.size());
		u32 a = 1, b = 0;
		for (size_t i = 0; i < raw.size(); i++)
		{
			a = (a + raw[i]) % 65521;
			b = (b + a) % 65521;
		}
		put_be32(z, (b << 16) | a);

		out.clear();
		const u8 signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
		out.insert(out.end(), signature, signature + 8);
		u8 ihdr[13] = { u8(w >> 24), u8(w >> 16), u8(w >> 8), u8(w), u8(h >> 24), u8(h >> 16), u8(h >> 8), u8(h),
		                8, 2, 0, 0, 0 }; // 8 bit, RGB, deflate, no filter, no interlace
		png_chunk(out, "IHDR", ihdr, sizeof(ihdr));
		png_chunk(out, "IDAT", z.data(), z.size());
		png_chunk(out, "IEND", nullptr, 0);
	}

	// ------------------------------------------------------------------------------------------------------------
	// JPEG through libjpeg, into memory

	struct jpeg_error_jump
	{
		jpeg_error_mgr mgr;
		jmp_buf jump;
	};

	struct jpeg_memory_dest
	{
		jpeg_destination_mgr mgr;
		xr_vector<u8>* out;
		JOCTET buffer[16384];
	};

	static void jpeg_error_exit(j_common_ptr cinfo)
	{
		char text[JMSG_LENGTH_MAX];
		(*cinfo->err->format_message)(cinfo, text);
		Msg("! Screenshot: JPEG encoding failed: %s", text);
		longjmp(reinterpret_cast<jpeg_error_jump*>(cinfo->err)->jump, 1);
	}

	static void jpeg_no_message(j_common_ptr) {}

	static void jpeg_dest_init(j_compress_ptr cinfo)
	{
		jpeg_memory_dest* d = reinterpret_cast<jpeg_memory_dest*>(cinfo->dest);
		d->mgr.next_output_byte = d->buffer;
		d->mgr.free_in_buffer = sizeof(d->buffer);
	}

	static jpeg_boolean jpeg_dest_empty(j_compress_ptr cinfo)
	{
		jpeg_memory_dest* d = reinterpret_cast<jpeg_memory_dest*>(cinfo->dest);
		d->out->insert(d->out->end(), d->buffer, d->buffer + sizeof(d->buffer));
		d->mgr.next_output_byte = d->buffer;
		d->mgr.free_in_buffer = sizeof(d->buffer);
		return TRUE;
	}

	static void jpeg_dest_term(j_compress_ptr cinfo)
	{
		jpeg_memory_dest* d = reinterpret_cast<jpeg_memory_dest*>(cinfo->dest);
		d->out->insert(d->out->end(), d->buffer, d->buffer + (sizeof(d->buffer) - d->mgr.free_in_buffer));
	}

	// rgb: w x h, 3 bytes per pixel. No C++ objects with destructors live in this frame (longjmp on errors)
	static bool encode_jpeg_rgb(u8* rgb, u32 w, u32 h, int quality, xr_vector<u8>* out)
	{
		jpeg_compress_struct cinfo;
		jpeg_error_jump err;
		jpeg_memory_dest dest;
		ZeroMemory(&cinfo, sizeof(cinfo));
		cinfo.err = jpeg_std_error(&err.mgr);
		err.mgr.error_exit = jpeg_error_exit;
		err.mgr.output_message = jpeg_no_message;
		if (setjmp(err.jump))
		{
			jpeg_destroy_compress(&cinfo);
			return false;
		}
		jpeg_create_compress(&cinfo);
		dest.out = out;
		dest.mgr.init_destination = jpeg_dest_init;
		dest.mgr.empty_output_buffer = jpeg_dest_empty;
		dest.mgr.term_destination = jpeg_dest_term;
		cinfo.dest = &dest.mgr;
		cinfo.image_width = w;
		cinfo.image_height = h;
		cinfo.input_components = 3;
		cinfo.in_color_space = JCS_RGB;
		jpeg_set_defaults(&cinfo);
		jpeg_set_quality(&cinfo, quality, TRUE);
		jpeg_start_compress(&cinfo, TRUE);
		while (cinfo.next_scanline < cinfo.image_height)
		{
			JSAMPROW row = rgb + size_t(cinfo.next_scanline) * w * 3;
			jpeg_write_scanlines(&cinfo, &row, 1);
		}
		jpeg_finish_compress(&cinfo);
		jpeg_destroy_compress(&cinfo);
		return true;
	}

	static bool encode_jpeg(const u32* px, u32 w, u32 h, xr_vector<u8>& out)
	{
		xr_vector<u8> rgb;
		rgb.resize(size_t(w) * h * 3);
		u8* p = rgb.data();
		for (size_t i = 0, n = size_t(w) * h; i < n; i++)
		{
			*p++ = u8(color_get_R(px[i]));
			*p++ = u8(color_get_G(px[i]));
			*p++ = u8(color_get_B(px[i]));
		}
		out.clear();
		return encode_jpeg_rgb(rgb.data(), w, h, 95, &out);
	}

	static void write_file(LPCSTR initial, LPCSTR name, const xr_vector<u8>& data)
	{
		IWriter* fs = FS.w_open(initial, name);
		if (!fs)
		{
			Msg("! Screenshot: cannot write %s", name);
			return;
		}
		fs->w(data.data(), u32(data.size()));
		FS.w_close(fs);
	}
}
#endif // USE_DX12

#if defined(USE_DX12)
void CRender::ScreenshotImpl(ScreenshotMode mode, LPCSTR name, CMemoryWriter* memory_writer)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}

	xr_vector<u32> pixels;
	u32 w = 0, h = 0;
	if (!dx12_screenshot::read_back_buffer(pixels, w, h))
		return;

	// Save
	switch (mode)
	{
	case IRender_interface::SM_FOR_GAMESAVE:
		{
			xr_vector<u8> dds;
			dx12_screenshot::make_dds(pixels, w, h, GAMESAVE_SIZE, GAMESAVE_SIZE, dx12_screenshot::dds_bc1, false, dds);
			dx12_screenshot::write_file(nullptr, name, dds);
		}
		break;
	case IRender_interface::SM_FOR_MPSENDING:
		{
			xr_vector<u8> dds;
			dx12_screenshot::make_dds(pixels, w, h, SM_FOR_SEND_WIDTH, SM_FOR_SEND_HEIGHT, dx12_screenshot::dds_bc1, false, dds);
			if (!memory_writer)
				dx12_screenshot::write_file(nullptr, name, dds);
			else
				memory_writer->w(dds.data(), u32(dds.size()));
		}
		break;
	case IRender_interface::SM_NORMAL:
		{
			string64 t_stemp;
			string_path buf;
			xr_sprintf(buf, sizeof(buf), "ss_%s_%s_(%s).%s", Core.UserName, timestamp(t_stemp),
				(g_pGameLevel) ? g_pGameLevel->name().c_str() : "mainmenu", ps_r_screenshot_token == 0 ? "jpg" : (ps_r_screenshot_token == 1 ? "png" : "tga"));
			if (ps_r_screenshot_token == 0)
			{
				xr_vector<u8> jpg;
				if (dx12_screenshot::encode_jpeg(pixels.data(), w, h, jpg))
					dx12_screenshot::write_file("$screenshots$", buf, jpg);
			}
			else if (ps_r_screenshot_token == 1)
			{
				xr_vector<u8> png;
				dx12_screenshot::encode_png(pixels.data(), w, h, png);
				dx12_screenshot::write_file("$screenshots$", buf, png);
			}
			else
			{
				IWriter* fs = FS.w_open("$screenshots$", buf);
				R_ASSERT(fs);
				TGAdesc p;
				p.format = IMG_32B;
				p.scanlenght = w * 4;
				p.width = w;
				p.height = h;
				p.data = pixels.data();
				p.maketga(*fs);
				FS.w_close(fs);
			}
		}
		break;
	case IRender_interface::SM_FOR_LEVELMAP:
	case IRender_interface::SM_FOR_CUBEMAP:
		{
			string_path buf;
			VERIFY(name);
			strconcat(sizeof(buf), buf, name, ".tga");

			xr_vector<u32> data;
			dx12_screenshot::resample(pixels, w, h, h, h, data, imf_lanczos3);

			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);

			TGAdesc p;
			p.format = IMG_24B;
			p.scanlenght = h * 4;
			p.width = h;
			p.height = h;
			p.data = data.data();
			p.maketga(*fs);
			FS.w_close(fs);
		}
		break;
	}
}

#elif defined(USE_DX10) || defined(USE_DX11)
void CRender::ScreenshotImpl(ScreenshotMode mode, LPCSTR name, CMemoryWriter* memory_writer)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}

	ID3DResource* pSrcTexture;
	HW.pBaseRT->GetResource(&pSrcTexture);

	VERIFY(pSrcTexture);

	// Save
	switch (mode)
	{
	case IRender_interface::SM_FOR_GAMESAVE:
		{
			ID3DTexture2D* pSrcSmallTexture;

			D3D_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = GAMESAVE_SIZE;
			desc.Height = GAMESAVE_SIZE;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_BC1_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D_USAGE_DEFAULT;
			desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
			CHK_DX(HW.pDevice->CreateTexture2D( &desc, NULL, &pSrcSmallTexture ));

			//	D3DX10_TEXTURE_LOAD_INFO *pLoadInfo

#ifdef USE_DX11
			CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
				NULL, pSrcSmallTexture ));
#else
			CHK_DX(D3DX10LoadTextureFromTexture( pSrcTexture,
				NULL, pSrcSmallTexture ));
#endif

			// save (logical & physical)
			ID3DBlob* saved = 0;
#ifdef USE_DX11
			HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
#else
			HRESULT hr = D3DX10SaveTextureToMemory(pSrcSmallTexture, D3DX10_IFF_DDS, &saved, 0);
			//HRESULT hr					= D3DXSaveTextureToFileInMemory (&saved,D3DXIFF_DDS,texture,0);
#endif
			if (hr == D3D_OK)
			{
				IWriter* fs = FS.w_open(name);
				if (fs)
				{
					fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
					FS.w_close(fs);
				}
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(pSrcSmallTexture);
		}
		break;
	case IRender_interface::SM_FOR_MPSENDING:
		{
			ID3DTexture2D* pSrcSmallTexture;

			D3D_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = SM_FOR_SEND_WIDTH;
			desc.Height = SM_FOR_SEND_HEIGHT;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_BC1_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D_USAGE_DEFAULT;
			desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
			CHK_DX(HW.pDevice->CreateTexture2D( &desc, NULL, &pSrcSmallTexture ));

			//	D3DX10_TEXTURE_LOAD_INFO *pLoadInfo

#ifdef USE_DX11
			CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
				NULL, pSrcSmallTexture ));
#else
			CHK_DX(D3DX10LoadTextureFromTexture( pSrcTexture,
				NULL, pSrcSmallTexture ));
#endif
			// save (logical & physical)
			ID3DBlob* saved = 0;
#ifdef USE_DX11
			HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
#else
			HRESULT hr = D3DX10SaveTextureToMemory(pSrcSmallTexture, D3DX10_IFF_DDS, &saved, 0);
			//HRESULT hr					= D3DXSaveTextureToFileInMemory (&saved,D3DXIFF_DDS,texture,0);
#endif
			if (hr == D3D_OK)
			{
				if (!memory_writer)
				{
					IWriter* fs = FS.w_open(name);
					if (fs)
					{
						fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
						FS.w_close(fs);
					}
				}
				else
				{
					memory_writer->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
				}
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(pSrcSmallTexture);
		}
		break;
	case IRender_interface::SM_NORMAL:
		{
			string64 t_stemp;
			string_path buf;
			xr_sprintf(buf, sizeof(buf), "ss_%s_%s_(%s).%s", Core.UserName, timestamp(t_stemp),
				(g_pGameLevel) ? g_pGameLevel->name().c_str() : "mainmenu", ps_r_screenshot_token == 0 ? "jpg" : (ps_r_screenshot_token == 1 ? "png" : "tga"));
			ID3DBlob* saved = 0;
#ifdef USE_DX11
			CHK_DX(D3DX11SaveTextureToMemory(HW.pContext, pSrcTexture, ps_r_screenshot_token == 0 ? D3DX11_IFF_JPG : (ps_r_screenshot_token == 1 ? D3DX11_IFF_PNG : D3DX11_IFF_BMP), &saved, 0));
#else
			CHK_DX(D3DX10SaveTextureToMemory( pSrcTexture, ps_r_screenshot_token == 0 ? D3DX10_IFF_JPG : (ps_r_screenshot_token == 1 ? D3DX10_IFF_PNG : D3DX10_IFF_BMP), &saved, 0));
#endif
			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);
			fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
			FS.w_close(fs);
			_RELEASE(saved);
		}
		break;
	case IRender_interface::SM_FOR_LEVELMAP:
	case IRender_interface::SM_FOR_CUBEMAP:
		{
		string_path buf;
		VERIFY(name);
		strconcat(sizeof(buf), buf, name, ".tga");

		// TODO: this won't work due to texture format incompat
		ID3DTexture2D* pTex = Target->t_ss_async;
		HW.pContext->CopyResource(pTex, pSrcTexture);

		D3D_MAPPED_TEXTURE2D MappedData;
#ifdef USE_DX11
		HW.pContext->Map(pTex, 0, D3D_MAP_READ, 0, &MappedData);
#else
		pTex->Map(0, D3D_MAP_READ, 0, &MappedData);
#endif
		// Swap r and b, but don't kill alpha
		{
			u32* pPixel = (u32*)MappedData.pData;
			u32* pEnd = pPixel + (Device.dwRenderWidth * Device.dwRenderHeight);

			for (; pPixel != pEnd; pPixel++)
			{
				u32 p = *pPixel;
				*pPixel = color_argb(color_get_A(p), color_get_B(p), color_get_G(p), color_get_R(p));
			}
		}

		u32* data = (u32*)xr_malloc(Device.dwRenderHeight * Device.dwRenderHeight * 4);
		imf_Process(data, Device.dwRenderHeight, Device.dwRenderHeight, (u32*)MappedData.pData, Device.dwRenderWidth, Device.dwRenderHeight, imf_lanczos3);
#ifdef USE_DX11
		HW.pContext->Unmap(pTex, 0);
#else
		pTex->Unmap(0);
#endif
		IWriter* fs = FS.w_open("$screenshots$", buf);
		R_ASSERT(fs);

		TGAdesc p;
		p.format = IMG_24B;
		p.scanlenght = Device.dwRenderHeight * 4;
		p.width = Device.dwRenderHeight;
		p.height = Device.dwRenderHeight;
		p.data = data;
		p.maketga(*fs);
		xr_free(data);
		FS.w_close(fs);
		}
		break;
	}

	_RELEASE(pSrcTexture);
}

#else	//	USE_DX10

void CRender::ScreenshotImpl(ScreenshotMode mode, LPCSTR name, CMemoryWriter* memory_writer)
{
	if (!Device.b_is_Ready) return;

	// Create temp-surface
	IDirect3DSurface9* pFB;
	D3DLOCKED_RECT D;

	HRESULT hr = HW.pDevice->CreateOffscreenPlainSurface(Device.dwRenderWidth, Device.dwRenderHeight, HW.DevPP.BackBufferFormat,
	                                                     D3DPOOL_SYSTEMMEM, &pFB, nullptr);
	if (FAILED(hr)) return;

	hr = HW.pDevice->GetRenderTargetData(HW.pBaseRT, pFB);
	if (FAILED(hr)) goto _end_;

	hr = pFB->LockRect(&D, 0, D3DLOCK_NOSYSLOCK);
	if (FAILED(hr)) goto _end_;

	// Image processing (gamma-correct)
	u32* pPixel = (u32*)D.pBits;
	u32* pEnd = pPixel + (Device.dwRenderWidth * Device.dwRenderHeight);

	//	Kill alpha
	for (; pPixel != pEnd; pPixel++)
	{
		u32 p = *pPixel;
		*pPixel = color_xrgb(color_get_R(p), color_get_G(p), color_get_B(p));
	}

	hr = pFB->UnlockRect();
	if (hr != D3D_OK) goto _end_;


	// Save
	switch (mode)
	{
	case IRender_interface::SM_FOR_GAMESAVE:
		{
			// texture
			ID3DTexture2D* texture = NULL;
			hr = D3DXCreateTexture(HW.pDevice,GAMESAVE_SIZE,GAMESAVE_SIZE, 1, 0, D3DFMT_DXT1, D3DPOOL_SCRATCH,
			                       &texture);
			if (hr != D3D_OK) goto _end_;
			if (NULL == texture) goto _end_;

			// resize&convert to surface
			IDirect3DSurface9* surface = 0;
			hr = texture->GetSurfaceLevel(0, &surface);
			if (hr != D3D_OK) goto _end_;
			VERIFY(surface);
			hr = D3DXLoadSurfaceFromSurface(surface, 0, 0, pFB, 0, 0,D3DX_DEFAULT, 0);
			_RELEASE(surface);
			if (hr != D3D_OK) goto _end_;

			// save (logical & physical)
			ID3DBlob* saved = 0;
			hr = D3DXSaveTextureToFileInMemory(&saved, D3DXIFF_DDS, texture, 0);
			if (hr != D3D_OK) goto _end_;

			IWriter* fs = FS.w_open(name);
			if (fs)
			{
				fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
				FS.w_close(fs);
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(texture);
		}
		break;
	case IRender_interface::SM_FOR_MPSENDING:
		{
			// texture
			ID3DTexture2D* texture = NULL;
			hr = D3DXCreateTexture(HW.pDevice,SM_FOR_SEND_WIDTH,SM_FOR_SEND_HEIGHT, 1, 0, D3DFMT_R8G8B8,
			                       D3DPOOL_SCRATCH, &texture);
			if (hr != D3D_OK) goto _end_;
			if (NULL == texture) goto _end_;

			// resize&convert to surface
			IDirect3DSurface9* surface = 0;
			hr = texture->GetSurfaceLevel(0, &surface);
			if (hr != D3D_OK) goto _end_;
			VERIFY(surface);
			hr = D3DXLoadSurfaceFromSurface(surface, 0, 0, pFB, 0, 0,D3DX_DEFAULT, 0);
			_RELEASE(surface);
			if (hr != D3D_OK) goto _end_;

			// save (logical & physical)
			ID3DBlob* saved = 0;
			hr = D3DXSaveTextureToFileInMemory(&saved, D3DXIFF_DDS, texture, 0);
			if (hr != D3D_OK) goto _end_;

			if (!memory_writer)
			{
				IWriter* fs = FS.w_open(name);
				if (fs)
				{
					fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
					FS.w_close(fs);
				}
			}
			else
			{
				memory_writer->w(saved->GetBufferPointer(), saved->GetBufferSize());
			}

			_RELEASE(saved);

			// cleanup
			_RELEASE(texture);
		}
		break;
	case IRender_interface::SM_NORMAL:
		{
			string64 t_stemp;
			string_path buf;
			xr_sprintf(buf, sizeof(buf), "ss_%s_%s_(%s).%s", Core.UserName, timestamp(t_stemp),
			           (g_pGameLevel) ? g_pGameLevel->name().c_str() : "mainmenu", ps_r_screenshot_token == 0 ? "jpg" : (ps_r_screenshot_token == 1 ? "png" : "tga"));
			ID3DBlob* saved = 0;
			CHK_DX(D3DXSaveSurfaceToFileInMemory (&saved, ps_r_screenshot_token == 0 ? D3DXIFF_JPG : (ps_r_screenshot_token == 1 ? D3DXIFF_PNG : D3DXIFF_TGA) ,pFB,0,0));
			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);
			fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
			FS.w_close(fs);
			_RELEASE(saved);
		}
		break;
	case IRender_interface::SM_FOR_LEVELMAP:
	case IRender_interface::SM_FOR_CUBEMAP:
		{
			//				string64			t_stemp;
			string_path buf;
			VERIFY(name);
			strconcat(sizeof(buf), buf, name, ".tga");
			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);
			TGAdesc p;
			p.format = IMG_24B;

			//	TODO: DX10: This is totally incorrect but mimics 
			//	original behavior. Fix later.
			hr = pFB->LockRect(&D, 0,D3DLOCK_NOSYSLOCK);
			if (hr != D3D_OK) return;
			hr = pFB->UnlockRect();
			if (hr != D3D_OK) goto _end_;

			// save
			u32* data = (u32*)xr_malloc(Device.dwRenderHeight * Device.dwRenderHeight * 4);
			imf_Process(data, Device.dwRenderHeight, Device.dwRenderHeight, (u32*)D.pBits, Device.dwRenderWidth, Device.dwRenderHeight,
			            imf_lanczos3);
			p.scanlenght = Device.dwRenderHeight * 4;
			p.width = Device.dwRenderHeight;
			p.height = Device.dwRenderHeight;
			p.data = data;
			p.maketga(*fs);
			xr_free(data);

			FS.w_close(fs);
		}
		break;
	}

_end_:
	_RELEASE(pFB);
}

#endif	//	USE_DX10

void CRender::Screenshot(ScreenshotMode mode, LPCSTR name)
{
	ScreenshotImpl(mode, name, NULL);
}

void CRender::Screenshot(ScreenshotMode mode, CMemoryWriter& memory_writer)
{
	if (mode != SM_FOR_MPSENDING)
	{
		Log("~ Not implemented screenshot mode...");
		return;
	}
	ScreenshotImpl(mode, NULL, &memory_writer);
}

void CRender::ScreenshotAsyncBegin()
{
	VERIFY(!m_bMakeAsyncSS);
	m_bMakeAsyncSS = true;
}

#if defined(USE_DX12)

void CRender::ScreenshotAsyncEnd(CMemoryWriter& memory_writer)
{
	VERIFY(!m_bMakeAsyncSS);

	//	Don't own. No need to release.
	ID3DTexture2D* pTex = Target->t_ss_async;
	if (!pTex)
		return;

	D3D11_TEXTURE2D_DESC desc;
	pTex->GetDesc(&desc);
	D3D11_MAPPED_SUBRESOURCE MappedData;
	if (FAILED(HW.pContext->Map(pTex, 0, D3D11_MAP_READ, 0, &MappedData)))
	{
		Msg("! ScreenshotAsyncEnd: staging texture map failed");
		return;
	}

	//	Kill alpha and swap r and b; rows follow the mapped pitch
	u32 w = desc.Width, h = desc.Height;
	memory_writer.w(&w, sizeof(w));
	memory_writer.w(&h, sizeof(h));
	xr_vector<u32> row(w);
	for (u32 y = 0; y < h; y++)
	{
		const u32* src = reinterpret_cast<const u32*>(static_cast<const u8*>(MappedData.pData) + size_t(y) * MappedData.RowPitch);
		for (u32 x = 0; x < w; x++)
		{
			const u32 p = src[x];
			row[x] = color_xrgb(color_get_B(p), color_get_G(p), color_get_R(p));
		}
		memory_writer.w(row.data(), w * 4);
	}

	HW.pContext->Unmap(pTex, 0);
}

#elif defined(USE_DX10) || defined(USE_DX11)

void CRender::ScreenshotAsyncEnd(CMemoryWriter& memory_writer)
{
	VERIFY(!m_bMakeAsyncSS);

	//	Don't own. No need to release.
	ID3DTexture2D* pTex = Target->t_ss_async;

	D3D_MAPPED_TEXTURE2D MappedData;

#ifdef USE_DX11
	HW.pContext->Map(pTex, 0, D3D_MAP_READ, 0, &MappedData);
#else
	pTex->Map(0, D3D_MAP_READ, 0, &MappedData);
#endif

	{
		u32* pPixel = (u32*)MappedData.pData;
		u32* pEnd = pPixel + (Device.dwRenderWidth * Device.dwRenderHeight);

		//	Kill alpha and swap r and b.
		for (; pPixel != pEnd; pPixel++)
		{
			u32 p = *pPixel;
			*pPixel = color_xrgb(
				color_get_B(p),
				color_get_G(p),
				color_get_R(p)
			);
		}

		memory_writer.w(&Device.dwRenderWidth, sizeof(Device.dwRenderWidth));
		memory_writer.w(&Device.dwRenderHeight, sizeof(Device.dwRenderHeight));
		memory_writer.w(MappedData.pData, (Device.dwRenderWidth * Device.dwRenderHeight) * 4);
	}

#ifdef USE_DX11
	HW.pContext->Unmap(pTex, 0);
#else
	pTex->Unmap(0);
#endif
}

#else	//	USE_DX10

void CRender::ScreenshotAsyncEnd(CMemoryWriter& memory_writer)
{
	if (!Device.b_is_Ready) return;

	VERIFY(!m_bMakeAsyncSS);

	D3DLOCKED_RECT D;
	HRESULT hr;
	IDirect3DSurface9* pFB;

	pFB = Target->pFB;

	hr = pFB->LockRect(&D, 0,D3DLOCK_NOSYSLOCK);
	if (hr != D3D_OK) return;

#if	RENDER == R_R1
	u32 rtWidth = Target->get_rtwidth();
	u32 rtHeight = Target->get_rtheight();
#else	//	RENDER != R_R1
	u32 rtWidth = Device.dwRenderWidth;
	u32 rtHeight = Device.dwRenderHeight;
#endif	//	RENDER != R_R1

	// Image processing (gamma-correct)
	u32* pPixel = (u32*)D.pBits;
	u32* pOrigin = pPixel;
	u32* pEnd = pPixel + (rtWidth * rtHeight);

	//	Kill alpha
#if	RENDER != R_R1
	if (Target->rt_Color->fmt == D3DFMT_A16B16G16R16F)
	{
		static const int iMaxPixelsInARow = 1024;
		D3DXFLOAT16* pPixelElement16 = (D3DXFLOAT16*)pPixel;

		FLOAT tmpArray[4 * iMaxPixelsInARow];
		while (pPixel != pEnd)
		{
			const int iProcessPixels = _min(iMaxPixelsInARow, (s32)(pEnd - pPixel));

			D3DXFloat16To32Array(tmpArray, pPixelElement16, iProcessPixels * 4);

			for (int i = 0; i < iProcessPixels; ++i)
			{
				*pPixel = color_argb_f(
					1.0f,
					tmpArray[i * 4],
					tmpArray[i * 4 + 1],
					tmpArray[i * 4 + 2]
				);

				++pPixel;
			}

			pPixelElement16 += iProcessPixels * 4;
		}
	}
	else
#endif	//	RENDER != R_R1
	{
		for (; pPixel != pEnd; pPixel++)
		{
			u32 p = *pPixel;
			*pPixel = color_xrgb(
				color_get_R(p),
				color_get_G(p),
				color_get_B(p)
			);
		}
	}

	{
		memory_writer.w(&rtWidth, sizeof(rtWidth));
		memory_writer.w(&rtHeight, sizeof(rtHeight));
		memory_writer.w(pOrigin, (rtWidth * rtHeight) * 4);
	}

	hr = pFB->UnlockRect();
}

#endif	//	USE_DX10

void DoAsyncScreenshot()
{
	RImplementation.Target->DoAsyncScreenshot();
}

// Antglobes: Export Screenshot Func + variable resolution & encoding
#if defined(USE_DX12)
void CRender::TakeScreenshot(LPCSTR path, Fvector2 dimensions, DxEncoding encoding)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}
	if (!Device.b_is_Ready) return;

	string_path fname;
	if (0 == strext(path)) strconcat(sizeof(fname), fname, path, ".dds");
	else xr_strcpy(fname, sizeof(fname), path);
	FS.update_path(fname, "$game_textures$", fname);

	xr_vector<u32> pixels;
	u32 actual_width = 0, actual_height = 0;
	if (!dx12_screenshot::read_back_buffer(pixels, actual_width, actual_height))
		return;

	// Clamp image dimensions
	int width = int(dimensions.x);
	int height = int(dimensions.y);
	clamp(width, 0, int(actual_width));
	clamp(height, 0, int(actual_height));
	if (!width || !height)
	{
		Msg("! TakeScreenshot: empty size %dx%d", width, height);
		return;
	}

	// As DX11: A8R8G8B8 with a full mip chain, DXT1, and DXT5 (and BC7, clamped to it) single level
	clamp(encoding, eDXE_A8R8G8B8, eDXE_DXT5);
	dx12_screenshot::dds_format format = dx12_screenshot::dds_bgra8;
	if (encoding == IRender_interface::eDXE_DXT1)
		format = dx12_screenshot::dds_bc1;
	else if (encoding == IRender_interface::eDXE_DXT5)
		format = dx12_screenshot::dds_bc3;

	xr_vector<u8> dds;
	dx12_screenshot::make_dds(pixels, actual_width, actual_height, u32(width), u32(height), format,
		encoding == eDXE_A8R8G8B8, dds);
	dx12_screenshot::write_file(nullptr, fname, dds);
}
#elif defined(USE_DX10) || defined(USE_DX11)
void CRender::TakeScreenshot(LPCSTR path, Fvector2 dimensions, DxEncoding encoding)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}
	if (!Device.b_is_Ready) return;

	string_path fname;
	if (0 == strext(path)) strconcat(sizeof(fname), fname, path, ".dds");
	else xr_strcpy(fname, sizeof(fname), path);
	FS.update_path(fname, "$game_textures$", fname);

	// Clamp image dimensions
	int actual_width = Device.dwRenderWidth;
	int actual_height = Device.dwRenderHeight;
	int width = dimensions.x;
	int height = dimensions.y;
	clamp(width, 0, actual_width);
	clamp(height, 0, actual_height);

	ID3DResource* pSrcTexture;
	HW.pBaseRT->GetResource(&pSrcTexture);

	VERIFY(pSrcTexture);


	DXGI_FORMAT dx_encoding;
	clamp(encoding, eDXE_A8R8G8B8, eDXE_DXT5);
	switch (encoding)
	{
	case IRender_interface::eDXE_A8R8G8B8: 
	{
		dx_encoding = DXGI_FORMAT_R8G8B8A8_UNORM;
	}
	break;
	case IRender_interface::eDXE_DXT1: 
	{
		dx_encoding = DXGI_FORMAT_BC1_UNORM;
	}
	break;
	case IRender_interface::eDXE_DXT5: 
	{
		dx_encoding = DXGI_FORMAT_BC5_UNORM;
	}
	break;
	case IRender_interface::eDXE_BC7:
	{
		dx_encoding = DXGI_FORMAT_BC7_UNORM;
	}
	break;
	default:
		dx_encoding = DXGI_FORMAT_R8G8B8A8_UNORM;
		break;
	}

	ID3DTexture2D* pSrcSmallTexture;

	D3D_TEXTURE2D_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.Width = u32(width);
	desc.Height = u32(height);
	desc.MipLevels = encoding == eDXE_A8R8G8B8 ? 0: 1;
	desc.ArraySize = 1;
	desc.Format = dx_encoding;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D_USAGE_DEFAULT;
	desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
	CHK_DX(HW.pDevice->CreateTexture2D(&desc, NULL, &pSrcSmallTexture));

	//	D3DX10_TEXTURE_LOAD_INFO *pLoadInfo

#ifdef USE_DX11
	CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
		NULL, pSrcSmallTexture));
#else
	CHK_DX(D3DX10LoadTextureFromTexture(pSrcTexture,
		NULL, pSrcSmallTexture));
#endif

	

	// save (logical & physical)
	ID3DBlob* saved = 0;
#ifdef USE_DX11
	HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
#else
	HRESULT hr = D3DX10SaveTextureToMemory(pSrcSmallTexture, D3DX10_IFF_DDS, &saved, 0);
	//HRESULT hr					= D3DXSaveTextureToFileInMemory (&saved,D3DXIFF_DDS,texture,0);
#endif
	if (hr == D3D_OK)
	{
		IWriter* fs = FS.w_open(fname);
		if (fs)
		{
			fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
			FS.w_close(fs);
		}
	}
	_RELEASE(saved);

	// cleanup
	_RELEASE(pSrcSmallTexture);
}
#else //DX
// Antglobes: Export Screenshot Func + variable resolution & encoding
void CRender::TakeScreenshot(LPCSTR path, Fvector2 dimensions, DxEncoding encoding)
{
	if (!Device.b_is_Ready) return;
	
	string_path fname;
	if (0 == strext(path)) strconcat(sizeof(fname), fname, path, ".dds");
	else xr_strcpy(fname, sizeof(fname), path);
	FS.update_path(fname, "$game_textures$", fname);

	// Clamp image dimensions
	int actual_width = Device.dwRenderWidth;
	int actual_height = Device.dwRenderHeight;
	int width = int(dimensions.x);
	int height = int(dimensions.y);
	clamp(width, 0, actual_width);
	clamp(height, 0, actual_height);

	// antglobes: dx encoding
	D3DFORMAT dx_encoding;
	switch (encoding)
	{
	case IRender_interface::eDXE_A8R8G8B8: {
		dx_encoding = D3DFMT_A8R8G8B8;
	}
	break;
	case IRender_interface::eDXE_DXT1: {
		dx_encoding = D3DFMT_DXT1;
	}
	break;
	case IRender_interface::eDXE_DXT5: {
		dx_encoding = D3DFMT_DXT5;
	}
	break;
	default:
		dx_encoding = D3DFMT_A8R8G8B8;
		break;
	}
	// Antglobes: DDS Image Prep from ScreenshotImpl

	// Create temp-surface
	IDirect3DSurface9* pFB;
	D3DLOCKED_RECT D;
	HRESULT hr = HW.pDevice->CreateOffscreenPlainSurface(Device.dwRenderWidth, Device.dwRenderHeight, HW.DevPP.BackBufferFormat,
		D3DPOOL_SYSTEMMEM, &pFB, nullptr);
	if (FAILED(hr)) return;

	hr = HW.pDevice->GetRenderTargetData(HW.pBaseRT, pFB);
	if (FAILED(hr)) goto _end_;

	hr = pFB->LockRect(&D, 0, D3DLOCK_NOSYSLOCK);
	if (FAILED(hr)) goto _end_;
	// Image processing (gamma-correct)
	u32* pPixel = (u32*)D.pBits;
	u32* pEnd = pPixel + (Device.dwRenderWidth * Device.dwRenderHeight);
	//	Kill alpha
	for (; pPixel != pEnd; pPixel++)
	{
		u32 p = *pPixel;
		*pPixel = color_xrgb(color_get_R(p), color_get_G(p), color_get_B(p));
	}

	hr = pFB->UnlockRect();
	if (hr != D3D_OK) goto _end_;
	// texture width/height = resolution
	ID3DTexture2D* texture = NULL;
	hr = D3DXCreateTexture(HW.pDevice, u32(width), u32(height), 1, 0, dx_encoding, D3DPOOL_SCRATCH,
		&texture);
	if (hr != D3D_OK) goto _end_;
	if (NULL == texture) goto _end_;
	// resize&convert to surface
	IDirect3DSurface9* surface = 0;
	hr = texture->GetSurfaceLevel(0, &surface);
	if (hr != D3D_OK) goto _end_;
	VERIFY(surface);

	hr = D3DXLoadSurfaceFromSurface(surface, 0, 0, pFB, 0, 0, D3DX_DEFAULT, 0);
	_RELEASE(surface);
	if (hr != D3D_OK) goto _end_;
	// save (logical & physical)
	ID3DBlob* saved = 0;
	hr = D3DXSaveTextureToFileInMemory(&saved, D3DXIFF_DDS, texture, 0);
	if (hr != D3D_OK) goto _end_;

	IWriter* fs = FS.w_open(fname);
	if (fs)
	{
		fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
		FS.w_close(fs);
	}
	_RELEASE(saved);

	// cleanup
	_RELEASE(texture);

_end_:
	_RELEASE(pFB);
}
#endif