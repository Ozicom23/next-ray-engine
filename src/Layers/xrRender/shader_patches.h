#pragma once

// neXt-Ray's patches of GAMMA's shaders, in the engine's gamedata under shaders\nextray\ (nextray\r3\ssfx_ao.ps patches
// r3\ssfx_ao.ps). A patched file's first line names the file it was made from, "CRC32 <8 hex digits>" (zlib's CRC-32
// of the file as the mods give it), and optionally "group <name>": it is read in place of the mods' file only while
// that one is exactly the known version, and a group's files only when all of them are (they depend on each other).
// Otherwise the mods' files as they are, with a log line. Decided once, at the first shader read. The changes inside
// stay picked by r4_shader_patches (NEXTRAY_PATCHES). The "neXt-Ray performance patches" MO2 mod of earlier builds
// held these files: its copies (they define NEXTRAY_PATCHES) are taken for the known version
namespace shader_patches
{
	inline u32 crc32_zlib(const void* data, u32 size)
	{
		static u32 table[256] = {};
		if (!table[1])
			for (u32 i = 0; i < 256; ++i)
			{
				u32 c = i;
				for (int k = 0; k < 8; ++k)
					c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				table[i] = c;
			}
		u32 crc = 0xFFFFFFFFu;
		const u8* p = static_cast<const u8*>(data);
		for (u32 i = 0; i < size; ++i)
			crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
		return crc ^ 0xFFFFFFFFu;
	}

	// Lower case, '\' separators: the key of a shader path
	inline xr_string key_of(LPCSTR path)
	{
		xr_string k = path;
		for (char& c : k)
			c = c == '/' ? '\\' : char(tolower(u8(c)));
		return k;
	}

	// The patched files in use, by the path they replace
	inline const xr_map<xr_string, xr_string>& in_use()
	{
		static xr_map<xr_string, xr_string> use; // replaced path -> patched path
		static bool decided = false;
		static xrCriticalSection lock;
		xrCriticalSectionGuard guard(&lock);
		if (decided)
			return use;
		decided = true;

		struct patch
		{
			xr_string replaced, patched, group;
			bool known;
		};
		xr_vector<patch> patches;
		xr_vector<char*>* files = FS.file_list_open("$game_shaders$", "nextray\\", FS_ListFiles);
		if (files)
		{
			for (char* name : *files)
			{
				patch p;
				p.replaced = key_of(name);
				p.patched = "nextray\\" + p.replaced;
				IReader* patched = FS.r_open("$game_shaders$", p.patched.c_str());
				if (!patched)
					continue;
				// The first line: "CRC32 xxxxxxxx", "group name"
				xr_string first((LPCSTR)patched->pointer(), _min(u32(patched->length()), 512u));
				FS.r_close(patched);
				first = first.substr(0, first.find('\n'));
				u32 want = 0;
				const size_t at_crc = first.find("CRC32 ");
				if (at_crc == xr_string::npos || sscanf(first.c_str() + at_crc + 6, "%x", &want) != 1)
				{
					Msg("! Shader patches: %s names no CRC32, left out", p.patched.c_str());
					continue;
				}
				const size_t at_group = first.find("group ");
				if (at_group != xr_string::npos)
				{
					p.group = first.substr(at_group + 6);
					p.group = p.group.substr(0, p.group.find_first_of(" ,.;)\r"));
				}
				IReader* original = FS.r_open("$game_shaders$", p.replaced.c_str());
				if (!original)
				{
					Msg("* Shader patches: %s not in the game (its mod not installed), patch left out", p.replaced.c_str());
					continue;
				}
				const u32 got = crc32_zlib(original->pointer(), original->length());
				const xr_string text((LPCSTR)original->pointer(), original->length());
				FS.r_close(original);
				const bool old_mod = text.find("NEXTRAY_PATCHES") != xr_string::npos;
				p.known = got == want || old_mod;
				if (!p.known)
					Msg("* Shader patches: %s is not the known version (CRC32 %08x, the patch is for %08x): kept as it is",
					    p.replaced.c_str(), got, want);
				else if (old_mod)
					Msg("* Shader patches: %s comes from the neXt-Ray performance patches MO2 mod: no longer needed, it can"
					    " be removed", p.replaced.c_str());
				patches.push_back(p);
			}
			FS.file_list_close(files);
		}

		u32 used = 0;
		for (const patch& p : patches)
		{
			bool ok = p.known;
			for (const patch& o : patches)
				if (!p.group.empty() && o.group == p.group && !o.known)
				{
					if (ok)
						Msg("* Shader patches: %s left out with %s (group %s)", p.replaced.c_str(), o.replaced.c_str(),
						    p.group.c_str());
					ok = false;
				}
			if (ok)
			{
				use[p.replaced] = p.patched;
				++used;
			}
		}
		Msg("* Shader patches: %u of %u in use", used, u32(patches.size()));
		return use;
	}

	// A shader source by its path under $game_shaders$ ("r3\\ssfx_ao.ps"): the patched file in use for it, else the
	// mods' one; nullptr when there is none. FS.r_close it
	inline IReader* open(LPCSTR path)
	{
		const xr_map<xr_string, xr_string>& use = in_use();
		if (!use.empty())
		{
			auto it = use.find(key_of(path));
			if (it != use.end())
				if (IReader* patched = FS.r_open("$game_shaders$", it->second.c_str()))
					return patched;
		}
		return FS.r_open("$game_shaders$", path);
	}
}
