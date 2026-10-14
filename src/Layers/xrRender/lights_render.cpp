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

// Light shadow cache (r4_light_shadow_cache): every frame all shadowed lights' shadow maps were drawn again into the
// atlas. When a batch has the same lights at the same places and sizes as last frame, none of them moving (their
// static casters were last captured long ago) and no trees or bushes (wind) or semi-transparent casters among the
// static ones, the atlas with only the static geometry drawn is kept and copied back; moving objects and grass are
// drawn on top as before. Same shadows, the static geometry drawn once instead of every frame.
namespace light_shadow_cache
{
	enum result { off, fill, hit };
#ifdef USE_DX11
	int ps_cache = 1;

	struct key_entry
	{
		light* L;
		u32 version;
		int x, y, size;
		Fmatrix combine;
	};
	struct slot
	{
		ID3D11Texture2D* tex = nullptr;
		xr_vector<key_entry> key;
		u32 frame = 0; // when it was last filled or used
		bool valid = false;
	};
	const u32 kSlots = 4;
	slot s_slots[kSlots];

	// stat_light_cache
	bool s_stats = false;
	u32 s_stats_end = 0;
	u32 s_hits = 0, s_fills = 0, s_off_moving = 0, s_off_trees = 0, s_off_forward = 0, s_off_other = 0, s_frames = 0;
	u32 s_last_stat_frame = 0;

	void start_stats(float seconds)
	{
		s_stats = true;
		s_stats_end = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
		s_hits = s_fills = s_off_moving = s_off_trees = s_off_forward = s_off_other = s_frames = 0;
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
			Msg("* [Light shadow cache] per frame over %u frames, shadow map batches: from cache %.2f, drawn and cached %.2f,"
				" not cached: moving %.2f, trees %.2f, semi-transparent %.2f, other %.2f", s_frames, s_hits / f, s_fills / f,
				s_off_moving / f, s_off_trees / f, s_off_forward / f, s_off_other / f);
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

	static ID3D11Texture2D* atlas()
	{
		ID3DBaseTexture* t = RImplementation.Target->rt_smap_depth->pSurface;
		return (ID3D11Texture2D*)t;
	}

	static bool ensure_texture(slot& s)
	{
		ID3D11Texture2D* a = atlas();
		if (!a)
			return false;
		D3D11_TEXTURE2D_DESC da;
		a->GetDesc(&da);
		if (s.tex)
		{
			D3D11_TEXTURE2D_DESC ds;
			s.tex->GetDesc(&ds);
			if (ds.Width == da.Width && ds.Height == da.Height && ds.Format == da.Format)
				return true;
			_RELEASE(s.tex);
			s.valid = false;
		}
		D3D11_TEXTURE2D_DESC d = da;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL; // a copy destination only, same as the atlas
		d.MiscFlags = 0;
		return SUCCEEDED(HW.pDevice->CreateTexture2D(&d, nullptr, &s.tex));
	}

	static void make_key(const xr_vector<batch_light>& batch, xr_vector<key_entry>& key)
	{
		key.clear();
		for (const batch_light& b : batch)
			key.push_back({ b.L, b.L->m_static_version, int(b.L->X.S.posX), int(b.L->X.S.posY), int(b.L->X.S.size), b.L->X.S.combine });
	}

	static bool same(const xr_vector<key_entry>& a, const xr_vector<key_entry>& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); i++)
		{
			if (a[i].L != b[i].L || a[i].version != b[i].version || a[i].x != b[i].x || a[i].y != b[i].y ||
				a[i].size != b[i].size || memcmp(&a[i].combine, &b[i].combine, sizeof(Fmatrix)))
				return false;
		}
		return true;
	}

	result lookup(u16 sid, const xr_vector<batch_light>& batch)
	{
		if (!ps_cache || sid >= kSlots || batch.empty())
			return off;
		for (const batch_light& b : batch)
		{
			if (b.decorative)
			{
				count(s_off_other);
				return off;
			}
			if (b.L->m_moving_frames < 32u)
			{
				count(s_off_moving);
				return off;
			}
			if (b.forward)
			{
				count(s_off_forward);
				return off;
			}
			if (b.L->GMLight.i_static_trees)
			{
				count(s_off_trees);
				return off;
			}
		}
		slot& s = s_slots[sid];
		static xr_vector<key_entry> key;
		make_key(batch, key);
		// Used in the frame before: the lights in the key are still the same objects
		if (s.valid && s.tex && s.frame + 1 >= Device.dwFrame && same(s.key, key))
		{
			s.frame = Device.dwFrame;
			count(s_hits);
			return hit;
		}
		count(s_fills);
		return fill;
	}

	void store(u16 sid, const xr_vector<batch_light>& batch)
	{
		slot& s = s_slots[sid];
		if (!ensure_texture(s))
		{
			s.valid = false;
			return;
		}
		HW.pContext->CopyResource(s.tex, atlas());
		make_key(batch, s.key);
		s.frame = Device.dwFrame;
		s.valid = true;
	}

	void restore(u16 sid)
	{
		HW.pContext->CopyResource(atlas(), s_slots[sid].tex);
	}

	void destroy()
	{
		for (slot& s : s_slots)
		{
			_RELEASE(s.tex);
			s.valid = false;
			s.key.clear();
		}
	}
#else
	result lookup(u16, const xr_vector<batch_light>&) { return off; }
	void store(u16, const xr_vector<batch_light>&) {}
	void restore(u16) {}
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
				std::sort(source.begin(), source.end(), [](light* _1, light* _2) {return _1->X.S.size > _2->X.S.size; });
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

				const int cache = light_shadow_cache::lookup(sid, batch);
				if (cache == light_shadow_cache::hit)
				{
					light_shadow_cache::restore(sid);
					for (const batch_light& b : batch)
					{
						setup(b.L);
						render_rest(b);
					}
				}
				else
				{
					for (const batch_light& b : batch)
					{
						setup(b.L);
						b.L->GMLight.r_dsgraph_render_static(0, false);
						if (cache != light_shadow_cache::fill)
							render_rest(b);
					}
					if (cache == light_shadow_cache::fill)
					{
						light_shadow_cache::store(sid, batch);
						for (const batch_light& b : batch)
						{
							setup(b.L);
							render_rest(b);
						}
					}
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
