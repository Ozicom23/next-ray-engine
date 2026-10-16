// neXt-Ray performance patches: the engine's copy of the G-buffer depth (rt_Position.z) for the ray marches of
// screenspace_common.h (SSFX_get_depth). Bit 0x400 of r4_shader_patches; the engine defines NEXTRAY_DEPTH when the copy
// is made (not with MSAA). The engine looks for this file to know the shaders can read the copy
#ifndef NEXTRAY_DEPTH_H
#define NEXTRAY_DEPTH_H
#if defined(NEXTRAY_DEPTH) && defined(NEXTRAY_DEPTH_MARCH) && !defined(USE_MSAA)
Texture2D<float> s_nextray_depth;
#endif
#endif
