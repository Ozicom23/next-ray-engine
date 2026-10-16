#include "stdafx.h"
#include "r4_gpu_profiler.h"

#include <algorithm>

namespace gpu_profiler
{
namespace
{
	const u32 kSlots = 4;       // frames in flight before results are read back
	const u32 kMaxEvents = 128; // per frame, later scopes are not timed
	const u32 kMaxDepth = 32;
	const u32 kNotTimed = u32(-1);

	struct Event
	{
		LPCWSTR name;
		u32 depth;
	};

	struct Slot
	{
		ID3D11Query* disjoint;
		ID3D11Query* stamps[kMaxEvents * 2]; // begin, end per event
		Event events[kMaxEvents];
		u32 count;
		bool pending;
	};

	Slot s_slots[kSlots] = {};
	u32 s_write = 0;
	bool s_failed = false;

	bool s_recording = false;
	DWORD s_thread = 0;
	u32 s_stack[kMaxDepth];
	u32 s_depth = 0;

	struct Sum
	{
		string64 name;
		u32 depth;
		u32 order; // position in the frame where it last ran
		double ms;
	};
	xr_vector<Sum> s_sums;
	u32 s_frames = 0;

	// Long capture (stat_gpu_passes <seconds>), summed next to the overlay's short interval
	bool s_capture = false;
	u32 s_capture_end = 0; // GetTickCount: real time, also under dbg_freeze (game time stands still)
	float s_capture_seconds = 0.f;
	xr_vector<Sum> s_capture_sums;
	u32 s_capture_frames = 0;

	bool create()
	{
		if (s_slots[0].disjoint)
			return true;
		if (s_failed)
			return false;

		D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		D3D11_QUERY_DESC stamp = { D3D11_QUERY_TIMESTAMP, 0 };
		for (Slot& s : s_slots)
		{
			if (FAILED(HW.pDevice->CreateQuery(&disjoint, &s.disjoint)))
			{
				destroy();
				s_failed = true;
				return false;
			}
			for (ID3D11Query*& q : s.stamps)
			{
				if (FAILED(HW.pDevice->CreateQuery(&stamp, &q)))
				{
					destroy();
					s_failed = true;
					return false;
				}
			}
			s.count = 0;
			s.pending = false;
		}
		s_write = 0;
		return true;
	}

	void add(xr_vector<Sum>& sums, const Event& e, u32 order, double ms)
	{
		string64 name;
		u32 n = 0;
		for (; e.name[n] && n < sizeof(name) - 1; ++n)
			name[n] = char(e.name[n]); // the scope names are plain ASCII identifiers
		name[n] = 0;

		for (Sum& s : sums)
		{
			if (s.depth == e.depth && !xr_strcmp(s.name, name))
			{
				s.ms += ms;
				s.order = order;
				return;
			}
		}
		Sum s;
		xr_strcpy(s.name, name);
		s.depth = e.depth;
		s.order = order;
		s.ms = ms;
		sums.push_back(s);
	}

	// In frame order, averaged over the frames
	void averages(const xr_vector<Sum>& sums, u32 frames, xr_vector<pass>& out)
	{
		out.clear();
		if (!frames)
			return;
		xr_vector<const Sum*> order;
		for (const Sum& s : sums)
			if (s.ms > 0.0)
				order.push_back(&s);
		std::sort(order.begin(), order.end(), [](const Sum* a, const Sum* b) { return a->order < b->order; });
		for (const Sum* s : order)
		{
			pass p;
			xr_strcpy(p.name, s->name);
			p.depth = s->depth;
			p.ms = float(s->ms / double(frames));
			out.push_back(p);
		}
	}

	void finish_capture()
	{
		xr_vector<pass> passes;
		averages(s_capture_sums, s_capture_frames, passes);
		float frame = 0.f;
		for (const pass& p : passes)
			if (!p.depth)
				frame += p.ms;
		Msg("* [GPU passes] average per frame over %.0f s (%u frames), GPU time in passes %.2f ms:", s_capture_seconds,
		    s_capture_frames, frame);
		for (const pass& p : passes)
			Msg("*   %*s%-34s %6.3f", int(p.depth * 2), "", p.name, p.ms);
		xrLogger::FlushLog();
		s_capture = false;
		s_capture_sums.clear();
		s_capture_frames = 0;
	}

	// True when the slot's results were read (or thrown away), false when they are not there yet
	bool read(Slot& s)
	{
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
		if (HW.pContext->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return false;

		static UINT64 t[kMaxEvents * 2];
		for (u32 i = 0; i < s.count * 2; ++i)
			if (HW.pContext->GetData(s.stamps[i], &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				return false;

		s.pending = false;
		if (dj.Disjoint || !dj.Frequency)
			return true;

		for (u32 i = 0; i < s.count; ++i)
		{
			const UINT64 t0 = t[i * 2], t1 = t[i * 2 + 1];
			const double ms = t1 > t0 ? double(t1 - t0) * 1000.0 / double(dj.Frequency) : 0.0;
			add(s_sums, s.events[i], i, ms);
			if (s_capture)
				add(s_capture_sums, s.events[i], i, ms);
		}
		++s_frames;
		if (s_capture)
			++s_capture_frames;
		return true;
	}
}

void frame_begin(bool enabled)
{
	s_recording = false;
	s_depth = 0;
	if (s_capture && s32(GetTickCount() - s_capture_end) >= 0)
		finish_capture();
	if (!(enabled || s_capture) || !create())
		return;

	// Oldest first
	for (u32 n = 0; n < kSlots; ++n)
	{
		Slot& s = s_slots[(s_write + n) % kSlots];
		if (s.pending && !read(s))
			break;
	}

	Slot& s = s_slots[s_write];
	if (s.pending)
		return; // GPU still behind, skip timing this frame

	HW.pContext->Begin(s.disjoint);
	s.count = 0;
	s_thread = GetCurrentThreadId();
	s_recording = true;
}

void frame_end()
{
	if (!s_recording)
		return;
	Slot& s = s_slots[s_write];

	// Close scopes still open at the end of the frame
	while (s_depth)
	{
		const u32 i = s_stack[--s_depth];
		if (i != kNotTimed)
			HW.pContext->End(s.stamps[i * 2 + 1]);
	}

	HW.pContext->End(s.disjoint);
	s.pending = true;
	s_write = (s_write + 1) % kSlots;
	s_recording = false;
}

void event_begin(LPCWSTR name)
{
	if (!s_recording || GetCurrentThreadId() != s_thread || s_depth >= kMaxDepth)
		return;
	Slot& s = s_slots[s_write];
	if (s.count >= kMaxEvents)
	{
		s_stack[s_depth++] = kNotTimed;
		return;
	}
	const u32 i = s.count++;
	s.events[i].name = name;
	s.events[i].depth = s_depth;
	HW.pContext->End(s.stamps[i * 2]);
	s_stack[s_depth++] = i;
}

void event_end()
{
	if (!s_recording || GetCurrentThreadId() != s_thread || !s_depth)
		return;
	const u32 i = s_stack[--s_depth];
	if (i != kNotTimed)
		HW.pContext->End(s_slots[s_write].stamps[i * 2 + 1]);
}

void collect(xr_vector<pass>& out)
{
	averages(s_sums, s_frames, out);
	for (Sum& s : s_sums)
		s.ms = 0.0;
	s_frames = 0;
}

void start_capture(float seconds)
{
	s_capture = true;
	s_capture_seconds = seconds;
	s_capture_end = GetTickCount() + u32(seconds * 1000.f);
	s_capture_sums.clear();
	s_capture_frames = 0;
	Msg("* [GPU passes] measuring for %.0f s, keep the view still...", seconds);
}

void destroy()
{
	for (Slot& s : s_slots)
	{
		_RELEASE(s.disjoint);
		for (ID3D11Query*& q : s.stamps)
			_RELEASE(q);
		s.count = 0;
		s.pending = false;
	}
	s_recording = false;
	s_depth = 0;
}
}
