// HW.h: interface for the CHW class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_HW_H__0E25CF4A_FFEC_11D3_B4E3_4854E82A090D__INCLUDED_)
#define AFX_HW_H__0E25CF4A_FFEC_11D3_B4E3_4854E82A090D__INCLUDED_
#pragma once

#if defined(USE_DX11)
#include <d3d11_4.h>
#include <dxgi1_4.h>
#endif

#include "hwcaps.h"

#include "../../build_config_defines.h"

#ifndef _MAYA_EXPORT
#include "stats_manager.h"
#endif

class CHW
#if defined(USE_DX10) || defined(USE_DX11)
	:	public pureAppActivate,
		public pureAppDeactivate
#endif	//	USE_DX10
{
	//	Functions section
public:
	int maxRefreshRate; //ECO_RENDER add
	CHW();
	~CHW();

	void CreateD3D();
	void DestroyD3D();
	void CreateDevice(HWND hw, bool move_window);

	void DestroyDevice();

	void Reset(HWND hw);

#if defined(USE_DX10) || defined(USE_DX11)
	IDXGIOutput* FindOutputOnCurrentAdapter(HMONITOR hMon);
#endif

	void selectResolution(u32& dwWidth, u32& dwHeight, BOOL bWindowed);
	D3DFORMAT selectDepthStencil(D3DFORMAT);
	u32 selectPresentInterval();
	u32 selectGPU();
	u32 selectRefresh(u32 dwWidth, u32 dwHeight, D3DFORMAT fmt);
	void updateWindowProps(HWND hw);
	BOOL support(D3DFORMAT fmt, DWORD type, DWORD usage);

#ifdef DEBUG
#if defined(USE_DX10) || defined(USE_DX11)
	void	Validate(void)	{};
#else	//	USE_DX10
	void	Validate(void)	{	VERIFY(pDevice); VERIFY(pD3D); };
#endif	//	USE_DX10
#else
	void Validate(void)
	{
	};
#endif

	//	Variables section
#if defined(USE_DX11)	//	USE_DX10
public:
    IDXGIFactory2*          m_pFactory; //  DXGI factory
	IDXGIAdapter1*			m_pAdapter;	//	pD3D equivalent
	IDXGIOutput*			m_pOutput;	//	the output we render to (belongs to m_pAdapter)
#if defined(USE_DX12)
	dx12::Device*			pDevice;
	dx12::Context*			pContext;
	IDXGISwapChain1*        m_pSwapChain;	//	on the D3D12 queue; its buffers are wrapped by pDevice
	dx12::RenderTargetView*	pBaseRT;	//	follows the swap chain's current buffer
	dx12::DepthStencilView*	pBaseZB;
	dx12::Context*			pAnnotation;	//	PIX events on the command list
#else
	ID3D11Device1*			pDevice;	//	combine with DX9 pDevice via typedef
	ID3D11DeviceContext1*   pContext;	//	combine with DX9 pDevice via typedef
	IDXGISwapChain1*        m_pSwapChain;
	ID3D11RenderTargetView*	pBaseRT;	//	combine with DX9 pBaseRT via typedef
	ID3D11DepthStencilView*	pBaseZB;
	ID3DUserDefinedAnnotation* pAnnotation;
#endif

	CHWCaps					Caps;

	D3D_DRIVER_TYPE					m_DriverType;	//	DevT equivalent
	DXGI_SWAP_CHAIN_DESC1			m_ChainDesc;	//	DevPP equivalent
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC m_ChainDescFullscreen;
    HWND                            m_hWnd;
	bool							m_bUsePerfhud;
	D3D_FEATURE_LEVEL				FeatureLevel;
	bool 							m_SupportsVRR; // whether we can use DXGI_PRESENT_ALLOW_TEARING etc.
	// Frame pacing (r4_frame_pacing.h): the swap chain created with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
	// (r4_swap_wait, windowed and borderless modes) and its waitable object, nullptr without
	bool							m_FrameLatencyWaitable = false;
	HANDLE							m_hFrameLatency = nullptr;
	bool							m_FrameWaitOwed = false; // a wait is due before the next Present (one per frame)
	UINT	SwapChainFlags() const;
	DXGI_FORMAT SwapChainFormat() const; // 10 bits with HDR10 (r4_hdr10_on) or HDR output (r4_hdr_output, DX12)
	void	CreateSwapChain();  // from m_ChainDesc / m_ChainDescFullscreen, with the color space and the frame latency
	void	DestroySwapChain();
	void	SetupFrameLatency(); // r4_frame_latency on the swap chain (waitable) or the DXGI device
	float	WaitFrameLatency(); // when a wait is due: blocks until the display queue has room; the ms spent
#elif defined(USE_DX10)
public:
	IDXGIAdapter*			m_pAdapter;	//	pD3D equivalent
	IDXGIOutput*			m_pOutput;	//	the output we render to (belongs to m_pAdapter)
	ID3D10Device1*       	pDevice1;	//	combine with DX9 pDevice via typedef
	ID3D10Device*        	pDevice;	//	combine with DX9 pDevice via typedef
	ID3D10Device1*       	pContext1;	//	combine with DX9 pDevice via typedef
	ID3D10Device*        	pContext;	//	combine with DX9 pDevice via typedef
	IDXGISwapChain*         m_pSwapChain;
	ID3D10RenderTargetView*	pBaseRT;	//	combine with DX9 pBaseRT via typedef
	ID3D10DepthStencilView*	pBaseZB;

	CHWCaps					Caps;

	D3D10_DRIVER_TYPE		m_DriverType;	//	DevT equivalent
	DXGI_SWAP_CHAIN_DESC	m_ChainDesc;	//	DevPP equivalent
	HWND					m_hWnd;
	bool					m_bUsePerfhud;
	D3D_FEATURE_LEVEL		FeatureLevel;
#else
private:
	HINSTANCE hD3D;

public:

	IDirect3D9* pD3D; // D3D
	IDirect3DDevice9* pDevice; // render device

	IDirect3DSurface9* pBaseRT;
	IDirect3DSurface9* pBaseZB;

	CHWCaps Caps;

	UINT DevAdapter;
	D3DDEVTYPE DevT;
	D3DPRESENT_PARAMETERS DevPP;
#endif	//	USE_DX10

#ifndef _MAYA_EXPORT
	stats_manager stats_manager;
#endif
#if defined(USE_DX10) || defined(USE_DX11)
	void			UpdateViews();
	DXGI_RATIONAL	selectRefresh(u32 dwWidth, u32 dwHeight, DXGI_FORMAT fmt);

	virtual	void	OnAppActivate();
	virtual void	OnAppDeactivate();
#endif	//	USE_DX10

private:
#if defined(USE_DX10) || defined(USE_DX11)
	void AcquireDefaultOutput();
#endif
#if defined(USE_DX11)
	void SelectAdapterAndOutput(HMONITOR hTargetMonitor);
#endif
	bool m_move_window;
};

extern ECORE_API CHW HW;

#endif // !defined(AFX_HW_H__0E25CF4A_FFEC_11D3_B4E3_4854E82A090D__INCLUDED_)
