#include "stdafx.h"
#include "r4_perf_overlay.h"
#include "r4_upscaler.h"
#include "r4_framegen.h"
#include "r4_frame_pacing.h"
#include "../../xrEngine/Stats.h"
#include "../../xrEngine/IGame_Persistent.h"

#include <dxgi1_4.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <algorithm>
#include <atomic>

#include "imgui.h"
#include "imgui_internal.h"

CPerfOverlay g_PerfOverlay;
CShaderProgressOverlay g_ShaderProgressOverlay;

int ps_r4_shader_progress = 1;

static const char* s_quality_names[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "Native AA" };
#ifdef USE_DX12
static const char* s_api = "DX12"; // the renderer the overlay is drawn by (both exes report renderer_r4)
#else
static const char* s_api = "DX11";
#endif

bool CPerfOverlay::CreateQueries()
{
	if (m_queries[0].disjoint)
		return true;
	if (m_queries_failed)
		return false;

	D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	D3D11_QUERY_DESC stamp = { D3D11_QUERY_TIMESTAMP, 0 };
	for (u32 i = 0; i < kGpuQueries; ++i)
	{
		GpuQuery& q = m_queries[i];
		if (FAILED(HW.pDevice->CreateQuery(&disjoint, &q.disjoint)) ||
			FAILED(HW.pDevice->CreateQuery(&stamp, &q.begin)) ||
			FAILED(HW.pDevice->CreateQuery(&stamp, &q.end)))
		{
			Msg("! perf_overlay: GPU timestamp queries are not available");
			Destroy();
			m_queries_failed = true;
			return false;
		}
		q.pending = false;
	}
	m_query_write = 0;
	return true;
}

void CPerfOverlay::CollectGpuTimes()
{
	// Oldest first; results arrive a few frames later, never wait for them
	for (u32 n = 0; n < kGpuQueries; ++n)
	{
		GpuQuery& q = m_queries[(m_query_write + n) % kGpuQueries];
		if (!q.pending)
			continue;

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
		if (HW.pContext->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			break;

		UINT64 t0 = 0, t1 = 0;
		if (HW.pContext->GetData(q.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
			HW.pContext->GetData(q.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			break;

		q.pending = false;
		if (!dj.Disjoint && dj.Frequency && t1 > t0)
		{
			const float ms = float(double(t1 - t0) * 1000.0 / double(dj.Frequency));
			m_sum.gpu += ms;
			++m_sum.gpu_n;
		}
	}
}

void CPerfOverlay::GpuFrameBegin()
{
	// Per-pass timing costs a few hundred queries per frame, only in detailed mode
	gpu_profiler::frame_begin(ps_perf_overlay >= 3);

	m_query_active = false;
	if (!ps_perf_overlay || !CreateQueries())
		return;

	CollectGpuTimes();

	GpuQuery& q = m_queries[m_query_write];
	if (q.pending)
		return; // results not back yet, skip timing this frame

	HW.pContext->Begin(q.disjoint);
	HW.pContext->End(q.begin);
	m_query_active = true;
}

void CPerfOverlay::GpuFrameEnd()
{
	gpu_profiler::frame_end();

	if (!ps_perf_overlay)
		return;

	m_draw_calls = RCache.stat.calls;
	m_polys = RCache.stat.polys;

	if (!m_query_active)
		return;

	GpuQuery& q = m_queries[m_query_write];
	HW.pContext->End(q.end);
	HW.pContext->End(q.disjoint);
	q.pending = true;
	m_query_write = (m_query_write + 1) % kGpuQueries;
	m_query_active = false;
}

void CPerfOverlay::PresentBegin()
{
	if (ps_perf_overlay)
		m_present_timer.Start();
}

void CPerfOverlay::PresentEnd()
{
	if (!ps_perf_overlay)
		return;
	m_present_ms = m_present_timer.GetElapsed_sec() * 1000.f;
}

void CPerfOverlay::UpdateVram()
{
	if (m_vram_failed)
		return;
	if (!m_adapter3 && FAILED(HW.m_pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&m_adapter3)))
	{
		m_adapter3 = nullptr;
		m_vram_failed = true;
		return;
	}

	if (m_vram_total_gb <= 0.f)
	{
		DXGI_ADAPTER_DESC desc = {};
		if (SUCCEEDED(m_adapter3->GetDesc(&desc)))
			m_vram_total_gb = float(double(desc.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0));
	}

	DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
	if (SUCCEEDED(m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
	{
		m_vram_used_gb = float(double(info.CurrentUsage) / (1024.0 * 1024.0 * 1024.0));
		m_vram_budget_gb = float(double(info.Budget) / (1024.0 * 1024.0 * 1024.0));
	}
}

void CPerfOverlay::UpdateRam()
{
	const double gb = 1024.0 * 1024.0 * 1024.0;

	// Game: physical memory in use by the process (working set, as in Task Manager)
	PROCESS_MEMORY_COUNTERS pmc = {};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		m_ram_used_gb = float(double(pmc.WorkingSetSize) / gb);

	// System: available and total physical memory
	MEMORYSTATUSEX ms = {};
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms))
	{
		m_ram_avail_gb = float(double(ms.ullAvailPhys) / gb);
		m_ram_total_gb = float(double(ms.ullTotalPhys) / gb);
	}
}

void CPerfOverlay::DumpPasses() const
{
	if (m_passes.empty())
	{
		Msg("* [GPU passes] no data: turn on the detailed overlay (perf_overlay detailed) and wait a moment");
		return;
	}
	Msg("* [GPU passes] average per frame over the last %.0f ms, GPU frame %.2f ms:", 250.f, m_show_gpu_ms);
	for (const gpu_profiler::pass& p : m_passes)
		Msg("*   %*s%-34s %6.3f", int(p.depth * 2), "", p.name, p.ms);
	xrLogger::FlushLog(); // write it out now, the log file is otherwise only written every so often
}

void CPerfOverlay::Destroy()
{
	for (u32 i = 0; i < kGpuQueries; ++i)
	{
		_RELEASE(m_queries[i].disjoint);
		_RELEASE(m_queries[i].begin);
		_RELEASE(m_queries[i].end);
		m_queries[i].pending = false;
	}
	m_query_active = false;
	_RELEASE(m_adapter3);
	gpu_profiler::destroy();
}

void CPerfOverlay::Draw()
{
	// The engine frame timers are collected while the overlay is on (normally only with rs_stats)
	g_stats_gather_timers = ps_perf_overlay != 0 || frame_pacing::recording();

	// Wall-clock frame time, measured here once per frame
	const float frame_ms = m_frame_timer_started ? m_frame_timer.GetElapsed_sec() * 1000.f : 0.f;
	m_frame_timer.Start();
	m_frame_timer_started = true;

	if (!ps_perf_overlay || frame_ms <= 0.f)
		return;

	// Start the statistics over when the main or pause menu opens or closes: menu frames are much faster
	// than gameplay and would keep the numbers off until they leave the time windows
	IMainMenu* menu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : nullptr;
	const bool in_menu = menu && menu->IsActive();
	if (in_menu != m_in_menu)
	{
		m_in_menu = in_menu;
		m_frame_count = 0;
		m_show_fps = 0.f;
		m_show_low_fps = 0.f;
		m_sum = {};
		return; // this frame spans the switch
	}

	m_frame_ms[m_frame_pos] = frame_ms;
	m_frame_pos = (m_frame_pos + 1) % kFrameHistory;
	m_frame_count = _min(m_frame_count + 1, kFrameHistory);
	m_graph[m_graph_pos] = frame_ms;
	m_graph_pos = (m_graph_pos + 1) % kGraphSamples;

	// Per-frame times, averaged over each refresh interval (the engine's own smoothed values fall too slowly).
	// CPU: engine update + render submission; RenderTOTAL includes Present, which waits for vsync/the GPU
	const CStats* st = Device.Statistic;
	m_sum.frame += frame_ms;
	++m_sum.n;
	if (st)
	{
		m_sum.cpu += _max(0.f, st->EngineTOTAL.last + st->RenderTOTAL.last - m_present_ms);
		const CStatTimer* timers[kTimers] = { &st->EngineTOTAL, &st->Sheduler, &st->UpdateClient, &st->Physics,
			&st->AI_Think, &st->Animation, &st->RenderTOTAL, &st->RenderCALC, &st->RenderCALC_HOM };
		for (u32 i = 0; i < kTimers; ++i)
			m_sum.timers[i] += timers[i]->last;
	}

	// Refresh the numbers 4 times per second
	m_refresh_ms += frame_ms;
	m_presents_ms += frame_ms;
	if (m_refresh_ms >= 250.f || m_show_fps <= 0.f)
	{
		m_refresh_ms = 0.f;

		// Walk back from the newest frame: FPS over the last half second, 1% low over the last 5 seconds,
		// so the numbers follow changes quickly instead of a fixed frame count
		float avg_sum = 0.f, low_time = 0.f;
		u32 avg_n = 0, low_n = 0;
		float recent[kFrameHistory];
		for (u32 i = 0; i < m_frame_count; ++i)
		{
			const float ms = m_frame_ms[(m_frame_pos + kFrameHistory - 1 - i) % kFrameHistory];
			if (avg_sum < 500.f)
			{
				avg_sum += ms;
				++avg_n;
			}
			if (low_time >= 5000.f)
				break;
			low_time += ms;
			recent[low_n++] = ms;
		}
		const float avg_ms = avg_n ? avg_sum / float(avg_n) : frame_ms;
		m_show_frame_ms = avg_ms;
		m_show_fps = avg_ms > 0.f ? 1000.f / avg_ms : 0.f;

		// 1% low: the frame time 99% of recent frames are faster than
		// Needs enough frames to mean something, shown as "-" until then
		if (low_n >= 100)
		{
			const u32 idx = u32(float(low_n - 1) * 0.99f);
			std::nth_element(recent, recent + idx, recent + low_n);
			m_show_low_fps = recent[idx] > 0.f ? 1000.f / recent[idx] : 0.f;
		}
		if (m_sum.n)
		{
			const float n = float(m_sum.n);
			m_show_interval_ms = m_sum.frame / n;
			m_show_cpu_ms = m_sum.cpu / n;
			for (u32 i = 0; i < kTimers; ++i)
				m_show_timers[i] = m_sum.timers[i] / n;
		}
		if (m_sum.gpu_n)
			m_show_gpu_ms = m_sum.gpu / float(m_sum.gpu_n);
		m_sum = {};
#ifdef USE_DX12
		UINT presents = 0;
		const bool counted = framegen::active() && HW.m_pSwapChain && SUCCEEDED(HW.m_pSwapChain->GetLastPresentCount(&presents));
		m_show_screen_fps = counted && m_presents_valid && m_presents_ms > 0.f ? float(presents - m_presents) * 1000.f / m_presents_ms : 0.f;
		m_presents = presents;
		m_presents_valid = counted;
		m_presents_ms = 0.f;
#endif

		UpdateVram();
		UpdateRam();
		gpu_memory::get_totals(m_gpu);
		gpu_profiler::collect(m_passes);
	}

	// Only inside an open ImGui frame (not before the engine's first ImGui::NewFrame)
	const ImGuiContext* imgui = ImGui::GetCurrentContext();
	if (!imgui || !imgui->WithinFrameScope)
		return;

	const ImGuiIO& io = ImGui::GetIO();
	const u32 corner = ps_perf_overlay_pos & 3;
	const float pad = 10.f;
	ImGui::SetNextWindowPos(ImVec2((corner & 1) ? io.DisplaySize.x - pad : pad, (corner & 2) ? io.DisplaySize.y - pad : pad),
	                        ImGuiCond_Always, ImVec2((corner & 1) ? 1.f : 0.f, (corner & 2) ? 1.f : 0.f));
	ImGui::SetNextWindowBgAlpha(0.6f);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;

	if (ImGui::Begin("##perf_overlay", nullptr, flags))
	{
		ImGui::SetWindowFontScale(_max(1.f, io.DisplaySize.y / 1080.f));

		if (m_show_low_fps > 0.f)
			ImGui::Text("%4.0f FPS   %5.2f ms   1%% low %4.0f", m_show_fps, m_show_frame_ms, m_show_low_fps);
		else
			ImGui::Text("%4.0f FPS   %5.2f ms   1%% low    -", m_show_fps, m_show_frame_ms);
		if (m_show_screen_fps > 0.f)
			ImGui::Text("%4.0f FPS on screen with frame generation", m_show_screen_fps);

		if (m_show_gpu_ms > 0.f)
		{
			ImGui::Text("GPU %5.2f ms   CPU %5.2f ms", m_show_gpu_ms, m_show_cpu_ms);
			ImGui::SameLine();
			// Whichever side takes (nearly) the whole frame limits it; neither means an FPS cap, vsync or waits
			const float limit = m_show_interval_ms * 0.85f;
			if (m_show_gpu_ms >= limit && m_show_gpu_ms >= m_show_cpu_ms)
				ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "  GPU-bound");
			else if (m_show_cpu_ms >= limit)
				ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "  CPU-bound");
			else
				ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "  capped / waiting");
		}
		else
			ImGui::Text("GPU   n/a     CPU %5.2f ms", m_show_cpu_ms);

		// RAM: the game's own, without its GPU memory; then all programs' in use (total minus available; where the GPU
		// shares the RAM, its memory too)
		if (m_ram_total_gb > 0.f)
			ImGui::Text("RAM  %.2f GB   system %.1f / %.1f GB", m_ram_used_gb,
			            _max(0.f, m_ram_total_gb - m_ram_avail_gb), m_ram_total_gb);
		// The game's GPU memory; budget is what the OS/driver lets it use before evicting, below the card's total
		if (m_vram_budget_gb > 0.f && m_vram_total_gb > 0.f)
			ImGui::Text("VRAM %.2f GB   budget %.1f / %.1f GB", m_vram_used_gb, m_vram_budget_gb, m_vram_total_gb);
		else if (m_vram_budget_gb > 0.f)
			ImGui::Text("VRAM %.2f GB   budget %.1f GB", m_vram_used_gb, m_vram_budget_gb);
		// What the VRAM is made of: "other" is everything the engine doesn't count itself (driver, DXVK,
		// upscaler, shaders, small buffers)
		{
			const double gb = 1024.0 * 1024.0 * 1024.0;
			const float tex = float(double(m_gpu.textures()) / gb);
			const float geo = float(double(m_gpu.geometry()) / gb);
			const float rt = float(double(m_gpu.bytes[gpu_memory::render_target]) / gb);
			if (m_vram_used_gb > 0.f)
				ImGui::Text("  Tex %.2f  Geo %.2f  RT %.2f  Other %.2f GB", tex, geo, rt, _max(0.f, m_vram_used_gb - tex - geo - rt));
			else
				ImGui::Text("  Tex %.2f  Geo %.2f  RT %.2f GB", tex, geo, rt);

			if (ps_perf_overlay >= 3)
			{
				const u64* b = m_gpu.bytes;
				ImGui::Text("  Tex: BC %.2f  uncompressed %.2f  no mips %.2f GB",
				            float(double(b[gpu_memory::tex_bc] + b[gpu_memory::tex_bc_nomips]) / gb),
				            float(double(b[gpu_memory::tex_raw] + b[gpu_memory::tex_raw_nomips]) / gb),
				            float(double(b[gpu_memory::tex_bc_nomips] + b[gpu_memory::tex_raw_nomips]) / gb));
				ImGui::Text("  Geo: level %.2f  models %.2f  grass %.2f GB", float(double(b[gpu_memory::buf_level]) / gb),
				            float(double(b[gpu_memory::buf_model]) / gb), float(double(b[gpu_memory::buf_grass]) / gb));
				if (b[gpu_memory::tex_staging])
					ImGui::Text("  Texture copies in RAM %.2f GB", float(double(b[gpu_memory::tex_staging]) / gb));
			}
		}

		CRenderTarget* target = RImplementation.Target;
		CUpscaler* upscaler = GetUpscaler(RImplementation.o.upscaler);
		if (target && upscaler && target->upscale_enabled())
		{
			const u32 q = _min(ps_r4_upscaler_quality, u32(upscaler_quality_native));
			extern float ps_r4_upscaler_scale;
			string32 custom;
			if (ps_r4_upscaler_scale > 0.f && q != upscaler_quality_native)
				xr_sprintf(custom, "Custom %.2f", float(target->upscale_render_width()) / float(_max(Device.dwWidth, 1u)));
			ImGui::Text("%s   %s %s   %ux%u -> %ux%u", s_api, upscaler->Name(),
			            ps_r4_upscaler_scale > 0.f && q != upscaler_quality_native ? custom : s_quality_names[q],
			            target->upscale_render_width(), target->upscale_render_height(), Device.dwWidth, Device.dwHeight);
		}
		else
			ImGui::Text("%s   Native   %ux%u", s_api, Device.dwWidth, Device.dwHeight);

		if (ps_perf_overlay >= 3)
		{
			ImGui::Separator();
			const float* t = m_show_timers;
			ImGui::Text("Update %5.2f   Scheduler %5.2f   Objects %5.2f", t[0], t[1], t[2]);
			ImGui::Text("Physics %5.2f   AI %5.2f   Animation %5.2f", t[3], t[4], t[5]);
			ImGui::Text("Render %5.2f   Calc %5.2f   HOM %5.2f", t[6], t[7], t[8]);
			ImGui::Text("Draw calls %u   Triangles %.2f M", m_draw_calls, float(m_polys) / 1000000.f);
		}

		// Frame time graph: compact + graph and detailed
		if (ps_perf_overlay >= 2)
		{
			float graph_max = 0.f;
			for (u32 i = 0; i < kGraphSamples; ++i)
				graph_max = _max(graph_max, m_graph[i]);
			graph_max = _max(graph_max * 1.1f, 16.7f);

			string64 label;
			xr_sprintf(label, "frame time (max %.1f ms)", graph_max);
			ImGui::PlotLines("##frametime", m_graph, int(kGraphSamples), int(m_graph_pos), label, 0.f, graph_max,
			                 ImVec2(320.f * _max(1.f, io.DisplaySize.y / 1080.f), 60.f * _max(1.f, io.DisplaySize.y / 1080.f)));
		}

		if (ps_perf_overlay >= 3)
		{
			// GPU time per render pass, nested like the PIX_EVENT scopes; small passes are left out to keep it short
			if (!m_passes.empty())
			{
				ImGui::Separator();
				ImGui::Text("GPU passes, ms");
				float top_level = 0.f;
				for (const gpu_profiler::pass& p : m_passes)
				{
					if (!p.depth)
						top_level += p.ms;
					if (p.depth > 2 || p.ms < 0.1f)
						continue;
					ImGui::Text("%*s%-28s %5.2f", int(p.depth * 2), "", p.name, p.ms);
				}
				if (m_show_gpu_ms > top_level)
					ImGui::Text("%-28s %5.2f", "outside passes (UI, present)", m_show_gpu_ms - top_level);
			}
		}
	}
	ImGui::End();
}

namespace shader_progress
{
	static std::atomic<const char*> s_what{ nullptr }; // nullptr: nothing to show
	static std::atomic<u32> s_total{ 0 };
	static std::atomic<u32> s_done{ 0 };
	static std::atomic<bool> s_running{ false };
	static std::atomic<u64> s_start_ms{ 0 };
	static std::atomic<u64> s_end_ms{ 0 };

	void begin(const char* what, u32 total)
	{
		if (!total)
			return;
		s_total = total;
		s_done = 0;
		s_start_ms = GetTickCount64();
		s_end_ms = 0;
		s_running = true;
		s_what = what;
	}

	void step() { ++s_done; }

	void end(bool completed)
	{
		s_end_ms = GetTickCount64();
		s_running = false;
		if (!completed)
			s_what = nullptr; // stopped (vid_restart, quit): not shown as done
	}

	// Compiles while the game runs, counted per burst: one more than 2 s after the last starts a new count
	static const u64 kLiveShowMs = 2000;
	static std::atomic<u32> s_live_count{ 0 };
	static std::atomic<u64> s_live_last_ms{ 0 };

	void compiled()
	{
		const u64 now = GetTickCount64();
		if (now - s_live_last_ms > kLiveShowMs)
			s_live_count = 1;
		else
			++s_live_count;
		s_live_last_ms = now;
	}
}

// "Compiling shaders: N" while shaders are compiled during the game (the frames stall meanwhile, so it mostly shows
// right after each one)
static void DrawLiveCompiles(const ImGuiIO& io, float scale)
{
	using namespace shader_progress;
	if (GetTickCount64() - s_live_last_ms > kLiveShowMs || !s_live_count)
		return;
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40.f * scale), ImGuiCond_Always, ImVec2(0.5f, 1.f));
	ImGui::SetNextWindowBgAlpha(0.7f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
	if (ImGui::Begin("##shader_compiles", nullptr, flags))
	{
		ImGui::SetWindowFontScale(scale);
		ImGui::Text("Compiling shaders: %u", u32(s_live_count));
	}
	ImGui::End();
}

void CShaderProgressOverlay::Draw()
{
	using namespace shader_progress;
	static const u64 kShowDoneMs = 4000;

	if (!ps_r4_shader_progress)
		return;

	// Only inside an open ImGui frame (not before the engine's first ImGui::NewFrame)
	const ImGuiContext* imgui = ImGui::GetCurrentContext();
	if (!imgui || !imgui->WithinFrameScope)
		return;
	const ImGuiIO& io = ImGui::GetIO();
	const float scale = _max(1.f, io.DisplaySize.y / 1080.f);

	// The start's window first; compiles during the game when it isn't shown
	const char* what = s_what;
	const bool running = s_running;
	const u64 now = GetTickCount64();
	const u64 end_ms = s_end_ms;
	if (!what || (!running && now - end_ms > kShowDoneMs))
	{
		DrawLiveCompiles(io, scale);
		return;
	}

	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40.f * scale), ImGuiCond_Always, ImVec2(0.5f, 1.f));
	ImGui::SetNextWindowBgAlpha(0.7f);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;

	if (ImGui::Begin("##shader_progress", nullptr, flags))
	{
		ImGui::SetWindowFontScale(scale);
		const u32 total = s_total;
		const u32 done = _min(u32(s_done), total);
		const float seconds = float(((running ? now : end_ms) - s_start_ms) / 1000.0);
		if (running)
		{
			ImGui::Text("%s   %u / %u   %.0f s", what, done, total, seconds);
			string32 percent;
			xr_sprintf(percent, "%.0f%%", 100.f * float(done) / float(total));
			ImGui::ProgressBar(float(done) / float(total), ImVec2(420.f * scale, 0.f), percent);
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "You can play meanwhile: until it is done, effects may stutter once");
		}
		else
			ImGui::Text("%s: done, %u in %.0f s", what, total, seconds);
	}
	ImGui::End();
}
