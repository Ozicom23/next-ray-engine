#ifndef dxApplicationRender_included
#define dxApplicationRender_included
#pragma once

#include "..\..\Include\xrRender\ApplicationRender.h"
#include "..\..\xrEngine\x_ray.h"

class dxApplicationRender : public IApplicationRender
{
public:
	virtual void Copy(IApplicationRender& _in);

	virtual void LoadBegin();
	virtual void destroy_loading_shaders();
	virtual void setLevelLogo(LPCSTR pszLogoName);
	virtual void load_draw_internal(CApplication& owner);
	virtual void draw_screen_fade(float alpha);
	//	?????
	virtual void KillHW();
private:
	ref_shader hLevelLogo;
	ref_shader hLevelLogo_Add;
	ref_geom ll_hGeom;
	ref_geom ll_hGeom2;

	ref_shader sh_progress;
	ref_shader m_fade_shader;
	ref_geom m_fade_geom;
	void draw_face(ref_shader& sh, Frect& coords, Frect& tex_coords, const Fvector2& tex_size);
};

#endif	//	ApplicationRender_included
