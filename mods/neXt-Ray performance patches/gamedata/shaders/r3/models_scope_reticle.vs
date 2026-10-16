// neXt-Ray performance patches: 3DSS scope parts jittered with the upscaler (nextray_upscale_jitter), the scope image sampled at the jittered position. Bit 0x200 of r4_shader_patches (NEXTRAY_PATCHES) picks the
// changed code, kept next to the original; engines without the define use it
#ifndef NEXTRAY_PATCHES
#define NEXTRAY_PATCHES 0xFFF
#endif
#include "common.h"
#include "skin.h"

#if (NEXTRAY_PATCHES & 0x200)
// neXt-Ray: the upscaler's jitter (zero without an upscaler), so this matches the frame FSR/DLSS resolve
uniform float4 nextray_upscale_jitter;

#endif
struct vf
{
    float4 hpos : SV_Position;
    float2 tc0 : TEXCOORD0;	
	float3 w_P : POSITION0;
	float3 w_T : TANGENT0;
	float3 w_B : BINORMAL0;
	float3 w_N : NORMAL0;
	float3 v_P : POSITION1;
	float3 v_T : TANGENT1;
	float3 v_B : BINORMAL1;
	float3 v_N : NORMAL1;
};

vf     _main (v_model v)
{
    vf o;

    o.hpos = mul(m_WVP, v.P);
#if (NEXTRAY_PATCHES & 0x200)
    o.hpos.xy += nextray_upscale_jitter.xy * o.hpos.w; // neXt-Ray: jittered like the rest of the frame
#endif
    o.tc0 = v.tc.xy;

	o.w_P = mul(m_W, v.P).xyz;
	o.w_T = mul(m_W, v.T).xyz;
	o.w_B = mul(m_W, v.B).xyz;
	o.w_N = mul(m_W, v.N).xyz;

	o.v_P = mul(m_WV, v.P).xyz;
	o.v_T = mul(m_WV, v.T).xyz;
	o.v_B = mul(m_WV, v.B).xyz;
	o.v_N = mul(m_WV, v.N).xyz;
	
    return o;
}

#ifdef SKIN_NONE
vf main(v_model v) { return _main(v); }
#endif

#ifdef SKIN_0
vf main(v_model_skinned_0 v) { return _main(skinning_0(v)); }
#endif

#ifdef SKIN_1
vf main(v_model_skinned_1 v) { return _main(skinning_1(v)); }
#endif

#ifdef SKIN_2
vf main(v_model_skinned_2 v) { return _main(skinning_2(v)); }
#endif

#ifdef SKIN_3
vf main(v_model_skinned_3 v) { return _main(skinning_3(v)); }
#endif

#ifdef SKIN_4
vf main(v_model_skinned_4 v) { return _main(skinning_4(v)); }
#endif
