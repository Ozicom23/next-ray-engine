#pragma once

#include "r4_upscaler.h"
#include <atomic>

// Intel XeSS Super Resolution through the XeSS SDK (D3D12, the DX12 renderer only; any GPU with Shader Model 6.4).
// libxess.dll is delay-loaded: without it, or without the SDK at build time (XR_XESS_SDK undefined), every call
// fails gracefully and XeSS is not offered.
class CXeSS : public CUpscaler
{
public:
	const char* Name() const override { return "XeSS"; }

	bool Init() override;
	void Shutdown() override;
	bool IsAvailable() const override { return m_available; }

	void GetRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h) override;
	void ClampRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h) override;

	bool CreateFeature(u32 render_w, u32 render_h, u32 out_w, u32 out_h, u32 quality) override;
	void ReleaseFeature() override;

	bool Evaluate(const UpscalerEvalParams& p) override;

private:
	bool InitFeature(bool mask);

	bool m_initialized = false; // Init() ran (successfully or not), not retried until Shutdown()
	bool m_available = false;
	void* m_context = nullptr; // xess_context_handle_t
	bool m_feature = false;    // xessD3D12Init done for the current sizes
	bool m_mask = false;       // initialized with the responsive pixel mask (r4_upscaler_reactive)
	u32 m_out_w = 0, m_out_h = 0, m_quality = 0;
	u32 m_eval_failures = 0;
	std::atomic<bool> m_eval_failed{ false }; // the last evaluation failed where it was recorded
};

extern CXeSS g_XeSS;
