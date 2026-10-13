#include "stdafx.h"
#pragma hdrstop
#include "dx10EventWrapper.h"
#include "../xrRender/HW.h"
#ifdef USE_DX11
#include "../xrRenderPC_R4/r4_gpu_profiler.h"
#endif

dxPixEventWrapper::dxPixEventWrapper(LPCWSTR wszName)
{
    if (HW.pAnnotation)
        HW.pAnnotation->BeginEvent(wszName);
#ifdef USE_DX11
    gpu_profiler::event_begin(wszName); // GPU time per scope for the performance overlay
#endif
}

dxPixEventWrapper::~dxPixEventWrapper()
{
#ifdef USE_DX11
    gpu_profiler::event_end();
#endif
    if (HW.pAnnotation)
        HW.pAnnotation->EndEvent();
}
