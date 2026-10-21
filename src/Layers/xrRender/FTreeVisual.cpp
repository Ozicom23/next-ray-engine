#include "stdafx.h"
#pragma hdrstop

#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/igame_level.h"
#include "../../xrEngine/environment.h"
#include "../../xrEngine/fmesh.h"

#include "../../build_config_defines.h"

#include "ftreevisual.h"

shared_str m_xform;
shared_str m_xform_v;
shared_str c_consts;
shared_str c_wave;
shared_str c_wind;
shared_str c_c_bias;
shared_str c_c_scale;
shared_str c_c_sun;

shared_str c_prev_wave;
shared_str c_prev_wind;

shared_str c_c_PrevBendersPos;
shared_str c_c_BendersPos;
shared_str c_c_BendersSetup;
shared_str c_nextray_benders;
int ps_r4_tree_bender_cull = 1; // benders out of a tree's reach moved behind the ones in reach (identical result)

FTreeVisual::FTreeVisual(void)
{
}

FTreeVisual::~FTreeVisual(void)
{
}

void FTreeVisual::Release()
{
	dxRender_Visual::Release();
}

void FTreeVisual::Load(const char* N, IReader* data, u32 dwFlags)
{
	dxRender_Visual::Load(N, data, dwFlags);

	D3DVERTEXELEMENT9* vFormat = NULL;

	// read vertices
	R_ASSERT(data->find_chunk(OGF_GCONTAINER));
	{
		// verts
		u32 ID = data->r_u32();
		vBase = data->r_u32();
		vCount = data->r_u32();
		vFormat = RImplementation.getVB_Format(ID);

		VERIFY(NULL==p_rm_Vertices);

		p_rm_Vertices = RImplementation.getVB(ID);
		p_rm_Vertices->AddRef();

		// indices
		dwPrimitives = 0;
		ID = data->r_u32();
		iBase = data->r_u32();
		iCount = data->r_u32();
		dwPrimitives = iCount / 3;

		VERIFY(NULL==p_rm_Indices);
		p_rm_Indices = RImplementation.getIB(ID);
		p_rm_Indices->AddRef();
	}

	// load tree-def
	R_ASSERT(data->find_chunk(OGF_TREEDEF2));
	{
		data->r(&xform, sizeof(xform));
		data->r(&c_scale, sizeof(c_scale));
		c_scale.rgb.mul(.5f);
		c_scale.hemi *= .5f;
		c_scale.sun *= .5f;
		data->r(&c_bias, sizeof(c_bias));
		c_bias.rgb.mul(.5f);
		c_bias.hemi *= .5f;
		c_bias.sun *= .5f;
		//Msg				("hemi[%f / %f], sun[%f / %f]",c_scale.hemi,c_bias.hemi,c_scale.sun,c_bias.sun);
	}

	// Geom
	rm_geom.create(vFormat, p_rm_Vertices, p_rm_Indices);

	// Get constants
	m_xform = "m_xform";
	m_xform_v = "m_xform_v";
	c_consts = "consts";
	c_wave = "wave";
	c_wind = "wind";
	c_c_bias = "c_bias";
	c_c_scale = "c_scale";
	c_c_sun = "c_sun";

	c_prev_wave = "prev_wave";
	c_prev_wind = "prev_wind";

	c_c_PrevBendersPos = "benders_prevpos";
	c_c_BendersPos = "benders_pos";
	c_nextray_benders = "nextray_benders";
	c_c_BendersSetup = "benders_setup";
}

struct FTreeVisual_setup
{
	u32 dwFrame;
	float scale;
	Fvector4 wave;
	Fvector4 wind;

	FTreeVisual_setup()
	{
		dwFrame = 0;
	}

	void calculate()
	{
		dwFrame = Device.dwFrame;

		// Calc wind-vector3, scale
		float tm_rot = PI_MUL_2 * Device.fTimeGlobal / ps_r__Tree_w_rot;

#ifdef TREE_WIND_EFFECT
		CEnvDescriptor& E = *g_pGamePersistent->Environment().CurrentEnv;
		float fValue = E.m_fTreeAmplitudeIntensity;
		wind.set(_sin(tm_rot), 0, _cos(tm_rot), 0);
		wind.normalize();
#if RENDER!=R_R1
		wind.mul(fValue); // dir1*amplitude
#else // R1
		wind.mul(ps_r__Tree_w_amp); // dir1*amplitude
#endif //-RENDER!=R_R1
#else //!TREE_WIND_EFFECT
		wind.set				(_sin(tm_rot),0,_cos(tm_rot),0);	wind.normalize	();	wind.mul(ps_r__Tree_w_amp);	// dir1*amplitude
#endif //-TREE_WIND_EFFECT
		scale = 1.f / float(FTreeVisual_quant);

		// setup constants
		wave.set(ps_r__Tree_Wave.x, ps_r__Tree_Wave.y, ps_r__Tree_Wave.z,
		         Device.fTimeGlobal * ps_r__Tree_w_speed); // wave
		wave.div(PI_MUL_2);
	}
};

void FTreeVisual::Render(float LOD)
{
	PROF_EVENT("FTreeVisual::Render");
	static FTreeVisual_setup tvs, prev_tvs;
	if (tvs.dwFrame != Device.dwFrame)
	{
		prev_tvs = tvs; // Save previous frame calculations
		tvs.calculate();
	}
	// setup constants
#if RENDER!=R_R1
	Fmatrix xform_v;
	xform_v.mul_43(RCache.get_xform_view(), xform);
	RCache.tree.set_m_xform_v(xform_v); // matrix
#endif
	float s = ps_r__Tree_SBC;
	RCache.tree.set_m_xform(xform); // matrix
	RCache.tree.set_consts(tvs.scale, tvs.scale, 0, 0); // consts/scale
	RCache.tree.set_wave(tvs.wave); // wave
	RCache.tree.set_wind(tvs.wind); // wind

	RCache.set_c(c_prev_wave, prev_tvs.wave);
	RCache.set_c(c_prev_wind, prev_tvs.wind);

#if RENDER!=R_R1
	s *= 1.3333f;
	RCache.tree.set_c_scale(s * c_scale.rgb.x, s * c_scale.rgb.y, s * c_scale.rgb.z, s * c_scale.hemi); // scale
	RCache.tree.set_c_bias(s * c_bias.rgb.x, s * c_bias.rgb.y, s * c_bias.rgb.z, s * c_bias.hemi); // bias
#else
	CEnvDescriptor& desc = *g_pGamePersistent->Environment().CurrentEnv;
	RCache.tree.set_c_scale(s * c_scale.rgb.x, s * c_scale.rgb.y, s * c_scale.rgb.z, s * c_scale.hemi); // scale
	RCache.tree.set_c_bias(s * c_bias.rgb.x + desc.ambient.x, s * c_bias.rgb.y + desc.ambient.y,
	                       s * c_bias.rgb.z + desc.ambient.z, s * c_bias.hemi); // bias
#endif
	RCache.tree.set_c_sun(s * c_scale.sun, s * c_bias.sun, 0, 0); // sun

#if RENDER==R_R4 || RENDER==R_R3

	if (ps_ssfx_grass_interactive.y > 0)
	{
		// Inter grass Settings
		RCache.set_c(c_c_BendersSetup, ps_ssfx_int_grass_params_1);

		// Grass benders data ( Player + Characters )
		IGame_Persistent::grass_data& GData = g_pGamePersistent->grass_shader_data;
		Fvector4 player_pos = { 0, 0, 0, 0 };
		int BendersQty = _min(16, ps_ssfx_grass_interactive.y + 1);

		// Add Player?
		if (ps_ssfx_grass_interactive.x > 0)
		{
			player_pos.set(Device.vCameraPosition.x, Device.vCameraPosition.y, Device.vCameraPosition.z, -1);
		}

		Fvector4* c_grass;
		{
			void* GrassData;
			RCache.get_ConstantDirect(c_c_BendersPos, BendersQty * sizeof(Fvector4) * 2, &GrassData, 0, 0);

			c_grass = (Fvector4*)GrassData;
		}
		VERIFY(c_grass);

		// Benders that can't reach this tree (farther in XZ than their radius from its bounding sphere, plus a margin
		// for the displacement of the benders before them) contribute exactly 0 in the leaf shaders' loops. The ones
		// that can come first, in their order, the rest are far benders of strength 0: the same result in any leaf
		// shader, and the neXt-Ray patch mod's leaf shaders stop after nextray_benders.x (.y for the previous frame)
		// Level trees keep their bounding sphere in world space (as the static graph uses it); a sphere far from the
		// tree's origin is taken as model space and placed with the tree's matrix, which the leaf shaders use
		const float tree_scale = _max(_max(xform.i.magnitude(), xform.j.magnitude()), xform.k.magnitude());
		Fvector tree_c = vis.sphere.P;
		float tree_r = vis.sphere.R;
		if (tree_c.distance_to(xform.c) > vis.sphere.R * _max(tree_scale, 1.f) * 2.f + 5.f)
		{
			xform.transform_tiny(tree_c, vis.sphere.P);
			tree_r = vis.sphere.R * tree_scale;
		}
		tree_r += 3.f;
		const bool cull = ps_r4_tree_bender_cull != 0;
		auto reaches = [&](const Fvector4& p)
		{
			const float r = p.w > 0.f ? p.w : ps_ssfx_int_grass_params_1.x;
			const float dx = p.x - tree_c.x, dz = p.z - tree_c.z;
			const float reach = r + 0.001f + tree_r;
			return dx * dx + dz * dz <= reach * reach;
		};
		static const Fvector4 far_pos = { 1.e5f, -1.e5f, 1.e5f, 0.001f };
		static const Fvector4 far_dir = { 0.f, -99.f, 0.f, 0.f };
		int count_cur = BendersQty, count_prev = BendersQty;

		if (c_grass)
		{
			Fvector4 pos[16], dir[16];
			pos[0].set(player_pos);
			dir[0].set(0.0f, -99.0f, 0.0f, 1.0f);
			for (int Bend = 1; Bend < BendersQty; Bend++)
			{
				pos[Bend].set(GData.pos[Bend].x, GData.pos[Bend].y, GData.pos[Bend].z, GData.radius_curr[Bend]);
				dir[Bend].set(GData.dir[Bend].x, GData.dir[Bend].y, GData.dir[Bend].z, GData.str[Bend]);
			}
			int k = 0;
			for (int Bend = 0; Bend < BendersQty; Bend++)
			{
				if (cull && !reaches(pos[Bend]))
					continue;
				c_grass[k].set(pos[Bend]);
				c_grass[k + 16].set(dir[Bend]);
				k++;
			}
			count_cur = k;
			for (; k < BendersQty; k++)
			{
				c_grass[k].set(far_pos);
				c_grass[k + 16].set(far_dir);
			}
		}

		Fvector4* c_prevgrass;
		{
			void* prevGrassData;
			RCache.get_ConstantDirect(c_c_PrevBendersPos, BendersQty * sizeof(Fvector4) * 2, &prevGrassData, 0, 0);

			c_prevgrass = (Fvector4*)prevGrassData;
		}
		VERIFY(c_prevgrass);

		if (c_prevgrass)
		{
			int k = 0;
			for (int Bend = 0; Bend < BendersQty; Bend++)
			{
				if (cull && !reaches(GData.prev_pos[Bend]))
					continue;
				c_prevgrass[k].set(GData.prev_pos[Bend]);
				c_prevgrass[k + 16].set(GData.prev_dir[Bend]);
				k++;
			}
			count_prev = k;
			for (; k < BendersQty; k++)
			{
				c_prevgrass[k].set(far_pos);
				c_prevgrass[k + 16].set(far_dir);
			}
		}
		RCache.set_c(c_nextray_benders, float(count_cur), float(count_prev), 1.f, 0.f); // z = 1: the counts are valid
	}
#endif
}

#define PCOPY(a)	a = pFrom->a

void FTreeVisual::Copy(dxRender_Visual* pSrc)
{
	dxRender_Visual::Copy(pSrc);

	FTreeVisual* pFrom = fast_dynamic_cast<FTreeVisual*>(pSrc);

	PCOPY(rm_geom);

	PCOPY(p_rm_Vertices);
	if (p_rm_Vertices) p_rm_Vertices->AddRef();

	PCOPY(vBase);
	PCOPY(vCount);

	PCOPY(p_rm_Indices);
	if (p_rm_Indices) p_rm_Indices->AddRef();

	PCOPY(iBase);
	PCOPY(iCount);
	PCOPY(dwPrimitives);

	PCOPY(xform);
	PCOPY(c_scale);
	PCOPY(c_bias);
}

//-----------------------------------------------------------------------------------
// Stripified Tree
//-----------------------------------------------------------------------------------
FTreeVisual_ST::FTreeVisual_ST(void)
{
}

FTreeVisual_ST::~FTreeVisual_ST(void)
{
}

void FTreeVisual_ST::Release()
{
	inherited::Release();
}

void FTreeVisual_ST::Load(const char* N, IReader* data, u32 dwFlags)
{
	inherited::Load(N, data, dwFlags);
}

#ifdef USE_DX11
// stat_tree_batches <seconds>: how many tree draws could be merged into instanced draws. Per pass kind (main view,
// shadow maps): draws, tree parts (visual objects) drawn, distinct geometry (model and detail level) and runs of
// draws in a row with the same geometry.
namespace tree_batch_stats
{
	struct kind_stats
	{
		u64 draws = 0, distinct = 0, runs = 0, objects = 0;
		xr_unordered_set<u64> seen;
		xr_unordered_set<u64> seen_objects;
		u64 last = 0;
		u32 last_frame = 0;
	};
	static kind_stats s_kinds[2];
	static bool s_active = false;
	static u32 s_frame_start = 0, s_frames = 0, s_end_time = 0, s_prev_frame = 0;

	void start(float seconds)
	{
		for (kind_stats& k : s_kinds)
			k = kind_stats();
		s_active = true;
		s_frame_start = Device.dwFrame;
		s_prev_frame = Device.dwFrame;
		s_end_time = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
		Msg("* [Tree batches] measuring for %.0f s...", seconds);
	}

	void finish()
	{
		s_active = false;
		const u32 frames = _max(1u, Device.dwFrame - s_frame_start);
		static const char* names[] = { "main view", "shadow maps" };
		Msg("* [Tree batches] per frame over %u frames:", frames);
		for (int i = 0; i < 2; i++)
		{
			kind_stats& k = s_kinds[i];
			k.distinct += k.seen.size();
			k.objects += k.seen_objects.size();
			Msg("*   %-12s draws %7.1f, tree parts drawn %7.1f, distinct geometry %6.1f, runs of the same geometry %7.1f",
				names[i], double(k.draws) / frames, double(k.objects) / frames, double(k.distinct) / frames, double(k.runs) / frames);
		}
		xrLogger::FlushLog();
	}

	void record(const void* object, const void* geom, u32 vbase, u32 ibase, u32 prims)
	{
		if (!s_active)
			return;
		if (Device.dwFrame != s_prev_frame)
		{
			// New frame: distinct counts are per frame
			for (kind_stats& k : s_kinds)
			{
				k.distinct += k.seen.size();
				k.seen.clear();
				k.objects += k.seen_objects.size();
				k.seen_objects.clear();
			}
			s_prev_frame = Device.dwFrame;
			if (Device.dwTimeGlobal >= s_end_time)
			{
				finish();
				return;
			}
		}
		kind_stats& k = s_kinds[RImplementation.phase == CRender::PHASE_SMAP ? 1 : 0];
		const u64 key = (u64(size_t(geom)) * 0x9E3779B97F4A7C15ull) ^ (u64(vbase) << 40) ^ (u64(ibase) << 20) ^ u64(prims);
		k.draws++;
		k.seen.insert(key);
		k.seen_objects.insert(u64(size_t(object)));
		if (key != k.last || k.last_frame != Device.dwFrame)
			k.runs++;
		k.last = key;
		k.last_frame = Device.dwFrame;
	}
}
#endif

void FTreeVisual_ST::Render(float LOD)
{
	inherited::Render(LOD);
#ifdef USE_DX11
	tree_batch_stats::record(this, &*rm_geom, vBase, iBase, dwPrimitives);
#endif
	RCache.set_Geometry(rm_geom);
	RCache.Render(D3DPT_TRIANGLELIST, vBase, 0, vCount, iBase, dwPrimitives);
	RCache.stat.r.s_flora.add(vCount);
}

#ifdef USE_DX11
static void write_rows(Fvector4* rows, const Fmatrix& m)
{
	// As R_constant sets a 3x4 matrix
	rows[0].set(m._11, m._21, m._31, m._41);
	rows[1].set(m._12, m._22, m._32, m._42);
	rows[2].set(m._13, m._23, m._33, m._43);
}

void FTreeVisual::write_instance(Fvector4* rows) const
{
	// The values Render sets for a tree
	Fmatrix xform_v;
	xform_v.mul_43(RCache.get_xform_view(), xform);
	write_rows(rows, xform);
	write_rows(rows + 3, xform_v);
	const float s = ps_r__Tree_SBC * 1.3333f;
	rows[6].set(s * c_scale.rgb.x, s * c_scale.rgb.y, s * c_scale.rgb.z, s * c_scale.hemi);
	rows[7].set(s * c_bias.rgb.x, s * c_bias.rgb.y, s * c_bias.rgb.z, s * c_bias.hemi);
	rows[8].set(s * c_scale.sun, s * c_bias.sun, 0, 0);
}

static u64 tree_instance_key(const void* geom, u32 vbase, u32 ibase, u32 prims)
{
	return ((u64(size_t(geom)) * 0x9E3779B97F4A7C15ull) ^ (u64(vbase) << 40) ^ (u64(ibase) << 20) ^ u64(prims)) | 1;
}

static int tree_pm_lod_id(FSlideWindowItem* pSWI, float LOD, u32 last_lod)
{
	return LOD >= 0.f ? iFloor((1.f - LOD) * float(pSWI->count - 1) + 0.5f) : int(last_lod);
}

u64 FTreeVisual_ST::instance_key(float LOD)
{
	if (!m_inst_key)
		m_inst_key = tree_instance_key(&*rm_geom, vBase, iBase, dwPrimitives);
	return m_inst_key;
}

void FTreeVisual_ST::render_instances(float LOD, u32 count)
{
	FTreeVisual::Render(LOD);
	RCache.set_Geometry(rm_geom);
	RCache.RenderInstanced(D3DPT_TRIANGLELIST, vBase, 0, vCount, iBase, dwPrimitives, count);
	RCache.stat.r.s_flora.add(vCount * count);
}
#endif

void FTreeVisual_ST::Copy(dxRender_Visual* pSrc)
{
	inherited::Copy(pSrc);
}

//-----------------------------------------------------------------------------------
// Progressive Tree
//-----------------------------------------------------------------------------------
FTreeVisual_PM::FTreeVisual_PM(void)
{
	pSWI = 0;
	last_lod = 0;
}

FTreeVisual_PM::~FTreeVisual_PM(void)
{
}

void FTreeVisual_PM::Release()
{
	inherited::Release();
}

void FTreeVisual_PM::Load(const char* N, IReader* data, u32 dwFlags)
{
	inherited::Load(N, data, dwFlags);
	R_ASSERT(data->find_chunk(OGF_SWICONTAINER));
	{
		u32 ID = data->r_u32();
		pSWI = RImplementation.getSWI(ID);
	}
}

void FTreeVisual_PM::Render(float LOD)
{
	inherited::Render(LOD);
	int lod_id = last_lod;
	if (LOD >= 0.f)
	{
		lod_id = iFloor((1.f - LOD) * float(pSWI->count - 1) + 0.5f);
		last_lod = lod_id;
	}
	VERIFY(lod_id>=0 && lod_id<int(pSWI->count));
	FSlideWindow& SW = pSWI->sw[lod_id];
#ifdef USE_DX11
	tree_batch_stats::record(this, &*rm_geom, vBase, iBase + SW.offset, SW.num_tris);
#endif
	RCache.set_Geometry(rm_geom);
	RCache.Render(D3DPT_TRIANGLELIST, vBase, 0, SW.num_verts, iBase + SW.offset, SW.num_tris);
	RCache.stat.r.s_flora.add(SW.num_verts);
}

#ifdef USE_DX11
u64 FTreeVisual_PM::instance_key(float LOD)
{
	// The geometry's key, the LOD in the top bits (the LOD picks the slide window as render_instances does)
	if (!m_inst_key)
	{
		m_inst_key = tree_instance_key(&*rm_geom, vBase, iBase, 0) & 0x0000FFFFFFFFFFFFull;
		m_inst_lods = pSWI->count;
	}
	const int lod_id = LOD >= 0.f ? iFloor((1.f - LOD) * float(m_inst_lods - 1) + 0.5f) : int(last_lod);
	return m_inst_key | (u64(lod_id + 1) << 48);
}

void FTreeVisual_PM::render_instances(float LOD, u32 count)
{
	FTreeVisual::Render(LOD);
	const int lod_id = tree_pm_lod_id(pSWI, LOD, last_lod);
	last_lod = lod_id;
	FSlideWindow& SW = pSWI->sw[lod_id];
	RCache.set_Geometry(rm_geom);
	RCache.RenderInstanced(D3DPT_TRIANGLELIST, vBase, 0, SW.num_verts, iBase + SW.offset, SW.num_tris, count);
	RCache.stat.r.s_flora.add(SW.num_verts * count);
}
#endif

void FTreeVisual_PM::Copy(dxRender_Visual* pSrc)
{
	inherited::Copy(pSrc);
	FTreeVisual_PM* pFrom = fast_dynamic_cast<FTreeVisual_PM*>(pSrc);
	PCOPY(pSWI);
}
