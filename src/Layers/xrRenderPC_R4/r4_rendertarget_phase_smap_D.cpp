#include "stdafx.h"

void CRenderTarget::phase_smap_direct(light* L, u32 sub_phase, float clear_depth)
{
	//	TODO: DX10: Check thst we will never need old SMap implementation
	// Targets
	if (RImplementation.o.HW_smap) u_setrt(rt_smap_surf, NULL, NULL, rt_smap_depth->pZRT);
		//else								u_setrt	(rt_smap_surf, NULL, NULL, rt_smap_ZB);
	else
		VERIFY(!"Use HW SMap only for DX10!");


	//	Don't have rect clear for DX10
	//	TODO: DX9:	Full clear must be faster for the near phase for SLI
	//	inobody clears this buffer _this_ frame.
	// Clear
	//if (SE_SUN_NEAR==sub_phase)			{
	// optimized clear
	//	D3DRECT		R;
	//	R.x1		= L->X.D.minX;
	//	R.x2		= L->X.D.maxX;
	//	R.y1		= L->X.D.minY;
	//	R.y2		= L->X.D.maxY;
	//	CHK_DX							(HW.pDevice->Clear( 1L, &R,	  D3DCLEAR_ZBUFFER,	0xFFFFFFFF, 1.0f, 0L));
	//} else {
	// full-clear
	//	CHK_DX							(HW.pDevice->Clear( 0L, NULL, D3DCLEAR_ZBUFFER,	0xFFFFFFFF, 1.0f, 0L));
	//}

	HW.pContext->ClearDepthStencilView(rt_smap_depth->pZRT, D3D_CLEAR_DEPTH, clear_depth, 0L);

	//	Prepare viewport for shadow map rendering
	if (sub_phase != SE_SUN_RAIN_SMAP)
		RImplementation.rmNormal();
	else
	{
		D3D_VIEWPORT VP = {
			(float)L->X.D.minX, (float)L->X.D.minY,
			(float)(L->X.D.maxX - L->X.D.minX),
			(float)(L->X.D.maxY - L->X.D.minY),
			0, 1
		};
		//CHK_DX								(HW.pDevice->SetViewport(&VP));
		HW.pContext->RSSetViewports(1, &VP);
	}

	// Stencil	- disable
	RCache.set_Stencil(FALSE);

	//	TODO: DX10:	Implement culling reverse for DX10
	// Misc		- draw only front/back-faces
	/*
	if (SE_SUN_NEAR==sub_phase)			RCache.set_CullMode			( CULL_CCW	);	// near
	else								{
		if (RImplementation.o.HW_smap)	RCache.set_CullMode			( CULL_CW	);	// far, reversed
		else							RCache.set_CullMode			( CULL_CCW	);	// far, front-faces
	}
	if (RImplementation.o.HW_smap)		RCache.set_ColorWriteEnable	( FALSE		);
	else								RCache.set_ColorWriteEnable	( );
	*/
}

void CRenderTarget::phase_smap_direct_tsh(light* L, u32 sub_phase)
{
	VERIFY(RImplementation.o.Tshadows);
	//u32		_clr						= 0xffffffff;	//color_rgba(127,127,12,12);
	FLOAT ColorRGBA[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	RCache.set_ColorWriteEnable();
	//	Prepare viewport for shadow map rendering
	RImplementation.rmNormal();
	HW.pContext->ClearRenderTargetView(RCache.get_RT(0), ColorRGBA);
	//CHK_DX								(HW.pDevice->Clear( 0L, NULL, D3DCLEAR_TARGET,	_clr,	1.0f, 0L));
}

// Sun shadow map view mask: only the part of a cascade's map that the view can sample needs casters. The view frustum's
// 8 corners go through the cascade's transform; their convex hull, grown by the shadow filter's reach, is the footprint.
// The map was cleared to depth 0 (everything hidden); the footprint is drawn at the far depth, so the depth test
// discards caster pixels outside it before their (alpha tested leaf) shaders run.
void CRenderTarget::smap_mask_view(const Fmatrix& light_xform, u32 map_size, u32 margin_texels)
{
	static const Fvector ndc_corners[8] = {
		{-1, -1, 0}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, 0}, {1, 1, 1}, {1, 1, 0}, {1, -1, 1}, {1, -1, 0}
	};
	Fmatrix view_inv; // full 4x4 inverse: the view transform is perspective
	D3DXMatrixInverse((D3DXMATRIX*)&view_inv, 0, (D3DXMATRIX*)&Device.mFullTransform);
	const float margin = 2.f * float(margin_texels) / float(_max(map_size, 1u));

	// Corners on the map, each with a square of the margin around it (the hull of them is the grown footprint)
	Fvector2 pts[32];
	u32 count = 0;
	for (u32 i = 0; i < 8; i++)
	{
		Fvector w, l;
		view_inv.transform(w, ndc_corners[i]);
		light_xform.transform(l, w);
		if (!_valid(l.x) || !_valid(l.y))
			continue;
		const float x = clampr(l.x, -8.f, 8.f), y = clampr(l.y, -8.f, 8.f); // beyond the map (-1..1) exactness doesn't matter
		pts[count++].set(x - margin, y - margin);
		pts[count++].set(x + margin, y - margin);
		pts[count++].set(x + margin, y + margin);
		pts[count++].set(x - margin, y + margin);
	}

	// Convex hull, Andrew's monotone chain
	std::sort(pts, pts + count, [](const Fvector2& a, const Fvector2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
	auto cross = [](const Fvector2& o, const Fvector2& a, const Fvector2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
	Fvector2 hull[66];
	u32 n = 0;
	for (u32 i = 0; i < count; i++)
	{
		while (n >= 2 && cross(hull[n - 2], hull[n - 1], pts[i]) <= 0.f) n--;
		hull[n++] = pts[i];
	}
	for (int i = int(count) - 2, t = int(n) + 1; i >= 0; i--)
	{
		while (int(n) >= t && cross(hull[n - 2], hull[n - 1], pts[i]) <= 0.f) n--;
		hull[n++] = pts[i];
	}
	if (n > 0) n--; // the first point repeated
	if (n < 3)
	{
		// Degenerate: no mask, the whole map usable
		HW.pContext->ClearDepthStencilView(rt_smap_depth->pZRT, D3D_CLEAR_DEPTH, 1.f, 0L);
		return;
	}

	// The footprint at the far depth (just inside the clip range), as a fan of triangles, in clip space
	const float z = 1.f - 1e-5f;
	const u32 tris = n - 2;
	u32 vBase;
	Fvector* pv = (Fvector*)RCache.Vertex.Lock(tris * 3, g_smap_mask->vb_stride, vBase);
	for (u32 t = 0; t < tris; t++)
	{
		pv[0].set(hull[0].x, hull[0].y, z);
		pv[1].set(hull[t + 1].x, hull[t + 1].y, z);
		pv[2].set(hull[t + 2].x, hull[t + 2].y, z);
		pv += 3;
	}
	RCache.Vertex.Unlock(tris * 3, g_smap_mask->vb_stride);
	RCache.set_xform_world(Fidentity);
	RCache.set_xform_view(Fidentity);
	RCache.set_xform_project(Fidentity);
	RCache.set_Element(s_occq->E[3]);
	RCache.set_Geometry(g_smap_mask);
	RCache.Render(D3DPT_TRIANGLELIST, vBase, tris);
}
