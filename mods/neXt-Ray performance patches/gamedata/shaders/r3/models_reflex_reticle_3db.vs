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
    float3 P : TEXCOORD1;
	float3 T : TEXCOORD2;
	float3 B : TEXCOORD3;
	float3 N : TEXCOORD4;
};

vf     _main (v_model v)
{
    vf o;

	o.hpos = mul(m_WVP, v.P);
#if (NEXTRAY_PATCHES & 0x200)
	o.hpos.xy += nextray_upscale_jitter.xy * o.hpos.w; // neXt-Ray: jittered like the rest of the frame
#endif
    o.tc0 = v.tc.xy;
    o.P = mul(m_WV, v.P).xyz;
	o.T = mul(m_WV, v.T).xyz;
	o.B = mul(m_WV, v.B).xyz;
	o.N = mul(m_WV, v.N).xyz;

    return o;
}

// Skinning
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
