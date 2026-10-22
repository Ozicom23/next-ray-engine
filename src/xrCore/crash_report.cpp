#include "stdafx.h"
#include "crash_report.h"
#include "mod_origin.h"
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "../3rd party/stackwalker/include/StackWalker.h"

namespace crash_report
{
void (*game_context)(char* out, u32 size) = nullptr;

namespace
{
// The exe's PE header: its link time and image size name the build a .sym file was written for
bool image_stamp(HMODULE module, u32& time, u32& size)
{
	const auto* dos = (const IMAGE_DOS_HEADER*)module;
	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const auto* nt = (const IMAGE_NT_HEADERS*)((const u8*)module + dos->e_lfanew);
	time = nt->FileHeader.TimeDateStamp;
	size = nt->OptionalHeader.SizeOfImage;
	return true;
}

// <exe folder>\<exe name>.sym
void sym_path(char* out, u32 size)
{
	GetModuleFileNameA(nullptr, out, size);
	if (char* dot = strrchr(out, '.'))
		xr_strcpy(dot, size - u32(dot - out), ".sym");
}

//------------------------------------------------------------------------------------------------------------------
// Function names from the .sym file: "<rva hex> <size hex> <name>" lines sorted by address
struct symbols
{
	std::vector<char> text;
	struct entry
	{
		u32 rva, size;
		const char* name;
	};
	std::vector<entry> entries;
	bool loaded = false;

	void load()
	{
		if (loaded)
			return;
		loaded = true;
		char path[MAX_PATH];
		sym_path(path, sizeof(path));
		FILE* f = fopen(path, "rb");
		if (!f)
			return;
		fseek(f, 0, SEEK_END);
		const long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		text.resize(size_t(n) + 1);
		const size_t got = fread(text.data(), 1, size_t(n), f);
		fclose(f);
		text[got] = 0;
		// "nextray-sym 1 <time> <image size>": for this build only
		u32 time = 0, size = 0, want_time = 0, want_size = 0;
		if (sscanf(text.data(), "nextray-sym 1 %x %x", &want_time, &want_size) != 2 ||
			!image_stamp(GetModuleHandle(nullptr), time, size) || time != want_time || size != want_size)
		{
			text.clear();
			return;
		}
		char* p = strchr(text.data(), '\n');
		while (p && *++p)
		{
			char* end = strchr(p, '\n');
			if (end)
				*end = 0;
			entry e;
			int name_at = 0;
			if (sscanf(p, "%x %x %n", &e.rva, &e.size, &name_at) == 2 && name_at)
			{
				e.name = p + name_at;
				entries.push_back(e);
			}
			p = end;
		}
	}

	const entry* find(u32 rva)
	{
		load();
		auto it = std::upper_bound(entries.begin(), entries.end(), rva, [](u32 v, const entry& e) { return v < e.rva; });
		if (it == entries.begin())
			return nullptr;
		--it;
		return rva - it->rva < _max(it->size, 1u) ? &*it : nullptr;
	}
};
symbols s_symbols;

//------------------------------------------------------------------------------------------------------------------
// The engine's stack: one line per frame, named from the PDB if there is one, else from the .sym file
class frame_walker : public StackWalker
{
public:
	std::vector<std::string> frames;
	std::string first_engine; // the innermost function outside the crash handling

	frame_walker() :
		StackWalker(RetrieveSymbol | RetrieveLine | RetrieveModuleInfo | SymBuildPath) {}

protected:
	void OnCallstackEntry(CallstackEntryType type, CallstackEntry& e) override
	{
		if (type == lastEntry || !e.offset || frames.size() >= 64)
			return;
		char line[1024];
		const char* name = e.undFullName[0] ? e.undFullName : (e.undName[0] ? e.undName : e.name);
		std::string func;
		if (name[0])
			func = name;
		else if (e.baseOfImage == DWORD64(GetModuleHandle(nullptr)))
		{
			const u32 rva = u32(e.offset - e.baseOfImage);
			if (const symbols::entry* s = s_symbols.find(rva))
			{
				xr_sprintf(line, "%s+0x%x", s->name, rva - s->rva);
				func = line;
			}
		}
		const char* module = e.moduleName[0] ? e.moduleName : "?";
		if (func.empty())
			xr_sprintf(line, "%s+0x%llx", module, (unsigned long long)(e.offset - e.baseOfImage));
		else if (e.lineFileName[0])
		{
			const char* file = strrchr(e.lineFileName, '\\');
			xr_sprintf(line, "%s (%s:%u)", func.c_str(), file ? file + 1 : e.lineFileName, e.lineNumber);
		}
		else
			xr_sprintf(line, "%s [%s]", func.c_str(), module);
		// The crash handling's own frames first: left out
		static const char* const skip[] = { "StackWalker", "crash_report::", "xrDebug::", "LogStackTrace", "UnhandledFilter",
			"KiUserExceptionDispatcher", "RtlDispatchException", "RtlRaiseException", "RaiseException", "UnhandledExceptionFilter" };
		if (frames.empty())
			for (const char* s : skip)
				if (strstr(line, s))
					return;
		frames.push_back(line);
		// A crash inside a fatal error's reporting: the cause is below xrDebug's frames
		if (strstr(line, "xrDebug::"))
		{
			first_engine.clear();
			return;
		}
		if (first_engine.empty() && !func.empty())
			first_engine = line;
	}
	void OnOutput(LPCSTR) override {}
	void OnSymInit(LPCSTR, DWORD, LPCSTR) override {}
	void OnLoadModule(LPCSTR, LPCSTR, DWORD64, DWORD, DWORD, LPCSTR, LPCSTR, ULONGLONG) override {}
	void OnDbgHelpErr(LPCSTR, DWORD, DWORD64) override {}
};

//------------------------------------------------------------------------------------------------------------------
// "...\gamedata\scripts\foo.script(12) : bar" or "...scripts\foo.script:12: attempt..." -> foo, 12
bool script_of(const std::string& line, std::string& script, int& at)
{
	const size_t ext = line.find(".script");
	if (ext == std::string::npos)
		return false;
	size_t start = line.find_last_of("\\/ ", ext);
	start = start == std::string::npos ? 0 : start + 1;
	script = line.substr(start, ext - start);
	at = 0;
	const size_t after = ext + 7;
	if (after < line.size() && (line[after] == '(' || line[after] == ':'))
		at = atoi(line.c_str() + after + 1);
	return !script.empty();
}

std::string with_mod(const std::string& script, int at)
{
	char buf[512];
	LPCSTR mod = mod_origin::of_script(script.c_str());
	if (at)
		xr_sprintf(buf, "%s.script line %d", script.c_str(), at);
	else
		xr_sprintf(buf, "%s.script", script.c_str());
	std::string s = buf;
	s += mod ? std::string(" (mod: ") + mod + ")" : std::string(" (the game's own gamedata, no MO2 mod has it)");
	return s;
}

// The log's last lines (the error, the stacks and what led to it)
std::vector<std::string> log_tail(u32 bytes)
{
	std::vector<std::string> lines;
	FILE* f = fopen(xrLogger::GetLogPath(), "rb");
	if (!f)
		return lines;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	const long from = n > long(bytes) ? n - long(bytes) : 0;
	fseek(f, from, SEEK_SET);
	std::string text(size_t(n - from), '\0');
	text.resize(fread(&text[0], 1, text.size(), f));
	fclose(f);
	size_t p = from ? text.find('\n') + 1 : 0; // a whole first line
	while (p < text.size())
	{
		size_t end = text.find('\n', p);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(p, end - p);
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();
		lines.push_back(line);
		p = end + 1;
	}
	return lines;
}

// The game's lines, guarded: the crash may have left its objects half destroyed (a level being unloaded)
void safe_game_context(char* out, u32 size)
{
	__try
	{
		game_context(out, size);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		xr_strcpy(out, size, "Where: (the game's state couldn't be read)");
	}
}

// Wine (CrossOver, Proton): its version, "" on Windows
std::string wine_version()
{
	typedef const char*(CDECL * get_version_fn)();
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	auto fn = ntdll ? (get_version_fn)GetProcAddress(ntdll, "wine_get_version") : nullptr;
	return fn ? std::string(" under Wine ") + fn() : std::string();
}
} // namespace

static std::atomic<bool> s_started{ false };
static std::atomic<u32> s_beats{ 0 };

bool started() { return s_started.load(); }
void beat() { s_beats.fetch_add(1, std::memory_order_relaxed); }
u32 beats() { return s_beats.load(std::memory_order_relaxed); }

LPCSTR write(LPCSTR what, const void* exception_context, void* thread)
{
	static string_path s_path = "";
	if (s_started.exchange(true)) // a crash inside the report's own writing
		return s_path;

	// The engine's stack
	frame_walker walker;
	if (exception_context)
	{
		CONTEXT copy = *(const CONTEXT*)exception_context;
#ifdef _M_AMD64
		// A call through a null pointer: the walk starts at its caller (the return address on the stack)
		if (!copy.Rip && copy.Rsp && !IsBadReadPtr((const void*)copy.Rsp, 8))
		{
			copy.Rip = *(const DWORD64*)copy.Rsp;
			copy.Rsp += 8;
		}
#endif
		walker.ShowCallstack(thread ? (HANDLE)thread : GetCurrentThread(), &copy);
	}
	else
		walker.ShowCallstack();

	// The log: Lua stacks and script errors
	xrLogger::FlushLog();
	const std::vector<std::string> lines = log_tail(256 * 1024);
	// The crash's own lines: from the last Lua stack or error marker before the end
	size_t crash_start = lines.size() > 150 ? lines.size() - 150 : 0;
	for (size_t i = lines.size(); i-- > 0 && lines.size() - i < 400;)
		if (lines[i].find("FATAL ERROR") != std::string::npos || lines[i].find("LUA error") != std::string::npos ||
			lines[i].find("SCRIPT RUNTIME ERROR") != std::string::npos)
		{
			crash_start = i > 60 ? i - 60 : 0;
			break;
		}
	std::vector<std::string> lua;
	std::string lua_cause, lua_any;
	for (size_t i = crash_start; i < lines.size(); ++i)
	{
		const std::string& l = lines[i];
		if (l.find("[Lua]") == std::string::npos && l.find("[LUA]") == std::string::npos)
			continue;
		std::string script;
		int at = 0;
		if (!script_of(l, script, at))
			continue;
		std::string entry = with_mod(script, at);
		const size_t fn = l.find(") : ");
		if (fn != std::string::npos)
			entry += ", in " + l.substr(fn + 4);
		if (std::find(lua.begin(), lua.end(), entry) == lua.end())
			lua.push_back(entry);
		if (lua_any.empty())
			lua_any = entry;
		if (lua_cause.empty() && mod_origin::of_script(script.c_str()))
			lua_cause = entry;
	}
	std::vector<std::string> script_errors;
	u32 script_error_count = 0;
	for (const std::string& l : lines)
		if (l.find("SCRIPT ERROR") != std::string::npos || l.find("SCRIPT RUNTIME ERROR") != std::string::npos ||
			(l.find(".script:") != std::string::npos && l.find("attempt to") != std::string::npos))
		{
			std::string script;
			int at = 0;
			if (!script_of(l, script, at))
				continue; // the header line; its script's line follows
			++script_error_count;
			std::string entry = with_mod(script, at) + ": " + l.substr(l.find(".script") + 7);
			if (std::find(script_errors.begin(), script_errors.end(), entry) == script_errors.end())
				script_errors.push_back(entry);
		}
	// The error's own text may name the script (a Lua error's first line)
	std::string what_script;
	{
		std::string script;
		int at = 0;
		if (what && script_of(what, script, at))
			what_script = with_mod(script, at);
	}

	// appdata\logs\crash_<date>.txt
	SYSTEMTIME t;
	GetLocalTime(&t);
	string_path folder;
	xr_strcpy(folder, xrLogger::GetLogPath());
	if (char* slash = strrchr(folder, '\\'))
		slash[1] = 0;
	else
		folder[0] = 0;
	xr_sprintf(s_path, "%scrash_%04d-%02d-%02d_%02d-%02d-%02d.txt", folder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
		t.wSecond);
	FILE* f = fopen(s_path, "wb");
	if (!f)
	{
		s_path[0] = 0;
		return s_path;
	}
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, sizeof(exe));
	const char* exe_name = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
	u32 link_time = 0, image_size = 0;
	image_stamp(GetModuleHandle(nullptr), link_time, image_size);
	char built[64] = "?";
	{
		const time_t tt = time_t(link_time);
		tm tmv;
		if (!gmtime_s(&tmv, &tt))
			strftime(built, sizeof(built), "%Y-%m-%d %H:%M UTC", &tmv);
	}
	fprintf(f, "neXt-Ray crash report, %04d-%02d-%02d %02d:%02d:%02d\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
		t.wSecond);
	fprintf(f, "%s (built %s)%s, MO2 profile: %s\r\n", exe_name, built, wine_version().c_str(),
		mod_origin::profile()[0] ? mod_origin::profile() : "-");
	char context[1024] = "";
	if (game_context)
	{
		safe_game_context(context, sizeof(context));
		if (context[0])
			fprintf(f, "%s\r\n", context);
	}

	fprintf(f, "\r\nPROBABLE CAUSE\r\n");
	// A hang (another thread's stack: the hang watchdog): where the scripts are stuck; the log's earlier script errors
	// are listed below but are not its cause
	if (thread)
	{
		const char* lua_at = strstr(context, "Lua: ");
		if (lua_at)
		{
			std::string l(lua_at + 5);
			l = l.substr(0, l.find_first_of("\r\n"));
			fprintf(f, "  Stuck in: %s\r\n", l.c_str());
		}
	}
	if (thread)
		; // the log's Lua stacks are from before the hang
	else if (!lua_cause.empty() || !what_script.empty())
		fprintf(f, "  Script: %s\r\n", (!what_script.empty() ? what_script : lua_cause).c_str());
	else if (!lua_any.empty())
		fprintf(f, "  Script: %s\r\n", lua_any.c_str());
	if (!walker.first_engine.empty())
		fprintf(f, "  Engine: %s\r\n", walker.first_engine.c_str());
	if (!script_errors.empty() && !thread)
		fprintf(f, "  Earlier in the log: %u script error(s), the last: %s\r\n", script_error_count, script_errors.back().c_str());

	fprintf(f, "\r\nERROR\r\n");
	for (const char* p = what ? what : ""; *p;)
	{
		const char* end = strchr(p, '\n');
		std::string l = end ? std::string(p, end) : std::string(p);
		while (!l.empty() && l.back() == '\r')
			l.pop_back();
		if (!l.empty())
			fprintf(f, "  %s\r\n", l.c_str());
		p = end ? end + 1 : p + strlen(p);
	}

	fprintf(f, "\r\nLUA STACK (innermost first)\r\n");
	if (lua.empty())
		fprintf(f, "  (none: not in a script)\r\n");
	for (const std::string& l : lua)
		fprintf(f, "  %s\r\n", l.c_str());

	fprintf(f, "\r\nENGINE STACK (innermost first)\r\n");
	s_symbols.load();
	if (s_symbols.entries.empty())
	{
		char sym[MAX_PATH];
		sym_path(sym, sizeof(sym));
		const char* name = strrchr(sym, '\\') ? strrchr(sym, '\\') + 1 : sym;
		fprintf(f, "  (no %s for this build next to the exe: functions shown as offsets)\r\n", name);
	}
	for (const std::string& l : walker.frames)
		fprintf(f, "  %s\r\n", l.c_str());

	if (!script_errors.empty())
	{
		fprintf(f, "\r\nSCRIPT ERRORS IN THE LOG (%u, different ones, the last 10)\r\n", script_error_count);
		for (size_t i = script_errors.size() > 10 ? script_errors.size() - 10 : 0; i < script_errors.size(); ++i)
			fprintf(f, "  %s\r\n", script_errors[i].c_str());
	}

	fprintf(f, "\r\nTHE LOG'S LAST LINES\r\n");
	for (size_t i = lines.size() > 250 ? lines.size() - 250 : 0; i < lines.size(); ++i)
		fprintf(f, "%s\r\n", lines[i].c_str());
	fclose(f);
	Msg("* Crash report: %s", s_path);
	xrLogger::FlushLog();
	return s_path;
}

//------------------------------------------------------------------------------------------------------------------
bool dump_symbols(LPCSTR out)
{
	HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
	if (!dbghelp)
		return false;
	auto set_options = (decltype(&SymSetOptions))GetProcAddress(dbghelp, "SymSetOptions");
	auto initialize = (decltype(&SymInitialize))GetProcAddress(dbghelp, "SymInitialize");
	auto load_module = (decltype(&SymLoadModuleEx))GetProcAddress(dbghelp, "SymLoadModuleEx");
	auto enum_symbols = (decltype(&SymEnumSymbols))GetProcAddress(dbghelp, "SymEnumSymbols");
	auto cleanup = (decltype(&SymCleanup))GetProcAddress(dbghelp, "SymCleanup");
	if (!set_options || !initialize || !load_module || !enum_symbols || !cleanup)
		return false;

	char exe[MAX_PATH], folder[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, sizeof(exe));
	xr_strcpy(folder, exe);
	if (char* slash = strrchr(folder, '\\'))
		*slash = 0;
	HMODULE module = GetModuleHandle(nullptr);
	u32 link_time = 0, image_size = 0;
	image_stamp(module, link_time, image_size);

	HANDLE process = GetCurrentProcess();
	set_options(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
	if (!initialize(process, folder, FALSE))
		return false;
	const DWORD64 base = load_module(process, nullptr, exe, nullptr, DWORD64(module), image_size, nullptr, 0);
	struct sym
	{
		u32 rva, size;
		bool function;
		std::string name;
	};
	struct collect
	{
		std::vector<sym> list;
		DWORD64 base;
	} c = { {}, base };
	if (base)
		enum_symbols(process, base, "*",
			[](PSYMBOL_INFO info, ULONG, PVOID user) -> BOOL
			{
				auto& c = *(collect*)user;
				// Functions with their size from the private symbols, publics (code without them) sized to the next
				if (info->Tag == 5 /* SymTagFunction */ || info->Tag == 10 /* SymTagPublicSymbol */)
					c.list.push_back({ u32(info->Address - c.base), info->Size, info->Tag == 5,
						std::string(info->Name, info->NameLen) });
				return TRUE;
			},
			&c);
	cleanup(process);
	if (c.list.size() < 1000)
	{
		fprintf(stderr, "dump_symbols: %u symbols (no PDB?)\n", u32(c.list.size()));
		return false;
	}
	std::sort(c.list.begin(), c.list.end(), [](const sym& a, const sym& b)
	{
		return a.rva != b.rva ? a.rva < b.rva : a.function > b.function;
	});
	FILE* f = fopen(out, "wb");
	if (!f)
		return false;
	fprintf(f, "nextray-sym 1 %x %x\n", link_time, image_size);
	u32 written = 0;
	for (size_t i = 0; i < c.list.size(); ++i)
	{
		const sym& s = c.list[i];
		if (i && c.list[i - 1].rva == s.rva)
			continue; // the function's, not its public name
		u32 size = s.size;
		if (!size || !s.function)
		{
			// Up to the next symbol, at most 64 KB
			size_t n = i + 1;
			while (n < c.list.size() && c.list[n].rva == s.rva)
				++n;
			size = n < c.list.size() ? _min(c.list[n].rva - s.rva, 0x10000u) : 0x1000;
		}
		fprintf(f, "%x %x %s\n", s.rva, size, s.name.c_str());
		++written;
	}
	fclose(f);
	printf("dump_symbols: %u functions to %s\n", written, out);
	return true;
}
}
