#include "stdafx.h"
#include "../../xrEngine/xr_object.h"
#include "FBasicVisual.h"
#include "SkeletonCustom.h"

extern int ps_r2_shadow_omnipart_vischeck;

// A light of a shadow map batch (lights sharing the shadow map atlas)
struct batch_light
{
	light* L;
	bool decorative;
	bool forward; // semi-transparent casters, drawn to the colour target as well
};

// Light shadow cache (r4_light_shadow_cache): every frame every shadowed light's shadow map was drawn again into the
// atlas, static geometry included. A light that doesn't move (its static casters were captured long ago) and has no
// semi-transparent casters now keeps its static geometry, without trees and bushes (they sway in the wind), in a depth
// texture of its own. Each frame that is copied into the light's place in the atlas by a small shader (D3D11 can't
// copy part of a depth texture), and the trees, moving objects and grass are drawn over it as before. It is drawn
// again when the light's size, transform or static capture change. Same shadows, the static geometry drawn once.
namespace light_shadow_cache
{
#ifdef USE_DX11
	int ps_cache = 1;

	struct entry
	{
		ID3D11Texture2D* tex = nullptr;
		ID3D11DepthStencilView* dsv = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		u32 size = 0, version = 0, used = 0;
		Fmatrix combine;
		bool valid = false;
		void release() { _RELEASE(srv); _RELEASE(dsv); _RELEASE(tex); valid = false; }
	};
	xr_map<light*, entry> s_entries;
	const u32 kUnusedFrames = 600;

	// The copy into the atlas
	ID3D11VertexShader* s_vs = nullptr;
	ID3D11PixelShader* s_ps = nullptr;
	ID3D11Buffer* s_cb = nullptr;
	ID3D11DepthStencilState* s_ds = nullptr;
	ID3D11RasterizerState* s_rs = nullptr;
	ID3D11BlendState* s_bs = nullptr;
	bool s_failed = false;

	const char* kCopyShader =
		"Texture2D<float> src : register(t0);\n"
		"cbuffer params : register(b0) { float4 origin; };\n"
		"float4 vs(uint id : SV_VertexID) : SV_Position\n"
		"{\n"
		"	float2 uv = float2((id << 1) & 2, id & 2);\n"
		"	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
		"}\n"
		"float ps(float4 pos : SV_Position) : SV_Depth\n"
		"{\n"
		"	return src.Load(int3(int2(pos.xy - origin.xy), 0));\n"
		"}\n";

	// stat_light_cache
	bool s_stats = false;
	u32 s_stats_end = 0, s_frames = 0, s_last_stat_frame = 0;
	u32 s_hits = 0, s_fills = 0, s_off_moving = 0, s_off_forward = 0, s_off_other = 0;

	void start_stats(float seconds)
	{
		s_stats = true;
		s_stats_end = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
		s_hits = s_fills = s_off_moving = s_off_forward = s_off_other = s_frames = 0;
		Msg("* [Light shadow cache] measuring for %.0f s...", seconds);
	}

	static void count(u32& c)
	{
		if (!s_stats)
			return;
		if (Device.dwTimeGlobal >= s_stats_end)
		{
			s_stats = false;
			const float f = float(_max(1u, s_frames));
			Msg("* [Light shadow cache] per frame over %u frames, %u lights cached: shadow maps from cache %.2f, drawn and cached %.2f,"
				" not cached: moving %.2f, semi-transparent %.2f, other %.2f", s_frames, u32(s_entries.size()), s_hits / f,
				s_fills / f, s_off_moving / f, s_off_forward / f, s_off_other / f);
			xrLogger::FlushLog();
			return;
		}
		if (s_last_stat_frame != Device.dwFrame)
		{
			s_last_stat_frame = Device.dwFrame;
			s_frames++;
		}
		c++;
	}

	static bool create_shaders()
	{
		if (s_ps)
			return true;
		if (s_failed)
			return false;
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		ID3DBlob* err = nullptr;
		HRESULT hr = D3DCompile(kCopyShader, xr_strlen(kCopyShader), "light_shadow_copy", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
		if (SUCCEEDED(hr))
			hr = D3DCompile(kCopyShader, xr_strlen(kCopyShader), "light_shadow_copy", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_vs);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_ps);
		if (SUCCEEDED(hr))
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = 16;
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			hr = HW.pDevice->CreateBuffer(&bd, nullptr, &s_cb);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_DEPTH_STENCIL_DESC dd = {};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
			hr = HW.pDevice->CreateDepthStencilState(&dd, &s_ds);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_RASTERIZER_DESC rd = {};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = FALSE;
			hr = HW.pDevice->CreateRasterizerState(&rd, &s_rs);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_BLEND_DESC bl = {};
			bl.RenderTarget[0].RenderTargetWriteMask = 0;
			hr = HW.pDevice->CreateBlendState(&bl, &s_bs);
		}
		if (FAILED(hr))
		{
			Msg("! Light shadow cache: copy shader failed: %s", err ? (const char*)err->GetBufferPointer() : "");
			s_failed = true;
		}
		_RELEASE(vs);
		_RELEASE(ps);
		_RELEASE(err);
		return !s_failed;
	}

	static bool ensure_texture(entry& e, u32 size)
	{
		if (e.tex && e.size == size)
			return true;
		e.release();
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = size;
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R24G8_TYPELESS;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
		dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		if (FAILED(HW.pDevice->CreateTexture2D(&d, nullptr, &e.tex)) || FAILED(HW.pDevice->CreateDepthStencilView(e.tex, &dv, &e.dsv)) ||
			FAILED(HW.pDevice->CreateShaderResourceView(e.tex, &sv, &e.srv)))
		{
			e.release();
			return false;
		}
		e.size = size;
		return true;
	}

	// The light's cache entry when it can be cached, else nullptr. fresh: the entry holds this frame's static geometry
	entry* get(const batch_light& b, bool& fresh)
	{
		fresh = false;
		if (!ps_cache)
			return nullptr;
		light* L = b.L;
		if (b.decorative || L->flags.bHudMode)
		{
			count(s_off_other);
			return nullptr;
		}
		if (L->m_moving_frames < 32u)
		{
			count(s_off_moving);
			return nullptr;
		}
		if (b.forward)
		{
			count(s_off_forward);
			return nullptr;
		}
		if (!create_shaders())
			return nullptr;
		if (s_entries.size() >= 64 && s_entries.find(L) == s_entries.end())
		{
			count(s_off_other); // enough lights cached
			return nullptr;
		}
		entry& e = s_entries[L];
		e.used = Device.dwFrame;
		const u32 size = L->X.S.size;
		fresh = e.valid && e.size == size && e.version == L->m_static_version && !memcmp(&e.combine, &L->X.S.combine, sizeof(Fmatrix));
		if (!ensure_texture(e, size))
		{
			s_entries.erase(L);
			return nullptr;
		}
		count(fresh ? s_hits : s_fills);
		return &e;
	}

	void filled(entry& e, light* L)
	{
		e.version = L->m_static_version;
		e.combine = L->X.S.combine;
		e.valid = true;
	}

	// Copies the entry into the light's place in the bound atlas; the caller rebinds the engine's state afterwards
	void copy_to_atlas(entry& e, light* L)
	{
		ID3D11DeviceContext* ctx = HW.pContext;
		D3D11_MAPPED_SUBRESOURCE m;
		if (FAILED(ctx->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
			return;
		float* o = (float*)m.pData;
		o[0] = float(L->X.S.posX);
		o[1] = float(L->X.S.posY);
		o[2] = o[3] = 0.f;
		ctx->Unmap(s_cb, 0);

		D3D11_VIEWPORT vp = { float(L->X.S.posX), float(L->X.S.posY), float(e.size), float(e.size), 0.f, 1.f };
		ctx->RSSetViewports(1, &vp);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(s_vs, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ctx->PSSetShader(s_ps, nullptr, 0);
		ctx->PSSetShaderResources(0, 1, &e.srv);
		ctx->PSSetConstantBuffers(0, 1, &s_cb);
		ctx->OMSetDepthStencilState(s_ds, 0);
		ctx->RSSetState(s_rs);
		const float blend[4] = {};
		ctx->OMSetBlendState(s_bs, blend, 0xffffffff);
		ctx->Draw(3, 0);
		ID3D11ShaderResourceView* none = nullptr;
		ctx->PSSetShaderResources(0, 1, &none);
	}

	// Entries of lights not drawn for a while
	void sweep()
	{
		for (auto it = s_entries.begin(); it != s_entries.end();)
		{
			if (Device.dwFrame - it->second.used > kUnusedFrames)
			{
				it->second.release();
				it = s_entries.erase(it);
			}
			else
				++it;
		}
	}

	void destroy()
	{
		for (auto& it : s_entries)
			it.second.release();
		s_entries.clear();
		_RELEASE(s_vs);
		_RELEASE(s_ps);
		_RELEASE(s_cb);
		_RELEASE(s_ds);
		_RELEASE(s_rs);
		_RELEASE(s_bs);
		s_failed = false;
	}
#endif
}

bool check_grass_shadow(light* L, CFrustum VB)
{
	// Grass shadows are allowed?
	if (ps_ssfx_grass_shadows.x < 3 || !psDeviceFlags2.test(rsGrassShadow))
		return false;

	// Inside the range?
	if (L->vis.distance > ps_ssfx_grass_shadows.z)
		return false;

	// Is in view? L->vis.visible?
	u32 mask = 0xff;
	if (!VB.testSphere(L->position, L->range * 0.6f, mask))
		return false;

	return true;
}

IC void hud_light_apply(xr_map<light*, std::pair<Fvector, Fvector>>& saved_pos, xr_vector<light*>& source)
{
	for (u32 it = 0; it < source.size(); it++)
	{
		light* L = source[it];
		if (!L->get_hud_mode()) continue;

		saved_pos.emplace(L, mk_pair(L->position, L->direction));

		Device.hud_to_world(L->position);
		Device.hud_to_world_dir(L->direction);
	}
}

IC void hud_light_restore(xr_map<light*, std::pair<Fvector, Fvector>>& saved_pos, xr_vector<light*>& source)
{
	for (const auto& saved : saved_pos)
	{
		light* L = saved.first;
		if (!L->get_hud_mode()) continue;

		L->position = saved.second.first;
		L->direction = saved.second.second;
	}
}

void CRender::render_lights(light_Package& LP)
{
#ifdef USE_DX11
	light_shadow_cache::sweep();
#endif
	xr_map<light*, std::pair<Fvector, Fvector>> saved_pos;
	stats.ls_shadowed_in += (u32)LP.v_shadowed.size();
	stats.ls_unshadowed_point_in += (u32)LP.v_point.size();
	stats.ls_unshadowed_spot_in += (u32)LP.v_spot.size();
	stats.ls_shadowed_peak_in = _max(stats.ls_shadowed_peak_in, (u32)LP.v_shadowed.size());
	//////////////////////////////////////////////////////////////////////////
	// 0. apply hud_mode projection if necessary
	hud_light_apply(saved_pos, LP.v_shadowed);
	hud_light_apply(saved_pos, LP.v_point);
	hud_light_apply(saved_pos, LP.v_spot);

	{
#if defined(USE_DX10) || defined(USE_DX11)
		PIX_EVENT(SHADOWED_LIGHTS);
#endif
		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(PHASE_VIS_UPDATE);
#endif
			xr_vector<light*>& source = LP.v_shadowed;
			source.erase(std::remove_if(source.begin(), source.end(), [](light* L)
			{
				if(L->m_parent)
				{
					if (ps_r2_shadow_omnipart_vischeck)
					{
						L->vis_update();
					}
					else if(L->m_parent->omnipart[0] == L)
					{
						L->m_parent->vis_update();
						for (int f = 0; f < 6; f++)
						{
							L->m_parent->omnipart[f]->vis.pending = L->m_parent->vis.pending;
							L->m_parent->omnipart[f]->vis.visible = L->m_parent->vis.visible;
						}
					}
				}
				else
					L->vis_update();
				if (L->vis.pending)
				{
					RImplementation.stats.ls_shadowed_pending_skipped++;
					return true;
				}
				if (!L->vis.visible)
				{
					RImplementation.stats.ls_shadowed_invisible_skipped++;
					return true;
				}

				L->optimize_smap_size();

				return false;
			}), source.end());
			stats.ls_shadowed_after_vis += (u32)source.size();
			stats.ls_shadowed_peak_after_vis = _max(stats.ls_shadowed_peak_after_vis, (u32)source.size());
		}

		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(PHASE_CALC_POOLS);
#endif
			xr_vector<light*>& source = LP.v_shadowed;
			static xr_vector<light*> refactored;
			refactored.clear();
			u32 total = (u32)source.size();

			for (u16 smap_ID = 0; refactored.size() != total; smap_ID++)
			{
				LP_smap_pool.initialize(o.smapsize);
				// Equal sizes in a fixed order, so the same lights get the same atlas places every frame (light shadow cache)
				std::sort(source.begin(), source.end(), [](light* _1, light* _2) {return _1->X.S.size != _2->X.S.size ? _1->X.S.size > _2->X.S.size : _1 < _2; });
				source.erase(std::remove_if(source.begin(), source.end(), [smap_ID](light* L)
				{
					SMAP_Rect R;
					if (RImplementation.LP_smap_pool.push(R, L->X.S.size))
					{
						L->X.S.posX = R.min.x;
						L->X.S.posY = R.min.y;
						L->vis.smap_ID = smap_ID;
						refactored.push_back(L);
						return true;
					}
					return false;
				}), source.end());
			}

			std::reverse(refactored.begin(), refactored.end());
			LP.v_shadowed = refactored;
		}

		//////////////////////////////////////////////////////////////////////////
		// sort lights by importance???
		// while (has_any_lights_that_cast_shadows) {
		//		if (has_point_shadowed)		->	generate point shadowmap
		//		if (has_spot_shadowed)		->	generate spot shadowmap
		//		switch-to-accumulator
		//		if (has_point_unshadowed)	-> 	accum point unshadowed
		//		if (has_spot_unshadowed)	-> 	accum spot unshadowed
		//		if (was_point_shadowed)		->	accum point shadowed
		//		if (was_spot_shadowed)		->	accum spot shadowed
		//	}
		//	if (left_some_lights_that_doesn't cast shadows)
		//		accumulate them
		while (!LP.v_shadowed.empty())
		{
			// if (has_spot_shadowed)
			static xr_vector<light*> L_spot_s;
			{
#if defined(USE_DX10) || defined(USE_DX11)
				PIX_EVENT(GENERATE_SHMAPS);
#endif
				// generate spot shadowmap
				Target->phase_smap_spot_clear();
				xr_vector<light*>& source = LP.v_shadowed;
				light* L = source.back();
				u16			sid = L->vis.smap_ID;
				static xr_vector<batch_light> batch;
				batch.clear();
				while (!source.empty())
				{
					if (source.empty())		break;
					L = source.back();
					if (L->vis.smap_ID != sid)	break;
					source.pop_back();
					// render
					phase = PHASE_SMAP;
#if defined(USE_DX10) || defined(USE_DX11)
					PIX_EVENT(RENDER_SHADOWS);
#endif
					bool decorative_light = false;
					{
						if ((L->decor_object[0] && !L->decor_object[0]->getDestroy()) || (L->decor_object[1] && !L->decor_object[1]->getDestroy()) || (L->decor_object[2] && !L->decor_object[2]->getDestroy()) || (L->decor_object[3] && !L->decor_object[3]->getDestroy()) || (L->decor_object[4] && !L->decor_object[4]->getDestroy()) || (L->decor_object[5] && !L->decor_object[5]->getDestroy()))
						{
							for (int f = 0; f < 6; f++)
							{
								if (L->decor_object[f] && !L->decor_object[f]->getDestroy())
								{
									L->decor_object[f]->renderable_Render(&L->GMLight);
									decorative_light = true;
								}
							}
						}
						else
						{
							if (L->m_moving_frames<32u)
							{
								L->GMLight.RGraph.clear_static<false>();
								L->GMLight.traverse((CSector*)L->SpatialComponent->spatial.sector, L->X.S.frustum, L->position, L->X.S.combine);
								L->GMLight.r_dsgraph_capture_static();
								L->m_moving_frames++;
								L->m_static_version++;
							}
							L->GMLight.r_dsgraph_capture_dynamic(L->ignore_object);
						}
					}

					bool bDeffered_Shadows = L->GMLight.RGraph.mapStaticPasses[0][0].size() || L->GMLight.RGraph.mapDynamicPasses[0][0].size();
					bool bForward_Shadows = L->GMLight.RGraph.mapStaticPasses[1][0].size() || L->GMLight.RGraph.mapDynamicPasses[1][0].size() || L->GMLight.RGraph.mapStaticSorted.Sorted.size() || L->GMLight.RGraph.mapDynamicSorted.Sorted.size();
					if (bDeffered_Shadows || bForward_Shadows)
					{
						L_spot_s.push_back(L);
						batch.push_back({ L, decorative_light, bForward_Shadows });
					}
					else if (L->flags.bVolumetric && ps_r2_ls_flags.test(R2FLAG_VOLUMETRIC_LIGHTS))
					{
						L_spot_s.push_back(L);
					}
				}

				// The batch's shadow maps: static geometry from the cache when nothing changed (r4_light_shadow_cache)
				auto setup = [&](light* L)
				{
					Target->phase_smap_spot(L);
					RCache.set_xform_world(Fidentity);
					RCache.set_xform_view(L->X.S.view);
					RCache.set_xform_project(L->X.S.project);
				};
				auto render_rest = [&](const batch_light& b)
				{
					light* L = b.L;
					L->GMLight.r_dsgraph_render_dynamic(0, true);
					if (Details && Details->dtFS && check_grass_shadow(L, ViewBase) && L->flags.bShadow && !b.decorative)
					{
						Details->fade_distance = -1; // Use light position to calc "fade"
						Details->light_position.set(L->position);
						Details->hw_Render(L);
					}

					L->X.S.transluent = FALSE;
					if (b.forward)
					{
						L->X.S.transluent = TRUE;
						Target->phase_smap_spot_tsh(L);

						L->GMLight.r_dsgraph_render_static(1, false);
						L->GMLight.r_dsgraph_render_dynamic(1, true);

						L->GMLight.r_dsgraph_render_sorted();			// strict-sorted geoms
					}
				};

				// Trees and bushes sway in the wind: never cached, drawn each frame over the cached rest
				auto render_trees = [&](light* L)
				{
					if (!L->GMLight.i_static_trees)
						return;
					L->GMLight.i_render_trees = 1;
					L->GMLight.r_dsgraph_render_static(0, false);
					L->GMLight.i_render_trees = -1;
				};
				for (const batch_light& b : batch)
				{
					light* L = b.L;
#ifdef USE_DX11
					bool fresh = false;
					if (light_shadow_cache::entry* e = light_shadow_cache::get(b, fresh))
					{
						if (!fresh)
						{
							// The light's own texture: static geometry without trees
							Target->u_setrt(e->size, e->size, NULL, NULL, NULL, e->dsv);
							HW.pContext->ClearDepthStencilView(e->dsv, D3D_CLEAR_DEPTH, 1.0f, 0);
							D3D_VIEWPORT VP = { 0.f, 0.f, float(e->size), float(e->size), 0.f, 1.f };
							HW.pContext->RSSetViewports(1, &VP);
							RCache.set_CullMode(CULL_CCW);
							RCache.set_Stencil(FALSE);
							RCache.set_ColorWriteEnable(FALSE);
							RCache.set_xform_world(Fidentity);
							RCache.set_xform_view(L->X.S.view);
							RCache.set_xform_project(L->X.S.project);
							L->GMLight.i_render_trees = 0;
							L->GMLight.r_dsgraph_render_static(0, false);
							L->GMLight.i_render_trees = -1;
							light_shadow_cache::filled(*e, L);
						}
						setup(L);
						light_shadow_cache::copy_to_atlas(*e, L);
						RCache.Invalidate(); // the copy bypassed the engine's state tracking
						setup(L);
						render_trees(L);
						render_rest(b);
						continue;
					}
#endif
					setup(L);
					L->GMLight.r_dsgraph_render_static(0, false);
					render_rest(b);
				}
				batch.clear();
			}
			//		if (was_spot_shadowed)		->	accum spot shadowed
			if (!L_spot_s.empty())
			{
				PROF_EVENT("ACCUM_SPOT");
				stats.ls_shadowed_rendered += (u32)L_spot_s.size();
				for (light* L : L_spot_s)
				{
					Target->accum_spot(L);
					render_indirect(L);
					if (L->flags.bVolumetric && RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
					{
#ifdef USE_DX11
						float w = float(Device.dwRenderWidth);
						float h = float(Device.dwRenderHeight);

						if (RImplementation.o.ssfx_volumetric)
							Target->set_viewport_size(HW.pContext, w / RImplementation.o.volsize, h / RImplementation.o.volsize);
#endif

						if (ps_pfx_volumetric_mode == 1)
							Target->accum_volumetric_lv(L);
						else
							Target->accum_volumetric(L);

#ifdef USE_DX11
						// Restore resolution
						if (RImplementation.o.ssfx_volumetric)
							Target->set_viewport_size(HW.pContext, w, h);
#endif
					}
				}

				L_spot_s.clear();
			}
		}
	}

	{
#if defined(USE_DX10) || defined(USE_DX11)
		PIX_EVENT(UNSHADOWED_LIGHTS);
#endif
        PROF_EVENT("UNSHADOWED_LIGHTS");
		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(POINT_LIGHTS_ACCUM_UNSH);
#endif
			// Point lighting (unshadowed, if left)
			if (!LP.v_point.empty())
			{
				for (light* L : LP.v_point)
				{
					L->vis_update();
					if (!L->vis.visible)
						continue;

					Target->accum_point(L);
					++stats.ls_unshadowed_point_rendered;
					render_indirect(L);
				}
				LP.v_point.clear();
			}
		}
		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(SPOT_LIGHTS_ACCUM_UNSH);
#endif
			// Spot lighting (unshadowed, if left)
			if (!LP.v_spot.empty())
			{
				for (light* L : LP.v_spot)
				{
					L->vis_update();
					if (!L->vis.visible)
						continue;

					Target->accum_spot(L);
					++stats.ls_unshadowed_spot_rendered;
					render_indirect(L);
				}
				LP.v_spot.clear();
			}
		}
	}

	// restore world projection if necessary
    {
        PROF_EVENT("hud_light_restore");
        hud_light_restore(saved_pos, LP.v_shadowed);
        hud_light_restore(saved_pos, LP.v_point);
        hud_light_restore(saved_pos, LP.v_spot);
    }
	
}

void CRender::render_indirect(light* L)
{
	if (!ps_r2_ls_flags.test(R2FLAG_GI)) return;

	light LIGEN;
	LIGEN.set_type(IRender_Light::REFLECTED);
	LIGEN.set_shadow(false);
	LIGEN.set_cone(PI_DIV_2 * 2.f);

	xr_vector<light_indirect>& Lvec = L->indirect;
	if (Lvec.empty()) return;
	float LE = L->color.intensity();
	for (u32 it = 0; it < Lvec.size(); it++)
	{
		light_indirect& LI = Lvec[it];

		// energy and color
		float LIE = LE * LI.E;
		if (LIE < ps_r2_GI_clip) continue;
		Fvector T;
		T.set(L->color.r, L->color.g, L->color.b).mul(LI.E);
		LIGEN.set_color(T.x, T.y, T.z);

		// geometric
		Fvector L_up, L_right;
		L_up.set(0, 1, 0);
		if (_abs(L_up.dotproduct(LI.D)) > .99f) L_up.set(0, 0, 1);
		L_right.crossproduct(L_up, LI.D).normalize();
		LIGEN.SpatialComponent->spatial.sector = LI.S;
		LIGEN.set_position(LI.P);
		LIGEN.set_rotation(LI.D, L_right);

		// range
		// dist^2 / range^2 = A - has infinity number of solutions
		// approximate energy by linear fallof Emax / (1 + x) = Emin
		float Emax = LIE;
		float Emin = 1.f / 255.f;
		float x = (Emax - Emin) / Emin;
		if (x < 0.1f) continue;
		LIGEN.set_range(x);

		Target->accum_reflected(&LIGEN);
	}
}
