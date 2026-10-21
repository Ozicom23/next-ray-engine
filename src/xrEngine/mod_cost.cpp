#include "stdafx.h"
#include "mod_cost.h"
#include "../xrCore/mod_origin.h"

namespace mod_cost
{
int enabled = 0;

namespace
{
xr_vector<row> s_frame;
float s_frame_total = 0.f;
xr_map<xr_string, float> s_load; // by mod
float s_load_total = 0.f;
}

void set_frame_rows(LPCSTR text, float total_ms, u32 frames)
{
	s_frame.clear();
	s_frame_total = total_ms;
	(void)frames;
	for (LPCSTR p = text; p && *p;)
	{
		LPCSTR end = strchr(p, '\n');
		xr_string line = end ? xr_string(p, size_t(end - p)) : xr_string(p);
		p = end ? end + 1 : nullptr;
		const size_t tab = line.find('\t');
		if (tab == xr_string::npos)
			continue;
		row r;
		r.name = line.substr(0, tab);
		r.avg_ms = r.peak_ms = r.calls = 0.f;
		sscanf(line.c_str() + tab + 1, "%f\t%f\t%f", &r.avg_ms, &r.peak_ms, &r.calls);
		s_frame.push_back(r);
	}
}

const xr_vector<row>& frame_rows() { return s_frame; }
float frame_total_ms() { return s_frame_total; }

void load_reset()
{
	s_load.clear();
	s_load_total = 0.f;
}

void load_add(LPCSTR script, float ms)
{
	LPCSTR mod = mod_origin::of_script(script);
	string256 name;
	if (mod)
		xr_strcpy(name, mod);
	else
		xr_sprintf(name, "%s.script", script);
	s_load[name] += ms;
	s_load_total += ms;
}

void load_rows(xr_vector<row>& rows, float& total_ms)
{
	rows.clear();
	for (const auto& it : s_load)
		rows.push_back({it.first, it.second, it.second, 0.f});
	std::sort(rows.begin(), rows.end(), [](const row& a, const row& b) { return a.avg_ms > b.avg_ms; });
	total_ms = s_load_total;
}
}
