// Instanced tree shadows (engine: r4_tree_instancing): shadow_direct_tree.vs of Screen Space Shaders, with the
// tree transform read per instance. The engine draws all trees of one geometry in a row with one call and
// fills xr_tree_inst with their transforms; without this file it draws them one by one as before.
// Needs Screen Space Shaders' screenspace_wind.h, which reads m_xform as a global.

#include "common.h"

#define XR_TREE_INSTANCES_MAX 128

cbuffer xr_tree_instances
{
	float4 xr_tree_inst[XR_TREE_INSTANCES_MAX * 3]; // 3 rows of m_xform per tree
}

// Per instance, set first in main: screenspace_wind.h reads it
static float3x4		m_xform;
uniform float4 			consts; 	// {1/quant,1/quant,???,???}

#include "screenspace_wind.h"

#ifdef	USE_AREF
v2p_shadow_direct_aref main ( v_shadow_direct_aref I, uint iid : SV_InstanceID )
#else
v2p_shadow_direct main ( v_shadow_direct I, uint iid : SV_InstanceID )
#endif
{
	m_xform = float3x4(xr_tree_inst[iid * 3], xr_tree_inst[iid * 3 + 1], xr_tree_inst[iid * 3 + 2]);

#ifdef	USE_AREF
	v2p_shadow_direct_aref 	O;
#else
	v2p_shadow_direct 		O;
#endif

	// Transform to world coords
	float3 	pos	= mul(m_xform , I.P);
	float 	H	= pos.y - m_xform._24;	// height of vertex (scaled, rotated, etc.)
	float2	tc = 0;
	float3	wind_result = 0;

#ifdef	USE_TREEWAVE
	wind_result	= 0;
#else

	#ifdef	USE_AREF
		tc = (I.tc * consts).xy;
		wind_result = ssfx_wind_tree_branches(pos, H, tc.y, ssfx_wind_setup(), false);
	#else
		wind_result.xz = ssfx_wind_tree_trunk(pos, H, ssfx_wind_setup(), false).xy;
	#endif

#endif

	float4 f_pos = float4(pos.xyz + wind_result.xyz, 1);

	O.hpos 	= mul(m_VP,	f_pos	);

#ifdef	USE_AREF
	O.tc0 = tc;
#endif

#ifndef USE_HWSMAP
	O.depth = O.hpos.z;
#endif
	return O;
}
FXVS;
