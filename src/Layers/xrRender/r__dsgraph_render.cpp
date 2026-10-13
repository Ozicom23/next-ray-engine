#include "stdafx.h"

#include "../../xrEngine/render.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "../../xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "CHudInitializer.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "flod.h"

#include "../../xrEngine/xr_object.h"

using namespace R_dsgraph;

extern float r_ssaDISCARD;
extern float r_ssaDONTSORT;
extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

ICF float calcLOD(float ssa/*fDistSq*/, float R)
{
	return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

#ifdef USE_DX11
#include "FTreeVisual.h"
#include "dxRenderDeviceRender.h"

extern int ps_r4_tree_instancing;

// Tree instancing: in a graph with i_tree_instancing, trees whose packets have the same state and geometry are drawn
// with one DrawIndexedInstanced. Their pass is swapped for a variant with an instanced vertex shader (the same name
// with "_inst", gamedata/shaders/r3), which reads each tree's transform from xr_tree_inst. Without the shader file
// (or Screen Space Shaders' wind header it needs) trees are drawn one by one as before.
namespace tree_instancing
{
	constexpr u32 instances_max = 128; // XR_TREE_INSTANCES_MAX in shadow_direct_tree_inst.vs

	struct variant
	{
		const SVS* orig_vs = nullptr;
		const R_constant_table* orig_constants = nullptr;
		ref_vs vs;
		ref_ctable constants;
		bool valid = false;
	};
	static xr_map<u64, variant> s_variants; // by original pass
	static shared_str s_instances_name;

	static bool shader_file_exists(LPCSTR name)
	{
		string_path path;
		strconcat(sizeof(path), path, ::Render->getShaderPath(), name);
		return FS.exist("$game_shaders$", path) != nullptr;
	}

	// Name of the instanced vertex shader for this one, or nullptr
	static bool instanced_name(LPCSTR vs_name, string256& result)
	{
		// Longest first; a skinning mode suffix (_<digits>) may follow
		static const char* names[] = { "shadow_direct_tree_s_aref", "shadow_direct_tree_aref", "shadow_direct_tree_s", "shadow_direct_tree" };
		for (const char* n : names)
		{
			const size_t len = xr_strlen(n);
			if (strncmp(vs_name, n, len))
				continue;
			const char* rest = vs_name + len;
			if (*rest && !(rest[0] == '_' && isdigit(u8(rest[1]))))
				continue;
			xr_sprintf(result, "%s_inst", n);
			return true;
		}
		return false;
	}

	static variant* get(SPass& pass)
	{
		variant& v = s_variants[u64(size_t(&pass))];
		if (v.orig_vs == &*pass.vs && v.orig_constants == &*pass.constants)
			return v.valid ? &v : nullptr;

		// New pass, or another one at the same address
		v = variant();
		v.orig_vs = &*pass.vs;
		v.orig_constants = &*pass.constants;

		string256 name;
		string256 file;
		if (!pass.vs || !pass.constants || !instanced_name(pass.vs->cName.c_str(), name))
			return nullptr;
		xr_sprintf(file, "%s.vs", name);
		if (!shader_file_exists(file) || !shader_file_exists("screenspace_wind.h"))
			return nullptr;

		v.vs = DEV->_CreateVS(name);
		if (!v.vs || !v.vs->vs)
			return nullptr;

		// The pass's constants with this vertex shader, bound as in the original pass
		R_constant_table table;
		table.merge(&pass.ps->constants);
		table.merge(&v.vs->constants);
		table.merge(&pass.gs->constants);
		table.merge(&pass.hs->constants);
		table.merge(&pass.ds->constants);
		for (ref_constant& c : table.table)
		{
			if (R_constant* orig = pass.constants->get(c->name))
				c->handler = orig->handler;
		}
		if (!s_instances_name.size())
			s_instances_name = "xr_tree_inst";
		if (!table.get(s_instances_name))
			return nullptr;

		v.constants = DEV->_CreateConstantTable(table);
		v.valid = true;
		static xr_vector<shared_str> logged;
		if (std::find(logged.begin(), logged.end(), v.vs->cName) == logged.end())
		{
			logged.push_back(v.vs->cName);
			Msg("* Tree instancing: %s -> %s", pass.vs->cName.c_str(), name);
		}
		return &v;
	}

	// Trees only; the packet's pass is the visual's shadow pass
	static FTreeVisual* tree_of(const RenderPacket& packet)
	{
		dxRender_Visual* V = packet.item.pVisual;
		if (V->Type != MT_TREE_ST && V->Type != MT_TREE_PM)
			return nullptr;
		return static_cast<FTreeVisual*>(V);
	}

	static SPass* pass_of(const RenderPacket& packet, u32 iPass)
	{
		ShaderElement* E = packet.item.pVisual->shader->E[SE_R2_SHADOW]._get();
		if (!E || iPass >= E->passes.size())
			return nullptr;
		SPass* pass = &*E->passes[iPass];
		return &*pass->vs == packet.pVS ? pass : nullptr;
	}
}

void tree_instancing_reset()
{
	tree_instancing::s_variants.clear();
}
#endif

template<typename T, bool Reverse>
void CDSGraphManager::r_dsgraph_render_graph_sorted(R_dsgraph::mapDSGraphItems<T, Reverse>& graph, bool _clear)
{
    if (graph.empty())
        return;

    std::sort(graph.begin(), graph.end());

	for (auto& item : graph)
	{
		dxRender_Visual* V = item.pVisual;
		VERIFY(V && V->shader._get());
		RCache.set_Element(item.pSE);
		RCache.set_xform_world(*item.pMatrix);
		RImplementation.apply_object(item.pObject);
		RImplementation.apply_lmaterial();
		//if (item.b_hud_mode)
		//{
		//	//new feature
		//}
		V->Render(calcLOD(item.ssa, V->vis.sphere.R));
	}

	if (_clear)
		graph.clear();

	RCache.set_xform_world(Fidentity);
}

void CDSGraphManager::r_dsgraph_render_graph(RenderQueueArray& queues, u32 _priority, bool _clear, bool static_geometry)
{
	RCache.set_xform_world(Fidentity);

	for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
	{
		auto& queue = queues[_priority][iPass];
		if (queue.empty())
			continue;

#ifdef USE_DX11
		// Trees of the same geometry next to each other in each run of the same state
		const bool instancing = i_tree_instancing && ps_r4_tree_instancing && static_geometry &&
			RImplementation.phase == CRender::PHASE_SMAP;
		if (instancing)
		{
			for (auto& packet : queue)
			{
				FTreeVisual* tree = tree_instancing::tree_of(packet);
				packet.instKey = tree ? tree->instance_key(calcLOD(packet.item.ssa, packet.item.pVisual->vis.sphere.R)) : 0;
			}
			std::sort(queue.begin(), queue.end(), [](const RenderPacket& a, const RenderPacket& b)
			{
				if (a.sortKey != b.sortKey)
					return a.sortKey < b.sortKey;
				return a.instKey < b.instKey;
			});
		}
		else
#endif
		// 1. Sort by generated sort key to replicate previous fixed map behaviour
		if (queue.size() < 4096)
            std::sort(queue.begin(), queue.end());
		else
            xr_parallel_sort(queue.begin(), queue.end());

		// 2. Render
		vs_type pVS = nullptr;

#if defined(USE_DX10) || defined(USE_DX11)
		gs_type pGS = nullptr;
#endif

		ps_type pPS = nullptr;

#ifdef USE_DX11
		hs_type pHS = nullptr;
		ds_type pDS = nullptr;
#endif
		
		R_constant_table* pCS = nullptr;
		ID3DState* pState = nullptr;
		STextureList* pTextures = nullptr;

        u64 high = 0;

        for (size_t packet_index = 0; packet_index < queue.size(); ++packet_index)
        {
            auto& packet = queue[packet_index];
            auto& currentKey = packet.sortKey;
            if (currentKey.high != high)
            {
                high = currentKey.high;

                if (packet.pState != pState)
                {
                    pState = packet.pState;
                    RCache.set_States(pState);
                }

#if defined(USE_DX10) || defined(USE_DX11)
                if (packet.pGS != pGS)
                {
                    pGS = packet.pGS;
                    RCache.set_GS(pGS);
                }
#endif

#ifdef USE_DX11
                if (packet.pHS != pHS)
                {
                    pHS = packet.pHS;
                    RCache.set_HS(pHS);
                }
                if (packet.pDS != pDS)
                {
                    pDS = packet.pDS;
                    RCache.set_DS(pDS);
                }
#endif
            }

#ifdef USE_DX11
			// Tree instancing: the run of packets with the same state and geometry, drawn with the pass's instanced
			// variant (also a run of one, so trees of one state don't switch between the two constant tables)
			tree_instancing::variant* inst = nullptr;
			size_t run_end = packet_index + 1;
			if (instancing && packet.instKey)
			{
				while (run_end < queue.size() && queue[run_end].instKey == packet.instKey && queue[run_end].sortKey == packet.sortKey)
					run_end++;
				if (SPass* pass = tree_instancing::pass_of(packet, iPass))
					inst = tree_instancing::get(*pass);
			}
			vs_type packet_vs = inst ? &*inst->vs : packet.pVS;
			R_constant_table* packet_cs = inst ? &*inst->constants : packet.pCS;
#else
			vs_type packet_vs = packet.pVS;
			R_constant_table* packet_cs = packet.pCS;
#endif

            // Compare low key stuff regardless, too high collision probability
            if (packet_vs != pVS)
            {
                pVS = packet_vs;
                RCache.set_VS(pVS);
            }

            if (packet.pPS != pPS)
            {
                pPS = packet.pPS;
                RCache.set_PS(pPS);
            }

            if (packet_cs != pCS)
            {
                pCS = packet_cs;
                RCache.set_Constants(pCS);
            }

            if (packet.pTextures != pTextures)
            {
                pTextures = packet.pTextures;
                RCache.set_Textures(pTextures);
                RImplementation.apply_lmaterial();
            }

			auto& item = packet.item;
			if (!static_geometry)
			{
				RCache.set_xform_world(*item.pMatrix);
				RImplementation.apply_object(item.pObject);
				RImplementation.apply_lmaterial();
			}

#ifdef USE_DX11
			if (inst)
			{
				const u32 count = u32(run_end - packet_index);
				{
					FTreeVisual* first = static_cast<FTreeVisual*>(item.pVisual);
					const float run_LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
					RCache.LOD.set_LOD(run_LOD);
					for (u32 done = 0; done < count; done += tree_instancing::instances_max)
					{
						const u32 n = _min(count - done, tree_instancing::instances_max);
						void* data = nullptr;
						RCache.get_ConstantDirect(tree_instancing::s_instances_name, n * 3 * sizeof(Fvector4), &data, nullptr, nullptr);
						if (!data)
							break;
						Fvector4* rows = (Fvector4*)data;
						for (u32 i = 0; i < n; i++)
							static_cast<FTreeVisual*>(queue[packet_index + done + i].item.pVisual)->write_instance(rows + i * 3);
						first->render_instances(run_LOD, n);
					}
					packet_index = run_end - 1;
					continue;
				}
			}
#endif

			float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
			RCache.LOD.set_LOD(LOD);
#endif
			item.pVisual->Render(LOD);
		}

		if (_clear)
			queue.clear();
	}
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void CDSGraphManager::r_dsgraph_render_hud()
{
	PROF_EVENT("r_dsgraph_render_hud");
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUD);

	RImplementation.rmNormal();

#if defined(USE_DX11) //  Redotix99: for 3D Shader Based Scopes 		
	if (scope_3D_fake_enabled)
	{
		RCache.set_RT(RImplementation.Target->rt_ssfx_temp->pRT, 3); // Render scope_3D to any buffer

		r_dsgraph_render_graph_sorted(RGraph.mapScopeHUD);

		if (!RImplementation.o.ssfx_motionvectors)
			RCache.set_RT(NULL, 3);
		else
			RCache.set_RT(RImplementation.Target->rt_ssfx_motion_vectors->pRT, 3);
	}
#endif

	if (RGraph.mapCamAttached.size())
	{
		RImplementation.rmNear();

		// Change projection
		initializer.SetCamMode();

		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapCamAttached);

		RImplementation.rmNormal();
	}
}

void CDSGraphManager::r_dsgraph_render_hud_ui()
{
	PROF_EVENT("r_dsgraph_render_hud_ui");
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	g_hud->RenderActiveItemUI();
	RImplementation.rmNormal();
}

void CDSGraphManager::r_dsgraph_render_cam_ui()
{
	PROF_EVENT("r_dsgraph_render_cam_ui");

	// Change projection
	CHudInitializer initializer(2);
	
	// Rendering
	RImplementation.rmNear();
	g_hud->RenderCamAttachedUI();
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_sorted(bool render_hud)
{
	{
		PROF_EVENT("r_dsgraph_render_sorted");
		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Sorted, true);
		r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Sorted, true);
	}

	if (render_hud)
		r_dsgraph_render_sorted_hud();

	// Camera Script Attachments
	if (RGraph.mapCamAttachedSorted.Sorted.size())
	{
		RImplementation.rmNear();
		// Change projection
		CHudInitializer initializer(2);

		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapCamAttachedSorted.Sorted, true);
		RImplementation.rmNormal();
	}
}

void CDSGraphManager::r_dsgraph_capture_hud()
{
	if (g_hud)
	{
		g_hud->Render_Last(dcast_IPortalTraverser());
		set_Object();
	}
}

#if defined(USE_DX11)
//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_ScopeSorted()  //  Redotix99: for 3D Shader Based Scopes 	
{
	// Change projection
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapScopeHUDSorted, true);
	RImplementation.rmNormal();
}
#endif

void CDSGraphManager::r_dsgraph_render_sorted_hud()
{
	PROF_EVENT("r_dsgraph_render_sorted_hud");
#if	RENDER==R_R4
	HW.pContext->CopyResource(RImplementation.Target->rt_Accumulator->pSurface, RImplementation.Target->rt_Generic_0->pSurface);
#endif
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, true);
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_emissive(bool clear, bool renderHUD)
{
	PROF_EVENT("r_dsgraph_render_emissive");
#if	RENDER!=R_R1
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Emissive, clear);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Emissive, clear);
	
	//	HACK: Calculate this only once
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Emissive, clear);
	
	if (renderHUD)
		r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, false);

	RImplementation.rmNormal();
#endif
}

void CDSGraphManager::r_dsgraph_render_water_ssr()
{
#ifdef USE_DX11
	PROF_EVENT("r_dsgraph_render_water_ssr");
	std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
	for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

		RCache.set_Shader(RImplementation.Target->s_ssfx_water_ssr);

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		RCache.set_c("cam_pos", RImplementation.Target->Position_previous.x, RImplementation.Target->Position_previous.y, RImplementation.Target->Position_previous.z, 0.0f);

		// Previous matrix data
		RCache.set_c("m_current", RImplementation.Target->Matrix_current);
		RCache.set_c("m_previous", RImplementation.Target->Matrix_previous);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
#endif
}

void CDSGraphManager::r_dsgraph_render_water()
{
	PROF_EVENT("r_dsgraph_render_water_ssr");
    std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
    for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

#ifdef USE_DX11
		if (RImplementation.o.ssfx_water)
		{
			RCache.set_Shader(RImplementation.Target->s_ssfx_water);
		}
#endif

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		// Wind settings
		float WindDir = g_pGamePersistent->Environment().CurrentEnv->wind_direction;
		float WindVel = g_pGamePersistent->Environment().CurrentEnv->wind_velocity;
		RCache.set_c("wind_setup", WindDir, WindVel, 0, 0);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
	RGraph.mapWater.clear();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_wmarks()
{
	PROF_EVENT("r_dsgraph_render_wmarks");
#if	RENDER!=R_R1
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Wmark);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Wmark);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Wmark);
	RImplementation.rmNormal();
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_distort()
{
	PROF_EVENT("r_dsgraph_render_distort");
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Distort, true);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Distort, true);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Distort);
	RImplementation.rmNormal();
}

#include"LightTrack.h"
void CDSGraphManager::r_dsgraph_capture_static()
{
	PROF_EVENT("r_dsgraph_capture_static")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		// Traverse sector/portal structure
		if (psDeviceFlags.test(rsDrawStatic))
		{
			if (dbg)
			{
				const u32 sector_nodes = u32(m_sector_frustums.size());
				dbg->static_sector_nodes += sector_nodes;
				if (opt_bucket)
					dbg->static_sector_nodes_opt += sector_nodes;
				else
					dbg->static_sector_nodes_noopt += sector_nodes;
			}

			// Determine visibility for static geometry hierrarhy
			for (auto& pair : m_sector_frustums)
			{
				xr_vector<CFrustum>& frustums = pair.val.first;
				if (frustums.empty())
					continue;

				if (frustums.size() == 1)
				{
					if (dbg)
					{
						++dbg->static_frustum_nodes;
						++dbg->static_add_root_calls;
						if (opt_bucket)
						{
							++dbg->static_frustum_nodes_opt;
							++dbg->static_add_root_calls_opt;
						}
						else
						{
							++dbg->static_frustum_nodes_noopt;
							++dbg->static_add_root_calls_noopt;
						}
					}
					add_Static((IRenderVisual*)pair.key->root(), frustums.front(), frustums.front().getMask());
					continue;
				}

				xr_vector<u32> masks;
				masks.reserve(frustums.size());
				for (CFrustum& frustum_node : frustums)
					masks.push_back(frustum_node.getMask());

				if (dbg)
				{
					dbg->static_frustum_nodes += u32(frustums.size());
					++dbg->static_add_root_calls;
					if (opt_bucket)
					{
						dbg->static_frustum_nodes_opt += u32(frustums.size());
						++dbg->static_add_root_calls_opt;
					}
					else
					{
						dbg->static_frustum_nodes_noopt += u32(frustums.size());
						++dbg->static_add_root_calls_noopt;
					}
				}

				add_Static_MultiFrustum((IRenderVisual*)pair.key->root(), frustums, masks);
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture_lights()
{
	PROF_EVENT("r_dsgraph_capture_lights")
	g_SpatialSpaceLights->q_frustum
	(
		lstLights,
		ISpatial_DB::O_ORDERED,
		STYPE_LIGHTSOURCE,
		i_frustum
	);

#if	RENDER==R_R1
	std::sort(lstLights.begin(), lstLights.end(), [](const ISpatialShared& _1, const ISpatialShared& _2) noexcept
	{
		if (!_1.get() || !_2.get()) return false;

		return	_1->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved) < _2->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved);
	});
#endif

	for (ISpatialShared spatial : lstLights)
	{
		if (0 == spatial) continue; spatial->spatial_updatesector();
		CSector* sector = (CSector*)spatial->spatial.sector;
		if (0 == sector) continue;

		if (!RImplementation.HOM.visible(spatial->spatial.sphere)) continue;

		if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
		{
			// lightsource
			if (light* L = (light*)(spatial->dcast_Light()))
			{
#if	RENDER==R_R1
				RImplementation.L_DB->add_light(L);
#else
				if (L->get_LOD() > ps_r2_shadow_lod_min && L->has_light_visible_from_sectors(*this))
				{
					RImplementation.Lights.add_light(L);
				}
#endif
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture_dynamic(CObject* O)
{
	PROF_EVENT("r_dsgraph_capture_dynamic")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		if (psDeviceFlags.test(rsDrawDynamic))
		{
			// Traverse object database
			g_SpatialSpace->q_frustum
			(
				lstRenderables,
				ISpatial_DB::O_ORDERED,
				i_doptions,
				i_frustum
			);
			if (dbg)
			{
				const u32 spatial_count = u32(lstRenderables.size());
				dbg->dynamic_spatials += spatial_count;
				if (opt_bucket)
					dbg->dynamic_spatials_opt += spatial_count;
				else
					dbg->dynamic_spatials_noopt += spatial_count;
			}
			set_Object();
#if	RENDER==R_R1
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				std::sort(lstRenderables.begin(), lstRenderables.end(), [](const ISpatialShared& _1, const ISpatialShared& _2) noexcept
				{
					if (!_1.get() || !_2.get()) return false;

					return	_1->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved) < _2->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved);
				});

				if (ps_actor_shadow_flags.test(1))
					g_hud->Render_First(dcast_IPortalTraverser());

				r_dsgraph_capture_hud();
			}
#endif
			u32 uID_LTRACK = u32(-1);
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				// update light-vis for current entity / actor
				if (CObject* O = g_pGameLevel->CurrentViewEntity())
				{
					if (!O->getDestroy())
					{
						if (CROS_impl* R = (CROS_impl*)O->ROS())
							R->update(O);
					}
				}

				RImplementation.uLastLTRACK++;
				if (!lstRenderables.empty())
				{
					uID_LTRACK = RImplementation.uLastLTRACK % lstRenderables.size();
#if	RENDER!=R_R1
					// update light-vis for selected entity
					// track lighting environment
					if (IRenderable* renderable = (IRenderable*)lstRenderables[uID_LTRACK]->dcast_Renderable())
					{
						if (CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
							T->update(renderable);
					}
#endif
				}

			}

			// Determine visibility for dynamic part of scene
			for (u32 o_it = 0; o_it < lstRenderables.size(); o_it++)
			{
				ISpatialShared spatial = lstRenderables[o_it];
				if (0 == spatial) continue;
				CSector* sector = (CSector*)spatial->spatial.sector;
				if (0 == sector) continue;

				if (i_mask[CDSGraphManager::fl_normal] && !RImplementation.HOM.visible(spatial->spatial.sphere))
					continue;

#if	RENDER==R_R1
				if ((spatial->spatial.type & STYPE_GLOW))
				{
					if (CGlow* glow = spatial->dcast_CGlow())
					{
						// It may be an glow
						RImplementation.L_Glows->add(glow);
					}
					continue;
				}
#endif

//				if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
//				{
//					// lightsource
//					if (light* L = (light*)(spatial->dcast_Light()))
//					{
//#if	RENDER==R_R1
//						RImplementation.L_DB->add_light(L);
//#else
//						if (L->get_LOD() > EPS_L && L->has_light_visible_from_sectors(PT))
//						{
//							RImplementation.Lights.add_light(L);
//						}
//#endif
//					}
//					continue;
//				}

				if(!(spatial->spatial.type & STYPE_RENDERABLE) && !(spatial->spatial.type & STYPE_PARTICLE) && !(spatial->spatial.type & STYPE_RENDERABLESHADOW))
					continue;
				if (!is_sector_visible(sector))
					continue;

				for (CFrustum& frustum : m_sector_frustums.find(sector)->val.first)
				{
					if (dbg)
					{
						++dbg->dynamic_frustum_tests;
						if (opt_bucket)
							++dbg->dynamic_frustum_tests_opt;
						else
							++dbg->dynamic_frustum_tests_noopt;
					}

					if (frustum.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
					{
						if (dbg)
						{
							++dbg->dynamic_frustum_hits;
							if (opt_bucket)
								++dbg->dynamic_frustum_hits_opt;
							else
								++dbg->dynamic_frustum_hits_noopt;
						}

						// renderable
						IRenderable* renderable = spatial->dcast_Renderable();
						if (0 == renderable) break;

						if (O && O->dcast_Renderable() == renderable) break;

						// Rendering
#if	RENDER==R_R1
						if (i_mask[CDSGraphManager::fl_normal] && o_it == uID_LTRACK && renderable->renderable_ROS())
						{
							// track lighting environment
							if(CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
								T->update(renderable);
						}
#endif
						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object(renderable);

						renderable->renderable_Render(dcast_IPortalTraverser());
						if (dbg)
						{
							++dbg->dynamic_rendered;
							if (opt_bucket)
								++dbg->dynamic_rendered_opt;
							else
								++dbg->dynamic_rendered_noopt;
						}

						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object();
						break;
					}
				}
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture(bool lights, bool dynamic, CObject* O)
{
	PROF_EVENT("r_dsgraph_capture")
	r_dsgraph_capture_static();

	if(lights)
		r_dsgraph_capture_lights();

	if (dynamic)
		r_dsgraph_capture_dynamic(O);
}
