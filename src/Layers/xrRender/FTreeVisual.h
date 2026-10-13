#pragma once

// refs
struct FSlideWindowItem;

#include "FBasicVisual.h"

class FTreeVisual : public dxRender_Visual, public IRender_Mesh
{
private:
	struct _5color
	{
		Fvector rgb; // - all static lighting
		float hemi; // - hemisphere
		float sun; // - sun
	};

protected:
	_5color c_scale;
	_5color c_bias;
	Fmatrix xform;
public:
	virtual void Render(float LOD); // LOD - Level Of Detail  [0.0f - min, 1.0f - max], Ignored
	virtual void Load(LPCSTR N, IReader* data, u32 dwFlags);
	virtual void Copy(dxRender_Visual* pFrom);
	virtual void Release();

#ifdef USE_DX11
	// Instanced drawing (r4_tree_instancing, r__dsgraph_render.cpp): trees with the same instance_key draw the
	// same geometry and can share one draw, each with its own transform
	virtual u64 instance_key(float LOD) { return 0; } // 0: can't be instanced
	void write_instance(Fvector4* rows) const; // 3 rows of m_xform
	virtual void render_instances(float LOD, u32 count) {} // constants of this tree, then count instances
#endif

	FTreeVisual(void);
	virtual ~FTreeVisual(void);
};

class FTreeVisual_ST : public FTreeVisual
{
	typedef FTreeVisual inherited;
public:
	FTreeVisual_ST(void);
	virtual ~FTreeVisual_ST(void);

	virtual void Render(float LOD); // LOD - Level Of Detail  [0.0f - min, 1.0f - max], Ignored
#ifdef USE_DX11
	virtual u64 instance_key(float LOD);
	virtual void render_instances(float LOD, u32 count);
#endif
	virtual void Load(LPCSTR N, IReader* data, u32 dwFlags);
	virtual void Copy(dxRender_Visual* pFrom);
	virtual void Release();
private:
	FTreeVisual_ST(const FTreeVisual_ST& other);
	void operator=(const FTreeVisual_ST& other);
};

class FTreeVisual_PM : public FTreeVisual
{
	typedef FTreeVisual inherited;
private:
	FSlideWindowItem* pSWI;
	u32 last_lod;
public:
	FTreeVisual_PM(void);
	virtual ~FTreeVisual_PM(void);

	virtual void Render(float LOD); // LOD - Level Of Detail  [0.0f - min, 1.0f - max], Ignored
#ifdef USE_DX11
	virtual u64 instance_key(float LOD);
	virtual void render_instances(float LOD, u32 count);
#endif
	virtual void Load(LPCSTR N, IReader* data, u32 dwFlags);
	virtual void Copy(dxRender_Visual* pFrom);
	virtual void Release();
private:
	FTreeVisual_PM(const FTreeVisual_PM& other);
	void operator=(const FTreeVisual_PM& other);
};

const int FTreeVisual_tile = 16;
const int FTreeVisual_quant = 32768 / FTreeVisual_tile;
