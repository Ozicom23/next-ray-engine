#include "stdafx.h"
#include "mod_origin.h"
#include <tlhelp32.h>
#include <mutex>
#include <unordered_map>

namespace mod_origin
{
namespace
{
std::mutex s_lock;
volatile bool s_ready = false;
std::unordered_map<std::string, std::string> s_scripts; // lower case name without .script -> mod
string256 s_profile = "";

std::string key_of(LPCSTR script)
{
	std::string k = script;
	const size_t slash = k.find_last_of("\\/");
	if (slash != std::string::npos)
		k.erase(0, slash + 1);
	for (char& c : k)
		c = char(tolower(u8(c)));
	if (k.size() > 7 && k.compare(k.size() - 7, 7, ".script") == 0)
		k.resize(k.size() - 7);
	return k;
}

bool file_exists(LPCSTR path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// The folder of the game's parent process when that is MO2 (the folder has ModOrganizer.ini)
bool mo2_from_parent(string_path& dir)
{
	const DWORD self = GetCurrentProcessId();
	DWORD parent = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return false;
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(pe);
	for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
		if (pe.th32ProcessID == self)
		{
			parent = pe.th32ParentProcessID;
			break;
		}
	CloseHandle(snap);
	if (!parent)
		return false;
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
	if (!process)
		return false;
	DWORD size = sizeof(dir);
	const BOOL ok = QueryFullProcessImageNameA(process, 0, dir, &size);
	CloseHandle(process);
	if (!ok)
		return false;
	if (char* slash = strrchr(dir, '\\'))
		*slash = 0;
	string_path ini;
	xr_sprintf(ini, "%s\\ModOrganizer.ini", dir);
	return file_exists(ini);
}

// MO2's folder: the parent process's, else ..\gamma or .. from the game's folder (the one with gamedata)
bool find_mo2(string_path& dir)
{
	if (mo2_from_parent(dir))
		return true;
	string_path root, ini;
	GetCurrentDirectoryA(sizeof(root), root);
	// The current folder is the game's bin: up from it
	for (LPCSTR rel : {"..\\..\\gamma", "..\\gamma", "..\\.."})
	{
		xr_sprintf(ini, "%s\\%s\\ModOrganizer.ini", root, rel);
		if (file_exists(ini))
		{
			xr_sprintf(dir, "%s\\%s", root, rel);
			return true;
		}
	}
	return false;
}

// A path setting of ModOrganizer.ini ("%BASE_DIR%/mods" style), or the default under the base folder
void ini_path(LPCSTR ini, LPCSTR key, LPCSTR base, LPCSTR fallback, string_path& out)
{
	string_path value = "";
	GetPrivateProfileStringA("Settings", key, "", value, sizeof(value), ini);
	if (!value[0])
	{
		xr_sprintf(out, "%s\\%s", base, fallback);
		return;
	}
	std::string v = value;
	const size_t at = v.find("%BASE_DIR%");
	if (at != std::string::npos)
		v.replace(at, 10, base);
	for (char& c : v)
		if (c == '/')
			c = '\\';
	xr_strcpy(out, v.c_str());
}

void read()
{
	string_path mo2;
	if (!find_mo2(mo2))
	{
		Msg("* Mod origins: Mod Organizer 2 not found");
		return;
	}
	string_path ini;
	xr_sprintf(ini, "%s\\ModOrganizer.ini", mo2);
	string_path base = "";
	GetPrivateProfileStringA("Settings", "base_directory", "", base, sizeof(base), ini);
	if (!base[0])
		xr_strcpy(base, mo2);
	// selected_profile=@ByteArray(G.A.M.M.A)
	string256 profile = "";
	GetPrivateProfileStringA("General", "selected_profile", "", profile, sizeof(profile), ini);
	LPCSTR name = profile;
	if (strstr(profile, "@ByteArray(") == profile)
	{
		name = profile + 11;
		if (char* end = strrchr(profile, ')'))
			*end = 0;
	}
	xr_strcpy(s_profile, name);
	string_path mods, profiles, overwrite, list;
	ini_path(ini, "mod_directory", base, "mods", mods);
	ini_path(ini, "profiles_directory", base, "profiles", profiles);
	ini_path(ini, "overwrite_directory", base, "overwrite", overwrite);
	xr_sprintf(list, "%s\\%s\\modlist.txt", profiles, s_profile);
	FILE* f = fopen(list, "rb");
	if (!f)
	{
		Msg("* Mod origins: %s not read", list);
		return;
	}
	// Highest priority first: "+name" enabled, "-name" disabled, "*name" not MO2's
	xr_vector<std::string> enabled;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\r' || line[n - 1] == '\n'))
			line[--n] = 0;
		if (line[0] == '+')
			enabled.push_back(line + 1);
	}
	fclose(f);

	auto scan = [](LPCSTR folder, const std::string& mod)
	{
		string_path mask;
		xr_sprintf(mask, "%s\\gamedata\\scripts\\*.script", folder);
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(mask, &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;
		do
			s_scripts[key_of(fd.cFileName)] = mod;
		while (FindNextFileA(h, &fd));
		FindClose(h);
	};
	// Lowest priority first: a mod higher in the list replaces the files of those below it
	for (auto it = enabled.rbegin(); it != enabled.rend(); ++it)
	{
		string_path folder;
		xr_sprintf(folder, "%s\\%s", mods, it->c_str());
		scan(folder, *it);
	}
	scan(overwrite, "Overwrite");
	Msg("* Mod origins: MO2 profile %s, %u enabled mods, %u of their scripts", s_profile, u32(enabled.size()),
	    u32(s_scripts.size()));
}
}

LPCSTR of_script(LPCSTR script)
{
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (!s_ready)
		{
			read();
			s_ready = true;
		}
	}
	return of_script_if_known(script);
}

LPCSTR of_script_if_known(LPCSTR script)
{
	if (!s_ready || !script || !script[0])
		return nullptr;
	auto it = s_scripts.find(key_of(script));
	return it == s_scripts.end() ? nullptr : it->second.c_str();
}

LPCSTR profile()
{
	return s_profile;
}
}
