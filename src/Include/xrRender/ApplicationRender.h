#ifndef ApplicationRender_included
#define ApplicationRender_included
#pragma once

class CApplication;

class IApplicationRender
{
public:
	virtual ~IApplicationRender() { ; }
	virtual void Copy(IApplicationRender& _in) = 0;

	virtual void LoadBegin() = 0;
	virtual void destroy_loading_shaders() = 0;
	virtual void setLevelLogo(LPCSTR pszLogoName) = 0;
	virtual void load_draw_internal(CApplication& owner) = 0;
	// A black quad over the whole frame, 0..1 opaque (CScreenFade)
	virtual void draw_screen_fade(float alpha) = 0;
	//	?????
	virtual void KillHW() = 0;
};

#endif	//	ApplicationRender_included
