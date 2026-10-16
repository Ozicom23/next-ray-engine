#pragma once

// GPU memory accounting for the DX11 renderer (console: stat_gpu_memory, also part of stat_memory and the
// performance overlay). Every counted D3D resource carries a small private-data object that D3D releases
// together with the resource, so the totals stay right no matter which code frees it.
namespace gpu_memory
{
	enum category : u32
	{
		tex_bc,         // block-compressed textures with mips
		tex_bc_nomips,  // block-compressed textures without mips (texture_lod can't reduce them)
		tex_raw,        // uncompressed textures with mips
		tex_raw_nomips, // uncompressed textures without mips
		tex_staging,    // CPU-side texture copies (r__no_ram_textures off), not in GPU memory
		render_target,  // render targets, depth buffers and UAV textures
		buf_level,      // level geometry (level.geom, level.geomx and the level's visuals)
		buf_model,      // models loaded through the model pool
		buf_grass,      // detail objects (grass)
		buf_dynamic,    // dynamic vertex and index streams
		buf_other,
		count,
		automatic = count, // textures by their description, buffers by the current scope
	};

	// Starts counting a resource; resources counted already are skipped. The name may be null
	void track(ID3DResource* res, const char* name = nullptr, category cat = automatic);

	// Buffers created on this thread while the scope lives are counted under its category and name
	class scope
	{
	public:
		scope(category cat, const char* name);
		~scope();

	private:
		category m_prev_cat;
		const char* m_prev_name;
	};

	struct totals
	{
		u64 bytes[count];
		u32 resources[count];

		u64 textures() const { return bytes[tex_bc] + bytes[tex_bc_nomips] + bytes[tex_raw] + bytes[tex_raw_nomips]; }
		u64 geometry() const { return bytes[buf_level] + bytes[buf_model] + bytes[buf_grass]; }
		u64 gpu() const; // everything except tex_staging
	};
	void get_totals(totals& out);

	// Summary to the log; with top_n > 0 also the largest textures, models and render targets
	void dump(u32 top_n);
}
